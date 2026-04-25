#pragma once

#include <stdint.h>
#include <string>

namespace otsh {

struct DbConfig {
  std::string host = "127.0.0.1";
  uint16_t port = 3306;
  std::string user = "root";
  std::string password = "";
  std::string database = "otsh";
};

struct TableParams {
  uint64_t n = 10000;        // 数据量
  int k = 2;                 // 踢出深度边界
  double load_factor = 0.90; // 目标负载因子

  uint64_t seed1 = 0; // bin 哈希种子
  uint64_t seed2 = 0; // pref/kick 种子
  uint64_t seed3 = 0; // fingerprint 种子

  uint32_t fingerprint_bits = 16; // 固定为 16 按设计文档

  uint64_t mini_bin_size = 0;  // mini-bin 大小
  uint64_t num_mini_bins = 0;  // mini-bin 数量
  uint64_t fallback_size = 4;  // fallback 大小
  uint64_t bin_size = 0;       // bin 大小
  uint64_t total_bins = 0;     // 总 bin 数量
  uint64_t capacity_slots = 0; // total_bins * bin_size
};

} // namespace otsh
