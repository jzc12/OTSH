#include "http_server.h"
#include "ht.h"
#include "metrics.h"
#include "storage.h"

#include <chrono>
#include <climits>
#include <cmath>
#include <ctime>
#include <fstream>
#include <httplib.h>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <map>
#include <unordered_map>
#include <vector>

namespace otsh {

using json = nlohmann::json;

static uint64_t j_u64(const json &j, const char *k, uint64_t defv) {
  if (!j.contains(k))
    return defv;
  return j.at(k).get<uint64_t>();
}
static int j_i(const json &j, const char *k, int defv) {
  if (!j.contains(k))
    return defv;
  return j.at(k).get<int>();
}
static double j_d(const json &j, const char *k, double defv) {
  if (!j.contains(k))
    return defv;
  return j.at(k).get<double>();
}
static std::string j_s(const json &j, const char *k, const std::string &defv) {
  if (!j.contains(k))
    return defv;
  return j.at(k).get<std::string>();
}
static uint16_t j_u16(const json &j, const char *k, uint16_t defv) {
  if (!j.contains(k))
    return defv;
  return static_cast<uint16_t>(j.at(k).get<uint32_t>());
}

static json parse_json_body_or_throw(const httplib::Request &req) {
  try {
    return json::parse(req.body.empty() ? "{}" : req.body);
  } catch (const std::exception &e) {
    throw std::runtime_error(std::string("invalid_json: ") + e.what());
  }
}

static void respond_json(httplib::Response &res, const json &out,
                         int status = 200) {
  res.status = status;
  res.set_content(out.dump(), "application/json");
}

static StorageResult dump_structure_to_storage(IStorage *storage, HashTable &ht,
                                               int64_t snapshot_id) {
  if (!storage || snapshot_id <= 0)
    return {false, "bad_snapshot"};
  const auto st0 = ht.state();
  std::vector<CubbyStructureView> nodes;
  ht.visit_structure(
      [&](const CubbyStructureView &v) { nodes.push_back(v); });

  std::vector<SqlCubbyRow> cubbies;
  std::vector<SqlSlotRow> slots;
  cubbies.reserve(nodes.size());
  std::unordered_map<int, int> tail_for_fac;

  int cid = 1;
  for (const auto &v : nodes) {
    SqlCubbyRow c;
    c.id = cid;
    c.facility_id = v.facility_id;
    c.tier = v.tier;
    c.capacity = v.capacity;
    c.size = v.size;
    c.is_tail = v.is_tail;
    cubbies.push_back(c);
    if (v.is_tail)
      tail_for_fac[v.facility_id] = cid;

    for (size_t si = 0; si < v.slot_keys.size(); si++) {
      SqlSlotRow sr;
      sr.cubby_id = cid;
      sr.slot_index = static_cast<int>(si);
      const auto &opt = v.slot_keys[si];
      sr.occupied = opt.has_value();
      sr.key_hash = opt.has_value() ? ht.pi_of(*opt) : 0;
      sr.probe_length = 0;
      slots.push_back(sr);
    }
    cid++;
  }

  std::vector<SqlFacilityRow> facilities;
  facilities.reserve(static_cast<size_t>(st0.facilities));
  for (uint64_t fi = 0; fi < st0.facilities; fi++) {
    SqlFacilityRow f;
    f.id = static_cast<int>(fi);
    auto it = tail_for_fac.find(f.id);
    f.tail_cubby_id = it != tail_for_fac.end() ? it->second : 0;
    facilities.push_back(f);
  }

  std::map<std::pair<int, int>, int> tier_cnt;
  for (const auto &c : cubbies)
    tier_cnt[{c.facility_id, c.tier}]++;
  std::vector<SqlTierStatRow> tier_stats;
  tier_stats.reserve(tier_cnt.size());
  for (const auto &e : tier_cnt) {
    SqlTierStatRow t;
    t.facility_id = e.first.first;
    t.tier = e.first.second;
    t.cubby_count = e.second;
    tier_stats.push_back(t);
  }

  return storage->analytics_replace_structure(snapshot_id, facilities, cubbies,
                                              slots, tier_stats);
}

static bool validate_init_params(uint64_t n, int k, double lf,
                                 std::string &err) {
  if (n < 10 || n > 5'000'000) {
    err = "n_out_of_range";
    return false;
  }
  if (k < 0 || k > 8) {
    err = "k_out_of_range";
    return false;
  }
  if (!(lf > 0.0 && lf < 1.0)) {
    err = "load_factor_out_of_range";
    return false;
  }
  return true;
}

struct LocalHist {
  std::vector<uint64_t> samples;
  void add(uint64_t ns) { samples.push_back(ns); }
  uint64_t quantile(double q) const {
    if (samples.empty())
      return 0;
    std::vector<uint64_t> tmp = samples;
    const size_t idx = static_cast<size_t>(std::min<double>(
        tmp.size() - 1, std::floor(q * static_cast<double>(tmp.size() - 1))));
    std::nth_element(tmp.begin(), tmp.begin() + idx, tmp.end());
    return tmp[idx];
  }
  uint64_t p50() const { return quantile(0.50); }
  uint64_t p99() const { return quantile(0.99); }
  uint64_t max() const {
    uint64_t m = 0;
    for (auto v : samples)
      m = std::max(m, v);
    return m;
  }
};

class Logger {
public:
  explicit Logger(std::string file_path) : file_path_(std::move(file_path)) {
    if (!file_path_.empty()) {
      file_.open(file_path_, std::ios::app);
    }
  }

