#pragma once

#include <cstdint>
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
  uint64_t n = 100000;       // target scale
  int k = 2;                 // kick depth bound
  double load_factor = 0.90; // target load factor

  uint64_t seed1 = 0; // bin hash seed
  uint64_t seed2 = 0; // pref/kick seed
  uint64_t seed3 = 0; // fingerprint seed

  uint32_t fingerprint_bits = 16; // fixed to 16 per design doc

  uint64_t mini_bin_size = 0;
  uint64_t num_mini_bins = 0;
  uint64_t fallback_size = 8;
  uint64_t bin_size = 0;
  uint64_t total_bins = 0;
  uint64_t capacity_slots = 0; // total_bins * bin_size
};

} // namespace otsh
