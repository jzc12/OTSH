#pragma once

// Header-only HTTP + JSON dependencies vendored under include/third_party/.
#include "config.h"
#include "db.h"
#include "ht.h"

#include <cstdint>
#include <optional>
#include <string>

namespace otsh {

struct ServerConfig {
  std::string bind_host = "0.0.0.0";
  int port = 8080;
};

int run_server(const DbConfig& db_cfg, const ServerConfig& srv_cfg);

} // namespace otsh

