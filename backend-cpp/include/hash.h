#pragma once

#include <cstdint>

namespace otsh {

uint64_t splitmix64(uint64_t x);

struct HashFunc {
  uint64_t seed1{};
  uint64_t seed2{};
  uint64_t seed3{};

  uint64_t h_bin(uint64_t key, uint64_t total_bins) const;
  uint64_t h_pref(uint64_t key, uint64_t num_mini_bins) const;
  uint16_t fingerprint(uint64_t key) const;

  // pseudo-random slot choice inside a mini-bin for kick
  uint64_t kick_slot(uint16_t fp, uint64_t depth, uint64_t mini_bin_size) const;
};

} // namespace otsh

