#pragma once

// HTTP/JSON 依赖由 CMake FetchContent 提供（cpp-httplib、nlohmann_json），
// 通过 <httplib.h> / <nlohmann/json.hpp> 包含，无需本地 vendor。
#include "config.h"
#include <string>

namespace otsh {

struct ServerConfig {
  std::string bind_host = "0.0.0.0";
  int port = 8080;
};

int run_server(const DbConfig &db_cfg, const ServerConfig &srv_cfg);

} // namespace otsh
