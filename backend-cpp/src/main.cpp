#include "http_server.h"

#include <cstdlib>
#include <iostream>

static const char* env_or(const char* k, const char* defv) {
  const char* v = std::getenv(k);
  return v ? v : defv;
}

int main() {
  otsh::DbConfig db;
  db.host = env_or("DB_HOST", "127.0.0.1");
  db.user = env_or("DB_USER", "root");
  db.password = env_or("DB_PASSWORD", "");
  db.database = env_or("DB_NAME", "otsh");
  db.port = static_cast<uint16_t>(std::atoi(env_or("DB_PORT", "3306")));

  otsh::ServerConfig srv;
  srv.bind_host = env_or("BIND_HOST", "0.0.0.0");
  srv.port = std::atoi(env_or("PORT", "8080"));

  try {
    return otsh::run_server(db, srv);
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}

