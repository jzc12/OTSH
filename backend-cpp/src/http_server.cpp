#include "http_server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>

namespace otsh {

using json = nlohmann::json;

static uint64_t j_u64(const json& j, const char* k, uint64_t defv) {
  if (!j.contains(k)) return defv;
  return j.at(k).get<uint64_t>();
}
static int j_i(const json& j, const char* k, int defv) {
  if (!j.contains(k)) return defv;
  return j.at(k).get<int>();
}
static double j_d(const json& j, const char* k, double defv) {
  if (!j.contains(k)) return defv;
  return j.at(k).get<double>();
}

static json parse_json_body_or_throw(const httplib::Request& req) {
  try {
    return json::parse(req.body.empty() ? "{}" : req.body);
  } catch (const std::exception& e) {
    throw std::runtime_error(std::string("invalid_json: ") + e.what());
  }
}

static void respond_json(httplib::Response& res, const json& out, int status = 200) {
  res.status = status;
  res.set_content(out.dump(), "application/json");
}

static bool validate_init_params(uint64_t n, int k, double lf, std::string& err) {
  if (n < 10 || n > 5'000'000) { err = "n_out_of_range"; return false; }
  if (k < 0 || k > 8) { err = "k_out_of_range"; return false; }
  if (!(lf > 0.0 && lf < 1.0)) { err = "load_factor_out_of_range"; return false; }
  return true;
}

static uint64_t q_u64(const httplib::Request& req, const char* k, uint64_t defv) {
  if (!req.has_param(k)) return defv;
  try {
    return static_cast<uint64_t>(std::stoull(req.get_param_value(k)));
  } catch (...) {
    return defv;
  }
}

class Logger {
public:
  explicit Logger(std::string file_path) : file_path_(std::move(file_path)) {
    if (!file_path_.empty()) {
      file_.open(file_path_, std::ios::app);
    }
  }

  template <class... Ts>
  void info(const Ts&... ts) {
    log("INFO", ts...);
  }
  template <class... Ts>
  void warn(const Ts&... ts) {
    log("WARN", ts...);
  }
  template <class... Ts>
  void error(const Ts&... ts) {
    log("ERROR", ts...);
  }

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

  template <class... Ts>
  void log(const char* level, const Ts&... ts) {
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

int run_server(const DbConfig& db_cfg, const ServerConfig& srv_cfg) {
  Db db(db_cfg);
  db.connect();
  HashTableDb ht(db);

  httplib::Server app;
  const char* log_path_env = std::getenv("LOG_FILE");
  Logger log(log_path_env ? std::string(log_path_env) : std::string("backend.log"));

  log.info("server starting, bind=", srv_cfg.bind_host, " port=", srv_cfg.port,
           " db=", db_cfg.user, "@", db_cfg.host, ":", db_cfg.port, "/", db_cfg.database);

  // avoid browser/proxy caching for GET endpoints like snapshot/stats
  app.set_default_headers({{"Cache-Control", "no-store"}});

  app.set_logger([&](const httplib::Request& req, const httplib::Response& res) {
    log.info(req.method, " ", req.path, " -> ", res.status, " (", res.body.size(), " bytes)");
  });

  app.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
    res.set_content("ok", "text/plain");
  });

