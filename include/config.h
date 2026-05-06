#pragma once

#include <cstdint>
#include <string>

namespace otsh {

struct ServerConfig {
  std::string bind_host = "0.0.0.0";
  int port = 8080;
};

struct TableParams {
  uint64_t n = 10000;        // 规模参数（N 会取 2^p，使 N <= n <= 2N）
  int k = 2;                 // 保留（对接设计文档的 k-kick 层数参数）
  double load_factor = 0.90; // 保留

  // 用于可逆置换 pi 以及其他哈希/随机选择的种子
  uint64_t seed1 = 0;
  uint64_t seed2 = 0;
  uint64_t seed3 = 0;

  std::string storage = "mysql";

  // mysql：连接信息
  std::string mysql_host = "127.0.0.1";
  uint16_t mysql_port = 3306;
  std::string mysql_user = "root";
  std::string mysql_password = "12345678";
  std::string mysql_database = "otsh";
  std::string mysql_table = "otsh_keys";
};

} // namespace otsh
