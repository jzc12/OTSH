// OTSH 性能基准：插入 / 查询 / 删除延迟 + Metrics 汇总。
// 用法: otsh_perf [n_insert] [n_rand_queries] [table_n_hint]

#include "config.h"
#include "hash.h"
#include "ht.h"
#include "metrics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using clockk = std::chrono::steady_clock;

uint64_t env_u64(const char *k, uint64_t defv) {
  const char *s = std::getenv(k);
  if (!s || !s[0])
    return defv;
  return static_cast<uint64_t>(std::strtoull(s, nullptr, 10));
}

uint64_t dur_ns(clockk::time_point a, clockk::time_point b) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

void sort_inplace(std::vector<uint64_t> &v) { std::sort(v.begin(), v.end()); }

uint64_t percentile_sorted(const std::vector<uint64_t> &sorted, double p) {
  if (sorted.empty())
    return 0;
  const double x = p * (static_cast<double>(sorted.size() - 1));
  const size_t i = static_cast<size_t>(std::floor(x));
  const size_t j = std::min(i + 1, sorted.size() - 1);
  const double t = x - static_cast<double>(i);
  return static_cast<uint64_t>(sorted[i] * (1 - t) + sorted[j] * t);
}

uint64_t mean_u64(const std::vector<uint64_t> &v) {
  if (v.empty())
    return 0;
  uint64_t s = 0;
  for (uint64_t x : v)
    s += x;
  return s / static_cast<uint64_t>(v.size());
}

struct PhaseStat {
  std::string name;
  uint64_t ops = 0;
  uint64_t elapsed_ns = 0;
  std::vector<uint64_t> lat_ns;
};

void summarize_phase(std::ostream &out, PhaseStat &ph) {
  sort_inplace(ph.lat_ns);
  const uint64_t p50 = percentile_sorted(ph.lat_ns, 0.50);
  const uint64_t p95 = percentile_sorted(ph.lat_ns, 0.95);
  const uint64_t p99 = percentile_sorted(ph.lat_ns, 0.99);
  const uint64_t avg = mean_u64(ph.lat_ns);
  const double sec = static_cast<double>(ph.elapsed_ns) * 1e-9;
  const double qps = sec > 0 ? static_cast<double>(ph.ops) / sec : 0;
  out << std::left << std::setw(24) << ph.name << " ops=" << ph.ops
      << " elapsed_ms=" << std::fixed << std::setprecision(2)
      << (ph.elapsed_ns / 1e6) << " qps=" << std::setprecision(0) << qps
      << "  lat_ns avg=" << avg << " p50=" << p50 << " p95=" << p95
      << " p99=" << p99 << '\n';
}

void json_phase_append(std::ostringstream &j, const char *name,
                       const PhaseStat &ph) {
  const auto &v = ph.lat_ns;
  j << ",\"" << name << "\":{"
    << "\"ops\":" << ph.ops << ",\"elapsed_ms\":" << std::fixed
    << std::setprecision(3) << (ph.elapsed_ns / 1e6) << ",\"lat_ns_avg\":"
    << mean_u64(v) << ",\"p50\":" << percentile_sorted(v, 0.50) << ",\"p95\":"
    << percentile_sorted(v, 0.95) << ",\"p99\":"
    << percentile_sorted(v, 0.99) << '}';
}

} // namespace