  // POST /api/init { n, k, load_factor, seed1?, seed2?, seed3? }
  app.Post("/api/init", [&](const httplib::Request& req, httplib::Response& res) {
    const auto t0 = std::chrono::high_resolution_clock::now();
    try {
      json in = parse_json_body_or_throw(req);
      uint64_t n = j_u64(in, "n", 100000);
      int k = j_i(in, "k", 2);
      double lf = j_d(in, "load_factor", 0.98);

      std::string verr;
      if (!validate_init_params(n, k, lf, verr)) {
        respond_json(res, json{{"ok", false}, {"error", verr}}, 400);
        return;
      }

      std::optional<uint64_t> s1, s2, s3;
      if (in.contains("seed1")) s1 = in["seed1"].get<uint64_t>();
      if (in.contains("seed2")) s2 = in["seed2"].get<uint64_t>();
      if (in.contains("seed3")) s3 = in["seed3"].get<uint64_t>();

      // reset meta row to force new config
      db.exec("DROP TABLE IF EXISTS ht_meta");
      auto p = ht.load_or_init_meta(n, k, lf, s1, s2, s3);
      ht.reset_kick_hist(p.k);
      // Simplified behavior: init clears all keys/fps (no rebuild / no persistence across init).
      auto r = ht.init_table(p);
      const auto t1 = std::chrono::high_resolution_clock::now();
      log.info("init done ok=", r.ok, " n=", p.n, " k=", p.k, " bins=", p.total_bins, " bin_size=", p.bin_size,
               " slots=", p.capacity_slots, " elapsed_ms=",
               std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
      if (!r.ok) {
        log.error("init failed: ", r.error);
      }

      json out;
      out["ok"] = r.ok;
      out["error"] = r.error;
      out["params"] = {
          {"n", p.n},
          {"k", p.k},
          {"load_factor", p.load_factor},
          {"seed1", p.seed1},
          {"seed2", p.seed2},
          {"seed3", p.seed3},
          {"mini_bin_size", p.mini_bin_size},
          {"num_mini_bins", p.num_mini_bins},
          {"fallback_size", p.fallback_size},
          {"bin_size", p.bin_size},
          {"total_bins", p.total_bins},
          {"capacity_slots", p.capacity_slots},
      };

      respond_json(res, out, r.ok ? 200 : 500);
    } catch (const std::exception& e) {
      respond_json(res, json{{"ok", false}, {"error", e.what()}}, 400);
    }
  });

  // POST /api/insert { key }
  app.Post("/api/insert", [&](const httplib::Request& req, httplib::Response& res) {
    const auto t0 = std::chrono::high_resolution_clock::now();
    try {
      json in = parse_json_body_or_throw(req);
      if (!in.contains("key")) {
        respond_json(res, json{{"ok", false}, {"error", "missing_key"}}, 400);
        return;
      }
      uint64_t key = j_u64(in, "key", 0);
      bool trace = in.contains("trace") ? in["trace"].get<bool>() : false;
      auto p = ht.load_or_init_meta(100000, 2, 0.98, std::nullopt, std::nullopt, std::nullopt);
      auto r = ht.insert_key(p, key, trace);
      const auto t1 = std::chrono::high_resolution_clock::now();
      log.info("insert key=", key, " ok=", r.ok, " probes=", r.probes, " trace=", trace ? r.trace.size() : 0,
               " elapsed_us=", std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
      if (!r.ok) {
        log.error("insert failed: ", r.error);
      }

      json out;
      out["ok"] = r.ok;
      out["probes"] = r.probes;
      out["error"] = r.error;
      out["bin"] = r.bin;
      out["mini"] = r.mini;
      out["fp"] = r.fp;
      if (trace) {
        out["trace"] = json::array();
        for (const auto& s : r.trace) {
          out["trace"].push_back({{"idx", s.idx}, {"from_fp", s.from_fp}, {"to_fp", s.to_fp}, {"depth", s.depth}, {"action", s.action}});
        }
      }
      respond_json(res, out, r.ok ? 200 : 500);
    } catch (const std::exception& e) {
      respond_json(res, json{{"ok", false}, {"error", e.what()}}, 400);
    }
  });

  // POST /api/find { key }
  app.Post("/api/find", [&](const httplib::Request& req, httplib::Response& res) {
    try {
      json in = parse_json_body_or_throw(req);
      if (!in.contains("key")) {
        respond_json(res, json{{"found", false}, {"probes", 0}, {"error", "missing_key"}}, 400);
        return;
      }
      uint64_t key = j_u64(in, "key", 0);
      auto p = ht.load_or_init_meta(100000, 2, 0.98, std::nullopt, std::nullopt, std::nullopt);
      auto r = ht.find_key(p, key);
      respond_json(res, json{{"found", r.ok}, {"probes", r.probes}, {"error", r.error}}, 200);
    } catch (const std::exception& e) {
      respond_json(res, json{{"found", false}, {"probes", 0}, {"error", e.what()}}, 400);
    }
  });

  // POST /api/erase { key }
  app.Post("/api/erase", [&](const httplib::Request& req, httplib::Response& res) {
    try {
      json in = parse_json_body_or_throw(req);
      if (!in.contains("key")) {
        respond_json(res, json{{"ok", false}, {"probes", 0}, {"error", "missing_key"}}, 400);
        return;
      }
      uint64_t key = j_u64(in, "key", 0);
      auto p = ht.load_or_init_meta(100000, 2, 0.98, std::nullopt, std::nullopt, std::nullopt);
      auto r = ht.erase_key(p, key);
      respond_json(res, json{{"ok", r.ok}, {"probes", r.probes}, {"error", r.error}}, r.ok ? 200 : 404);
    } catch (const std::exception& e) {
      respond_json(res, json{{"ok", false}, {"probes", 0}, {"error", e.what()}}, 400);
    }
  });

  // POST /api/batch_insert { count, distribution: "uniform"|"skewed", key_space?, skew? , with_trace? }
  app.Post("/api/batch_insert", [&](const httplib::Request& req, httplib::Response& res) {
    json in = json::parse(req.body.empty() ? "{}" : req.body);
    uint64_t count = j_u64(in, "count", 1000);
    count = std::min<uint64_t>(count, 200000);
    std::string dist = in.contains("distribution") ? in["distribution"].get<std::string>() : "uniform";
    uint64_t key_space = j_u64(in, "key_space", 0);
    if (key_space == 0) key_space = count * 10;
    double skew = j_d(in, "skew", 1.2);

    auto p = ht.load_or_init_meta(100000, 2, 0.98, std::nullopt, std::nullopt, std::nullopt);
    log.info("batch_insert start count=", count, " dist=", dist, " key_space=", key_space, " skew=", skew);

    // generate keys
    uint64_t seed = j_u64(in, "seed", 0);
    if (seed == 0) seed = p.seed2 ^ 0xBADC0FFEEULL;
    uint64_t ok = 0;
    uint64_t probes = 0;
    uint64_t max_depth = 0;

    auto t0 = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < count; i++) {
      uint64_t key;
      if (dist == "skewed") {
        // simple heavy-tail: choose rank ~ geometric-like distribution
        // (not perfect Zipf, but good enough to create skew for demo)
        uint64_t r = splitmix64(seed + i) % key_space;
        double u = (double)(splitmix64(seed ^ (i + 17)) & 0xFFFFFFFFu) / (double)0x100000000ULL;
        double x = std::pow(std::max(1e-12, u), -skew);
        key = (static_cast<uint64_t>(x) + r) % key_space;
      } else {
        key = splitmix64(seed + i) % key_space;
      }

      auto r = ht.insert_key(p, key, false);
      probes += r.probes;
      ok += r.ok ? 1 : 0;
      for (auto& s : r.trace) (void)s;

      if ((i + 1) % 5000 == 0) {
        log.info("batch_insert progress ", (i + 1), "/", count, " ok=", ok);
      }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // post stats
    auto st = ht.stats(p);
    (void)ht.bin_stats(p, 0, std::min<uint64_t>(p.total_bins, 2000));

    json out;
    out["ok_count"] = ok;
    out["total"] = count;
    out["avg_probes"] = count ? (double)probes / (double)count : 0.0;
    out["elapsed_ms"] = ms;
    out["used_slots"] = st.used_slots;
    out["fallback_used"] = st.fallback_used;
    (void)max_depth;
    log.info("batch_insert done ok=", ok, "/", count, " avg_probes=", (count ? (double)probes / (double)count : 0.0),
             " elapsed_ms=", ms, " used=", st.used_slots, " fallback_used=", st.fallback_used);
    res.set_content(out.dump(), "application/json");
  });

  // POST /api/query_test { count, hit_rate }
  app.Post("/api/query_test", [&](const httplib::Request& req, httplib::Response& res) {
    json in = json::parse(req.body.empty() ? "{}" : req.body);
    uint64_t count = j_u64(in, "count", 5000);
    count = std::min<uint64_t>(count, 200000);
    double hit_rate = j_d(in, "hit_rate", 0.5);
    hit_rate = std::min(1.0, std::max(0.0, hit_rate));

    auto p = ht.load_or_init_meta(100000, 2, 0.98, std::nullopt, std::nullopt, std::nullopt);
    uint64_t seed = j_u64(in, "seed", p.seed1 ^ 0xC001D00DULL);

    std::vector<uint64_t> probes_list;
    probes_list.reserve(count);
    uint64_t found = 0;
    for (uint64_t i = 0; i < count; i++) {
      bool want_hit = ((double)(splitmix64(seed + i) & 0xFFFFFFFFu) / (double)0x100000000ULL) < hit_rate;
      uint64_t key = want_hit ? (splitmix64(seed ^ (i + 11)) % (p.n + 1)) : (splitmix64(seed ^ (i + 11)) + (1ULL << 60));
      auto r = ht.find_key(p, key);
      probes_list.push_back(r.probes);
      if (r.ok) found++;
    }
    std::sort(probes_list.begin(), probes_list.end());
    auto p99 = probes_list.empty() ? 0 : probes_list[(size_t)std::floor(0.99 * (probes_list.size() - 1))];
    uint64_t maxp = probes_list.empty() ? 0 : probes_list.back();
    double avg = 0.0;
    for (auto v : probes_list) avg += (double)v;
    if (!probes_list.empty()) avg /= (double)probes_list.size();

    json out;
    out["count"] = count;
    out["found"] = found;
    out["avg_probes"] = avg;
    out["p99_probes"] = p99;
    out["max_probes"] = maxp;
    res.set_content(out.dump(), "application/json");
  });

  // GET /api/experiment/kick_depth_hist
  // In-memory histogram (cleared on /api/init). Still useful for frontend charting.
  app.Get("/api/experiment/kick_depth_hist", [&](const httplib::Request&, httplib::Response& res) {
    auto hist = ht.kick_hist_snapshot();
    json out;
    out["hist"] = json::array();
    for (size_t d = 0; d < hist.size(); d++) {
      out["hist"].push_back({{"depth", static_cast<int>(d)}, {"count", hist[d]}});
    }
    res.set_content(out.dump(), "application/json");
  });

  // POST /api/experiment/probe_vs_n { ns:[], ks:[], inserts, queries }
  // Warning: will re-init table multiple times (slow, destructive).
  app.Post("/api/experiment/probe_vs_n", [&](const httplib::Request& req, httplib::Response& res) {
    log.warn("probe_vs_n starts (will re-init table multiple times)");
    json in = json::parse(req.body.empty() ? "{}" : req.body);
    auto ns = in.contains("ns") ? in["ns"] : json::array({10000, 30000, 100000});
    auto ks = in.contains("ks") ? in["ks"] : json::array({1, 2, 3});
    uint64_t inserts = j_u64(in, "inserts", 20000);
    uint64_t queries = j_u64(in, "queries", 5000);

    json out;
    out["series"] = json::array();

    for (auto& kJ : ks) {
      int k = kJ.get<int>();
      json series;
      series["k"] = k;
      series["points"] = json::array();

      for (auto& nJ : ns) {
        uint64_t n = nJ.get<uint64_t>();
        db.exec("DROP TABLE IF EXISTS ht_meta");
        auto p = ht.load_or_init_meta(n, k, 0.98, std::nullopt, std::nullopt, std::nullopt);
        auto initr = ht.init_table(p);
        if (!initr.ok) {
          series["points"].push_back({{"n", n}, {"error", initr.error}});
          continue;
        }

        // insert a subset (avoid filling full n for speed)
        uint64_t to_insert = std::min<uint64_t>(inserts, n);
        uint64_t seed = p.seed2 ^ 0x1234;
        for (uint64_t i = 0; i < to_insert; i++) {
          uint64_t key = splitmix64(seed + i);
          (void)ht.insert_key(p, key, false);
        }

        // query test (all misses/hits mixed)
        json qt_in;
        qt_in["count"] = queries;
        qt_in["hit_rate"] = 0.5;
        qt_in["seed"] = p.seed1 ^ 0x8888;
        // reuse local logic instead of HTTP call
        std::vector<uint64_t> probes_list;
        probes_list.reserve(queries);
        uint64_t found = 0;
        for (uint64_t i = 0; i < queries; i++) {
          bool want_hit = ((double)(splitmix64(seed + i) & 0xFFFFFFFFu) / (double)0x100000000ULL) < 0.5;
          uint64_t key = want_hit ? (splitmix64(seed ^ (i + 11)) % (p.n + 1)) : (splitmix64(seed ^ (i + 11)) + (1ULL << 60));
          auto r = ht.find_key(p, key);
          probes_list.push_back(r.probes);
          if (r.ok) found++;
        }
        std::sort(probes_list.begin(), probes_list.end());
        auto p99 = probes_list.empty() ? 0 : probes_list[(size_t)std::floor(0.99 * (probes_list.size() - 1))];
        uint64_t maxp = probes_list.empty() ? 0 : probes_list.back();
        double avg = 0.0;
        for (auto v : probes_list) avg += (double)v;
        if (!probes_list.empty()) avg /= (double)probes_list.size();

        series["points"].push_back({{"n", n}, {"avg_probes", avg}, {"p99_probes", p99}, {"max_probes", maxp}});
      }
      out["series"].push_back(series);
    }

    res.set_content(out.dump(), "application/json");
    log.warn("probe_vs_n done");
  });

  // POST /api/experiment/fallback_vs_load { n,k, load_targets:[], step, distribution }
  // Warning: will re-init table (destructive).
  app.Post("/api/experiment/fallback_vs_load", [&](const httplib::Request& req, httplib::Response& res) {
    log.warn("fallback_vs_load starts (will re-init table)");
    json in = json::parse(req.body.empty() ? "{}" : req.body);
    uint64_t n = j_u64(in, "n", 100000);
    int k = j_i(in, "k", 2);
    auto targets = in.contains("load_targets") ? in["load_targets"] : json::array({0.7, 0.8, 0.9, 0.95, 0.98});
    uint64_t step = j_u64(in, "step", 2000);
    std::string dist = in.contains("distribution") ? in["distribution"].get<std::string>() : "uniform";
    double skew = j_d(in, "skew", 1.2);

    db.exec("DROP TABLE IF EXISTS ht_meta");
    auto p = ht.load_or_init_meta(n, k, 0.98, std::nullopt, std::nullopt, std::nullopt);
    auto initr = ht.init_table(p);
    json out;
    out["ok"] = initr.ok;
    if (!initr.ok) {
      out["error"] = initr.error;
      res.set_content(out.dump(), "application/json");
      return;
    }

    uint64_t seed = p.seed2 ^ 0xCAFEBABEULL;
    uint64_t key_space = n * 10;
    uint64_t inserted = 0;

    out["points"] = json::array();
    for (auto& tJ : targets) {
      double target = tJ.get<double>();
      // insert until reaching target load
      while (true) {
        auto st = ht.stats(p);
        double load = p.capacity_slots ? (double)st.used_slots / (double)p.capacity_slots : 0.0;
        if (load >= target) break;

        auto t0 = std::chrono::high_resolution_clock::now();
        for (uint64_t i = 0; i < step; i++) {
          uint64_t key;
          if (dist == "skewed") {
            uint64_t r = splitmix64(seed + inserted + i) % key_space;
            double u = (double)(splitmix64(seed ^ (inserted + i + 17)) & 0xFFFFFFFFu) / (double)0x100000000ULL;
            double x = std::pow(std::max(1e-12, u), -skew);
            key = (static_cast<uint64_t>(x) + r) % key_space;
          } else {
            key = splitmix64(seed + inserted + i) % key_space;
          }
          (void)ht.insert_key(p, key, false);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        (void)t0; (void)t1;
        inserted += step;
      }

      auto st = ht.stats(p);
      double load = p.capacity_slots ? (double)st.used_slots / (double)p.capacity_slots : 0.0;
      double fb_ratio = st.used_slots ? (double)st.fallback_used / (double)st.used_slots : 0.0;
      out["points"].push_back({{"target", target}, {"load", load}, {"fallback_used", st.fallback_used}, {"fallback_ratio", fb_ratio}, {"used_slots", st.used_slots}});
    }

    res.set_content(out.dump(), "application/json");
    log.warn("fallback_vs_load done");
  });

  // GET /api/stats
  app.Get("/api/stats", [&](const httplib::Request&, httplib::Response& res) {
    auto p = ht.load_or_init_meta(100000, 2, 0.98, std::nullopt, std::nullopt, std::nullopt);
    auto s = ht.stats(p);
    // resize state (best-effort)
    bool is_resizing = false;
    uint64_t resize_progress = 0;
    uint64_t new_total_bins = 0;
    try {
      auto rows = db.query("SELECT is_resizing,resize_progress,new_total_bins FROM ht_meta WHERE id=1");
      if (!rows.empty() && rows[0].size() >= 3) {
        is_resizing = (std::stoull(rows[0][0]) != 0);
        resize_progress = std::stoull(rows[0][1]);
        new_total_bins = std::stoull(rows[0][2]);
      }
    } catch (...) {
    }
    json out{
        {"used_slots", s.used_slots},
        {"fallback_used", s.fallback_used},
        {"capacity_slots", p.capacity_slots},
        {"load_factor", p.capacity_slots ? (double)s.used_slots / (double)p.capacity_slots : 0.0},
        {"resize",
         {{"is_resizing", is_resizing},
          {"resize_progress", resize_progress},
          {"new_total_bins", new_total_bins},
          {"total_bins", p.total_bins}}},
        {"params",
         {{"n", p.n},
          {"mini_bin_size", p.mini_bin_size},
          {"num_mini_bins", p.num_mini_bins},
          {"fallback_size", p.fallback_size},
          {"bin_size", p.bin_size},
          {"total_bins", p.total_bins}}},
    };
    res.set_content(out.dump(), "application/json");
  });

  // GET /api/bins?start=0&count=200
  app.Get("/api/bins", [&](const httplib::Request& req, httplib::Response& res) {
    auto p = ht.load_or_init_meta(100000, 2, 0.98, std::nullopt, std::nullopt, std::nullopt);
    uint64_t start = q_u64(req, "start", 0);
    uint64_t count = q_u64(req, "count", 200);
    count = std::min<uint64_t>(count, 2000);

    auto bins = ht.bin_stats(p, start, count);
    json out;
    out["bin_start"] = start;
    out["bin_count"] = count;
    out["total_bins"] = p.total_bins;
    out["bin_size"] = p.bin_size;
    out["mini_bin_size"] = p.mini_bin_size;
    out["num_mini_bins"] = p.num_mini_bins;
    out["fallback_size"] = p.fallback_size;
    out["bins"] = json::array();
    for (const auto& b : bins) {
      out["bins"].push_back({{"bin", b.bin}, {"used_slots", b.used_slots}, {"fallback_used", b.fallback_used}});
    }
    res.set_content(out.dump(), "application/json");
  });

  // GET /api/snapshot?bin_start=0&bin_count=50
  app.Get("/api/snapshot", [&](const httplib::Request& req, httplib::Response& res) {
    auto p = ht.load_or_init_meta(100000, 2, 0.98, std::nullopt, std::nullopt, std::nullopt);
    uint64_t bin_start = q_u64(req, "bin_start", 0);
    uint64_t bin_count = q_u64(req, "bin_count", 50);
    bin_count = std::min<uint64_t>(bin_count, 200);

    auto snap = ht.snapshot_bins(p, bin_start, bin_count);
    json out;
    out["bin_start"] = bin_start;
    out["bin_count"] = bin_count;
    out["bin_size"] = p.bin_size;
    out["mini_total"] = p.mini_bin_size * p.num_mini_bins;
    out["slots"] = json::array();
    for (const auto& row : snap) {
      // row = [idx, fp, reserved]
      // Frontend expects `is_used` (and may read kick_depth). Keep backward-compatible fields.
      uint64_t fp = row[1];
      out["slots"].push_back({{"idx", row[0]}, {"fp", fp}, {"is_used", fp ? 1 : 0}, {"kick_depth", 0}});
    }
    res.set_content(out.dump(), "application/json");
  });

  if (!app.listen(srv_cfg.bind_host, srv_cfg.port)) {
    throw std::runtime_error("failed to listen");
  }
  return 0;
}

} // namespace otsh

