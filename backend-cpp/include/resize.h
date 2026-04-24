#pragma once

#include "config.h"

#include <cstdint>
#include <string>

namespace otsh {

struct ResizeMeta {
  bool is_resizing = false;
  uint64_t total_bins = 0;      // current (old) total_bins for hash_table
  uint64_t new_total_bins = 0;  // target total_bins for hash_table_next
  uint64_t resize_progress = 0; // migrated bins in [0, total_bins]
};

// Fixed table names used by online resize.
inline constexpr const char* kActiveTable = "hash_table";
inline constexpr const char* kNextTable = "hash_table_next";
inline constexpr const char* kOldTable = "hash_table_old";

} // namespace otsh

