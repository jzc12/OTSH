#include "ht.h"
#include "hash.h"
#include "metrics.h"
#include "otsh/cubby.h"
#include "otsh/facility.h"
#include "otsh/kkick.h"
#include "otsh/meta_entry.h"
#include "otsh/rebuild.h"
#include "otsh/system_params.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_set>

namespace otsh {

static void init_cubby_free_slots(Cubby &c) {
  const size_t cap_i = std::min(
      c.capacity,
      static_cast<size_t>((std::numeric_limits<int>::max)()));
  c.free_slots.capacity = static_cast<int>(cap_i);
  c.free_slots.build();
}

static void clear_cubby_slot(Cubby &c, size_t slot) {
  c.slots[slot].reset();
  c.array_b.erase(slot);
  c.array_m.mark_free(slot);
}

static std::optional<size_t> cubby_find_free(Cubby &c) {
  const int i = c.free_slots.find_free();
  if (i < 0)
    return std::nullopt;
  return static_cast<size_t>(i);
}

static uint64_t now_seed() {
  return static_cast<uint64_t>(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

// 设计文档 2.1 节 PermutationHash::F
static uint64_t Feistel_F(uint64_t x, uint64_t k) {
  x ^= k;
  x *= 0x9e3779b97f4a7c15ULL;
  x ^= x >> 32;
  return x;
}

uint64_t PermutationHash::pi(uint64_t x) const {
  uint32_t L = static_cast<uint32_t>(x >> 32);
  uint32_t R = static_cast<uint32_t>(x & 0xffffffffu);
  const uint64_t keys[3] = {k1, k2, k3};
  for (int i = 0; i < 3; i++) {
    uint32_t newL = R;
    uint32_t newR = static_cast<uint32_t>(
        static_cast<uint64_t>(L) ^
        Feistel_F(static_cast<uint64_t>(R), keys[i]));
    L = newL;
    R = newR;
  }
  return (static_cast<uint64_t>(L) << 32) | static_cast<uint64_t>(R);
}

uint64_t PermutationHash::inverse(uint64_t y) const {
  uint32_t L = static_cast<uint32_t>(y >> 32);
  uint32_t R = static_cast<uint32_t>(y & 0xffffffffu);
  const uint64_t keys[3] = {k1, k2, k3};
  for (int i = 2; i >= 0; i--) {
    uint32_t newR = L;
    uint32_t newL = static_cast<uint32_t>(
        static_cast<uint64_t>(R) ^
        Feistel_F(static_cast<uint64_t>(L), keys[i]));
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
    derived_ = derive_params(p);
    params_ = apply_derived(p, derived_);
    n_ = 0;
    global_metrics().on_init();

    active_.reset();
    old_.reset();
    migrate_progress_ = 0;

    active_.N = derived_.N;
    active_.K = derived_.K;
    const uint64_t facilities_cnt =
        std::max<uint64_t>(1, active_.N / active_.K);
    active_.facilities.clear();
    active_.facilities.resize(static_cast<size_t>(facilities_cnt));

    uint64_t s = now_seed() ^ p.seed1 ^ (p.seed2 << 1) ^ (p.seed3 << 2);
    pi_.k1 = splitmix64(s ^ 0x1111111111111111ULL);
    pi_.k2 = splitmix64(s ^ 0x2222222222222222ULL);
    pi_.k3 = splitmix64(s ^ 0x3333333333333333ULL);
    pi_.k4 = splitmix64(s ^ 0x4444444444444444ULL);

    for (auto &f : active_.facilities) {
      f.tiers.clear();
      f.max_tier = derived_.max_tier;
      f.D.assign(static_cast<size_t>(active_.K), Router{});
      f.ma.configure(derived_.fanout, derived_.node_max_bits);
      f.ma.reset(static_cast<size_t>(active_.K));
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
    r.ok = true;
    const size_t fi = facility_index_of(active_, &f);
    const size_t b = route_bucket_for(active_, gx);
    auto [loc, steps] = router_at(f, b).locate(key);
    uint64_t local_steps = 0;
    if (loc) {
      r.found = query_cubby_via_local_router(*loc->first, gx, fi, active_,
                                             local_steps, loc->second);
      if (r.found)
        r.cubby_tier = loc->first->tier;
    }
    r.router_probe_steps = steps + local_steps;
    global_metrics().on_query(r.router_probe_steps);

    if (!r.found && old_) {
      const Facility &of = facility_for_key(*old_, gx);
      const size_t ofi = facility_index_of(*old_, &of);
      const size_t ob = route_bucket_for(*old_, gx);
      auto [oloc, osteps] = router_at(of, ob).locate(key);
      uint64_t olocal = 0;
      if (oloc) {
        r.found = query_cubby_via_local_router(*oloc->first, gx, ofi, *old_,
                                               olocal, oloc->second);
        if (r.found)
          r.cubby_tier = oloc->first->tier;
      }
      r.router_probe_steps = std::max(r.router_probe_steps, osteps + olocal);
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
    const size_t b = route_bucket_for(active_, gx);
    auto [loc, steps] = router_at(f, b).locate(key);
    r.router_probe_steps = steps;
    uint64_t moved_total = 0;
    bool deleted_any = false;

    if (loc) {
      deleted_any = true;
      Cubby *c = loc->first;
      r.cubby_tier = c->tier;
      size_t slot = loc->second;
      clear_cubby_slot(*c, slot);
      remove_occupied(*c, slot);
      c->free_slots.mark_free(static_cast<int>(slot));
      c->size--;
      router_at(f, b).erase(key);
      sync_facility_bucket(f, b);
      local_router_erase(*c, gx, active_.K);
      n_--;

      if (c == f.tail) {
        promote_tail_if_empty(active_, f);
      } else {
        ensure_tail(active_, f);
        if (f.tail && !f.tail->occupied.empty()) {
          // §3.4：随机从 tail 取一元素回填
          const size_t pick =
              splitmix64(key ^ pi_.k4 ^ 0x9e3779b97f4a7c15ULL) %
              f.tail->occupied.size();
          size_t take_slot = f.tail->occupied[pick];
          const size_t fr_tail = facility_index_of(active_, &f);
          const auto moved_opt =
              recover_key_from_slot(*f.tail, take_slot, fr_tail, active_);
          if (moved_opt) {
            const uint64_t moved_key = *moved_opt;

            clear_cubby_slot(*f.tail, take_slot);
            remove_occupied(*f.tail, take_slot);
            f.tail->free_slots.mark_free(static_cast<int>(take_slot));
            f.tail->size--;

            const uint64_t mgx = pi_.pi(moved_key);
            ensure_kick_geom(*c);
            uint32_t pj = 0;
            if (c->kick_geom) {
              for (int d = 0; d <= c->kick_geom->k(); ++d) {
                const BinRange br = c->kick_geom->preference_bin(mgx, d);
                if (slot >= br.start && slot < br.end) {
                  pj = static_cast<uint32_t>(c->kick_geom->probe_index(
                      mgx, d, slot - br.start));
                  break;
                }
              }
            }
            const uint32_t ins = (0u) | ((pj & 0x0fffu) << 4);
            put_quotient_slot(*c, slot, moved_key, mgx, fr_tail, active_, ins);
            c->occupied.push_back(slot);
            c->size++;
            c->free_slots.mark_used(static_cast<int>(slot));
            c->array_m.mark_used(slot);
            local_router_put(*c, mgx, active_.K, moved_key, pj);

            const size_t mb = route_bucket_for(active_, mgx);
            router_at(f, mb).erase(moved_key);
            sync_facility_bucket(f, mb);
            router_at(f, mb).insert(moved_key, std::make_pair(c, slot));
            sync_facility_bucket(f, mb);
            moved_total += 1;
          }
        }
        promote_tail_if_empty(active_, f);
      }
      prune_empty_cubby_from_tiers(f, c);
    }

    if (old_) {
      Facility &of = facility_for_key(*old_, gx);
      const size_t ob = route_bucket_for(*old_, gx);
      auto [oloc, osteps] = router_at(of, ob).locate(key);
      steps = std::max(steps, osteps);
      if (oloc) {
        deleted_any = true;
        Cubby *c = oloc->first;
        if (!loc)
          r.cubby_tier = c->tier;
        size_t slot = oloc->second;
        c->slots[slot].reset();
        c->array_b.update(slot, MiniArray::Bits{}, 0);
        remove_occupied(*c, slot);
        c->free_slots.mark_free(static_cast<int>(slot));
        c->size--;
        router_at(of, ob).erase(key);
        n_--;
      }
    }

    r.ok = true;
    r.deleted = deleted_any;
    r.kick_count = moved_total;
    const uint64_t meta_bits = op_meta_bits_estimate(f, b);
    global_metrics().on_delete(moved_total, /*router_steps=*/steps,
                               /*meta_bits=*/meta_bits);
    maybe_schedule_rebuild(f);
    maybe_resize_locked();
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
    return HashTableState{
        .n = n_,
        .N = active_.N,
        .K = active_.K,
        .facilities = static_cast<uint64_t>(active_.facilities.size()),
        .k_kick = derived_.k_kick,
        .k_polylog_exp = derived_.k_polylog_exp,
        .preset_id = derived_.preset_id};
  }

  uint64_t pi_of(uint64_t key) const {
    std::lock_guard<std::mutex> lk(mu_);
    return pi_.pi(key);
  }

  void drain_background_work() {
    std::lock_guard<std::mutex> lk(mu_);
    drain_background_work_locked();
  }

  static uint64_t facility_active_meta_bits(const Facility &f) {
    uint64_t s = 0;
    for (size_t b = 0; b < f.D.size(); ++b) {
      if (f.D[b].entry_count() == 0)
        continue;
      s += f.D[b].bits_total();
      if (b < f.ma.size())
        s += f.ma.bitlen(b);
    }
    return s;
  }

  uint64_t logical_meta_bits() const {
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t total = 0;
    for (const Facility &f : active_.facilities) {
      total += facility_active_meta_bits(f);
      auto add_cubby = [&](const Cubby *c) {
        if (!c)
          return;
        // 仅统计占用槽位的 B[i] 变长编码，避免空槽位 capacity 虚增 bpk
        for (size_t si = 0; si < c->slots.size(); ++si) {
          if (!c->slots[si].has_value())
            continue;
          total += c->array_b.bitlen(si);
        }
      };
      if (f.tail)
        add_cubby(f.tail);
      for (const auto &tier : f.tiers)
        for (const auto &up : tier)
          add_cubby(up.get());
    }
    return total;
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
          v.tiers_slot = static_cast<int>(j);
          v.capacity = static_cast<int>(c.capacity);
          v.size = static_cast<int>(c.size);
          v.is_tail = (f.tail == up.get());
          v.slot_keys.assign(c.slots.size(), std::nullopt);
          for (size_t si = 0; si < c.slots.size(); ++si) {
            if (!c.slots[si].has_value())
              continue;
            v.slot_keys[si] = recover_key_from_slot(c, si, fi, active_);
          }
          fn(v);
        }
      }
      if (f.tail_owned) {
        const Cubby &c = *f.tail_owned;
        CubbyStructureView v;
        v.facility_id = static_cast<int>(fi);
        v.tier = c.tier;
        v.tiers_slot = -1;
        v.capacity = static_cast<int>(c.capacity);
        v.size = static_cast<int>(c.size);
        v.is_tail = true;
        v.slot_keys.assign(c.slots.size(), std::nullopt);
        for (size_t si = 0; si < c.slots.size(); ++si) {
          if (!c.slots[si].has_value())
            continue;
          v.slot_keys[si] = recover_key_from_slot(c, si, fi, active_);
        }
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
    return tier_target_count(j, derived_.n_hint, active_.K, derived_.tier_use_canon,
                             derived_.tier_target_divisor);
  }

  size_t tier_capacity(int tier) const {
    return tier_cubby_capacity(active_.K, derived_.n_hint, tier,
                               derived_.tier_use_canon);
  }

  void setup_cubby_storage(Cubby &c, uint64_t table_K,
                           uint64_t n_hint) const {
    c.array_b.configure(derived_.fanout, derived_.node_max_bits);
    c.array_b.reset(c.capacity);
    c.kick_geom = std::make_unique<KKickGeometry>(
        derived_.k_kick, c.capacity, table_K, n_hint);
    c.array_m.reset(c.kick_geom.get(), c.capacity);
    init_cubby_free_slots(c);
  }

  void ensure_kick_geom(Cubby &c) const {
    if (c.kick_geom)
      return;
    c.kick_geom = std::make_unique<KKickGeometry>(
        derived_.k_kick, c.capacity, active_.K, derived_.n_hint);
    c.array_m.reset(c.kick_geom.get(), c.capacity);
  }

  bool query_cubby_via_local_router(const Cubby &c, uint64_t gx,
                                    size_t facility_r, const TableState &t,
                                    uint64_t &local_steps,
                                    size_t hint_slot) const {
    local_steps = 0;
    if (!c.kick_geom)
      return false;
    const size_t rb = preferred_router_bucket(gx, t.K, c.capacity);
    if (rb >= c.array_a.size())
      return false;
    local_steps += 1;
    const auto j_opt = c.array_a[rb].query(gx);
    if (j_opt) {
      local_steps += 1;
      const auto slot_opt = probe_j_to_slot(*c.kick_geom, gx, *j_opt);
      if (slot_opt &&
          slot_pi_matches(c, *slot_opt, facility_r, t, gx)) {
        local_steps += 1;
        return true;
      }
    }
    // Facility Router 给出 cubby+槽位提示时，用 meta 中的 probe_j 走 §5.2 路径
    if (hint_slot < c.slots.size() && c.slots[hint_slot].has_value()) {
      MetaEntry me;
      if (decode_slot_meta(c, hint_slot, t, me)) {
        const uint32_t pj = (me.insert_bits >> 4) & 0x0fffu;
        local_steps += 1;
        const auto slot_opt = probe_j_to_slot(*c.kick_geom, gx, pj);
        if (slot_opt &&
            slot_pi_matches(c, *slot_opt, facility_r, t, gx))
          return true;
      }
    }
    return slot_pi_matches(c, hint_slot, facility_r, t, gx);
  }

  void promote_tail_if_empty(TableState &t, Facility &f) {
    if (!f.tail || f.tail->size > 0)
      return;
    if (static_cast<int>(f.tiers.size()) <= f.tail_tier)
      return;
    auto &tier0 = f.tiers[static_cast<size_t>(f.tail_tier)];
    if (tier0.empty())
      return;
    // §3.4：tail 空时从 1-tiered cubby 提升为 tail，可能触发拆分
    f.tail_owned = std::move(tier0.back());
    tier0.pop_back();
    f.tail = f.tail_owned.get();
    f.tail_tier = 0;
    maybe_schedule_rebuild(f);
  }

  void prune_empty_cubby_from_tiers(Facility &f, Cubby *c) {
    if (!c || c == f.tail || c->size > 0)
      return;
    for (auto &tier : f.tiers) {
      const auto it = std::find_if(
          tier.begin(), tier.end(),
          [&](const std::unique_ptr<Cubby> &up) { return up.get() == c; });
      if (it != tier.end()) {
        tier.erase(it);
        return;
      }
    }
  }

  std::unique_ptr<Cubby> create_cubby(uint64_t K, uint64_t n_hint, int tier) {
    auto c = std::make_unique<Cubby>();
    c->tier = tier;
    c->capacity =
        tier_cubby_capacity(K, n_hint, tier, derived_.tier_use_canon);
    c->size = 0;
    c->slots.assign(c->capacity, std::nullopt);
    c->occupied.clear();
    c->occupied.reserve(c->capacity);
    c->array_a.assign(c->capacity, PrefixRouter{});
    setup_cubby_storage(*c, K, n_hint);
    return c;
  }

  void maybe_schedule_rebuild(Facility &f) {
    if (in_rebuild_)
      return;
    if (!params_.enable_rebuild_down && !params_.enable_rebuild_up)
      return;
    const size_t fi = facility_index_of(active_, &f);
    // tier_level：论文 j-tiered（1..max_tier）；tiers[idx] 中 idx = tier_level - 1
    for (int tier_level = 1; tier_level < f.max_tier; ++tier_level) {
      const int idx = tier_level - 1;
      if (static_cast<int>(f.tiers.size()) <= idx)
        continue;
      const uint64_t tj = target_tier_count(tier_level);
      const uint64_t cnt =
          static_cast<uint64_t>(f.tiers[static_cast<size_t>(idx)].size());
      if (params_.enable_rebuild_down && cnt >= 3 * tj && cnt >= tj) {
        scheduler_.enqueue([this, fi, tier_level]() {
          if (fi >= active_.facilities.size())
            return;
          rebuild_down(fi, tier_level);
        });
      } else if (params_.enable_rebuild_up && cnt <= tj / 2) {
        const int up_idx = tier_level;
        if (static_cast<int>(f.tiers.size()) > up_idx &&
            !f.tiers[static_cast<size_t>(up_idx)].empty()) {
          scheduler_.enqueue([this, fi, tier_level]() {
            if (fi >= active_.facilities.size())
              return;
            rebuild_up(fi, tier_level);
          });
        }
      }
    }
  }

  void run_rebuild_budget() { scheduler_.step_budget(1); }

  void drain_background_work_locked() {
    for (int i = 0; i < 2'000'000 && !scheduler_.empty(); ++i)
      scheduler_.step_budget(16);
    for (int i = 0; i < 2'000'000 && old_; ++i)
      run_migrate_budget();
  }

  void rebuild_down(size_t fi, int tier_level) {
    const auto t0 = std::chrono::steady_clock::now();
    if (fi >= active_.facilities.size())
      return;
    Facility &f = active_.facilities[fi];
    // 禁止合并出超过 max_tier 的 cubby（如 max_tier=3 时仅 1→2、2→3）
    if (tier_level < 1 || tier_level >= f.max_tier)
      return;
    const int idx = tier_level - 1;
    const int up_level = tier_level + 1;
    const int up_idx = tier_level;
    if (static_cast<int>(f.tiers.size()) <= idx)
      return;
    const uint64_t tj = target_tier_count(tier_level);
    auto &tier = f.tiers[static_cast<size_t>(idx)];
    if (tj == 0 || tier.size() < tj)
      return;

    if (static_cast<int>(f.tiers.size()) <= up_idx)
      f.tiers.resize(static_cast<size_t>(up_idx + 1));

    const size_t cap_up = tier_capacity(up_level);
    const size_t grab = std::min(static_cast<size_t>(tj), tier.size());

    std::vector<std::unique_ptr<Cubby>> grabbed;
    grabbed.reserve(grab);
    size_t est_keys = 0;
    for (size_t i = 0; i < grab && !tier.empty(); ++i) {
      grabbed.push_back(std::move(tier.back()));
      tier.pop_back();
      if (grabbed.back())
        est_keys += grabbed.back()->size;
    }
    if (est_keys > cap_up) {
      for (auto it = grabbed.rbegin(); it != grabbed.rend(); ++it)
        tier.push_back(std::move(*it));
      return;
    }

    std::vector<uint64_t> keys;
    keys.reserve(est_keys);
    size_t slots_cleared = 0;
    std::unordered_set<size_t> dirty_buckets;
    for (auto &cp : grabbed) {
      if (!cp)
        continue;
      Cubby &c = *cp;
      for (size_t si = 0; si < c.slots.size(); ++si) {
        if (!c.slots[si].has_value())
          continue;
        ++slots_cleared;
        if (auto rk = recover_key_from_slot(c, si, fi, active_); rk) {
          const uint64_t gkx = pi_.pi(*rk);
          const size_t bk = route_bucket_for(active_, gkx);
          router_at(f, bk).erase(*rk);
          local_router_erase(c, gkx, active_.K);
          dirty_buckets.insert(bk);
          keys.push_back(*rk);
        }
      }
      purge_cubby_from_routers(f, cp.get(), &dirty_buckets);
    }
    sync_facility_buckets(f, dirty_buckets);
    grabbed.clear();

    if (keys.empty()) {
      if (slots_cleared > 0 && n_ >= slots_cleared)
        n_ -= slots_cleared;
      return;
    }

    if (n_ >= slots_cleared)
      n_ -= slots_cleared;

    auto merged = create_cubby(active_.K, derived_.n_hint, up_level);
    in_rebuild_ = true;
    size_t reinserted = 0;
    for (uint64_t k : keys) {
      const auto kr = kkick_insert(f, merged.get(), k, fi);
      if (kr.ok)
        ++reinserted;
    }
    in_rebuild_ = false;
    n_ += reinserted;

    if (merged->size == 0)
      return;

    f.tiers[static_cast<size_t>(up_idx)].push_back(std::move(merged));
    const auto t1 = std::chrono::steady_clock::now();
    const uint64_t elapsed = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    // 论文口径：向上合并（j→j+1 tier）计 rebuild_up；实现函数名 rebuild_down 表合并方向
    global_metrics().on_rebuild_up(elapsed);
  }

  // 设计文档：(j+1)-tiered 拆成多个 j-tiered（rebuild_up）。
  void rebuild_up(size_t fi, int tier_level) {
    const auto t0 = std::chrono::steady_clock::now();
    if (fi >= active_.facilities.size())
      return;
    Facility &f = active_.facilities[fi];
    const int up_level = tier_level + 1;
    if (tier_level < 1 || up_level > f.max_tier)
      return;
    const int idx = tier_level - 1;
    const int up_idx = tier_level;
    if (static_cast<int>(f.tiers.size()) <= up_idx)
      return;
    auto &upper = f.tiers[static_cast<size_t>(up_idx)];
    if (upper.empty())
      return;

    const uint64_t tj = target_tier_count(tier_level);
    if (tj == 0)
      return;

    auto big = std::move(upper.back());
    upper.pop_back();

    std::vector<uint64_t> keys;
    keys.reserve(big->size);
    size_t slots_cleared = 0;
    std::unordered_set<size_t> dirty_buckets;
    for (size_t si = 0; si < big->slots.size(); ++si) {
      if (!big->slots[si].has_value())
        continue;
      ++slots_cleared;
      if (auto rk = recover_key_from_slot(*big, si, fi, active_); rk) {
        const uint64_t gkx = pi_.pi(*rk);
        const size_t bk = route_bucket_for(active_, gkx);
        router_at(f, bk).erase(*rk);
        local_router_erase(*big, gkx, active_.K);
        dirty_buckets.insert(bk);
        keys.push_back(*rk);
      }
    }
    purge_cubby_from_routers(f, big.get(), &dirty_buckets);
    big.reset();
    sync_facility_buckets(f, dirty_buckets);

    if (keys.empty()) {
      if (slots_cleared > 0 && n_ >= slots_cleared)
        n_ -= slots_cleared;
      return;
    }

    if (n_ >= slots_cleared)
      n_ -= slots_cleared;

    if (static_cast<int>(f.tiers.size()) <= idx)
      f.tiers.resize(static_cast<size_t>(idx + 1));
    auto &tier_j = f.tiers[static_cast<size_t>(idx)];

    std::vector<std::unique_ptr<Cubby>> pieces;
    pieces.reserve(static_cast<size_t>(tj));
    for (uint64_t i = 0; i < tj; ++i)
      pieces.push_back(create_cubby(active_.K, derived_.n_hint, tier_level));

    in_rebuild_ = true;
    size_t reinserted = 0;
    for (size_t ki = 0; ki < keys.size(); ++ki) {
      Cubby *target = pieces[ki % pieces.size()].get();
      const auto kr = kkick_insert(f, target, keys[ki], fi);
      if (kr.ok)
        ++reinserted;
    }
    in_rebuild_ = false;
    n_ += reinserted;

    for (auto &p : pieces) {
      if (p && p->size > 0)
        tier_j.push_back(std::move(p));
    }
    const auto t1 = std::chrono::steady_clock::now();
    const uint64_t elapsed = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    // 论文口径：向下拆分（j+1→j tier）计 rebuild_down
    global_metrics().on_rebuild_down(elapsed);
  }

  void maybe_resize_locked() {
    if (!params_.enable_resize || in_rebuild_ || active_.facilities.empty() ||
        active_.N < 2)
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

    derived_ = derive_params(params_);
    derived_.N = next_pow2(newN);
    derived_.K = choose_K(derived_.N, derived_.k_polylog_exp);
    params_ = apply_derived(params_, derived_);
    active_.N = derived_.N;
    active_.K = derived_.K;
    const uint64_t facilities_cnt =
        std::max<uint64_t>(1, active_.N / active_.K);
    active_.facilities.resize(static_cast<size_t>(facilities_cnt));
    for (auto &f : active_.facilities) {
      f.tiers.clear();
      f.max_tier = derived_.max_tier;
      f.D.assign(static_cast<size_t>(active_.K), Router{});
      f.ma.configure(derived_.fanout, derived_.node_max_bits);
      f.ma.reset(static_cast<size_t>(active_.K));
      f.tail = nullptr;
      f.tail_owned.reset();
      ensure_tail(active_, f);
    }
  }

  void run_migrate_budget() {
    run_rebuild_budget();
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
    const size_t oidx = migrate_progress_;
    std::vector<uint64_t> keys;
    if (of.tail_owned) {
      for (size_t si = 0; si < of.tail_owned->slots.size(); ++si) {
        if (!of.tail_owned->slots[si].has_value())
          continue;
        if (auto rk = recover_key_from_slot(*of.tail_owned, si, oidx, *old_);
            rk)
          keys.push_back(*rk);
      }
    }
    for (auto &tier : of.tiers) {
      for (auto &cp : tier) {
        for (size_t si = 0; si < cp->slots.size(); ++si) {
          if (!cp->slots[si].has_value())
            continue;
          if (auto rk = recover_key_from_slot(*cp, si, oidx, *old_); rk)
            keys.push_back(*rk);
        }
      }
    }
    for (uint64_t k : keys) {
      // 迁移期间：如果 key 已在 active（可能被新插入覆盖），跳过
      uint64_t gx = pi_.pi(k);
      Facility &nf = facility_for_key(active_, gx);
      const size_t nb = route_bucket_for(active_, gx);
      if (router_at(nf, nb).locate(k).first.has_value())
        continue;
      ensure_tail(active_, nf);
      (void)kkick_insert(nf, nf.tail, k,
                         facility_index_of(active_, &nf));
    }

    // 清空旧 facility，标记完成（保留 D 桶数，避免迁移窗口内 query 仍路由到已处理
    // facility 时 vector 为空导致越界）
    for (auto &rt : of.D)
      rt.clear();
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

  static uint64_t choose_K(uint64_t N, int polylog_exp) {
    TableParams tp;
    tp.n = N;
    tp.k_polylog_exp = polylog_exp;
    return derive_params(tp).K;
  }

  static size_t preferred_router_bucket(uint64_t gx, uint64_t K, size_t cap) {
    if (K == 0 || cap == 0)
      return 0;
    const uint64_t g_k = gx & (K - 1);
    return static_cast<size_t>(g_k % cap);
  }

  static void local_router_put(Cubby &c, uint64_t gx, uint64_t K, uint64_t key,
                               uint32_t probe_j) {
    const size_t b = preferred_router_bucket(gx, K, c.capacity);
    if (b < c.array_a.size())
      c.array_a[b].insert(gx, probe_j);
    (void)key;
  }

  static void local_router_erase(Cubby &c, uint64_t gx, uint64_t K) {
    const size_t b = preferred_router_bucket(gx, K, c.capacity);
    if (b < c.array_a.size())
      c.array_a[b].erase(gx);
  }

  static void remove_occupied(Cubby &c, size_t slot) {
    auto it = std::find(c.occupied.begin(), c.occupied.end(), slot);
    if (it != c.occupied.end()) {
      *it = c.occupied.back();
      c.occupied.pop_back();
    }
  }

  static size_t route_bucket_for(const TableState &t, uint64_t gx) {
    if (t.N < 2 || t.K == 0)
      return 0;
    const uint64_t g = gx & (t.N - 1);
    return static_cast<size_t>(g % t.K);
  }

  static size_t facility_index_of(const TableState &t, const Facility *fp) {
    for (size_t i = 0; i < t.facilities.size(); ++i) {
      if (&t.facilities[i] == fp)
        return i;
    }
    return 0;
  }

  static uint32_t logn_of(const TableState &t) { return log2_pow2(t.N); }

  bool decode_slot_meta(const Cubby &c, size_t slot, const TableState &t,
                        MetaEntry &me) const {
    const uint32_t bl = c.array_b.bitlen(slot);
    if (bl == 0)
      return false;
    const auto bits = c.array_b.access(slot);
    return decode_meta_entry(bits, bl, meta_layout(t.K, c.capacity), me);
  }

  bool slot_pi_matches(const Cubby &c, size_t slot, size_t facility_r,
                       const TableState &t, uint64_t key_gx) const {
    if (slot >= c.slots.size() || !c.slots[slot].has_value())
      return false;
    MetaEntry me;
    if (!decode_slot_meta(c, slot, t, me))
      return false;
    const auto gx = reconstruct_gx_pi(facility_r, t.N, t.K, c.capacity, slot,
                                      *c.slots[slot], me, logn_of(t));
    return gx.has_value() && *gx == key_gx;
  }

  std::optional<uint64_t> recover_key_from_slot(const Cubby &c, size_t slot,
                                                size_t facility_r,
                                                const TableState &t) const {
    if (slot >= c.slots.size() || !c.slots[slot].has_value())
      return std::nullopt;
    MetaEntry me;
    if (!decode_slot_meta(c, slot, t, me))
      return std::nullopt;
    const auto gx = reconstruct_gx_pi(facility_r, t.N, t.K, c.capacity, slot,
                                        *c.slots[slot], me, logn_of(t));
    if (!gx.has_value())
      return std::nullopt;
    return pi_.inverse(*gx);
  }

  void put_quotient_slot(Cubby &c, size_t slot, uint64_t key, uint64_t gx_pi,
                         size_t facility_r, const TableState &t, uint32_t ins) {
    (void)facility_r;
    const uint32_t ln = logn_of(t);
    const MetaCodecLayout ly = meta_layout(t.K, c.capacity);
    const MetaEntry me = make_meta_entry(key, gx_pi, slot, facility_r, t.N, t.K,
                                         c.capacity, ins);
    auto pr = encode_meta_entry(me, ly);
    c.slots[slot] = quotient_payload(gx_pi, ln);
    c.array_b.update(slot, pr.first, pr.second);
  }

  static void sync_facility_bucket(Facility &f, size_t b) {
    if (b >= f.ma.size() || b >= f.D.size())
      return;
    const size_t cnt = f.D[b].entry_count();
    const uint64_t enc = f.D[b].bits_total();
    MiniArray::Bits payload{(cnt & 0xFFFFu) | ((enc & 0xFFFFu) << 16)};
    f.ma.update(b, payload, 32);
  }

  static void purge_cubby_from_routers(Facility &f, const Cubby *cb,
                                       std::unordered_set<size_t> *dirty) {
    for (size_t b = 0; b < f.D.size(); ++b) {
      const size_t before = f.D[b].entry_count();
      f.D[b].erase_cubby(cb);
      if (dirty && f.D[b].entry_count() != before)
        dirty->insert(b);
    }
  }

  static void sync_facility_buckets(Facility &f,
                                    const std::unordered_set<size_t> &buckets) {
    for (size_t b : buckets) {
      if (b < f.ma.size() && b < f.D.size())
        sync_facility_bucket(f, b);
    }
  }

  static uint64_t facility_router_bits_sum(const Facility &f) {
    uint64_t s = 0;
    for (const auto &rt : f.D)
      s += rt.bits_total();
    return s;
  }

  // 热路径元数据估计：仅统计本操作涉及的桶，避免 O(K) 全表扫描。
  static uint64_t op_meta_bits_estimate(const Facility &f, size_t bucket) {
    uint64_t s = 0;
    if (bucket < f.D.size())
      s += f.D[bucket].bits_total();
    if (f.tail)
      s += f.tail->array_b.bits_total();
    return s;
  }

  Router &router_at(Facility &f, size_t b) { return f.D.at(b); }

  const Router &router_at(const Facility &f, size_t b) const {
    return f.D.at(b);
  }

  // g* = π(x) 低 log N 位 → 设施 r = g/K，桶 b = g mod K（设计文档 3.2/插入流程）。
  Facility &facility_for_key(TableState &t, uint64_t gx) {
    static Facility unused{};
    if (t.facilities.empty() || t.K == 0)
      return unused;
    const uint64_t g = gx & (t.N - 1);
    const uint64_t r = g / t.K;
    return t.facilities[static_cast<size_t>(r % t.facilities.size())];
  }

  const Facility &facility_for_key(const TableState &t, uint64_t gx) const {
    static const Facility unused{};
    if (t.facilities.empty() || t.K == 0)
      return unused;
    const uint64_t g = gx & (t.N - 1);
    const uint64_t r = g / t.K;
    return t.facilities[static_cast<size_t>(r % t.facilities.size())];
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
      maybe_schedule_rebuild(f);
    }

    // 新建 tail（1-tiered）。
    f.tail_tier = 0;
    auto c = create_cubby(t.K, derived_.n_hint, 1);
    f.tail = c.get();
    f.tail_owned = std::move(c);
  }

  // §4.4 k-kick：层级 bin + 饱和踢出 + 向上 ReInsert。
  KickInsertResult kkick_insert(Facility &f, Cubby *c, uint64_t key,
                                size_t facility_r) {
    KickInsertResult out;
    if (!c || c->capacity == 0) {
      out.ok = false;
      return out;
    }

    ensure_kick_geom(*c);
    const KKickGeometry &geom = *c->kick_geom;
    const uint64_t gx = pi_.pi(key);

    auto router_erase_key = [&](uint64_t ky) {
      const size_t bk = route_bucket_for(active_, pi_.pi(ky));
      router_at(f, bk).erase(ky);
      sync_facility_bucket(f, bk);
    };
    auto router_put_key = [&](uint64_t ky, Cubby *cb, size_t slot) {
      const size_t bk = route_bucket_for(active_, pi_.pi(ky));
      router_at(f, bk).insert(ky, std::make_pair(cb, slot));
      sync_facility_bucket(f, bk);
    };

    KKickReadSlot read_slot = [&](size_t slot, std::optional<uint64_t> *kout) {
      KKickSlotView v{};
      if (slot >= c->slots.size() || !c->slots[slot].has_value()) {
        if (kout)
          *kout = std::nullopt;
        return v;
      }
      v.occupied = true;
      if (kout)
        *kout = recover_key_from_slot(*c, slot, facility_r, active_);
      MetaEntry me;
      if (decode_slot_meta(*c, slot, active_, me))
        v.insert_depth = me.insert_bits & 0x0fu;
      return v;
    };

    KKickWriteSlot write_slot = [&](size_t slot, uint64_t ky, uint32_t depth,
                                    uint32_t probe_j) -> bool {
      if (slot >= c->capacity)
        return false;
      const bool was = c->slots[slot].has_value();
      if (was) {
        if (auto oldk = recover_key_from_slot(*c, slot, facility_r, active_)) {
          router_erase_key(*oldk);
          local_router_erase(*c, pi_.pi(*oldk), active_.K);
        }
      } else {
        c->occupied.push_back(slot);
        c->size++;
        c->free_slots.mark_used(static_cast<int>(slot));
        c->array_m.mark_used(slot);
      }
      const uint64_t gxc = pi_.pi(ky);
      const uint32_t ins =
          (depth & 0x0fu) | ((probe_j & 0x0fffu) << 4);
      put_quotient_slot(*c, slot, ky, gxc, facility_r, active_, ins);
      router_put_key(ky, c, slot);
      local_router_put(*c, gxc, active_.K, ky, probe_j);
      return true;
    };

    KKickClearSlot clear_slot = [&](size_t slot) {
      if (slot >= c->slots.size() || !c->slots[slot].has_value())
        return;
      if (auto oldk = recover_key_from_slot(*c, slot, facility_r, active_)) {
        router_erase_key(*oldk);
        local_router_erase(*c, pi_.pi(*oldk), active_.K);
      }
      c->slots[slot].reset();
      c->array_b.erase(slot);
      remove_occupied(*c, slot);
      c->free_slots.mark_free(static_cast<int>(slot));
      c->array_m.mark_free(slot);
      c->size--;
    };

    auto random_depth = [&](int max_d, uint64_t gx_pi) -> uint32_t {
      const uint64_t h = splitmix64(gx_pi ^ pi_.k4 ^ 0x9e3779b97f4a7c15ULL);
      return static_cast<uint32_t>(h % static_cast<uint64_t>(max_d + 1));
    };

    auto gx_of = [&](uint64_t ky) { return pi_.pi(ky); };
    const auto kr = kkick_insert_cubby(geom, key, gx, read_slot, write_slot,
                                       clear_slot, random_depth, gx_of,
                                       &c->array_m);
    out.ok = kr.ok;
    out.slot = kr.slot;
    out.moved = kr.kick_count;
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
    const size_t b = route_bucket_for(active_, gx);
    if (auto loc = router_at(f, b).locate(key); loc.first.has_value()) {
      r.ok = true;
      r.inserted = false;
      r.router_probe_steps = loc.second;
      r.kick_count = 0;
      r.cubby_tier = -1;
      const uint64_t meta_bits = op_meta_bits_estimate(f, b);
      global_metrics().on_insert(/*moved=*/0, /*router_steps=*/loc.second,
                                 /*meta_bits=*/meta_bits);
      return r;
    }
    if (old_) {
      Facility &of = facility_for_key(*old_, gx);
      const size_t ob = route_bucket_for(*old_, gx);
      if (auto oloc = router_at(of, ob).locate(key); oloc.first.has_value()) {
        r.ok = true;
        r.inserted = false;
        r.router_probe_steps = oloc.second;
        r.kick_count = 0;
        r.cubby_tier = -1;
        const uint64_t meta_bits = op_meta_bits_estimate(f, b);
        global_metrics().on_insert(/*moved=*/0, /*router_steps=*/oloc.second,
                                   /*meta_bits=*/meta_bits);
        return r;
      }
    }
    ensure_tail(active_, f);

    // Step D: k-kick insertion into tail cubby
    auto kr = kkick_insert(f, f.tail, key,
                           facility_index_of(active_, &f));
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
    auto [__, steps] = router_at(f, b).locate(key);
    r.router_probe_steps = steps;
    r.cubby_tier = f.tail ? f.tail->tier : -1;
    const uint64_t meta_bits = op_meta_bits_estimate(f, b);
    global_metrics().on_insert(/*moved=*/kr.moved, /*router_steps=*/steps,
                               /*meta_bits=*/meta_bits);
    maybe_schedule_rebuild(f);
    return r;
  }

  mutable std::mutex mu_;
  TableParams params_{};
  DerivedParams derived_{};
  PermutationHash pi_{};
  uint64_t n_ = 0;
  bool in_rebuild_ = false;
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

void HashTable::drain_background_work() { impl_->drain_background_work(); }

uint64_t HashTable::logical_meta_bits() const {
  return impl_->logical_meta_bits();
}

} // namespace otsh
