#pragma once

namespace otsh {

// Phase5: k-kick tree 占位接口。
// 后续会替换 HashTable 的 cubby 内插入/删除为该结构，并把 moved 计数接入
// metrics。
class KKickTree {
public:
  explicit KKickTree(int k) : k_(k) {}
  int k() const { return k_; }

private:
  int k_ = 2;
};

} // namespace otsh
