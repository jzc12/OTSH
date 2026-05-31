#pragma once

#include "otsh/mini_array.h"
#include "otsh/router.h"

#include <memory>
#include <vector>

namespace otsh {

struct Cubby;

// §1.2 / §2 Facility：聚合多 tier cubbies + §2.1 facility mini-array。
struct Facility {
  std::vector<std::vector<std::unique_ptr<Cubby>>> tiers;
  int max_tier = 0;

  int tail_tier = 0; // tiers[tail_tier] 为 1-tiered cubby 集合（恒为 tiers[0]）
  Cubby *tail = nullptr;
  std::unique_ptr<Cubby> tail_owned;

  // §2.1：K 桶 facility mini-array（每桶路由表项数 + 编码位宽摘要）
  MiniArray ma;
  // D[b]：g*(x) mod K = b 时 key 的 (cubby, slot) 定位
  std::vector<Router> D;
};

} // namespace otsh
