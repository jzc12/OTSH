#pragma once

namespace otsh {

// 论文 k-kick 树：当前由 HashTable::Impl::kkick_insert（src/ht.cpp）在 tail 单 cubby
// 内有界随机探针 + 踢键链实现；本类仅保留 k 参数语义占位，供后续对齐完整树结构。
class KKickTree {
public:
  explicit KKickTree(int k) : k_(k) {}
  int k() const { return k_; } // 层数

private:
  int k_ = 2; // 层数
};

} // namespace otsh