  template <class... Ts> void info(const Ts &...ts) { log("INFO", ts...); }
  template <class... Ts> void warn(const Ts &...ts) { log("WARN", ts...); }
  template <class... Ts> void error(const Ts &...ts) { log("ERROR", ts...); }

private:
  std::string file_path_;
  std::ofstream file_;
  std::mutex mu_;

  static std::string now_str() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
  }

  template <class... Ts> void log(const char *level, const Ts &...ts) {
    std::ostringstream oss;
    (oss << ... << ts);
    const std::string line = now_str() + " [" + level + "] " + oss.str();
    std::lock_guard<std::mutex> lk(mu_);
    std::cout << line << "\n";
    std::cout.flush();
    if (file_.is_open()) {
      file_ << line << "\n";
      file_.flush();
    }
  }
};

int run_server(const ServerConfig &srv_cfg) {
  httplib::Server app;
  app.set_default_headers({{"Cache-Control", "no-store"}});

  const char *log_path_env = std::getenv("LOG_FILE");
  Logger log(log_path_env ? std::string(log_path_env)
                          : std::string("backend.log"));

  log.info("server starting, bind=", srv_cfg.bind_host, " port=", srv_cfg.port);

  HashTable ht;
  std::unique_ptr<IStorage> storage;
  int64_t active_snapshot_id = -1;
  std::mutex service_mu; // 序列化 init 与写入路径（简单可靠）

  struct JobState {
    std::string id;
    std::string kind;
    std::string status; // running|done|error
    int progress = 0;   // 0..100
    std::string message;
    json result = json::object();
    std::string error;
  };
  std::mutex jobs_mu;
  std::unordered_map<std::string, JobState> jobs;
  uint64_t job_seq = 0;

  auto new_job_id = [&]() -> std::string {
    job_seq++;
    return "job_" + std::to_string(job_seq) + "_" +
           std::to_string(
               static_cast<uint64_t>(std::chrono::high_resolution_clock::now()
                                         .time_since_epoch()
                                         .count()));
  };

  app.set_logger(
      [&](const httplib::Request &req, const httplib::Response &res) {
        log.info(req.method, " ", req.path, " -> ", res.status, " (",
                 res.body.size(), " bytes)");
      });

  app.Get("/health", [&](const httplib::Request &, httplib::Response &res) {
    res.set_content("ok", "text/plain");
  });

  // POST /api/init
  // {
  //   n,k,
  //   reset?: true|false
  //
  // 其余参数（load_factor / seeds / mysql 连接信息）全部使用 config.h 默认值。
  // }
  app.Post("/api/init", [&](const httplib::Request &req,
                            httplib::Response &res) {
    const auto t0 = std::chrono::high_resolution_clock::now();
    try {
      json in = parse_json_body_or_throw(req);
      uint64_t n = j_u64(in, "n", 100000);
      int k = j_i(in, "k", 2);
      bool reset = true;
      if (in.contains("reset")) {
        const json &jr = in["reset"];
        if (jr.is_boolean())
          reset = jr.get<bool>();
        else if (jr.is_number())
          reset = jr.get<double>() != 0.0;
        else if (jr.is_string()) {
          const std::string s = jr.get<std::string>();
          reset = !(s == "0" || s == "false");
        }
      }
      std::string verr;
      // 使用默认 load_factor 做校验口径（前端不会传 lf）
      const double lf = TableParams{}.load_factor;
      if (!validate_init_params(n, k, lf, verr)) {
        respond_json(res, json{{"ok", false}, {"error", verr}}, 400);
        return;
      }

      TableParams p;
      p.n = n;
      p.k = k;
      // 其余保持默认（含 load_factor、seeds、mysql_*）

      std::lock_guard<std::mutex> lk(service_mu);

      storage = make_storage_mysql();

      StorageOpenOptions opt;
      opt.mysql_host = p.mysql_host;
      opt.mysql_port = p.mysql_port;
      opt.mysql_user = p.mysql_user;
      opt.mysql_password = p.mysql_password;
      opt.mysql_database = p.mysql_database;
      opt.mysql_table = p.mysql_table;

      auto openr = storage->open(opt);
      if (!openr.ok) {
        storage.reset();
        active_snapshot_id = -1;
        log.error("init mysql open failed: ", openr.error);
        respond_json(res, json{{"ok", false}, {"error", openr.error}}, 500);
        return;
      }

      auto r = ht.init(p);
      if (!r.ok) {
        storage.reset();
        active_snapshot_id = -1;
        log.error("init ht.init failed: ", r.error);
        respond_json(res, json{{"ok", false}, {"error", r.error}}, 500);
        return;
      }

      if (reset) {
        auto clr = storage->clear();
        if (!clr.ok) {
          storage.reset();
          active_snapshot_id = -1;
          log.error("init storage.clear failed: ", clr.error);
          respond_json(res, json{{"ok", false}, {"error", clr.error}}, 500);
          return;
        }
      } else {
        std::vector<uint64_t> keys;
        auto lr =
            storage->for_each_key([&](uint64_t kk) { keys.push_back(kk); });
        if (!lr.ok) {
          storage.reset();
          active_snapshot_id = -1;
          log.error("init for_each_key failed: ", lr.error);
          respond_json(res, json{{"ok", false}, {"error", lr.error}}, 500);
          return;
        }
        (void)ht.bulk_load(keys);
      }

      auto st = ht.state();
      const std::string snap_tag = j_s(in, "snapshot_tag", "init");
      active_snapshot_id =
          storage->analytics_create_snapshot(snap_tag, st.n, st.N, st.K);
      storage->analytics_flush_metrics(true);
      const auto t1 = std::chrono::high_resolution_clock::now();
      log.info("init done ok=", r.ok, " n=", p.n, " k=", p.k, " N=", st.N,
               " K=", st.K, " facilities=", st.facilities,
               " snapshot_id=", active_snapshot_id,
               " storage=mysql elapsed_ms=",
               std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                   .count());

      respond_json(res,
                   json{{"ok", true},
                        {"error", ""},
                        {"snapshot_id", active_snapshot_id},
                        {"snapshot_tag", snap_tag},
                        {"params",
                         {{"n", p.n},
                          {"k", p.k},
                          {"load_factor", p.load_factor},
                          {"seed1", p.seed1},
                          {"seed2", p.seed2},
                          {"seed3", p.seed3},
                          {"reset", reset},
                          {"mysql_host", p.mysql_host},
                          {"mysql_port", p.mysql_port},
                          {"mysql_user", p.mysql_user},
                          {"mysql_database", p.mysql_database},
                          {"mysql_table", p.mysql_table}}},
                        {"state",
                         {{"n", st.n},
                          {"N", st.N},
                          {"K", st.K},
                          {"facilities", st.facilities}}}},
                   200);
    } catch (const std::exception &e) {
      respond_json(res, json{{"ok", false}, {"error", e.what()}}, 400);
    }
  });

  // POST /api/insert { key }
  app.Post("/api/insert", [&](const httplib::Request &req,
                              httplib::Response &res) {
    try {
      json in = parse_json_body_or_throw(req);
      if (!in.contains("key")) {
        respond_json(res, json{{"ok", false}, {"error", "missing_key"}}, 400);
        return;
      }
      uint64_t key = j_u64(in, "key", 0);

      std::lock_guard<std::mutex> lk(service_mu);
      if (!storage) {
        respond_json(res, json{{"ok", false}, {"error", "not_initialized"}},
                     400);
        return;
      }

      const auto t0 = std::chrono::high_resolution_clock::now();
      auto r = ht.insert(key);
      const auto t1 = std::chrono::high_resolution_clock::now();
      global_metrics().on_ht_insert_ns(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()));
      if (!r.ok) {
        respond_json(res, json{{"ok", false}, {"error", r.error}}, 500);
        return;
      }
      if (r.inserted) {
        auto pr = storage->put(key);
        if (!pr.ok) {
          respond_json(res, json{{"ok", false}, {"error", pr.error}}, 500);
          return;
        }
      }
      if (active_snapshot_id > 0) {
        const int64_t lat =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        SqlMetricRow row;
        row.snapshot_id = active_snapshot_id;
        row.operation_type = "insert";
        row.probe_count = static_cast<int>(
            std::min<uint64_t>(r.router_probe_steps,
                               static_cast<uint64_t>(INT_MAX)));
        row.kick_count = static_cast<int>(
            std::min<uint64_t>(r.kick_count, static_cast<uint64_t>(INT_MAX)));
        row.latency_ns = lat;
        row.cubby_tier = r.cubby_tier;
        storage->analytics_enqueue_metric(row);
      }
      respond_json(
          res,
          json{{"ok", true},
               {"inserted", r.inserted},
               {"error", ""},
               {"ht_ns",
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                    .count()}},
          200);
    } catch (const std::exception &e) {
      respond_json(res, json{{"ok", false}, {"error", e.what()}}, 400);
    }
  });

  // POST /api/query { key }
  app.Post("/api/query", [&](const httplib::Request &req,
                             httplib::Response &res) {
    try {
      json in = parse_json_body_or_throw(req);
      if (!in.contains("key")) {
        respond_json(
            res,
            json{{"ok", false}, {"found", false}, {"error", "missing_key"}},
            400);
        return;
      }
      uint64_t key = j_u64(in, "key", 0);
      std::lock_guard<std::mutex> lk(service_mu);
      if (!storage) {
        respond_json(
            res,
            json{{"ok", false}, {"found", false}, {"error", "not_initialized"}},
            400);
        return;
      }
      const auto t0 = std::chrono::high_resolution_clock::now();
      auto r = ht.query(key);
      const auto t1 = std::chrono::high_resolution_clock::now();
      global_metrics().on_ht_query_ns(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()));
      if (active_snapshot_id > 0) {
        const int64_t lat =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        SqlMetricRow row;
        row.snapshot_id = active_snapshot_id;
        row.operation_type = "query";
        row.probe_count = static_cast<int>(
            std::min<uint64_t>(r.router_probe_steps,
                               static_cast<uint64_t>(INT_MAX)));
        row.kick_count = 0;
        row.latency_ns = lat;
        row.cubby_tier = r.cubby_tier;
        storage->analytics_enqueue_metric(row);
      }
      respond_json(
          res,
          json{{"ok", r.ok},
               {"found", r.found},
               {"error", r.error},
               {"ht_ns",
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                    .count()}},
          200);
    } catch (const std::exception &e) {
      respond_json(
          res, json{{"ok", false}, {"found", false}, {"error", e.what()}}, 400);
    }
  });

  // POST /api/delete { key }
  app.Post("/api/delete", [&](const httplib::Request &req,
                              httplib::Response &res) {
    try {
      json in = parse_json_body_or_throw(req);
      if (!in.contains("key")) {
        respond_json(
            res,
            json{{"ok", false}, {"deleted", false}, {"error", "missing_key"}},
            400);
        return;
      }
      uint64_t key = j_u64(in, "key", 0);
      std::lock_guard<std::mutex> lk(service_mu);
      if (!storage) {
        respond_json(res,
                     json{{"ok", false},
                          {"deleted", false},
                          {"error", "not_initialized"}},
                     400);
        return;
      }

      const auto t0 = std::chrono::high_resolution_clock::now();
      auto r = ht.erase(key);
      const auto t1 = std::chrono::high_resolution_clock::now();
      global_metrics().on_ht_delete_ns(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()));
      if (!r.ok) {
        respond_json(
            res, json{{"ok", false}, {"deleted", false}, {"error", r.error}},
            500);
        return;
      }
      if (r.deleted) {
        auto dr = storage->erase(key);
        if (!dr.ok) {
          respond_json(
              res, json{{"ok", false}, {"deleted", false}, {"error", dr.error}},
              500);
          return;
        }
      }
      if (active_snapshot_id > 0) {
        const int64_t lat =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        SqlMetricRow row;
        row.snapshot_id = active_snapshot_id;
        row.operation_type = "delete";
        row.probe_count = static_cast<int>(
            std::min<uint64_t>(r.router_probe_steps,
                               static_cast<uint64_t>(INT_MAX)));
        row.kick_count = static_cast<int>(
            std::min<uint64_t>(r.kick_count, static_cast<uint64_t>(INT_MAX)));
        row.latency_ns = lat;
        row.cubby_tier = r.cubby_tier;
        storage->analytics_enqueue_metric(row);
      }
      respond_json(
          res,
          json{{"ok", true},
               {"deleted", r.deleted},
               {"error", ""},
               {"ht_ns",
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                    .count()}},
          200);
    } catch (const std::exception &e) {
      respond_json(res,
                   json{{"ok", false}, {"deleted", false}, {"error", e.what()}},
                   400);
    }
  });

  // POST /api/stats
  // 只读：输出指标与当前表状态（用于 theory-like 验收）。
  app.Post("/api/stats", [&](const httplib::Request &, httplib::Response &res) {
    try {
      std::lock_guard<std::mutex> lk(service_mu);
      const auto ms = global_metrics().snapshot();
      const auto st = ht.state();
      const double n_d = static_cast<double>(std::max<uint64_t>(1, st.n));
      const double lower_per_key = 64.0 - std::log2(n_d);
      const double lower_bits = lower_per_key * n_d;
      const double payload_bits =
          64.0 * n_d; // 目前未做 quotienting，按完整 key 估算
      const double meta_bits =
          static_cast<double>(ms.meta_bits_max); // 保守：先用 max 近似展示
      const double total_bits = payload_bits + meta_bits;
      const double wasted_bpk = (total_bits - lower_bits) / n_d;
      if (storage)
        storage->analytics_flush_metrics(true);
      json out = {
          {"ok", true},
          {"error", ""},
          {"snapshot_id", active_snapshot_id},
          {"metrics",
           {{"ops",
             {{"init", ms.ops_init},
              {"insert", ms.ops_insert},
              {"query", ms.ops_query},
              {"delete", ms.ops_delete}}},
            {"moved",
             {{"insert_total", ms.insert_moved_total},
              {"insert_max", ms.insert_moved_max},
              {"delete_total", ms.delete_moved_total},
              {"delete_max", ms.delete_moved_max}}},
            {"router_steps",
             {{"total", ms.router_steps_total}, {"max", ms.router_steps_max}}},
            {"meta_bits",
             {{"total", ms.meta_bits_total}, {"max", ms.meta_bits_max}}},
            {"ht_ns",
             {{"init",
               {{"count", ms.ht_init.count},
                {"total_ns", ms.ht_init.total_ns},
                {"max_ns", ms.ht_init.max_ns},
                {"p50_ns", ms.ht_init.p50_ns},
                {"p99_ns", ms.ht_init.p99_ns}}},
              {"insert",
               {{"count", ms.ht_insert.count},
                {"total_ns", ms.ht_insert.total_ns},
                {"max_ns", ms.ht_insert.max_ns},
                {"p50_ns", ms.ht_insert.p50_ns},
                {"p99_ns", ms.ht_insert.p99_ns}}},
              {"query",
               {{"count", ms.ht_query.count},
                {"total_ns", ms.ht_query.total_ns},
                {"max_ns", ms.ht_query.max_ns},
                {"p50_ns", ms.ht_query.p50_ns},
                {"p99_ns", ms.ht_query.p99_ns}}},
              {"delete",
               {{"count", ms.ht_delete.count},
                {"total_ns", ms.ht_delete.total_ns},
                {"max_ns", ms.ht_delete.max_ns},
                {"p50_ns", ms.ht_delete.p50_ns},
                {"p99_ns", ms.ht_delete.p99_ns}}}}},
            {"events",
             {{"rebuild_down", ms.events.rebuild_down},
              {"rebuild_up", ms.events.rebuild_up},
              {"resize_start", ms.events.resize_start},
              {"resize_finish", ms.events.resize_finish}}}}},
          {"state",
           {{"n", st.n},
            {"N", st.N},
            {"K", st.K},
            {"facilities", st.facilities}}},
          {"space_bits",
           {{"payload_bits", payload_bits},
            {"meta_bits_est", meta_bits},
            {"total_bits_est", total_bits},
            {"lower_bound_bits_est", lower_bits},
            {"wasted_bits_per_key_est", wasted_bpk}}}};
      if (storage && active_snapshot_id > 0) {
        try {
          out["analytics_db"] = json::parse(
              storage->analytics_summary_json(active_snapshot_id));
        } catch (...) {
          out["analytics_db"] = json::object();
        }
      }
      respond_json(res, out, 200);
    } catch (const std::exception &e) {
      respond_json(res, json{{"ok", false}, {"error", e.what()}}, 500);
    }
  });

  app.Post("/api/analytics/snapshots", [&](const httplib::Request &,
                                            httplib::Response &res) {
    try {
      std::lock_guard<std::mutex> lk(service_mu);
      if (!storage) {
        respond_json(res, json{{"ok", false}, {"error", "not_initialized"}}, 400);
        return;
      }
      const std::string raw = storage->analytics_list_snapshots_json();
      json items = json::parse(raw);
      respond_json(res, json{{"ok", true}, {"items", items}}, 200);
    } catch (const std::exception &e) {
      respond_json(res, json{{"ok", false}, {"error", e.what()}}, 500);
    }
  });

  app.Post("/api/analytics/summary", [&](const httplib::Request &req,
                                         httplib::Response &res) {
    try {
      json in = parse_json_body_or_throw(req);
      int64_t sid = static_cast<int64_t>(j_u64(in, "snapshot_id", 0));
      std::lock_guard<std::mutex> lk(service_mu);
      if (!storage) {
        respond_json(res, json{{"ok", false}, {"error", "not_initialized"}}, 400);
        return;
      }
      if (sid <= 0)
        sid = active_snapshot_id;
      if (sid <= 0) {
        respond_json(res,
                     json{{"ok", false}, {"error", "no_snapshot_selected"}}, 400);
        return;
      }
      json summary = json::parse(storage->analytics_summary_json(sid));
      respond_json(res, json{{"ok", true}, {"summary", summary}}, 200);
    } catch (const std::exception &e) {
      respond_json(res, json{{"ok", false}, {"error", e.what()}}, 500);
    }
  });

  app.Post("/api/analytics/structure_dump", [&](const httplib::Request &req,
                                                httplib::Response &res) {
    try {
      json in = parse_json_body_or_throw(req);
      int64_t sid = static_cast<int64_t>(j_u64(in, "snapshot_id", 0));
      std::lock_guard<std::mutex> lk(service_mu);
      if (!storage) {
        respond_json(res, json{{"ok", false}, {"error", "not_initialized"}}, 400);
        return;
      }
      if (sid <= 0)
        sid = active_snapshot_id;
      if (sid <= 0) {
        respond_json(res,
                     json{{"ok", false}, {"error", "no_snapshot_selected"}}, 400);
        return;
      }
      const auto dr = dump_structure_to_storage(storage.get(), ht, sid);
      respond_json(res,
                   json{{"ok", dr.ok},
                        {"error", dr.ok ? "" : dr.error},
                        {"snapshot_id", sid}},
                   dr.ok ? 200 : 500);
    } catch (const std::exception &e) {
      respond_json(res, json{{"ok", false}, {"error", e.what()}}, 400);
    }
  });

  // POST /api/jobs/get { id }
  // 轮询实验进度/结果。
  app.Post("/api/jobs/get", [&](const httplib::Request &req,
                                httplib::Response &res) {
    try {
      json in = parse_json_body_or_throw(req);
      const std::string id = j_s(in, "id", "");
      if (id.empty()) {
        respond_json(res, json{{"ok", false}, {"error", "missing_id"}}, 400);
        return;
      }
      std::lock_guard<std::mutex> lk(jobs_mu);
      auto it = jobs.find(id);
      if (it == jobs.end()) {
        respond_json(res, json{{"ok", false}, {"error", "job_not_found"}}, 404);
        return;
      }
      const auto &j = it->second;
      respond_json(res,
                   json{{"ok", true},
                        {"error", ""},
                        {"id", j.id},
                        {"kind", j.kind},
                        {"status", j.status},
                        {"progress", j.progress},
                        {"message", j.message},
                        {"result", j.result},
                        {"job_error", j.error}},
                   200);
    } catch (const std::exception &e) {
      respond_json(res, json{{"ok", false}, {"error", e.what()}}, 400);
    }
  });

  app.Post("/api/experiment/o1_vs_n", [&](const httplib::Request &req,
                                          httplib::Response &res) {
    try {
      json in = parse_json_body_or_throw(req);
      const int k = j_i(in, "k", 2);
      const double inserts_factor = j_d(in, "inserts_factor", 0.5);
      const int query_samples = j_i(in, "query_samples", 2000);
      std::vector<uint64_t> ns;
      if (in.contains("ns") && in["ns"].is_array()) {
        for (auto &x : in["ns"])
          ns.push_back(x.get<uint64_t>());
      } else {
        ns = {10000, 20000, 30000, 40000};
      }

      // async job with progress
      std::string job_id;
      {
        std::lock_guard<std::mutex> lk(jobs_mu);
        job_id = new_job_id();
        jobs[job_id] = JobState{.id = job_id,
                                .kind = "o1_vs_n",
                                .status = "running",
                                .progress = 0,
                                .message = "queued",
                                .result = json::object(),
                                .error = ""};
      }

      std::thread([=, &ht, &storage, &service_mu, &jobs_mu, &jobs]() {
        try {
          {
            std::lock_guard<std::mutex> jl(jobs_mu);
            jobs[job_id].message = "starting";
            jobs[job_id].progress = 1;
          }

          std::lock_guard<std::mutex> lk(service_mu);
          if (!storage) {
            std::lock_guard<std::mutex> jl(jobs_mu);
            jobs[job_id].status = "error";
            jobs[job_id].error = "not_initialized";
            jobs[job_id].progress = 100;
            return;
          }

          std::mt19937_64 rng(42);
          json points = json::array();
          const size_t total = ns.size();

          for (size_t idx = 0; idx < total; idx++) {
            const uint64_t n = ns[idx];
            {
              std::lock_guard<std::mutex> jl(jobs_mu);
              jobs[job_id].message = "n=" + std::to_string(n);
              jobs[job_id].progress = static_cast<int>(
                  std::floor((idx * 100.0) / std::max<size_t>(1, total)));
            }

            TableParams p;
            p.n = n;
            p.k = k;
            (void)ht.init(p);

            const uint64_t inserts =
                static_cast<uint64_t>(std::max(1.0, n * inserts_factor));
            std::vector<uint64_t> keys;
            keys.reserve(static_cast<size_t>(inserts));
            for (uint64_t i = 0; i < inserts; i++) {
              uint64_t key = rng();
              keys.push_back(key);
              (void)ht.insert(key);
            }

            LocalHist hist;
            for (int i = 0; i < query_samples; i++) {
              const uint64_t key =
                  keys.empty() ? rng()
                               : keys[static_cast<size_t>(rng() % keys.size())];
              const auto t0 = std::chrono::high_resolution_clock::now();
              (void)ht.query(key);
              const auto t1 = std::chrono::high_resolution_clock::now();
              hist.add(static_cast<uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                      .count()));
            }

            points.push_back(json{{"n", n},
                                  {"query_p50_ns", hist.p50()},
                                  {"query_p99_ns", hist.p99()},
                                  {"query_max_ns", hist.max()}});
          }

          {
            std::lock_guard<std::mutex> jl(jobs_mu);
            jobs[job_id].status = "done";
            jobs[job_id].progress = 100;
            jobs[job_id].message = "done";
            jobs[job_id].result = json{{"ok", true},
                                       {"kind", "o1_vs_n"},
                                       {"k", k},
                                       {"points", points}};
          }
        } catch (const std::exception &e) {
          std::lock_guard<std::mutex> jl(jobs_mu);
          jobs[job_id].status = "error";
          jobs[job_id].progress = 100;
          jobs[job_id].error = e.what();
        }
      }).detach();

      respond_json(res, json{{"ok", true}, {"error", ""}, {"job_id", job_id}},
                   200);
    } catch (const std::exception &e) {
      respond_json(res, json{{"ok", false}, {"error", e.what()}}, 400);
    }
  });

  // POST /api/experiment/ok_vs_k
  // { n?: number, ks?: number[], inserts_factor?: number }
  //
  // 说明：固定 n，遍历 k，测 insert/delete HT-only 延迟与空间估计。
  app.Post("/api/experiment/ok_vs_k", [&](const httplib::Request &req,
                                          httplib::Response &res) {
    try {
      json in = parse_json_body_or_throw(req);
      const uint64_t n = j_u64(in, "n", 100000);
      const double inserts_factor = j_d(in, "inserts_factor", 0.5);
      std::vector<int> ks;
      if (in.contains("ks") && in["ks"].is_array()) {
        for (auto &x : in["ks"])
          ks.push_back(x.get<int>());
      } else {
        ks = {1, 2, 3, 4};
      }

      std::string job_id;
      {
        std::lock_guard<std::mutex> lk(jobs_mu);
        job_id = new_job_id();
        jobs[job_id] = JobState{.id = job_id,
                                .kind = "ok_vs_k",
                                .status = "running",
                                .progress = 0,
                                .message = "queued",
                                .result = json::object(),
                                .error = ""};
      }

      std::thread([=, &ht, &storage, &service_mu, &jobs_mu, &jobs]() {
        try {
          {
            std::lock_guard<std::mutex> jl(jobs_mu);
            jobs[job_id].message = "starting";
            jobs[job_id].progress = 1;
          }

          std::lock_guard<std::mutex> lk(service_mu);
          if (!storage) {
            std::lock_guard<std::mutex> jl(jobs_mu);
            jobs[job_id].status = "error";
            jobs[job_id].error = "not_initialized";
            jobs[job_id].progress = 100;
            return;
          }

          std::mt19937_64 rng(43);
          json series = json::array();
          const size_t total = ks.size();

          for (size_t idx = 0; idx < total; idx++) {
            const int k = ks[idx];
            {
              std::lock_guard<std::mutex> jl(jobs_mu);
              jobs[job_id].message = "k=" + std::to_string(k);
              jobs[job_id].progress = static_cast<int>(
                  std::floor((idx * 100.0) / std::max<size_t>(1, total)));
            }

            TableParams p;
            p.n = n;
            p.k = k;
            (void)ht.init(p);

            const uint64_t inserts =
                static_cast<uint64_t>(std::max(1.0, n * inserts_factor));
            std::vector<uint64_t> keys;
            keys.reserve(static_cast<size_t>(inserts));
            LocalHist insHist;
            for (uint64_t i = 0; i < inserts; i++) {
              uint64_t key = rng();
              keys.push_back(key);
              const auto t0 = std::chrono::high_resolution_clock::now();
              (void)ht.insert(key);
              const auto t1 = std::chrono::high_resolution_clock::now();
              insHist.add(static_cast<uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                      .count()));
            }

            LocalHist delHist;
            const uint64_t dels = inserts / 2;
            for (uint64_t i = 0; i < dels; i++) {
              const auto t0 = std::chrono::high_resolution_clock::now();
              (void)ht.erase(keys[static_cast<size_t>(i)]);
              const auto t1 = std::chrono::high_resolution_clock::now();
              delHist.add(static_cast<uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                      .count()));
            }

            const auto ms = global_metrics().snapshot();
            const auto st = ht.state();
            const double n_d = static_cast<double>(std::max<uint64_t>(1, st.n));
            const double lower_per_key = 64.0 - std::log2(n_d);
            const double lower_bits = lower_per_key * n_d;
            const double payload_bits = 64.0 * n_d;
            const double meta_bits = static_cast<double>(ms.meta_bits_max);
            const double total_bits = payload_bits + meta_bits;
            const double wasted_bpk = (total_bits - lower_bits) / n_d;

            series.push_back(json{{"k", k},
                                  {"insert_p50_ns", insHist.p50()},
                                  {"insert_p99_ns", insHist.p99()},
                                  {"delete_p50_ns", delHist.p50()},
                                  {"delete_p99_ns", delHist.p99()},
                                  {"wasted_bits_per_key_est", wasted_bpk}});
          }

          {
            std::lock_guard<std::mutex> jl(jobs_mu);
            jobs[job_id].status = "done";
            jobs[job_id].progress = 100;
            jobs[job_id].message = "done";
            jobs[job_id].result = json{{"ok", true},
                                       {"kind", "ok_vs_k"},
                                       {"n", n},
                                       {"series", series}};
          }
        } catch (const std::exception &e) {
          std::lock_guard<std::mutex> jl(jobs_mu);
          jobs[job_id].status = "error";
          jobs[job_id].progress = 100;
          jobs[job_id].error = e.what();
        }
      }).detach();

      respond_json(res, json{{"ok", true}, {"error", ""}, {"job_id", job_id}},
                   200);
    } catch (const std::exception &e) {
      respond_json(res, json{{"ok", false}, {"error", e.what()}}, 400);
    }
  });

  if (!app.listen(srv_cfg.bind_host, srv_cfg.port)) {
    throw std::runtime_error("failed to listen");
  }
  return 0;
}

} // namespace otsh
