#include "config.h"

#include "ht.h"

#include "metrics.h"

#include "otsh/system_params.h"

#include "otsh/kkick.h"
#include "otsh/mini_array.h"

#include <chrono>

#include <cstdlib>

#include <filesystem>

#include <fstream>

#include <iostream>

#include <random>

#include <sstream>

#include <string>

#include <unordered_set>

#include <vector>



namespace otsh {

namespace {



const char *env_or(const char *k, const char *defv) {

  const char *v = std::getenv(k);

  return v && v[0] ? v : defv;

}



void persist_session_line(const std::string &line) {

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

  out << line << '\n';

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



std::string metrics_log_snippet() {
  const Metrics::Snapshot m = global_metrics().snapshot();
  std::ostringstream s;
  s << "metrics ops_insert=" << m.ops_insert << " ops_query=" << m.ops_query
    << " ops_delete=" << m.ops_delete
    << " insert_moved_total=" << m.insert_moved_total
    << " delete_moved_total=" << m.delete_moved_total;
  return s.str();
}



bool run_workload(HashTable &ht, int n_ins, uint64_t seed) {

  std::mt19937_64 rng(seed);

  std::unordered_set<uint64_t> keys;

  keys.reserve(static_cast<size_t>(n_ins));

  while (static_cast<int>(keys.size()) < n_ins)

    keys.insert(rng());



  for (uint64_t k : keys) {

    const InsertResult r = ht.insert(k);

    if (!r.ok || !r.inserted)

      return false;

  }

  ht.drain_background_work();

  for (uint64_t k : keys) {

    const QueryResult r = ht.query(k);

    if (!r.ok || !r.found)

      return false;

  }

  std::vector<uint64_t> key_vec(keys.begin(), keys.end());

  std::shuffle(key_vec.begin(), key_vec.end(), rng);

  const size_t n_del = key_vec.size() / 2;

  for (size_t i = 0; i < n_del; i++) {

    const DeleteResult r = ht.erase(key_vec[i]);

    if (!r.ok || !r.deleted)

      return false;

  }

  for (size_t i = 0; i < n_del; i++) {

    const QueryResult r = ht.query(key_vec[i]);

    if (!r.ok || r.found)

      return false;

  }

  return true;

}



} // namespace

} // namespace otsh



int main() {

  using namespace otsh;

  std::vector<Case> cases;



  // --- 基础初始化（重构基准参数）---

  {

    HashTable ht;

    TableParams p = preset_by_name("baseline");

    p.n = 10'000;

    const OpResult ir = ht.init(p);

    run_case("init_ok", ir.ok, cases);

    if (ir.ok) {

      const HashTableState st = ht.state();

      run_case("state_N_K", st.N > 0 && st.K > 0, cases);

      run_case("state_k_kick", st.k_kick == 4, cases);

      run_case("state_preset", st.preset_id == "baseline", cases);

    }

  }



  // --- 标准工作负载 ---

  {

    HashTable ht;

    TableParams p = preset_by_name("baseline");

    p.n = 50'000;
    p.tier_target_divisor = 1; // 单元测试用论文 t_j，避免高频 merge 干扰

    if (run_case("workload_init", ht.init(p).ok, cases))

      run_case("workload_crud", run_workload(ht, 8000, 42), cases);

  }



  // --- k-kick 层数对照（§6.1）---

  for (const char *pid : {"k3", "k5", "k7"}) {

    HashTable ht;

    TableParams p = preset_by_name(pid);

    p.n = 20'000;
    p.tier_target_divisor = 1;

    std::string nm = std::string("kkick_") + pid;

    if (run_case((nm + "_init").c_str(), ht.init(p).ok, cases))

      run_case((nm + "_crud").c_str(), run_workload(ht, 3000, 100), cases);

  }



  // --- K 指数对照 ---

  for (const char *pid : {"K_log2n", "K_log4n"}) {

    HashTable ht;

    TableParams p = preset_by_name(pid);

    p.n = 30'000;
    p.tier_target_divisor = 1;

    const auto d = derive_params(p);

    std::string nm = std::string("Kexp_") + pid;

    if (run_case((nm + "_init").c_str(), ht.init(p).ok, cases)) {

      run_case((nm + "_derived_K").c_str(), d.K > 0, cases);

      run_case((nm + "_crud").c_str(), run_workload(ht, 4000, 7), cases);

    }

  }



  // --- 派生参数公式 ---

  {

    const auto d = derive_params(preset_by_name("n_1e6"));

    run_case("derive_N_pow2", (d.N & (d.N - 1)) == 0 && d.N >= d.n_hint / 2, cases);

    run_case("derive_k_range", d.k_kick >= 3 && d.k_kick <= 7, cases);

    run_case("tier_t1_canon",
             tier_target_count(1, 1'000'000, 4096, true, 1) == 4, cases);
    run_case("tier_t2_canon",
             tier_target_count(2, 1'000'000, 4096, true, 1) == 2, cases);
    run_case("tier_r1_canon",
             tier_cubby_capacity(4096, 1'000'000, 1, true) == 10, cases);
    run_case("tier_t1_formula",
             tier_target_count(1, 1'000'000, 4096, false, 1) == 1, cases);
    run_case("tier_t2_formula",
             tier_target_count(2, 1'000'000, 4096, false, 1) == 1, cases);
    run_case("tier_r1_formula",
             tier_cubby_capacity(4096, 1'000'000, 1, false) == 10, cases);

    const double lg = std::log2(static_cast<double>(std::max<uint64_t>(2, d.n_hint)));
    const size_t expect_s0 =
        static_cast<size_t>(std::max(1.0, std::floor(std::pow(lg, 3.0))));
    run_case("kkick_s0_log3n", kkick_bin_size(0, d.K, d.n_hint) == expect_s0,
             cases);
    const size_t expect_s1 =
        static_cast<size_t>(std::max(1.0, std::floor(std::pow(lg, 6.0))));
    run_case("kkick_s1_log6n", kkick_bin_size(1, d.K, d.n_hint) == expect_s1,
             cases);

    {
      KKickGeometry geom(4, 256, d.K, d.n_hint);
      run_case("kkick_g0_full_cubby",
               geom.preference_bin(0x42, 0).start == 0 &&
                   geom.preference_bin(0x42, 0).end == 256,
               cases);
      const BinRange g3 = geom.preference_bin(0x123456789ULL, 3);
      run_case("kkick_nested_le_nominal",
               g3.size() <= geom.bin_sizes()[3] && g3.size() >= 1, cases);
      run_case("kkick_probe_len",
               geom.probe_sequence_length() >= geom.cubby_capacity(), cases);
    }

  }

  {
    MiniArray ma(64);
    ma.configure(8, 2);
    ma.update(3, MiniArray::Bits{0x42ULL}, 8);
    run_case("miniarray_roundtrip",
             ma.bitlen(3) == 8 && !ma.access(3).empty(), cases);
    run_case("miniarray_rank_select",
             MiniArray::rank_u64(0b1011ULL, 3) == 2 &&
                 MiniArray::select_u64(0b1011ULL, 1) == 1,
             cases);
    ma.update(0, MiniArray::Bits{1ULL}, 8);
    ma.update(5, MiniArray::Bits{2ULL}, 8);
    ma.update(9, MiniArray::Bits{3ULL}, 8);
    run_case("miniarray_logical_access",
             ma.occupied_count() == 4 &&
                 ma.access_logical(2).has_value(),
             cases);
  }

  {
    HashTable ht;
    TableParams p = preset_by_name("baseline");
    p.n = 5000;
    p.tier_target_divisor = 1;
    run_case("facility_ma_init", ht.init(p).ok, cases);
  }



  int fails = 0;

  for (const auto &c : cases)

    if (!c.ok)

      ++fails;



  const auto t =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

  std::ostringstream log;
  log << "=== otsh_tests === unix_time=" << static_cast<long long>(t) << '\n';
  for (const auto &c : cases) {
    log << (c.ok ? "PASS " : "FAIL ") << c.name << '\n';
  }
  log << "--- summary ---\n"
      << "passes=" << (cases.size() - static_cast<size_t>(fails))
      << " fails=" << fails
      << " all_passed=" << (fails == 0 ? "true" : "false") << '\n'
      << metrics_log_snippet() << '\n';

  const std::string report = log.str();
  persist_session_line(report);
  std::cout << report;



  return fails == 0 ? 0 : 1;

}

