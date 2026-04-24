#include "hash.h"

namespace otsh {

uint64_t splitmix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

uint64_t HashFunc::h_bin(uint64_t key, uint64_t total_bins) const {
  return splitmix64(key ^ seed1) % total_bins;
}

uint64_t HashFunc::h_pref(uint64_t key, uint64_t num_mini_bins) const {
  return splitmix64(key ^ seed2) % num_mini_bins;
}

uint16_t HashFunc::fingerprint(uint64_t key) const {
  // fp==0 is reserved to mean "empty slot" (per design doc / DB schema usage).
  uint16_t fp = static_cast<uint16_t>(splitmix64(key ^ seed3) & 0xFFFFu);
  return fp == 0 ? static_cast<uint16_t>(1) : fp;
}

uint64_t HashFunc::kick_slot(uint16_t fp, uint64_t depth, uint64_t mini_bin_size) const {
  uint64_t x = (static_cast<uint64_t>(fp) << 32) ^ depth ^ seed2;
  return splitmix64(x) % mini_bin_size;
}

} // namespace otsh

