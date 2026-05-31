#include "otsh/bin_free_map.h"
#include "otsh/kkick.h"

namespace otsh {

void BinFreeMap::reset(const KKickGeometry * /*geom*/, size_t cubby_capacity) {
  cap_ = cubby_capacity;
  slot_ma_.configure(8, 2);
  slot_ma_.reset(cap_);
  const MiniArray::Bits one{1ULL};
  for (size_t s = 0; s < cap_; ++s)
    slot_ma_.update(s, one, 1);
}

bool BinFreeMap::slot_flag(size_t slot) const {
  if (slot >= cap_)
    return false;
  if (slot_ma_.bitlen(slot) == 0)
    return false;
  const auto bits = slot_ma_.access(slot);
  return !bits.empty() && ((bits[0] & 1ULL) != 0);
}

void BinFreeMap::mark_used(size_t slot) {
  if (slot >= cap_ || !slot_flag(slot))
    return;
  slot_ma_.erase(slot);
}

void BinFreeMap::mark_free(size_t slot) {
  if (slot >= cap_ || slot_flag(slot))
    return;
  MiniArray::Bits one{1ULL};
  slot_ma_.update(slot, one, 1);
}

bool BinFreeMap::is_free(size_t slot) const {
  return slot_flag(slot);
}

bool BinFreeMap::bin_has_free(const BinRange &bin) const {
  for (size_t s = bin.start; s < bin.end && s < cap_; ++s) {
    if (slot_flag(s))
      return true;
  }
  return false;
}

std::optional<size_t> BinFreeMap::first_free_in_bin(const BinRange &bin) const {
  for (size_t s = bin.start; s < bin.end && s < cap_; ++s) {
    if (slot_flag(s))
      return s;
  }
  return std::nullopt;
}

} // namespace otsh
