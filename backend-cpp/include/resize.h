#pragma once

#include <stdint.h>

namespace otsh {

struct ResizeMeta {
  bool is_resizing = false;
  uint64_t total_bins = 0;      // 当前 (旧的) total_bins for hash_table
  uint64_t new_total_bins = 0;  // 目标 total_bins for hash_table_next
  uint64_t resize_progress = 0; // 迁移的 bins in [0, total_bins]

  // 统计/展示
  uint64_t started_at_ms = 0;     // 扩容开始时间（epoch ms）
  uint64_t migrated_keys = 0;     // 已迁移 key 数（累计）
  uint64_t last_step_ms = 0;      // 最近一步迁移耗时（ms）
  uint64_t elapsed_ms = 0;        // 扩容已运行时长（ms，累计）
  uint64_t finished_total_ms = 0; // 扩容完成总耗时（ms，完成后保留）
};

// 在线 resize 使用的固定表名。
inline constexpr const char *kActiveTable = "hash_table";
inline constexpr const char *kNextTable = "hash_table_next";
inline constexpr const char *kOldTable = "hash_table_old";

} // namespace otsh
