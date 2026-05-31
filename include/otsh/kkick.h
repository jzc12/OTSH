#pragma once

#include "otsh/system_params.h"

#include <cstdint>
#include <functional>
#include <optional>

namespace otsh {

struct MetaEntry;
struct PermutationHash;
struct TableState;
struct Facility;
class BinFreeMap;

// k-kick 几何：偏好 bin、探测序列（§4.2）。
struct BinRange {
  size_t start = 0;
  size_t end = 0;
  size_t size() const { return end > start ? end - start : 0; }
};

class KKickGeometry {
public:
  explicit KKickGeometry(int k_depth, size_t cubby_capacity, uint64_t K,
                           uint64_t n_hint);

  int k() const { return k_; }
  size_t cubby_capacity() const { return cap_; }
  const std::vector<size_t> &bin_sizes() const { return s_; }

  BinRange preference_bin(uint64_t gx_pi, int depth) const;
  // 在父 bin 内切分深度 d 的子 bin 宽度：min(s_d, parent_span)
  size_t split_size(int depth, size_t parent_span) const;
  size_t probe_index(uint64_t gx_pi, int depth, size_t offset_in_bin) const;
  size_t probe_sequence_length() const;
  size_t probe_base_at(int depth) const;

private:
  int k_ = 0;
  size_t cap_ = 0;
  std::vector<size_t> s_;
  std::vector<size_t> probe_base_; // 每层在探测序列中的起始下标
};

struct KKickSlotView {
  bool occupied = false;
  uint32_t insert_depth = 0;
};

// 在单个 cubby 内执行 §4.4 插入 + 踢出链（元素为 π(x) 语义下的 key）。
struct KKickInsertResult {
  bool ok = false;
  size_t slot = 0;
  uint64_t kick_count = 0;
  uint32_t insert_depth = 0;
  uint32_t probe_j = 0; // 探测序列下标，供 Local Router 记录
};

using KKickReadSlot =
    std::function<KKickSlotView(size_t slot, std::optional<uint64_t> *key_out)>;
using KKickWriteSlot = std::function<bool(
    size_t slot, uint64_t key, uint32_t insert_depth, uint32_t probe_j)>;
using KKickClearSlot = std::function<void(size_t slot)>;

// 探测序列下标 j → 物理槽位（§5.2 查询路径）。
std::optional<size_t> probe_j_to_slot(const KKickGeometry &geom, uint64_t gx_pi,
                                      uint32_t j);

KKickInsertResult kkick_insert_cubby(
    const KKickGeometry &geom, uint64_t key, uint64_t gx_pi,
    KKickReadSlot read_slot, KKickWriteSlot write_slot, KKickClearSlot clear_slot,
    const std::function<uint32_t(int max_d, uint64_t gx)> &random_depth,
    const std::function<uint64_t(uint64_t key)> &gx_of_key = nullptr,
    BinFreeMap *free_map = nullptr);

} // namespace otsh
