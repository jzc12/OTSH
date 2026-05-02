#pragma once

#include "otsh/router.h"

#include <memory>
#include <vector>

namespace otsh {

struct Cubby;

// Phase2: Facility 聚合多 tier cubbies + 顶层 router + 唯一 tail。
struct Facility {
  // tiers[j]：tier=j 的“满”cubbies（不包含 tail）。
  std::vector<std::vector<std::unique_ptr<Cubby>>> tiers;
  int max_tier = 0;

  // tail：唯一允许非满的 cubby（默认在 tier=0）。
  int tail_tier = 0;
  Cubby *tail = nullptr; // owned by tiers[tail_tier] 或 tail_owned
  std::unique_ptr<Cubby> tail_owned; // tail 独占所有权（便于快速替换/提升）

  Router router;
};

} // namespace otsh

