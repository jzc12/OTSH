#include "http_server.h"

#include <cstdlib>
#include <iostream>

static const char *env_or(const char *k, const char *defv) {
  const char *v = std::getenv(k);
  return v ? v : defv;
}

int main() {
  otsh::ServerConfig srv;
  srv.bind_host = env_or("BIND_HOST", "0.0.0.0");
  srv.port = std::atoi(env_or("PORT", "8080"));

  try {
    return otsh::run_server(srv);
  } catch (const std::exception &e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}

