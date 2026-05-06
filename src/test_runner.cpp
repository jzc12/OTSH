#include "config.h"
#include "ht.h"
#include "metrics.h"
#include "storage.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace otsh {
namespace {

const char *env_or(const char *k, const char *defv) {
  const char *v = std::getenv(k);
  return v && v[0] ? v : defv;
}

StorageOpenOptions storage_opts_from_env() {
  StorageOpenOptions o;
  o.mysql_host = env_or("OTSH_MYSQL_HOST", "127.0.0.1");
  const char *pp = std::getenv("OTSH_MYSQL_PORT");
  o.mysql_port = pp ? static_cast<uint16_t>(std::atoi(pp)) : 3306;
  o.mysql_user = env_or("OTSH_MYSQL_USER", "root");
  o.mysql_password = env_or("OTSH_MYSQL_PASSWORD", "");
  o.mysql_database = env_or("OTSH_MYSQL_DATABASE", "otsh");
  o.mysql_table = env_or("OTSH_MYSQL_TABLE", "otsh_keys");
  return o;
}

void persist_session_line(const std::string &json_line) {
  auto st = make_storage_mysql();
  const StorageOpenOptions o = storage_opts_from_env();
  if (st->open(o).ok && st->append_test_session_log(json_line).ok)
    return;

  namespace fs = std::filesystem;
  const char *path_env = std::getenv("OTSH_TEST_LOG");
  fs::path path =
      path_env && path_env[0]
          ? fs::path(path_env)
          : fs::path("test_output") / "test_session.log";
  std::error_code ec;
  if (path.has_parent_path())
    fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::app);
  if (!out) {
    std::cerr << "warn: could not open log file " << path.string() << "\n";
    return;
  }
  out << json_line << '\n';
}

std::string json_escape(std::string_view s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '"' || c == '\\')
      o += '\\';
    o += c;
  }
  return o;
}

struct Case {
  std::string name;
  bool ok = false;
};

bool run_case(const char *name, bool cond, std::vector<Case> &cases) {
  cases.push_back({name, cond});
  if (!cond)
    std::cerr << "FAIL: " << name << "\n";
  return cond;
}

std::string metrics_json_snippet() {
  const Metrics::Snapshot m = global_metrics().snapshot();
  std::ostringstream j;
  j << "\"metrics\":{"
    << "\"ops_insert\":" << m.ops_insert << ",\"ops_query\":" << m.ops_query
    << ",\"ops_delete\":" << m.ops_delete
    << ",\"insert_moved_total\":" << m.insert_moved_total
    << ",\"delete_moved_total\":" << m.delete_moved_total << "}";
  return j.str();
}

} // namespace
} // namespace otsh

int main() {
  using namespace otsh;
  std::vector<Case> cases;

  {
    HashTable ht;
    TableParams p;
    p.n = 10'000;
    p.k = 2;
    p.load_factor = 0.90;
    p.seed1 = 1;
    p.seed2 = 2;
    p.seed3 = 3;
    const OpResult ir = ht.init(p);
    run_case("init_ok", ir.ok, cases);
    if (!ir.ok) {
      std::cerr << ir.error << "\n";
    } else {
      const HashTableState st = ht.state();
      run_case("state_after_init", st.N > 0 && st.facilities > 0, cases);
    }
  }

  {
    HashTable ht;
    TableParams p;
    p.n = 50'000;
    p.k = 2;
    p.load_factor = 0.92;
    p.seed1 = 11;
    p.seed2 = 22;
    p.seed3 = 33;
    if (!run_case("workload_init", ht.init(p).ok, cases)) {
      // skip rest
    } else {
      std::mt19937_64 rng(42);
      std::unordered_set<uint64_t> keys;
      const int n_ins = 8000;
      keys.reserve(static_cast<size_t>(n_ins));
      while (static_cast<int>(keys.size()) < n_ins) {
        keys.insert(rng());
      }
      bool ins_ok = true;
      for (uint64_t k : keys) {
        const InsertResult r = ht.insert(k);
        if (!r.ok || !r.inserted) {
          ins_ok = false;
          break;
        }
      }
      run_case("bulk_insert_unique", ins_ok, cases);

      bool q_ok = true;
      for (uint64_t k : keys) {
        const QueryResult r = ht.query(k);
        if (!r.ok || !r.found) {
          q_ok = false;
          break;
        }
      }
      run_case("query_all_inserted", q_ok, cases);

      std::vector<uint64_t> key_vec(keys.begin(), keys.end());
      std::shuffle(key_vec.begin(), key_vec.end(), rng);
      const size_t n_del = key_vec.size() / 2;
      bool del_ok = true;
      for (size_t i = 0; i < n_del; i++) {
        const DeleteResult r = ht.erase(key_vec[i]);
        if (!r.ok || !r.deleted) {
          del_ok = false;
          break;
        }
      }
      run_case("delete_half", del_ok, cases);

      bool absent_ok = true;
      for (size_t i = 0; i < n_del; i++) {
        const QueryResult r = ht.query(key_vec[i]);
        if (!r.ok || r.found) {
          absent_ok = false;
          break;
        }
      }
      run_case("query_deleted_absent", absent_ok, cases);

      bool dup_ins = true;
      for (size_t i = n_del; i < key_vec.size(); i++) {
        const InsertResult r = ht.insert(key_vec[i]);
        if (!r.ok || r.inserted) {
          dup_ins = false;
          break;
        }
      }
      run_case("reinsert_existing_noop", dup_ins, cases);
    }
  }

  int fails = 0;
  for (const auto &c : cases)
    if (!c.ok)
      ++fails;

  const auto t =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::ostringstream json;
  json << "{\"tag\":\"otsh_tests\",\"unix_time\":" << static_cast<long long>(t)
       << ",\"all_passed\":" << (fails == 0 ? "true" : "false")
       << ",\"passes\":" << (cases.size() - static_cast<size_t>(fails))
       << ",\"fails\":" << fails << ",\"cases\":[";
  for (size_t i = 0; i < cases.size(); i++) {
    if (i)
      json << ',';
    json << "{\"name\":\"" << json_escape(cases[i].name) << "\","
         << "\"ok\":" << (cases[i].ok ? "true" : "false") << "}";
  }
  json << "]," << metrics_json_snippet() << "}";

  const std::string line = json.str();
  persist_session_line(line);
  std::cout << line << "\n";

  return fails == 0 ? 0 : 1;
}
