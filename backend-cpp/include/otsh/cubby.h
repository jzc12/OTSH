#pragma once

#include "otsh/mini_array.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace otsh {

// Phase1/2: cubby 是 facility 内的基本存储单元。
// 后续会增加 Meta(MiniArray)、FreeSlotTree、k-kick 元数据等。
struct Cubby {
  int tier = 0;
  size_t capacity = 0;
  size_t size = 0;
  std::vector<std::optional<uint64_t>> slots;
  std::vector<size_t> occupied;

  // Phase4：每个 slot 对应一条变长 meta（暂未写入真实 OTSH 字段，但先接入结构）
  MiniArray meta;
};

} // namespace otsh

