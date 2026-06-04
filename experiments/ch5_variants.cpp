// CH5 三方案对比：V1 多层 cubby / V2 纯 SQLite / V3 hot-tail + SQLite per-r。
//
// 用法：
//   otsh_ch5_variants [--variant=V1|V2|V3|all]
//   [--scale=5e3|1e4|5e4|1e5|5e5|all]
//                     [--workload=core|all]
//                     [--seed=N] [--runs=N] [--quick]
//                     [--db-dir=PATH] [--keep-db] [--fast-process-exit]
//
// 输出：标准 out + 形如 CH5_RESULT variant=... insert_avg_us=...
// query_avg_us=... delete_avg_us=... bits_per_key=... 的日志行。

#include "otsh/keystore.h"
#include "otsh/system_params.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace
{
    using clk = std::chrono::steady_clock;

    void setup_utf8()
    {
#ifdef _WIN32
        SetConsoleOutputCP(65001);
        SetConsoleCP(65001);
#endif
        std::cout << std::unitbuf;
    }

    uint64_t dur_ns(clk::time_point a, clk::time_point b)
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
    }

    struct Histogram
    {
        std::vector<uint64_t> v;
        void add(uint64_t ns) { v.push_back(ns); }
        uint64_t total() const
        {
            uint64_t s = 0;
            for (auto x : v)
                s += x;
            return s;
        }
        double avg() const
        {
            return v.empty() ? 0.0 : static_cast<double>(total()) / v.size();
        }
    };

    enum class Workload
    {
        Core,
    };

    const char *workload_name(Workload w)
    {
        switch (w)
        {
        case Workload::Core:
            return "core";
        }
        return "?";
    }

    Workload workload_from(const std::string &s)
    {
        (void)s;
        return Workload::Core;
    }

    // 生成 n_keys 个唯一的 64-bit key。
    std::vector<uint64_t> gen_unique_keys(size_t n_keys, uint64_t seed)
    {
        std::vector<uint64_t> out;
        out.reserve(n_keys);
        std::mt19937_64 rng(seed ^ 0xA5A5A5A5A5A5A5A5ULL);
        std::unordered_set<uint64_t> seen;
        seen.reserve(n_keys * 2);
        while (out.size() < n_keys)
        {
            uint64_t k = rng();
            if (k == 0)
                continue;
            if (seen.insert(k).second)
                out.push_back(k);
        }
        return out;
    }

    // Zipf 采样器（α，1..n）。一次性预计算累积分布；适合中等 n。
    class ZipfSampler
    {
    public:
        ZipfSampler(size_t n, double alpha, uint64_t seed) : rng_(seed)
        {
            cdf_.resize(n);
            double s = 0;
            for (size_t i = 0; i < n; ++i)
            {
                s += 1.0 / std::pow(static_cast<double>(i + 1), alpha);
                cdf_[i] = s;
            }
            const double total = s;
            for (auto &x : cdf_)
                x /= total;
        }
        size_t sample()
        {
            const double u = std::uniform_real_distribution<double>{0.0, 1.0}(rng_);
            auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
            return static_cast<size_t>(it - cdf_.begin());
        }

    private:
        std::vector<double> cdf_;
        std::mt19937_64 rng_;
    };

    size_t sample_zipf_newer_index(size_t n, ZipfSampler &zs)
    {
        if (n == 0)
            return 0;
        const size_t rank = std::min(zs.sample(), n - 1);
        // Zipf rank 0 对应最新 key，rank 越大越旧。
        return (n - 1) - rank;
    }

    struct ScaleSpec
    {
        const char *name;
        uint64_t n;
    };

    ScaleSpec scale_from(const std::string &s)
    {
        if (s == "5e3")
            return {"5e3", 5'000};
        if (s == "1e4")
            return {"1e4", 10'000};
        if (s == "5e4")
            return {"5e4", 50'000};
        if (s == "1e5")
            return {"1e5", 100'000};
        if (s == "5e5")
            return {"5e5", 500'000};
        return {"5e3", 5'000};
    }

    struct RunCfg
    {
        otsh::StoreVariant variant;
        ScaleSpec scale;
        Workload workload;
        uint64_t seed = 1;
        bool quick = false;
        std::string db_dir = "test_output/ch5_db";
        bool keep_db = false;
    };

    otsh::StoreParams build_params(const RunCfg &cfg)
    {
        otsh::StoreParams p;
        p.table.n = cfg.scale.n;
        p.table.k = 4;
        p.table.k_polylog_exp = 3;
        p.table.node_max_bits = 2;
        p.table.max_tier = 3;
        p.table.load_factor = 1;
        // CH5：V1 采用单表固定 K，避免迁移开销污染对比
        p.table.enable_rebuild_up = true;
        p.table.enable_rebuild_down = true;
        p.table.tier_use_canon = false;
        p.table.seed1 = cfg.seed ^ 0x1111111111111111ULL;
        p.table.seed2 = cfg.seed ^ 0x2222222222222222ULL;
        p.table.seed3 = cfg.seed ^ 0x3333333333333333ULL;
        p.table.preset_id = std::string("ch5_") + cfg.scale.name;

        // V2/V3 用 SQLite，路径区分 variant/scale/workload 避免互踩
        if (cfg.variant == otsh::StoreVariant::V2_SqliteOnly ||
            cfg.variant == otsh::StoreVariant::V3_HotTailSqlite)
        {
            fs::create_directories(cfg.db_dir);
            std::ostringstream oss;
            oss << cfg.db_dir << "/" << otsh::variant_name(cfg.variant) << "_"
                << cfg.scale.name << "_" << workload_name(cfg.workload) << "_s"
                << cfg.seed << ".db";
            p.sqlite_path = oss.str();
        }
        p.frozen_queue_max = 4;
        p.tail_capacity_override = 0; // 让 V3 自动选 max(64, K/16)
        return p;
    }

    struct WorkloadStats
    {
        Histogram ins, qry, del;
        uint64_t kicks = 0;
        uint64_t inserts_done = 0;
        uint64_t queries_done = 0;
        uint64_t deletes_done = 0;
        uint64_t queries_found = 0;
        uint64_t expected_inserts = 0;
        uint64_t expected_queries = 0;
        uint64_t expected_deletes = 0;
        bool correctness_ok = true;
        std::string err;
    };

    // 把 keys 全部 insert 进去；中途记录延迟与 kick。
    bool do_build(otsh::IKeyStore &store, const std::vector<uint64_t> &keys,
                  WorkloadStats &st)
    {
        st.ins.v.reserve(keys.size());
        st.expected_inserts += static_cast<uint64_t>(keys.size());
        for (uint64_t k : keys)
        {
            const auto t0 = clk::now();
            auto r = store.insert(k);
            const auto t1 = clk::now();
            if (!r.ok)
            {
                st.err = "insert: " + r.error;
                return false;
            }
            st.ins.add(dur_ns(t0, t1));
            st.kicks += r.kick_count;
            if (r.inserted)
                ++st.inserts_done;
        }
        store.drain_background_work();
        return true;
    }

    bool verify_all_present(otsh::IKeyStore &store,
                            const std::vector<uint64_t> &keys)
    {
        for (uint64_t k : keys)
        {
            auto r = store.query(k);
            if (!r.ok || !r.found)
                return false;
        }
        return true;
    }

    WorkloadStats run_workload(const RunCfg &cfg, otsh::IKeyStore &store)
    {
        WorkloadStats st;

        const size_t base_n = cfg.quick ? std::min<size_t>(cfg.scale.n, 1000)
                                        : static_cast<size_t>(cfg.scale.n);
        auto keys = gen_unique_keys(base_n, cfg.seed);

        // Core workload:
        // 1) 插入 N 个 key，衡量 insert_avg_us。
        // 2) 查询 N 次，访问分布按“越新的 key 越热”的 Zipf rank，体现近期局部性。
        // 3) 删除最新 20% key，衡量 delete_avg_us，保持查询/删除都符合近期局部性。
        if (!do_build(store, keys, st))
            return st;

        if (!verify_all_present(store, keys))
        {
            st.correctness_ok = false;
            st.err = "verify_all_present failed";
            return st;
        }

        ZipfSampler query_rank(keys.size(), 1.1, cfg.seed ^ 0xBEEF);
        st.expected_queries += static_cast<uint64_t>(base_n);
        st.qry.v.reserve(base_n);
        for (size_t i = 0; i < base_n; ++i)
        {
            const size_t idx = sample_zipf_newer_index(keys.size(), query_rank);
            const auto t0 = clk::now();
            auto r = store.query(keys[idx]);
            const auto t1 = clk::now();
            if (!r.ok)
            {
                st.err = "query: " + r.error;
                break;
            }
            st.qry.add(dur_ns(t0, t1));
            ++st.queries_done;
            if (r.found)
                ++st.queries_found;
        }

        const size_t delete_n = std::max<size_t>(1, base_n / 5);
        st.expected_deletes += static_cast<uint64_t>(delete_n);
        st.del.v.reserve(delete_n);
        for (size_t i = 0; i < delete_n; ++i)
        {
            const uint64_t key = keys[keys.size() - 1 - i];
            const auto t0 = clk::now();
            auto r = store.erase(key);
            const auto t1 = clk::now();
            if (!r.ok)
            {
                st.err = "erase: " + r.error;
                break;
            }
            st.del.add(dur_ns(t0, t1));
            if (r.deleted)
                ++st.deletes_done;
        }

        store.drain_background_work();

        if (st.inserts_done != st.expected_inserts ||
            st.queries_found != st.expected_queries ||
            st.deletes_done != st.expected_deletes)
        {
            st.correctness_ok = false;
            if (st.err.empty())
            {
                std::ostringstream oss;
                oss << "correctness insert=" << st.inserts_done << "/"
                    << st.expected_inserts << " query=" << st.queries_found << "/"
                    << st.expected_queries << " delete=" << st.deletes_done << "/"
                    << st.expected_deletes;
                st.err = oss.str();
            }
        }

        return st;
    }

    void print_log_metrics(const RunCfg &cfg, const WorkloadStats &st,
                           const otsh::StoreStats &ss)
    {
        const char *v = otsh::variant_name(cfg.variant);
        const char *sc = cfg.scale.name;
        const char *wn = workload_name(cfg.workload);
        const double bits_per_key =
            ss.n ? (static_cast<double>(ss.mem_meta_bits) +
                    static_cast<double>(ss.disk_file_bytes) * 8.0) /
                       static_cast<double>(ss.n)
                 : 0.0;
        const uint64_t expected_total =
            st.expected_inserts + st.expected_queries + st.expected_deletes;
        const uint64_t success_total =
            st.inserts_done + st.queries_found + st.deletes_done;
        const double correctness_pct =
            expected_total ? 100.0 * static_cast<double>(success_total) /
                                 static_cast<double>(expected_total)
                           : 100.0;
        std::cout << "CH5_RESULT"
                  << " variant=" << v << " scale=" << sc << " workload=" << wn
                  << " insert_avg_us=" << std::fixed << std::setprecision(3)
                  << st.ins.avg() / 1000.0
                  << " query_avg_us=" << st.qry.avg() / 1000.0
                  << " delete_avg_us=" << st.del.avg() / 1000.0
                  << " bits_per_key=" << bits_per_key
                  << " correctness_pct=" << correctness_pct << "\n";
    }

    bool run_once(const RunCfg &cfg, bool fast_process_exit)
    {
        std::cout << "[CH5] variant=" << otsh::variant_name(cfg.variant)
                  << " scale=" << cfg.scale.name
                  << " workload=" << workload_name(cfg.workload)
                  << " seed=" << cfg.seed << " quick=" << (cfg.quick ? "1" : "0")
                  << "\n";

        auto store = otsh::make_keystore(cfg.variant);
        const auto params = build_params(cfg);
        auto init_r = store->init(params);
        if (!init_r.ok)
        {
            std::cout << "  init failed: " << init_r.error << "\n";
            return false;
        }
        const auto t0 = clk::now();
        WorkloadStats st = run_workload(cfg, *store);
        const auto t1 = clk::now();
        auto ss = store->stats();
        std::cout << "  ok=" << (st.err.empty() ? "1" : "0")
                  << " correctness=" << (st.correctness_ok ? "1" : "0")
                  << " elapsed=" << std::fixed << std::setprecision(3)
                  << dur_ns(t0, t1) / 1e6 << "ms"
                  << " ins=" << st.inserts_done << " qry=" << st.queries_done
                  << " del=" << st.deletes_done << " n=" << ss.n << "\n";
        if (!st.err.empty())
            std::cout << "  err: " << st.err << "\n";
        const double bits_per_key =
            ss.n ? (static_cast<double>(ss.mem_meta_bits) +
                    static_cast<double>(ss.disk_file_bytes) * 8.0) /
                       static_cast<double>(ss.n)
                 : 0.0;
        const uint64_t expected_total =
            st.expected_inserts + st.expected_queries + st.expected_deletes;
        const uint64_t success_total =
            st.inserts_done + st.queries_found + st.deletes_done;
        const double correctness_pct =
            expected_total ? 100.0 * static_cast<double>(success_total) /
                                 static_cast<double>(expected_total)
                           : 100.0;
        std::cout << "  insert_avg=" << st.ins.avg() / 1000.0 << "us"
                  << " query_avg=" << st.qry.avg() / 1000.0 << "us"
                  << " delete_avg=" << st.del.avg() / 1000.0 << "us"
                  << " bits_per_key=" << bits_per_key
                  << " correctness=" << correctness_pct << "%\n";
        print_log_metrics(cfg, st, ss);

        // 清理 DB 文件（除非 keep_db）
        if (!cfg.keep_db && !params.sqlite_path.empty() &&
            params.sqlite_path != ":memory:")
        {
            std::error_code ec;
            fs::remove(params.sqlite_path, ec);
            fs::remove(params.sqlite_path + "-wal", ec);
            fs::remove(params.sqlite_path + "-shm", ec);
            fs::remove(params.sqlite_path + "-journal", ec);
        }
        if (fast_process_exit && cfg.variant == otsh::StoreVariant::V1_TieredCubby)
        {
            // The CH5 runner starts one child process per V1 data point.  Releasing the
            // large in-memory V1 table avoids minutes of allocator teardown after the
            // result is already persisted; the OS reclaims the address space on exit.
            (void)store.release();
            return true;
        }
        return false;
    }

} // namespace

int main(int argc, char **argv)
{
    setup_utf8();

    std::string variant = "all";
    std::string scale = "1e4";
    std::string workload = "all";
    bool scale_all = false;
    uint64_t seed = 1;
    int runs = 1;
    bool quick = false;
    std::string db_dir = "test_output/ch5_db";
    bool keep_db = false;
    bool fast_process_exit = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto match = [&](const char *prefix, std::string &dst) -> bool
        {
            const size_t L = std::strlen(prefix);
            if (a.size() > L && a.compare(0, L, prefix) == 0)
            {
                dst = a.substr(L);
                return true;
            }
            return false;
        };
        if (match("--variant=", variant))
            continue;
        if (match("--scale=", scale))
        {
            if (scale == "all")
                scale_all = true;
            continue;
        }
        if (match("--workload=", workload))
            continue;
        if (match("--db-dir=", db_dir))
            continue;
        std::string sv;
        if (match("--seed=", sv))
        {
            seed = std::strtoull(sv.c_str(), nullptr, 10);
            continue;
        }
        std::string rv;
        if (match("--runs=", rv))
        {
            runs = std::max(1, std::atoi(rv.c_str()));
            continue;
        }
        if (a == "--quick")
        {
            quick = true;
            continue;
        }
        if (a == "--keep-db")
        {
            keep_db = true;
            continue;
        }
        if (a == "--fast-process-exit")
        {
            fast_process_exit = true;
            continue;
        }
    }

    std::vector<otsh::StoreVariant> variants;
    if (variant == "all")
    {
        variants = {otsh::StoreVariant::V1_TieredCubby,
                    otsh::StoreVariant::V2_SqliteOnly,
                    otsh::StoreVariant::V3_HotTailSqlite};
    }
    else
    {
        variants.push_back(otsh::variant_from_string(variant));
    }

    std::vector<ScaleSpec> scales;
    if (scale_all)
        scales = {{"5e3", 5'000},
                  {"1e4", 10'000},
                  {"5e4", 50'000},
                  {"1e5", 100'000},
                  {"5e5", 500'000}};
    else
        scales.push_back(scale_from(scale));

    std::vector<Workload> wls;
    if (workload == "all")
    {
        wls = {Workload::Core};
    }
    else
    {
        wls.push_back(workload_from(workload));
    }

    std::cout << "=== otsh_ch5_variants ===\n"
              << "variants=" << variants.size() << " scales=" << scales.size()
              << " workloads=" << wls.size() << " runs=" << runs
              << " quick=" << (quick ? "1" : "0") << "\n";

    bool released_for_fast_exit = false;
    for (auto sc : scales)
        for (auto v : variants)
            for (auto wl : wls)
                for (int r = 0; r < runs; ++r)
                {
                    RunCfg cfg;
                    cfg.variant = v;
                    cfg.scale = sc;
                    cfg.workload = wl;
                    cfg.seed = seed + static_cast<uint64_t>(r);
                    cfg.quick = quick;
                    cfg.db_dir = db_dir;
                    cfg.keep_db = keep_db;
                    released_for_fast_exit =
                        run_once(cfg, fast_process_exit) || released_for_fast_exit;
                }

    std::cout << "Done\n"
              << std::flush;
    if (released_for_fast_exit)
        std::_Exit(0);
    return 0;
}
