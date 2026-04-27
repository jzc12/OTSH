#include "hash.h"

namespace otsh {

// 计算哈希值
uint64_t splitmix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

// 计算 key 所属的 bin 索引 使用 seed1 和 total_bins 计算
uint64_t HashFunc::h_bin(uint64_t key, uint64_t total_bins) const {
  return splitmix64(key ^ seed1) % total_bins;
}

// 计算 key 所属的偏好 mini-bin 索引 使用 seed2 和 num_mini_bins 计算
uint64_t HashFunc::h_pref(uint64_t key, uint64_t num_mini_bins) const {
  return splitmix64(key ^ seed2) % num_mini_bins;
}

// 计算 key 的指纹 使用 seed3 计算
uint16_t HashFunc::fingerprint(uint64_t key) const {
  // fp==0 表示空槽
  uint16_t fp = static_cast<uint16_t>(splitmix64(key ^ seed3) & 0xFFFFu);
  return fp == 0 ? static_cast<uint16_t>(1) : fp;
}

// 计算踢出槽的索引
uint64_t HashFunc::kick_slot(uint16_t fp, uint64_t depth,
                             uint64_t mini_bin_size) const {
  uint64_t x = (static_cast<uint64_t>(fp) << 32) ^ depth ^ seed2;
  return splitmix64(x) % mini_bin_size;
}

} // namespace otsh
