#pragma once

#include "otsh/free_slot_tree.h"
#include "otsh/mini_array.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace otsh {

// cubby 是 facility 内的基本存储单元。
struct Cubby {

  int tier = 0;                               // 层级
  size_t capacity = 0;                        // 容量
  size_t size = 0;                            // 大小
  // 商压缩：存 quotient payload（π(x) 的高位）；完整键由 Meta + 路由上下文恢复。
  std::vector<std::optional<uint64_t>> slots;
  std::vector<size_t> occupied;               // 占用索引数组

  FreeSlotTree free_slots; // 设计文档：快速找空槽
  MiniArray meta;          // 元数据
};

} // namespace otsh
