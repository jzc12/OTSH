#pragma once

#include "otsh/router.h"

#include <memory>
#include <vector>

namespace otsh {

struct Cubby;

// Facility 聚合多 tier cubbies + 顶层 router + 唯一 tail。
struct Facility {
  // 多 tier cubbies
  std::vector<std::vector<std::unique_ptr<Cubby>>> tiers;
  int max_tier = 0;

  // tail：唯一允许非满的 cubby（默认在 tier=0）。
  int tail_tier = 0;
  Cubby *tail = nullptr;             // 尾 cubby 指针
  std::unique_ptr<Cubby> tail_owned; // 尾 cubby 独占所有权

  // 设计文档：D[b] 为 LocalQueryRouter，键经 g* 低位拆出桶索引 b。
  std::vector<Router> D;
};

} // namespace otsh
