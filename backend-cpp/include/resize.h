#pragma once

#include "config.h"

#include <cstdint>
#include <string>

namespace otsh {

struct ResizeMeta {
  bool is_resizing = false;
  uint64_t total_bins = 0;      // 当前 (旧的) total_bins for hash_table
  uint64_t new_total_bins = 0;  // 目标 total_bins for hash_table_next
  uint64_t resize_progress = 0; // 迁移的 bins in [0, total_bins]
};

// 在线 resize 使用的固定表名。
inline constexpr const char *kActiveTable = "hash_table";
inline constexpr const char *kNextTable = "hash_table_next";
inline constexpr const char *kOldTable = "hash_table_old";

} // namespace otsh
