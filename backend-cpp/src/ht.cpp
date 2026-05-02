#include "ht.h"
#include "hash.h"
#include "metrics.h"
#include "otsh/cubby.h"
#include "otsh/facility.h"
#include "otsh/rebuild.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace otsh {

static uint64_t now_seed() {
  return static_cast<uint64_t>(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

static uint64_t Fmix(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

uint64_t PermutationHash::pi(uint64_t x) const {
  uint32_t L = static_cast<uint32_t>(x >> 32);
  uint32_t R = static_cast<uint32_t>(x & 0xffffffffu);
  const uint64_t keys[4] = {k1, k2, k3, k4};
  for (int i = 0; i < 4; i++) {
    uint32_t newL = R;
    uint32_t f =
        static_cast<uint32_t>(Fmix((static_cast<uint64_t>(R) << 1) ^ keys[i]));
    uint32_t newR = L ^ f;
    L = newL;
    R = newR;
  }
  return (static_cast<uint64_t>(L) << 32) | static_cast<uint64_t>(R);
}

uint64_t PermutationHash::inverse(uint64_t y) const {
  uint32_t L = static_cast<uint32_t>(y >> 32);
  uint32_t R = static_cast<uint32_t>(y & 0xffffffffu);
  const uint64_t keys[4] = {k1, k2, k3, k4};
  for (int i = 3; i >= 0; i--) {
    uint32_t newR = L;
    uint32_t f = static_cast<uint32_t>(
        Fmix((static_cast<uint64_t>(newR) << 1) ^ keys[i]));
    uint32_t newL = R ^ f;
    L = newL;
    R = newR;
  }
  return (static_cast<uint64_t>(L) << 32) | static_cast<uint64_t>(R);
}

class HashTable::Impl {
public:
  Impl() {
    uint64_t s = now_seed();
    pi_.k1 = splitmix64(s ^ 0x1111111111111111ULL);
    pi_.k2 = splitmix64(s ^ 0x2222222222222222ULL);
    pi_.k3 = splitmix64(s ^ 0x3333333333333333ULL);
    pi_.k4 = splitmix64(s ^ 0x4444444444444444ULL);
  }

  OpResult init(const TableParams &p) {
    std::lock_guard<std::mutex> lk(mu_);
    params_ = p;
    n_ = 0;
    global_metrics().on_init();

    active_.reset();
    old_.reset();
    migrate_progress_ = 0;

    active_.N = next_pow2(std::max<uint64_t>(2, p.n));
    active_.K = choose_K(active_.N);
    uint64_t facilities_cnt = std::max<uint64_t>(1, active_.N / active_.K);
    active_.facilities.clear();
    active_.facilities.resize(static_cast<size_t>(facilities_cnt));

    uint64_t s = now_seed() ^ p.seed1 ^ (p.seed2 << 1) ^ (p.seed3 << 2);
    pi_.k1 = splitmix64(s ^ 0x1111111111111111ULL);
    pi_.k2 = splitmix64(s ^ 0x2222222222222222ULL);
    pi_.k3 = splitmix64(s ^ 0x3333333333333333ULL);
    pi_.k4 = splitmix64(s ^ 0x4444444444444444ULL);

    for (auto &f : active_.facilities) {
      f.tiers.clear();
      f.max_tier = std::max(0, params_.k); // 先用 k 作为 tier 上界占位
      f.router.clear();
      f.tail = nullptr;
      f.tail_owned.reset();
      ensure_tail(active_, f);
    }

    return {true, ""};
  }

  InsertResult insert(uint64_t key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto r = insert_no_lock(key, true);
    if (r.ok && r.inserted)
      maybe_resize_locked();
    if (r.ok)
      run_rebuild_budget();
    run_migrate_budget();
    return r;
  }

  QueryResult query(uint64_t key) const {
    std::lock_guard<std::mutex> lk(mu_);
    QueryResult r;
    if (active_.facilities.empty()) {
      r.ok = false;
      r.error = "not_initialized";
      return r;
    }
    uint64_t gx = pi_.pi(key);
    const Facility &f = facility_for_key(active_, gx);
    r.found = f.router.contains(key);
    r.ok = true;
    // 近似：locate 一次，步数由 router 返回（即使不存在也能统计）。
    auto [loc, steps] = f.router.locate(key);
    r.router_probe_steps = steps;
    if (r.found && loc.has_value()) {
      r.cubby_tier = loc->first->tier;
    }
    global_metrics().on_query(steps);

    if (!r.found && old_) {
      const Facility &of = facility_for_key(*old_, gx);
      r.found = of.router.contains(key);
      if (r.found) {
        auto [oloc, osteps] = of.router.locate(key);
        r.router_probe_steps = std::max(r.router_probe_steps, osteps);
        if (oloc.has_value())
          r.cubby_tier = oloc->first->tier;
      }
    }
    return r;
  }

  DeleteResult erase(uint64_t key) {
    std::lock_guard<std::mutex> lk(mu_);
    DeleteResult r;
    if (active_.facilities.empty()) {
      r.ok = false;
      r.error = "not_initialized";
      return r;
    }
    uint64_t gx = pi_.pi(key);
    Facility &f = facility_for_key(active_, gx);
    auto [loc, steps] = f.router.locate(key);
    r.router_probe_steps = steps;
    uint64_t moved_total = 0;
    bool deleted_any = false;

    if (loc) {
      deleted_any = true;
      Cubby *c = loc->first;
      r.cubby_tier = c->tier;
      size_t slot = loc->second;
      c->slots[slot].reset();
      c->meta.update(slot, MiniArray::Bits{}, 0);
      remove_occupied(*c, slot);
      c->size--;
      f.router.erase(key);
      n_--;

      if (c != f.tail) {
        ensure_tail(active_, f);
        if (f.tail && !f.tail->occupied.empty()) {
          size_t take_slot = f.tail->occupied.back();
          uint64_t moved_key = *f.tail->slots[take_slot];

          f.tail->slots[take_slot].reset();
          const auto moved_meta_bits = f.tail->meta.access(take_slot);
          const auto moved_meta_len =
              static_cast<uint32_t>(f.tail->meta.bitlen(take_slot));
          f.tail->meta.update(take_slot, MiniArray::Bits{}, 0);
          f.tail->occupied.pop_back();
          f.tail->size--;

          c->slots[slot] = moved_key;
          c->occupied.push_back(slot);
          c->size++;
          c->meta.update(slot, moved_meta_bits, moved_meta_len);

          f.router.erase(moved_key);
          f.router.insert(moved_key, std::make_pair(c, slot));
          moved_total += 1;
        }
      }
    }

    if (old_) {
      Facility &of = facility_for_key(*old_, gx);
      auto [oloc, osteps] = of.router.locate(key);
      steps = std::max(steps, osteps);
      if (oloc) {
        deleted_any = true;
        Cubby *c = oloc->first;
        if (!loc)
          r.cubby_tier = c->tier;
        size_t slot = oloc->second;
        c->slots[slot].reset();
        c->meta.update(slot, MiniArray::Bits{}, 0);
        remove_occupied(*c, slot);
        c->size--;
        of.router.erase(key);
        n_--;
      }
    }

    r.ok = true;
    r.deleted = deleted_any;
    r.kick_count = moved_total;
    const uint64_t meta_bits =
        f.router.bits_total() + (f.tail ? f.tail->meta.bits_total() : 0);
    global_metrics().on_delete(moved_total, /*router_steps=*/steps,
                               /*meta_bits=*/meta_bits);
    maybe_resize_locked();
    run_rebuild_budget();
    run_migrate_budget();
    return r;
  }

  OpResult bulk_load(const std::vector<uint64_t> &keys) {
    std::lock_guard<std::mutex> lk(mu_);
    for (uint64_t k : keys) {
      (void)insert_no_lock(k, false);
    }
    return {true, ""};
  }

  HashTableState state() const {
    std::lock_guard<std::mutex> lk(mu_);
    return HashTableState{.n = n_,
                          .N = active_.N,
                          .K = active_.K,
                          .facilities =
                              static_cast<uint64_t>(active_.facilities.size())};
  }

  uint64_t pi_of(uint64_t key) const {
    std::lock_guard<std::mutex> lk(mu_);
    return pi_.pi(key);
  }

  void visit_structure(
      const std::function<void(const CubbyStructureView &)> &fn) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (active_.facilities.empty())
      return;
    for (size_t fi = 0; fi < active_.facilities.size(); ++fi) {
      const Facility &f = active_.facilities[fi];
      for (size_t j = 0; j < f.tiers.size(); ++j) {
        for (const auto &up : f.tiers[j]) {
          if (!up)
            continue;
          const Cubby &c = *up;
          CubbyStructureView v;
          v.facility_id = static_cast<int>(fi);
          v.tier = c.tier;
          v.capacity = static_cast<int>(c.capacity);
          v.size = static_cast<int>(c.size);
          v.is_tail = (f.tail == up.get());
          v.slot_keys = c.slots;
          fn(v);
        }
      }
      if (f.tail_owned) {
        const Cubby &c = *f.tail_owned;
        CubbyStructureView v;
        v.facility_id = static_cast<int>(fi);
        v.tier = c.tier;
        v.capacity = static_cast<int>(c.capacity);
        v.size = static_cast<int>(c.size);
        v.is_tail = true;
        v.slot_keys = c.slots;
        fn(v);
      }
    }
  }

private:
  struct TableState {
    uint64_t N = 0;
    uint64_t K = 0;
    std::vector<Facility> facilities;
    void reset() {
      N = 0;
      K = 0;
      facilities.clear();
    }
  };

  TableState active_;
  std::optional<TableState> old_;
  size_t migrate_progress_ = 0;

  struct KickInsertResult {
    bool ok = false;
    size_t slot = 0;
    uint64_t moved = 0; // kick/relocation count
  };

  // --- Phase6: distribution invariant + rebuild scheduler (工程正确版) ---
  RebuildScheduler scheduler_;

  static double iter_log2(double x, int t) {
    x = std::max(2.0, x);
    for (int i = 0; i < t; i++)
      x = std::log2(std::max(2.0, x));
    return x;
  }

  uint64_t target_tier_count(int j) const {
    if (j < 0)
      return 0;
    const double n = std::max<double>(2.0, static_cast<double>(std::max<uint64_t>(1, n_)));
    const double a = iter_log2(n, j);
    const double b = iter_log2(n, j + 1);
    const double tj = (a * a) / std::max(1.0, b * b);
    return static_cast<uint64_t>(std::max(1.0, std::floor(tj)));
  }

  std::unique_ptr<Cubby> create_cubby(uint64_t K, uint64_t N, int tier) {
    auto c = std::make_unique<Cubby>();
    c->tier = tier;
    c->capacity = cubby_capacity(K, N, tier);
    c->size = 0;
    c->slots.assign(c->capacity, std::nullopt);
    c->occupied.clear();
    c->occupied.reserve(c->capacity);
    c->meta.reset(c->capacity);
    return c;
  }

  void maybe_schedule_rebuild(Facility &f) {
    for (int j = 0; j < f.max_tier; j++) {
      if (static_cast<int>(f.tiers.size()) <= j)
        continue;
      const uint64_t tj = target_tier_count(j);
      const uint64_t cnt =
          static_cast<uint64_t>(f.tiers[static_cast<size_t>(j)].size());
      if (cnt > 3 * tj) {
        Facility *pf = &f;
        scheduler_.enqueue([this, pf, j]() { this->rebuild_down(*pf, j); });
      }
    }
  }

  void run_rebuild_budget() { scheduler_.step_budget(1); }

  void rebuild_down(Facility &f, int j) {
    if (j < 0 || j >= f.max_tier)
      return;
    if (static_cast<int>(f.tiers.size()) <= j)
      return;
    const uint64_t tj = target_tier_count(j);
    auto &tier = f.tiers[static_cast<size_t>(j)];
    if (tier.size() < tj || tj == 0)
      return;

    const int j2 = j + 1;
    if (static_cast<int>(f.tiers.size()) <= j2)
      f.tiers.resize(static_cast<size_t>(j2 + 1));

    std::vector<uint64_t> keys;
    for (uint64_t i = 0; i < tj; i++) {
      auto c = std::move(tier.back());
      tier.pop_back();
      for (auto &slot : c->slots) {
        if (slot.has_value())
          keys.push_back(*slot);
      }
    }

    auto newc = create_cubby(active_.K, active_.N, j2);
    for (uint64_t k : keys) {
      auto free = find_free_slot(*newc);
      if (!free)
        break;
      const size_t pos = *free;
      newc->slots[pos] = k;
      newc->occupied.push_back(pos);
      newc->size++;
      newc->meta.update(pos, MiniArray::Bits(1, 1ULL), 1);
      f.router.erase(k);
      f.router.insert(k, std::make_pair(newc.get(), pos));
    }
    f.tiers[static_cast<size_t>(j2)].push_back(std::move(newc));
    global_metrics().on_rebuild_down();
  }

  void maybe_resize_locked() {
    if (active_.facilities.empty() || active_.N < 2)
      return;
    const double lf = (params_.load_factor > 0.0 && params_.load_factor < 1.0)
                          ? params_.load_factor
                          : 0.90;
    if (!old_ && static_cast<double>(n_) > static_cast<double>(active_.N) * lf) {
      start_migration(active_.N * 2);
    } else if (!old_ && n_ > 0 && n_ < active_.N / 4) {
      start_migration(std::max<uint64_t>(2, active_.N / 2));
    }
  }

  void start_migration(uint64_t newN) {
    // Phase7: 双表迁移（渐进）
    scheduler_.clear();
    global_metrics().on_resize_start();
    old_ = std::move(active_);
    active_.reset();
    migrate_progress_ = 0;

    active_.N = next_pow2(newN);
    active_.K = choose_K(active_.N);
    uint64_t facilities_cnt = std::max<uint64_t>(1, active_.N / active_.K);
    active_.facilities.resize(static_cast<size_t>(facilities_cnt));
    for (auto &f : active_.facilities) {
      f.tiers.clear();
      f.max_tier = std::max(0, params_.k);
      f.router.clear();
      f.tail = nullptr;
      f.tail_owned.reset();
      ensure_tail(active_, f);
    }
  }

  void run_migrate_budget() {
    if (!old_)
      return;
    if (migrate_progress_ >= old_->facilities.size()) {
      old_.reset();
      migrate_progress_ = 0;
      global_metrics().on_resize_finish();
      return;
    }

    // 搬迁一个 facility
    Facility &of = old_->facilities[migrate_progress_];
    // 收集 keys（不依赖 old 的 router，以防一致性问题）
    std::vector<uint64_t> keys;
    if (of.tail_owned) {
      for (auto &slot : of.tail_owned->slots) {
        if (slot.has_value())
          keys.push_back(*slot);
      }
    }
    for (auto &tier : of.tiers) {
      for (auto &cp : tier) {
        for (auto &slot : cp->slots) {
          if (slot.has_value())
            keys.push_back(*slot);
        }
      }
    }
    for (uint64_t k : keys) {
      // 迁移期间：如果 key 已在 active（可能被新插入覆盖），跳过
      uint64_t gx = pi_.pi(k);
      Facility &nf = facility_for_key(active_, gx);
      if (nf.router.contains(k))
        continue;
      ensure_tail(active_, nf);
      (void)kkick_insert(nf, nf.tail, k);
      nf.router.erase(k);
      // kkick_insert already inserted mapping
    }

    // 清空旧 facility，标记完成
    of.router.clear();
    of.tiers.clear();
    of.tail_owned.reset();
    of.tail = nullptr;
    migrate_progress_++;
  }

  static uint64_t next_pow2(uint64_t x) {
    if (x <= 1)
      return 1;
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;
    return x + 1;
  }

  static uint64_t choose_K(uint64_t N) {
    double lg = std::log2(std::max<uint64_t>(2, N));
    uint64_t K = static_cast<uint64_t>(std::ceil(lg * lg));
    K = std::max<uint64_t>(64, std::min<uint64_t>(4096, K));
    return K;
  }

  static size_t cubby_capacity(uint64_t K, uint64_t N, int tier) {
    double x = static_cast<double>(std::max<uint64_t>(2, N));
    for (int i = 0; i <= tier; i++)
      x = std::log2(std::max(2.0, x));
    double denom = std::max(1.0, x);
    double cap = static_cast<double>(K) / (denom * denom);
    size_t out = static_cast<size_t>(std::max(4.0, std::floor(cap)));
    out = std::min<size_t>(out, static_cast<size_t>(K));
    return out;
  }

  static std::optional<size_t> find_free_slot(const Cubby &c) {
    for (size_t i = 0; i < c.slots.size(); i++) {
      if (!c.slots[i].has_value())
        return i;
    }
    return std::nullopt;
  }

  static void remove_occupied(Cubby &c, size_t slot) {
    auto it = std::find(c.occupied.begin(), c.occupied.end(), slot);
    if (it != c.occupied.end()) {
      *it = c.occupied.back();
      c.occupied.pop_back();
    }
  }

  Facility &facility_for_key(TableState &t, uint64_t gx) {
    uint64_t r =
        t.facilities.empty() ? 0 : ((gx >> 1) % t.facilities.size());
    return t.facilities[static_cast<size_t>(r)];
  }

  const Facility &facility_for_key(const TableState &t, uint64_t gx) const {
    uint64_t r =
        t.facilities.empty() ? 0 : ((gx >> 1) % t.facilities.size());
    return t.facilities[static_cast<size_t>(r)];
  }

  void ensure_tail(TableState &t, Facility &f) {
    if (f.tail && f.tail->size < f.tail->capacity)
      return;

    // 如果旧 tail 已满，把它作为“满 cubby”挂回 tiers[tail_tier]。
    if (f.tail_owned && f.tail_owned->size == f.tail_owned->capacity) {
      if (static_cast<int>(f.tiers.size()) <= f.tail_tier)
        f.tiers.resize(static_cast<size_t>(f.tail_tier + 1));
      f.tiers[static_cast<size_t>(f.tail_tier)].push_back(
          std::move(f.tail_owned));
      f.tail = nullptr;
    }

    // 新建一个 tail（默认 tier=0）。
    f.tail_tier = 0;
    auto c = std::make_unique<Cubby>();
    c->tier = f.tail_tier;
    c->capacity = cubby_capacity(t.K, t.N, c->tier);
    c->size = 0;
    c->slots.assign(c->capacity, std::nullopt);
    c->occupied.clear();
    c->occupied.reserve(c->capacity);
    c->meta.reset(c->capacity);

    f.tail = c.get();
    f.tail_owned = std::move(c);
  }

  size_t probe_slot(uint64_t key, size_t cap, uint64_t i) const {
    // 伪随机探针序列：splitmix64(key_bits ^ i) % cap
    // 先用 pi(key) 打散，避免 key 低位模式影响。
    const uint64_t gx = pi_.pi(key);
    const uint64_t h = splitmix64(gx ^ (i * 0x9e3779b97f4a7c15ULL));
    return static_cast<size_t>(cap == 0 ? 0 : (h % cap));
  }

  // Step D: 工程正确版 k-kick（有界 kick 链）。
  // 语义：在同一个 cubby 内进行 bounded random-walk kick，最多 kick k 次。
  // - 如果遇到空槽，链终止。
  // - 每次 kick 交换 slot 上的 (key, meta)。
  // - 每次把 key 放入某个 slot 后，都更新 router(key -> (cubby,slot))。
  KickInsertResult kkick_insert(Facility &f, Cubby *c, uint64_t key) {
    KickInsertResult out;
    if (!c || c->capacity == 0) {
      out.ok = false;
      return out;
    }

    const uint64_t kmax = static_cast<uint64_t>(std::max(0, params_.k));

    uint64_t cur_key = key;
    MiniArray::Bits cur_meta(1, 1ULL);
    uint32_t cur_meta_len = 1;

    for (uint64_t step = 0; step <= kmax; step++) {
      const size_t pos = probe_slot(cur_key, c->capacity, step);
      if (!c->slots[pos].has_value()) {
        // empty: place and finish
        c->slots[pos] = cur_key;
        c->occupied.push_back(pos);
        c->size++;
        c->meta.update(pos, cur_meta, cur_meta_len);

        // update router for placed key
        f.router.erase(cur_key);
        f.router.insert(cur_key, std::make_pair(c, pos));

        out.ok = true;
        out.slot = pos;
        out.moved = step;
        return out;
      }

      // occupied: kick
      const uint64_t kicked_key = *c->slots[pos];
      const auto kicked_meta = c->meta.access(pos);
      const uint32_t kicked_meta_len =
          static_cast<uint32_t>(c->meta.bitlen(pos));

      // place current into pos
      c->slots[pos] = cur_key;
      c->meta.update(pos, cur_meta, cur_meta_len);
      f.router.erase(cur_key);
      f.router.insert(cur_key, std::make_pair(c, pos));

      // continue with kicked
      cur_key = kicked_key;
      cur_meta = kicked_meta;
      cur_meta_len = kicked_meta_len;
    }

    // fallback: bounded kicks failed (should be rare given tail has slack)
    // Put into any free slot if possible.
    auto free = find_free_slot(*c);
    if (!free) {
      out.ok = false;
      return out;
    }
    const size_t pos = *free;
    c->slots[pos] = cur_key;
    c->occupied.push_back(pos);
    c->size++;
    c->meta.update(pos, cur_meta, cur_meta_len);
    f.router.erase(cur_key);
    f.router.insert(cur_key, std::make_pair(c, pos));
    out.ok = true;
    out.slot = pos;
    out.moved = kmax + 1;
    return out;
  }

  InsertResult insert_no_lock(uint64_t key, bool /*persist_semantics*/) {
    InsertResult r;
    if (active_.facilities.empty()) {
      r.ok = false;
      r.error = "not_initialized";
      return r;
    }
    uint64_t gx = pi_.pi(key);
    Facility &f = facility_for_key(active_, gx);
    if (f.router.contains(key) || (old_ && facility_for_key(*old_, gx).router.contains(key))) {
      r.ok = true;
      r.inserted = false;
      auto [_, steps] = f.router.locate(key);
      r.router_probe_steps = steps;
      r.kick_count = 0;
      r.cubby_tier = -1;
      const uint64_t meta_bits = f.router.bits_total();
      global_metrics().on_insert(/*moved=*/0, /*router_steps=*/steps,
                                 /*meta_bits=*/meta_bits);
      return r;
    }
    ensure_tail(active_, f);

    // Step D: k-kick insertion into tail cubby
    auto kr = kkick_insert(f, f.tail, key);
    if (!kr.ok) {
      r.ok = false;
      r.error = "tail_full";
      r.router_probe_steps = 0;
      r.kick_count = 0;
      r.cubby_tier = f.tail ? f.tail->tier : -1;
      return r;
    }
    n_++;
    r.ok = true;
    r.inserted = true;
    r.kick_count = kr.moved;
    auto [__, steps] = f.router.locate(key);
    r.router_probe_steps = steps;
    r.cubby_tier = f.tail ? f.tail->tier : -1;
    const uint64_t meta_bits =
        f.router.bits_total() + f.tail->meta.bits_total();
    global_metrics().on_insert(/*moved=*/kr.moved, /*router_steps=*/steps,
                               /*meta_bits=*/meta_bits);
    maybe_schedule_rebuild(f);
    return r;
  }

  mutable std::mutex mu_;
  TableParams params_{};
  PermutationHash pi_{};
  uint64_t n_ = 0;
};

HashTable::HashTable() : impl_(std::make_unique<Impl>()) {}
HashTable::~HashTable() = default;
HashTable::HashTable(HashTable &&) noexcept = default;
HashTable &HashTable::operator=(HashTable &&) noexcept = default;

OpResult HashTable::init(const TableParams &p) { return impl_->init(p); }
InsertResult HashTable::insert(uint64_t key) { return impl_->insert(key); }
QueryResult HashTable::query(uint64_t key) const { return impl_->query(key); }
DeleteResult HashTable::erase(uint64_t key) { return impl_->erase(key); }
OpResult HashTable::bulk_load(const std::vector<uint64_t> &keys) {
  return impl_->bulk_load(keys);
}
HashTableState HashTable::state() const { return impl_->state(); }

void HashTable::visit_structure(
    const std::function<void(const CubbyStructureView &)> &fn) const {
  impl_->visit_structure(fn);
}

uint64_t HashTable::pi_of(uint64_t key) const { return impl_->pi_of(key); }

} // namespace otsh