int main(int argc, char **argv) {
  using namespace otsh;

  const uint64_t n_ins =
      argc >= 2 ? std::max<uint64_t>(1, std::strtoull(argv[1], nullptr, 10))
                : 50'000;
  const uint64_t n_rand_q =
      argc >= 3 ? std::strtoull(argv[2], nullptr, 10) : 200'000;
  const uint64_t table_n_hint =
      argc >= 4 ? std::strtoull(argv[3], nullptr, 10)
                : std::max<uint64_t>(10'000, n_ins * 4);
  const uint64_t seed = env_u64("OTSH_SEED", 1);

  std::cerr << "=== OTSH perf bench ===\n"
            << "n_insert=" << n_ins << " n_rand_queries=" << n_rand_q
            << " table_n_hint=" << table_n_hint << " seed=" << seed << "\n\n";

  global_metrics().on_init();

  HashTable ht;
  TableParams p;
  p.n = table_n_hint;
  p.k = 4;
  p.load_factor = 0.90;
  p.seed1 = seed ^ 0x1111111111111111ULL;
  p.seed2 = seed ^ 0x2222222222222222ULL;
  p.seed3 = seed ^ 0x3333333333333333ULL;

  const auto t_init0 = clockk::now();
  if (!ht.init(p).ok) {
    std::cerr << "init failed\n";
    return 1;
  }
  const auto t_init1 = clockk::now();
  std::cerr << "init elapsed_ms=" << std::fixed << std::setprecision(3)
            << dur_ns(t_init0, t_init1) / 1e6 << "\n\n";

  std::mt19937_64 rng(static_cast<uint64_t>(seed));
  std::unordered_set<uint64_t> uniq;
  uniq.reserve(static_cast<size_t>(n_ins * 2));
  std::vector<uint64_t> keys;
  keys.reserve(static_cast<size_t>(n_ins));
  while (keys.size() < static_cast<size_t>(n_ins)) {
    const uint64_t k = rng();
    if (uniq.insert(k).second)
      keys.push_back(k);
  }

  PhaseStat ph_ins{.name = "insert"};
  {
    const auto w0 = clockk::now();
    for (uint64_t k : keys) {
      const auto a = clockk::now();
      const InsertResult r = ht.insert(k);
      const auto b = clockk::now();
      const uint64_t ns = dur_ns(a, b);
      ph_ins.lat_ns.push_back(ns);
      global_metrics().on_ht_insert_ns(ns);
      if (!r.ok || !r.inserted) {
        std::cerr << "insert failed at key=" << k << '\n';
        return 2;
      }
      ++ph_ins.ops;
    }
    ph_ins.elapsed_ns = dur_ns(w0, clockk::now());
  }
  summarize_phase(std::cerr, ph_ins);

  PhaseStat ph_q1{.name = "query_all(seq)"};
  {
    const auto w0 = clockk::now();
    for (uint64_t k : keys) {
      const auto a = clockk::now();
      const QueryResult r = ht.query(k);
      const auto b = clockk::now();
      const uint64_t ns = dur_ns(a, b);
      ph_q1.lat_ns.push_back(ns);
      global_metrics().on_ht_query_ns(ns);
      if (!r.ok || !r.found) {
        std::cerr << "query miss key=" << k << '\n';
        return 3;
      }
      ++ph_q1.ops;
    }
    ph_q1.elapsed_ns = dur_ns(w0, clockk::now());
  }
  summarize_phase(std::cerr, ph_q1);

  PhaseStat ph_qr{.name = "query_random(hit)"};
  std::uniform_int_distribution<size_t> dist(0, keys.size() - 1);
  {
    const auto w0 = clockk::now();
    for (uint64_t i = 0; i < n_rand_q; ++i) {
      const uint64_t k = keys[dist(rng)];
      const auto a = clockk::now();
      const QueryResult r = ht.query(k);
      const auto b = clockk::now();
      const uint64_t ns = dur_ns(a, b);
      ph_qr.lat_ns.push_back(ns);
      global_metrics().on_ht_query_ns(ns);
      if (!r.ok || !r.found) {
        std::cerr << "random query miss\n";
        return 4;
      }
      ++ph_qr.ops;
    }
    ph_qr.elapsed_ns = dur_ns(w0, clockk::now());
  }
  summarize_phase(std::cerr, ph_qr);

  std::vector<uint64_t> del_keys = keys;
  std::shuffle(del_keys.begin(), del_keys.end(), rng);
  const size_t n_del = del_keys.size() / 2;
  std::unordered_set<uint64_t> deleted;
  deleted.reserve(n_del * 2);
  for (size_t i = 0; i < n_del; ++i)
    deleted.insert(del_keys[i]);
  std::vector<uint64_t> remaining;
  remaining.reserve(keys.size() - n_del);
  for (uint64_t k : keys) {
    if (!deleted.count(k))
      remaining.push_back(k);
  }

  PhaseStat ph_del{.name = "delete_half"};
  {
    const auto w0 = clockk::now();
    for (size_t i = 0; i < n_del; ++i) {
      const auto a = clockk::now();
      const DeleteResult r = ht.erase(del_keys[i]);
      const auto b = clockk::now();
      const uint64_t ns = dur_ns(a, b);
      ph_del.lat_ns.push_back(ns);
      global_metrics().on_ht_delete_ns(ns);
      if (!r.ok || !r.deleted) {
        std::cerr << "delete failed\n";
        return 5;
      }
      ++ph_del.ops;
    }
    ph_del.elapsed_ns = dur_ns(w0, clockk::now());
  }
  summarize_phase(std::cerr, ph_del);

  PhaseStat ph_qm{.name = "query_mixed(hit+miss)"};
  std::bernoulli_distribution hit_bias(0.7);
  {
    const auto w0 = clockk::now();
    const uint64_t n_mixed = std::max<uint64_t>(1, n_rand_q / 2);
    for (uint64_t i = 0; i < n_mixed; ++i) {
      uint64_t k = 0;
      if (hit_bias(rng) && !remaining.empty()) {
        std::uniform_int_distribution<size_t> dr(0, remaining.size() - 1);
        k = remaining[dr(rng)];
      } else {
        k = rng();
      }
      const auto a = clockk::now();
      (void)ht.query(k);
      const auto b = clockk::now();
      const uint64_t ns = dur_ns(a, b);
      ph_qm.lat_ns.push_back(ns);
      global_metrics().on_ht_query_ns(ns);
      ++ph_qm.ops;
    }
    ph_qm.elapsed_ns = dur_ns(w0, clockk::now());
  }
  summarize_phase(std::cerr, ph_qm);

  const HashTableState st = ht.state();
  const Metrics::Snapshot ms = global_metrics().snapshot();

  std::cerr << "\n--- HashTable state ---\n"
            << "n=" << st.n << " N=" << st.N << " K=" << st.K
            << " facilities=" << st.facilities << "\n";

  std::cerr << "\n--- Metrics snapshot (cumulative) ---\n"
            << "insert_moved_total=" << ms.insert_moved_total
            << " insert_moved_max=" << ms.insert_moved_max << '\n'
            << "delete_moved_total=" << ms.delete_moved_total
            << " delete_moved_max=" << ms.delete_moved_max << '\n'
            << "router_steps_total=" << ms.router_steps_total
            << " router_steps_max=" << ms.router_steps_max << '\n'
            << "meta_bits_total=" << ms.meta_bits_total
            << " meta_bits_max=" << ms.meta_bits_max << '\n'
            << "rebuild_down=" << ms.events.rebuild_down
            << " rebuild_up=" << ms.events.rebuild_up
            << " resize_start=" << ms.events.resize_start
            << " resize_finish=" << ms.events.resize_finish << '\n';

  const double avg_meta_per_ins =
      ms.ops_insert > 0
          ? static_cast<double>(ms.meta_bits_total) /
                static_cast<double>(ms.ops_insert)
          : 0;

  std::cerr << "avg_meta_bits_per_insert_reported=" << std::fixed
            << std::setprecision(2) << avg_meta_per_ins << '\n';

  std::ostringstream j;
  j << '{'
    << "\"bench\":\"otsh_perf\""
    << ",\"n_insert\":" << n_ins << ",\"n_rand_queries\":" << n_rand_q
    << ",\"table_n_hint\":" << table_n_hint << ",\"seed\":" << seed
    << ",\"state\":{\"n\":" << st.n << ",\"N\":" << st.N << ",\"K\":" << st.K
    << ",\"facilities\":" << st.facilities << '}'
    << ",\"elapsed_ms\":{\"init\":" << std::fixed << std::setprecision(3)
    << (dur_ns(t_init0, t_init1) / 1e6) << ",\"insert_all\":"
    << (ph_ins.elapsed_ns / 1e6) << ",\"query_seq\":" << (ph_q1.elapsed_ns / 1e6)
    << ",\"query_rand\":" << (ph_qr.elapsed_ns / 1e6) << ",\"delete_half\":"
    << (ph_del.elapsed_ns / 1e6) << ",\"query_mixed\":" << (ph_qm.elapsed_ns / 1e6)
    << '}';

  json_phase_append(j, "insert", ph_ins);
  json_phase_append(j, "query_seq", ph_q1);
  json_phase_append(j, "query_rand", ph_qr);
  json_phase_append(j, "delete", ph_del);
  json_phase_append(j, "query_mixed", ph_qm);

  j << ",\"metrics\":{"
    << "\"ops_insert\":" << ms.ops_insert << ",\"ops_query\":" << ms.ops_query
    << ",\"ops_delete\":" << ms.ops_delete
    << ",\"insert_moved_total\":" << ms.insert_moved_total
    << ",\"insert_moved_max\":" << ms.insert_moved_max
    << ",\"delete_moved_total\":" << ms.delete_moved_total
    << ",\"delete_moved_max\":" << ms.delete_moved_max
    << ",\"router_steps_total\":" << ms.router_steps_total
    << ",\"router_steps_max\":" << ms.router_steps_max
    << ",\"meta_bits_total\":" << ms.meta_bits_total
    << ",\"meta_bits_max\":" << ms.meta_bits_max
    << ",\"rebuild_down\":" << ms.events.rebuild_down << ",\"rebuild_up\":"
    << ms.events.rebuild_up << ",\"resize_start\":" << ms.events.resize_start
    << ",\"resize_finish\":" << ms.events.resize_finish
    << ",\"avg_meta_bits_per_insert\":" << std::setprecision(4)
    << avg_meta_per_ins << '}';

  j << ",\"latency_hist\":{";
  bool first = true;
  auto emit_hist = [&](const char *name, const Metrics::LatencySummary &L) {
    if (!first)
      j << ',';
    first = false;
    j << '"' << name << "\":{\"count\":" << L.count << ",\"p50_ns\":"
      << L.p50_ns << ",\"p99_ns\":" << L.p99_ns << ",\"max_ns\":" << L.max_ns
      << '}';
  };
  emit_hist("ht_insert", ms.ht_insert);
  emit_hist("ht_query", ms.ht_query);
  emit_hist("ht_delete", ms.ht_delete);
  j << '}';

  j << '}';
  std::cout << j.str() << '\n';
  return 0;
}
