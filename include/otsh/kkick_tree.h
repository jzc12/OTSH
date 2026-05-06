#pragma once

namespace otsh {

// k-kick tree 占位接口。
class KKickTree {
public:
  explicit KKickTree(int k) : k_(k) {}
  int k() const { return k_; } // 层数

private:
  int k_ = 2; // 层数
};

} // namespace otsh
