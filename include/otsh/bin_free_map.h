#pragma once

#include "otsh/mini_array.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace otsh {

struct BinRange;
class KKickGeometry;

// §3.3 mini-array M：cubby 槽位空闲位图（与 preference bin 配合，按区间扫描）。
class BinFreeMap {
public:
  void reset(const KKickGeometry *geom, size_t cubby_capacity);
  void mark_used(size_t slot);
  void mark_free(size_t slot);

  bool is_free(size_t slot) const;
  std::optional<size_t> first_free_in_bin(const BinRange &bin) const;
  bool bin_has_free(const BinRange &bin) const;

  const MiniArray &slot_bitmap() const { return slot_ma_; }

private:
  size_t cap_ = 0;
  MiniArray slot_ma_;
  bool slot_flag(size_t slot) const;
};

} // namespace otsh
