#include "otsh/kkick.h"
#include "otsh/bin_free_map.h"
#include "hash.h"

#include <algorithm>
#include <limits>

namespace otsh {
namespace {

static bool slot_occupied(KKickReadSlot &read, size_t p) {
  std::optional<uint64_t> k;
  return read(p, &k).occupied;
}

static bool bin_is_saturated(const BinRange &bin, int depth_d,
                             KKickReadSlot &read) {
  bool any = false;
  for (size_t p = bin.start; p < bin.end; ++p) {
    std::optional<uint64_t> k;
    const auto v = read(p, &k);
    if (!v.occupied)
      return false;
    any = true;
    if (v.insert_depth < static_cast<uint32_t>(depth_d))
      return false;
  }
  return any;
}

static int max_usable_depth(const KKickGeometry &geom, uint64_t gx,
                            KKickReadSlot &read) {
  for (int d = geom.k(); d >= 0; --d) {
    const BinRange b = geom.preference_bin(gx, d);
    if (!bin_is_saturated(b, d, read))
      return d;
  }
  return 0;
}

static std::optional<size_t> first_free_in_bin(const BinRange &bin,
                                              KKickReadSlot &read,
                                              BinFreeMap *free_map) {
  if (free_map) {
    if (auto p = free_map->first_free_in_bin(bin))
      return p;
    return std::nullopt;
  }
  for (size_t p = bin.start; p < bin.end; ++p) {
    if (!slot_occupied(read, p))
      return p;
  }
  return std::nullopt;
}

static std::optional<std::pair<size_t, uint32_t>>
min_depth_occupant(const BinRange &bin, KKickReadSlot &read) {
  std::optional<std::pair<size_t, uint32_t>> best;
  for (size_t p = bin.start; p < bin.end; ++p) {
    std::optional<uint64_t> k;
    const auto v = read(p, &k);
    if (!v.occupied)
      continue;
    if (!best || v.insert_depth < best->second)
      best = std::make_pair(p, v.insert_depth);
  }
  return best;
}

// §4.4 ReInsert：向上层 bin 找空位；insert_depth=0 时仍在 d=0 层其它槽位安置。
static bool reinsert_upward(const KKickGeometry &geom, uint64_t key,
                            uint64_t gx_pi, uint32_t victim_depth,
                            KKickReadSlot &read_slot, KKickWriteSlot &write_slot,
                            BinFreeMap *free_map) {
  int start = static_cast<int>(victim_depth) - 1;
  if (start < 0)
    start = 0;
  for (int d = start; d >= 0; --d) {
    const BinRange bin = geom.preference_bin(gx_pi, d);
    if (bin_is_saturated(bin, d, read_slot))
      continue;
    const auto pos = first_free_in_bin(bin, read_slot, free_map);
    if (!pos)
      continue;
    const size_t off = *pos - bin.start;
    const uint32_t pj =
        static_cast<uint32_t>(geom.probe_index(gx_pi, d, off));
    if (write_slot(*pos, key, static_cast<uint32_t>(d), pj))
      return true;
  }
  return false;
}

static KKickInsertResult insert_kick_once(
    const KKickGeometry &geom, uint64_t key, uint64_t gx_pi,
    KKickReadSlot read_slot, KKickWriteSlot write_slot,
    KKickClearSlot clear_slot,
    const std::function<uint32_t(int max_d, uint64_t gx)> &random_depth,
    const std::function<uint64_t(uint64_t)> &gx_of_key, BinFreeMap *free_map) {
  KKickInsertResult out;
  if (!read_slot || !write_slot || !clear_slot)
    return out;

  const int max_d = max_usable_depth(geom, gx_pi, read_slot);
  const uint32_t s_x = random_depth ? random_depth(max_d, gx_pi) : 0;
  const int depth =
      static_cast<int>(std::min<uint32_t>(static_cast<uint32_t>(max_d), s_x));

  const BinRange target = geom.preference_bin(gx_pi, depth);
  const auto free_pos = first_free_in_bin(target, read_slot, free_map);
  if (free_pos) {
    const size_t off = *free_pos - target.start;
    out.probe_j =
        static_cast<uint32_t>(geom.probe_index(gx_pi, depth, off));
    if (!write_slot(*free_pos, key, static_cast<uint32_t>(depth), out.probe_j))
      return out;
    out.ok = true;
    out.slot = *free_pos;
    out.insert_depth = static_cast<uint32_t>(depth);
    return out;
  }

  const auto victim = min_depth_occupant(target, read_slot);
  if (!victim)
    return out;

  std::optional<uint64_t> evicted_key;
  (void)read_slot(victim->first, &evicted_key);
  if (!evicted_key)
    return out;

  clear_slot(victim->first);
  const size_t off = victim->first - target.start;
  out.probe_j =
      static_cast<uint32_t>(geom.probe_index(gx_pi, depth, off));
  if (!write_slot(victim->first, key, static_cast<uint32_t>(depth), out.probe_j))
    return out;

  out.ok = true;
  out.slot = victim->first;
  out.insert_depth = static_cast<uint32_t>(depth);
  out.kick_count = 1;

  const uint64_t ev_gx = gx_of_key ? gx_of_key(*evicted_key) : gx_pi;
  if (!reinsert_upward(geom, *evicted_key, ev_gx, victim->second, read_slot,
                       write_slot, free_map)) {
    // 回滚：恢复被踢元素，撤销本次插入
    clear_slot(victim->first);
    const size_t voff = victim->first - target.start;
    const uint32_t vpj = static_cast<uint32_t>(
        geom.probe_index(ev_gx, victim->second, voff));
    (void)write_slot(victim->first, *evicted_key, victim->second, vpj);
    out.ok = false;
    out.kick_count = 0;
    out.slot = 0;
    return out;
  }
  return out;
}

} // namespace

KKickGeometry::KKickGeometry(int k_depth, size_t cubby_capacity, uint64_t K,
                             uint64_t n_hint) {
  k_ = std::max(0, k_depth);
  cap_ = std::max<size_t>(1, cubby_capacity);
  s_.resize(static_cast<size_t>(k_ + 1));
  for (int i = 0; i <= k_; ++i)
    s_[static_cast<size_t>(i)] = kkick_bin_size(i, K, n_hint);

  // §4.2：探测序下标按 k→0 每层贡献 s_i 个槽位
  probe_base_.assign(static_cast<size_t>(k_ + 1), 0);
  size_t acc = 0;
  for (int i = k_; i >= 0; --i) {
    probe_base_[static_cast<size_t>(i)] = acc;
    acc += s_[static_cast<size_t>(i)];
  }
}

size_t KKickGeometry::split_size(int depth, size_t parent_span) const {
  if (depth <= 0)
    return parent_span;
  if (depth > k_)
    return 1;
  const size_t nominal = s_[static_cast<size_t>(depth)];
  return std::max<size_t>(1, std::min(nominal, parent_span));
}

BinRange KKickGeometry::preference_bin(uint64_t gx_pi, int depth) const {
  // g_0(x) = 整个 cubby [0, I)
  BinRange r{0, cap_};
  if (depth <= 0)
    return r;
  // g_d 在 g_{d-1} 内按 s_d 均匀切分（§4.1 / §4.2）
  for (int d = 1; d <= depth; ++d) {
    const size_t span = r.size();
    const size_t sz = split_size(d, span);
    const size_t num_bins = std::max<size_t>(1, span / sz);
    const size_t idx =
        static_cast<size_t>(splitmix64(gx_pi ^ static_cast<uint64_t>(d) * 0x9e37ULL) %
                            num_bins);
    const size_t start = r.start + idx * sz;
    const size_t end = std::min(start + sz, r.end);
    r = BinRange{start, end};
  }
  return r;
}

size_t KKickGeometry::probe_index(uint64_t gx_pi, int depth,
                                  size_t offset_in_bin) const {
  (void)gx_pi;
  return probe_base_[static_cast<size_t>(depth)] + offset_in_bin;
}

size_t KKickGeometry::probe_sequence_length() const {
  size_t t = 0;
  for (size_t s : s_)
    t += s;
  return t;
}

size_t KKickGeometry::probe_base_at(int depth) const {
  if (depth < 0 || depth > k_)
    return 0;
  return probe_base_[static_cast<size_t>(depth)];
}

std::optional<size_t> probe_j_to_slot(const KKickGeometry &geom, uint64_t gx_pi,
                                      uint32_t j) {
  for (int d = geom.k(); d >= 0; --d) {
    const size_t base = geom.probe_base_at(d);
    const size_t sz = geom.bin_sizes()[static_cast<size_t>(d)];
    if (j < base || j >= base + sz)
      continue;
    const size_t off = static_cast<size_t>(j) - base;
    const BinRange bin = geom.preference_bin(gx_pi, d);
    if (off >= bin.size())
      return std::nullopt;
    return bin.start + off;
  }
  return std::nullopt;
}

KKickInsertResult kkick_insert_cubby(
    const KKickGeometry &geom, uint64_t key, uint64_t gx_pi,
    KKickReadSlot read_slot, KKickWriteSlot write_slot,
    KKickClearSlot clear_slot,
    const std::function<uint32_t(int max_d, uint64_t gx)> &random_depth,
    const std::function<uint64_t(uint64_t key)> &gx_of_key,
    BinFreeMap *free_map) {
  auto gx_of = [&](uint64_t k) -> uint64_t {
    return gx_of_key ? gx_of_key(k) : gx_pi;
  };
  return insert_kick_once(geom, key, gx_pi, read_slot, write_slot, clear_slot,
                          random_depth, gx_of, free_map);
}

} // namespace otsh
