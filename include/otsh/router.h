#pragma once

#include "hash.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace otsh {

class Cubby;

// LocalQueryRouter（正确版，工程实现）
// - key -> (cubby, slot)
// - 用一个二叉 trie 管理哈希后的 bit 串（本实现用 64-bit hash 串），并序列化为
// bitstream
// - 为了先跑通：insert/erase 后允许重建 trie/编码（元素数为
// polylog，重建成本可接受）
class Router {
public:
  using Value = std::pair<Cubby *, size_t>;

  std::pair<std::optional<Value>, uint64_t> locate(uint64_t key) const;
  bool contains(uint64_t key) const;
  uint64_t insert(uint64_t key, Value v);
  uint64_t erase(uint64_t key);
  void clear();

  // 编码后的 bitstream 大小（用于空间统计）。
  uint64_t bits_total() const { return static_cast<uint64_t>(encoded_bits_); }

private:
  struct Entry {
    uint64_t key = 0;
    Value v{};
    uint64_t bits = 0; // hashed bits used by trie
  };
  struct Node {
    int left = -1;
    int right = -1;
    int entry_idx = -1;    // leaf: index into entries_
    int bucket_idx = -1;   // hash-collision bucket (split_bit==64)
    uint8_t split_bit = 0; // internal node: branching bit position [0,64]
  };

  std::vector<Entry> entries_;
  std::vector<Node> nodes_;
  std::vector<std::vector<int>> buckets_;
  int root_ = -1;

  // 真实 bitstream 编码长度（通过 bit-writer 计算）。
  size_t encoded_bits_ = 0;

  static uint64_t key_bits(uint64_t key) {
    // 与设计文档一致：router 对键使用随机 bit 串；这里用 splitmix64 作为稳定
    // hash。
    return splitmix64(key ^ 0x9e3779b97f4a7c15ULL);
  }

  void rebuild();
  static bool bit_at(uint64_t bits, int bitpos) {
    return ((bits >> (63 - bitpos)) & 1ULL) != 0;
  }

  // Patricia build helpers
  int build_range(std::vector<int> &idxs, int l, int r, int bit_lo);
  static int first_diff_bit(uint64_t a, uint64_t b);

  // Bitstream encode helpers
  void recompute_encoded_bits();
  void encode_node(int node);
};

} // namespace otsh
