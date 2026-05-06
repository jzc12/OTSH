#pragma once

#include <cstdint>
#include <vector>

namespace otsh {

// 设计文档 3.1：空闲槽位树 — 线性化存储，O(log padded) 定位首个空槽。
// `bitmaps[i]` 低 32 位存线段树节点 i 的区间空闲槽数量（1-based 下标，与根到叶 BFS 一致）。
struct FreeSlotTree {
  int capacity = 0;
  std::vector<uint64_t> bitmaps;

  void build();
  int find_free() const;
  void mark_used(int i);
  void mark_free(int i);

private:
  int n = 0; // 叶层宽度（>=capacity 的最小 2 幂）

  void pull_up(int p);
  bool valid_idx(int i) const {
    return i >= 0 && i < capacity;
  }
};

} // namespace otsh
