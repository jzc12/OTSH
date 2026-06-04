// 实验口径对齐 experiments/out.log：四组表 n / Kfixed / k / tier
//
// 用法:
//   otsh_experiment [--group=n|Kfixed|k|tier|all] [--quick] [--seed N] [--runs
//   N]

#include "config.h"
#include "ht.h"
#include "metrics.h"
#include "otsh/system_params.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
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

namespace
{

    using clockk = std::chrono::steady_clock;

    enum class TierWl
    {
        InsertOnly,
        DeleteOnly,
        Mix8020,
        Mix5050
    };

    struct Workload
    {
        size_t inserts = 0;
        size_t queries = 0;
        size_t deletes = 0;
        bool uniform_keys = true;
        size_t dense_facilities = 0;
        bool measure_at_insert_peak = false; // k/K：插入后测真实负载与 bpk
        bool rebuild_from_delete_phase =
            false;                      // tier Delete-only：只计删除阶段 rebuild
        bool insert_until_fail = false; // k 组：插满至失败
        bool tier_phased_mix = false;   // tier 混合：预热后分批交错增删
        size_t mix_ins_per_del = 4;     // 混合插入:删除 ≈ 4:1 或 1:1
    };

    struct Result
    {
        bool ok = true;
        uint64_t ins_avg_ns = 0;
        uint64_t qry_avg_ns = 0;
        uint64_t del_avg_ns = 0;
        uint64_t kick_total = 0;
        uint64_t kick_insert = 0; // 插入阶段踢出（K/k 组口径）
        uint64_t insert_moved_max = 0; // 单次插入最大踢出/移动次数（metrics）
        uint64_t rebuild_down = 0;
        uint64_t rebuild_up = 0;
        double rebuild_avg_ms = 0.0;
        double bits_per_key = 0.0;
        double load_factor_pct = 0.0;
        uint64_t final_n = 0;
        uint64_t slot_capacity = 0;
        uint64_t slots_occupied = 0;
    };

    void apply_seed(otsh::TableParams &p, uint64_t seed)
    {
        p.seed1 = seed ^ 0x1111111111111111ULL;
        p.seed2 = seed ^ 0x2222222222222222ULL;
        p.seed3 = seed ^ 0x3333333333333333ULL;
    }

    uint64_t dur_ns(clockk::time_point a, clockk::time_point b)
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
    }

    uint64_t median_u64(std::vector<uint64_t> v)
    {
        if (v.empty())
            return 0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }

    double median_d(std::vector<double> v)
    {
        if (v.empty())
            return 0.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }

    double ns_to_us(uint64_t ns) { return static_cast<double>(ns) / 1000.0; }

    void setup_console_utf8()
    {
#ifdef _WIN32
        SetConsoleOutputCP(65001);
        SetConsoleCP(65001);
#endif
        std::cout << std::unitbuf;
    }

    std::string format_n_label(uint64_t n)
    {
        struct Pow
        {
            uint64_t v;
            int e;
        };
        static const Pow kPow[] = {
            {10, 1}, {100, 2}, {1'000, 3}, {10'000, 4}, {100'000, 5}, {1'000'000, 6}, {10'000'000, 7}, {100'000'000, 8}, {1'000'000'000, 9}};
        for (int i = static_cast<int>(sizeof(kPow) / sizeof(kPow[0])) - 1; i >= 0;
             --i)
        {
            if (n == kPow[i].v)
                return std::string("10^") + std::to_string(kPow[i].e);
        }
        return std::to_string(n);
    }

    uint64_t facility_count(const otsh::DerivedParams &d)
    {
        return d.K > 0 ? std::max<uint64_t>(1, d.N / d.K) : 1;
    }

    const char *tier_wl_display(TierWl tw)
    {
        switch (tw)
        {
        case TierWl::InsertOnly:
            return "Insert-only";
        case TierWl::DeleteOnly:
            return "Delete-only";
        case TierWl::Mix8020:
            return "80/20 混合";
        case TierWl::Mix5050:
            return "50/50 混合";
        }
        return "Insert-only";
    }

    size_t insert_cap(const otsh::DerivedParams &d)
    {
        return static_cast<size_t>(static_cast<double>(d.N) * d.load_factor * 0.95);
    }

    size_t target_inserts(const otsh::DerivedParams &d, otsh::ExperimentGroup group,
                          bool quick)
    {
        const size_t cap = insert_cap(d);
        if (group == otsh::ExperimentGroup::NMicro)
        {
            // 小 n：按 n 的 85% 填充，便于逐级观察 ins/qry/del/rebuild
            return std::min(static_cast<size_t>(static_cast<double>(d.n_hint) * 0.85),
                            cap);
        }
        if (quick)
        {
            const size_t floor_n = std::min<size_t>(500, static_cast<size_t>(d.n_hint));
            const size_t scaled = std::max(floor_n, d.n_hint / 2000);
            return std::min({size_t(5000), scaled, cap});
        }
        if (group == otsh::ExperimentGroup::KKickDepth ||
            group == otsh::ExperimentGroup::KFixed)
        {
            // 按表容量高负载填充（≈98%·load_factor），与 out.log 口径一致
            return static_cast<size_t>(static_cast<double>(d.N) * d.load_factor * 0.98);
        }
        return std::min(static_cast<size_t>(d.n_hint), cap);
    }

    Workload make_workload(const otsh::DerivedParams &d,
                           otsh::ExperimentGroup group, TierWl tw, bool quick)
    {
        Workload w;
        w.inserts = target_inserts(d, group, quick);
        w.queries = w.inserts;

        if (group == otsh::ExperimentGroup::TierCubby)
        {
            w.uniform_keys = false;
            if (quick)
                w.dense_facilities = 4;
            else if (d.n_hint >= 100'000)
                w.dense_facilities = 32;
            else if (d.n_hint >= 10'000)
                w.dense_facilities = 16;
            else
                w.dense_facilities = 4;
            switch (tw)
            {
            case TierWl::InsertOnly:
                w.deletes = 0;
                break;
            case TierWl::DeleteOnly:
                w.deletes = w.inserts;
                w.rebuild_from_delete_phase = true;
                break;
            case TierWl::Mix8020:
                w.deletes = w.inserts / 5;
                w.tier_phased_mix = true;
                w.mix_ins_per_del = 4;
                break;
            case TierWl::Mix5050:
                w.deletes = w.inserts / 2;
                w.tier_phased_mix = true;
                w.mix_ins_per_del = 1;
                break;
            }
            return w;
        }

        w.uniform_keys = true;
        if (group == otsh::ExperimentGroup::KKickDepth)
        {
            // 与 K 固定组同口径：高负载填充后在插入峰值测 bpk（k 增大应降低
            // Cubby 元数据 bits/key）；不再 insert_until_fail，避免 ~22% 低负载趋同。
            w.deletes = w.inserts / 4;
            w.measure_at_insert_peak = true;
        }
        else if (group == otsh::ExperimentGroup::KFixed)
        {
            w.deletes = w.inserts / 4;
            w.measure_at_insert_peak = true;
        }
        else
            w.deletes = w.inserts / 4;
        return w;
    }

    void storage_load_from_ht(const otsh::HashTable &ht, uint64_t &occupied,
                              uint64_t &capacity)
    {
        occupied = 0;
        capacity = 0;
        ht.visit_structure([&](const otsh::CubbyStructureView &cv)
                           {
    capacity += static_cast<uint64_t>(cv.capacity);
    occupied += static_cast<uint64_t>(cv.size); });
    }

    double storage_load_pct(const otsh::HashTable &ht)
    {
        uint64_t occ = 0, cap = 0;
        storage_load_from_ht(ht, occ, cap);
        if (!cap)
            return 0.0;
        return std::min(100.0,
                        100.0 * static_cast<double>(occ) / static_cast<double>(cap));
    }

    double peak_load_pct(const otsh::HashTable &ht, uint64_t N)
    {
        const double slot = storage_load_pct(ht);
        if (slot > 0.01)
            return slot;
        if (N == 0)
            return 0.0;
        return std::min(100.0, 100.0 * static_cast<double>(ht.state().n) /
                                   static_cast<double>(N));
    }

    std::vector<uint64_t>
    gen_keys(otsh::HashTable &ht, const otsh::DerivedParams &d, const Workload &w)
    {
        const size_t need = w.insert_until_fail
                                ? std::max(w.inserts, insert_cap(d) + 1000)
                                : w.inserts;
        std::vector<uint64_t> out;
        out.reserve(need);

        if (w.uniform_keys)
        {
            std::mt19937_64 rng(d.n_hint ^ 0x42);
            std::unordered_set<uint64_t> seen;
            seen.reserve(need * 2);
            while (seen.size() < need)
            {
                const uint64_t k = rng();
                if (seen.insert(k).second)
                    out.push_back(k);
            }
            return out;
        }

        const size_t fac_cnt = static_cast<size_t>(
            std::max<uint64_t>(1, d.N / std::max<uint64_t>(1, d.K)));
        const size_t dense =
            std::max<size_t>(1, std::min(w.dense_facilities, fac_cnt));
        for (uint64_t cand = 0; out.size() < need && cand < 50'000'000ULL; ++cand)
        {
            const uint64_t gx = ht.pi_of(cand);
            const uint64_t g = gx & (d.N - 1);
            const size_t fidx =
                static_cast<size_t>((g / d.K) % static_cast<uint64_t>(fac_cnt));
            if (fidx < dense)
                out.push_back(cand);
        }
        return out;
    }

    Result run_once(const otsh::TableParams &params, const otsh::DerivedParams &d,
                    const Workload &w)
    {
        otsh::global_metrics().on_init();
        const otsh::Metrics::Snapshot m0 = otsh::global_metrics().snapshot();

        Result r;
        otsh::HashTable ht;
        if (!ht.init(params).ok)
        {
            r.ok = false;
            return r;
        }

        std::vector<uint64_t> keys = gen_keys(ht, d, w);
        if (!w.insert_until_fail && keys.size() < w.inserts)
        {
            r.ok = false;
            return r;
        }
        if (!w.insert_until_fail)
            keys.resize(w.inserts);

        std::vector<uint64_t> lat_ins, lat_qry, lat_del;
        lat_ins.reserve(w.inserts);
        lat_qry.reserve(w.queries);
        lat_del.reserve(w.deletes);

        std::vector<uint64_t> live_keys;
        live_keys.reserve(w.inserts);

        std::mt19937_64 rng(params.seed1 ^ params.seed2 ^ 0x42);

        auto do_insert = [&](uint64_t k) -> bool
        {
            const auto t0 = clockk::now();
            const otsh::InsertResult ir = ht.insert(k);
            const auto t1 = clockk::now();
            if (!ir.ok || !ir.inserted)
                return false;
            r.kick_total += ir.kick_count;
            lat_ins.push_back(dur_ns(t0, t1));
            live_keys.push_back(k);
            return true;
        };

        auto do_delete = [&](uint64_t k) -> bool
        {
            const auto t0 = clockk::now();
            const otsh::DeleteResult dr = ht.erase(k);
            const auto t1 = clockk::now();
            if (!dr.ok || !dr.deleted)
                return false;
            r.kick_total += dr.kick_count;
            lat_del.push_back(dur_ns(t0, t1));
            return true;
        };

        size_t key_i = 0;
        if (w.tier_phased_mix)
        {
            const size_t warm = std::max<size_t>(1, w.inserts * 50 / 100);
            const size_t ratio = std::max<size_t>(1, w.mix_ins_per_del);
            const size_t batch_ins =
                std::max<size_t>(64, std::min<size_t>(2000, w.inserts / 30));
            const size_t batch_del =
                std::max<size_t>(1, (ratio == 1) ? batch_ins : batch_ins * 2 / ratio);
            size_t done_ins = 0;
            size_t done_del = 0;

            while (done_ins < warm && key_i < keys.size())
            {
                if (!do_insert(keys[key_i++]))
                {
                    r.ok = false;
                    return r;
                }
                ++done_ins;
            }

            while (done_ins < w.inserts || done_del < w.deletes)
            {
                const size_t ins_now =
                    std::min({batch_ins, w.inserts - done_ins, keys.size() - key_i});
                for (size_t t = 0; t < ins_now; ++t)
                {
                    if (!do_insert(keys[key_i++]))
                    {
                        r.ok = false;
                        return r;
                    }
                    ++done_ins;
                }
                if (done_del >= w.deletes || live_keys.empty())
                {
                    if (done_ins >= w.inserts)
                        break;
                    continue;
                }
                const size_t del_now =
                    std::min({batch_del, w.deletes - done_del, live_keys.size()});
                for (size_t t = 0; t < del_now; ++t)
                {
                    const size_t pick = static_cast<size_t>(
                        rng() % static_cast<uint64_t>(live_keys.size()));
                    const uint64_t dk = live_keys[pick];
                    if (!do_delete(dk))
                    {
                        r.ok = false;
                        return r;
                    }
                    live_keys[pick] = live_keys.back();
                    live_keys.pop_back();
                    ++done_del;
                }
            }
        }
        else
        {
            size_t inserted = 0;
            for (; key_i < keys.size(); ++key_i)
            {
                if (!w.insert_until_fail && inserted >= w.inserts)
                    break;
                if (!do_insert(keys[key_i]))
                {
                    if (w.insert_until_fail)
                        break;
                    r.ok = false;
                    return r;
                }
                ++inserted;
            }
            if (!w.insert_until_fail && inserted < w.inserts)
            {
                r.ok = false;
                return r;
            }
            if (w.insert_until_fail && inserted == 0)
            {
                r.ok = false;
                return r;
            }
        }
        ht.drain_background_work();

        if (w.measure_at_insert_peak)
        {
            const otsh::Metrics::Snapshot m_ins = otsh::global_metrics().snapshot();
            r.insert_moved_max = m_ins.insert_moved_max;
            r.kick_insert = r.kick_total;
            r.load_factor_pct = peak_load_pct(ht, d.N);
            r.final_n = ht.state().n;
            const uint64_t meta_bits = ht.logical_meta_bits();
            r.bits_per_key = r.final_n ? static_cast<double>(meta_bits) /
                                             static_cast<double>(r.final_n)
                                       : 0.0;
            storage_load_from_ht(ht, r.slots_occupied, r.slot_capacity);
        }

        size_t query_count = w.queries;
        size_t delete_count = w.deletes;
        if (w.insert_until_fail)
        {
            query_count = live_keys.size();
            delete_count = live_keys.size() / 5;
        }

        otsh::Metrics::Snapshot m_rebuild_base = m0;
        if (w.rebuild_from_delete_phase)
            m_rebuild_base = otsh::global_metrics().snapshot();

        const size_t n_q = std::min(query_count, live_keys.size());
        for (size_t i = 0; i < n_q; ++i)
        {
            const auto t0 = clockk::now();
            const otsh::QueryResult qr = ht.query(live_keys[i]);
            const auto t1 = clockk::now();
            if (!qr.ok || !qr.found)
            {
                r.ok = false;
                return r;
            }
            lat_qry.push_back(dur_ns(t0, t1));
        }

        if (!w.tier_phased_mix)
        {
            std::vector<uint64_t> del_keys = live_keys;
            std::shuffle(del_keys.begin(), del_keys.end(), rng);
            const size_t n_del = std::min(delete_count, del_keys.size());

            for (size_t i = 0; i < n_del; ++i)
            {
                const auto t0 = clockk::now();
                const otsh::DeleteResult dr = ht.erase(del_keys[i]);
                const auto t1 = clockk::now();
                if (!dr.ok || !dr.deleted)
                {
                    r.ok = false;
                    return r;
                }
                r.kick_total += dr.kick_count;
                lat_del.push_back(dur_ns(t0, t1));
            }
            ht.drain_background_work();

            for (size_t i = 0; i < n_del; ++i)
            {
                const otsh::QueryResult qr = ht.query(del_keys[i]);
                if (!qr.ok || qr.found)
                {
                    r.ok = false;
                    return r;
                }
            }
        }
        else
        {
            ht.drain_background_work();
        }

        const otsh::Metrics::Snapshot m1 = otsh::global_metrics().snapshot();
        r.rebuild_up = m1.events.rebuild_up - m_rebuild_base.events.rebuild_up;
        r.rebuild_down = m1.events.rebuild_down - m_rebuild_base.events.rebuild_down;
        const uint64_t rebuild_cnt = r.rebuild_up + r.rebuild_down;
        const uint64_t rebuild_ns =
            m1.rebuild_elapsed_ns_total - m0.rebuild_elapsed_ns_total;
        r.rebuild_avg_ms =
            rebuild_cnt ? static_cast<double>(rebuild_ns) / rebuild_cnt / 1e6 : 0.0;

        if (!w.measure_at_insert_peak)
        {
            r.final_n = ht.state().n;
            const uint64_t meta_bits = ht.logical_meta_bits();
            r.bits_per_key =
                r.final_n ? static_cast<double>(meta_bits) / r.final_n : 0.0;
            r.load_factor_pct = peak_load_pct(ht, d.N);
            storage_load_from_ht(ht, r.slots_occupied, r.slot_capacity);
            r.insert_moved_max = m1.insert_moved_max;
        }

        r.ins_avg_ns = lat_ins.empty() ? 0 : median_u64(lat_ins);
        r.qry_avg_ns = lat_qry.empty() ? 0 : median_u64(lat_qry);
        r.del_avg_ns = lat_del.empty() ? 0 : median_u64(lat_del);
        return r;
    }

    Result run_median(const otsh::TableParams &base, const otsh::DerivedParams &d,
                      const Workload &w, int runs, uint64_t seed)
    {
        std::vector<Result> all;
        all.reserve(static_cast<size_t>(runs));
        for (int i = 0; i < runs; ++i)
        {
            otsh::TableParams p = base;
            if (runs > 1)
            {
                const uint64_t s =
                    seed + static_cast<uint64_t>(i) * 0x9E3779B97F4A7C15ULL;
                apply_seed(p, s);
            }
            all.push_back(run_once(p, d, w));
        }

        Result out = all.front();
        if (runs == 1)
            return out;

        std::vector<uint64_t> ins, qry, del, kick, rdn, rup, moved_max;
        std::vector<double> bpk, lf, rms;
        for (const Result &x : all)
        {
            if (!x.ok)
                out.ok = false;
            ins.push_back(x.ins_avg_ns);
            qry.push_back(x.qry_avg_ns);
            del.push_back(x.del_avg_ns);
            kick.push_back(x.kick_total);
            rdn.push_back(x.rebuild_down);
            rup.push_back(x.rebuild_up);
            bpk.push_back(x.bits_per_key);
            lf.push_back(x.load_factor_pct);
            rms.push_back(x.rebuild_avg_ms);
            moved_max.push_back(x.insert_moved_max);
        }
        out.ins_avg_ns = median_u64(ins);
        out.qry_avg_ns = median_u64(qry);
        out.del_avg_ns = median_u64(del);
        out.kick_total = median_u64(kick);
        out.rebuild_down = median_u64(rdn);
        out.rebuild_up = median_u64(rup);
        out.bits_per_key = median_d(bpk);
        out.load_factor_pct = median_d(lf);
        out.rebuild_avg_ms = median_d(rms);
        out.insert_moved_max = median_u64(moved_max);
        return out;
    }

    const char *tier_wl_name(TierWl tw)
    {
        switch (tw)
        {
        case TierWl::InsertOnly:
            return "insert";
        case TierWl::DeleteOnly:
            return "delete";
        case TierWl::Mix8020:
            return "mix8020";
        case TierWl::Mix5050:
            return "mix5050";
        }
        return "insert";
    }

    uint64_t report_kick(const Result &r, bool insert_peak)
    {
        if (insert_peak && r.kick_insert > 0)
            return r.kick_insert;
        return r.kick_total;
    }

    void print_section_header(otsh::ExperimentGroup g, otsh::Ch4Scale scale)
    {
        const char *nlab = otsh::ch4_scale_label(scale);
        const char *stag = otsh::ch4_scale_name(scale);
        const char *kref = (scale == otsh::Ch4Scale::E5)   ? "4096"
                           : (scale == otsh::Ch4Scale::E3) ? "64"
                                                           : "256";
        switch (g)
        {
        case otsh::ExperimentGroup::NMicro:
            std::cout << "\n==========================================================="
                         "=====================\n"
                      << "表 0  渐进验证（n=200→5000, K=log^3 n, k=4, 键均匀分布）  → "
                         "tab:exp-n-micro\n"
                      << "============================================================="
                         "===================\n"
                      << "列: n | ins(μs) | qry(μs) | del(μs) | 总踢出 | rebuild_down "
                         "| rebuild_up | bits/key\n\n";
            break;
        case otsh::ExperimentGroup::NScale:
            std::cout << "\n==========================================================="
                         "=====================\n"
                      << "表 1  规模 n 实验（n=10^3…10^5, K=log^3 n, k=4）  → "
                         "tab:exp-n-fixed\n"
                      << "============================================================="
                         "===================\n"
                      << "列: n | ins(μs) | qry(μs) | del(μs) | 总踢出 | rebuild_down "
                         "| rebuild_up | bits/key\n\n";
            break;
        case otsh::ExperimentGroup::KFixed:
            std::cout << "\n==========================================================="
                         "=====================\n"
                      << "表 2  Facility 规模 K 实验（n=" << nlab
                      << ", k=4）  → tab:exp-K-fixed-" << stag << "\n"
                      << "============================================================="
                         "===================\n"
                      << "列: K | ins(μs) | qry(μs) | del(μs) | 总踢出 | bits/key\n\n";
            break;
        case otsh::ExperimentGroup::KKickDepth:
            std::cout
                << "\n================================================================="
                   "===============\n"
                << "表 3  k-kick 深度 k 实验（n=" << nlab << ", K=" << kref
                << "）  → tab:exp-k-fixed-" << stag << "\n"
                << "==================================================================="
                   "=============\n"
                << "列: k | ins(μs) | qry(μs) | del(μs) | 总踢出 | insert_moved_max "
                   "| bits/key | 平均负载率\n\n";
            break;
        case otsh::ExperimentGroup::TierCubby:
            std::cout
                << "\n================================================================="
                   "===============\n"
                << "表 4  多层级 Cubby 动态重构（n=" << nlab << ", K=" << kref
                << ", k=4）  → tab:exp-tier-fixed-" << stag << "\n"
                << "==================================================================="
                   "=============\n"
                << "列: Workload | rebuild_down | rebuild_up | 平均重构耗时(ms)\n\n";
            break;
        default:
            break;
        }
    }

    void print_row(otsh::ExperimentGroup g, otsh::Ch4Scale scale,
                   const otsh::DerivedParams &d, TierWl tw, const Result &r)
    {
        const char *stag = otsh::ch4_scale_name(scale);
        std::cout << std::fixed << std::setprecision(2);
        switch (g)
        {
        case otsh::ExperimentGroup::NMicro:
        case otsh::ExperimentGroup::NScale:
        {
            const std::string nlab = format_n_label(d.n_hint);
            const uint64_t fac = facility_count(d);
            std::cout << "n=" << nlab << '\n'
                      << "  实测基线: ins=" << ns_to_us(r.ins_avg_ns)
                      << " qry=" << ns_to_us(r.qry_avg_ns)
                      << " del=" << ns_to_us(r.del_avg_ns) << " kick=" << r.kick_total
                      << " rebuild=" << r.rebuild_down << '/' << r.rebuild_up
                      << " bpk=" << std::setprecision(2) << r.bits_per_key
                      << " facilities=" << fac << '\n';
            std::cout << "CH4_TABLE,n," << d.n_hint << ',' << std::fixed
                      << std::setprecision(2) << ns_to_us(r.ins_avg_ns) << ','
                      << ns_to_us(r.qry_avg_ns) << ',' << ns_to_us(r.del_avg_ns) << ','
                      << r.kick_total << ',' << r.rebuild_down << ',' << r.rebuild_up
                      << ',' << r.bits_per_key << '\n';
            break;
        }
        case otsh::ExperimentGroup::KFixed:
        {
            const uint64_t kick = report_kick(r, true);
            std::cout << "K=" << d.K << " (n=" << otsh::ch4_scale_label(scale) << ")\n"
                      << "  实测基线: ins=" << ns_to_us(r.ins_avg_ns)
                      << " qry=" << ns_to_us(r.qry_avg_ns)
                      << " del=" << ns_to_us(r.del_avg_ns) << " kick=" << kick
                      << " bpk=" << std::setprecision(2) << r.bits_per_key << '\n';
            std::cout << "CH4_TABLE,K," << stag << ',' << d.K << ',' << std::fixed
                      << std::setprecision(2) << ns_to_us(r.ins_avg_ns) << ','
                      << ns_to_us(r.qry_avg_ns) << ',' << ns_to_us(r.del_avg_ns) << ','
                      << kick << ',' << r.bits_per_key << '\n';
            break;
        }
        case otsh::ExperimentGroup::KKickDepth:
        {
            const uint64_t kick = report_kick(r, true);
            std::cout << "k=" << d.k_kick << " (n=" << otsh::ch4_scale_label(scale)
                      << ", K=" << d.K << ")\n"
                      << "  实测基线: ins=" << ns_to_us(r.ins_avg_ns)
                      << " qry=" << ns_to_us(r.qry_avg_ns)
                      << " del=" << ns_to_us(r.del_avg_ns) << " kick=" << kick
                      << " moved_max=" << r.insert_moved_max
                      << " bpk=" << std::setprecision(2) << r.bits_per_key
                      << " load=" << r.load_factor_pct << "%\n";
            std::cout << "CH4_TABLE,k," << stag << ',' << d.k_kick << ',' << std::fixed
                      << std::setprecision(2) << ns_to_us(r.ins_avg_ns) << ','
                      << ns_to_us(r.qry_avg_ns) << ',' << ns_to_us(r.del_avg_ns) << ','
                      << kick << ',' << r.insert_moved_max << ',' << r.bits_per_key
                      << ',' << r.load_factor_pct << '\n';
            break;
        }
        case otsh::ExperimentGroup::TierCubby:
            std::cout << tier_wl_display(tw) << " (n=" << otsh::ch4_scale_label(scale)
                      << ")\n"
                      << "  实测基线: rebuild=" << r.rebuild_down << '/' << r.rebuild_up
                      << " ms=" << std::setprecision(2) << r.rebuild_avg_ms << '\n';
            std::cout << "CH4_TABLE,tier," << stag << ',' << tier_wl_name(tw) << ','
                      << std::fixed << std::setprecision(2) << r.rebuild_down << ','
                      << r.rebuild_up << ',' << r.rebuild_avg_ms << '\n';
            break;
        default:
            break;
        }
        std::cout << '\n';
    }

    void run_group(otsh::ExperimentGroup g, otsh::Ch4Scale scale, uint64_t seed,
                   bool quick, int runs, bool &all_ok)
    {
        std::cout << "\n--- scale=" << otsh::ch4_scale_name(scale) << " ---\n";
        print_section_header(g, scale);
        for (const auto &p : otsh::presets_for_group(g, scale))
        {
            otsh::TableParams params = otsh::apply_ch4_mode(p);
            if (g == otsh::ExperimentGroup::TierCubby)
                params.tier_use_canon =
                    true; // §6.2 规范 t_j，否则 t_j=1 导致拆分永不触发
            apply_seed(params, seed);
            const otsh::DerivedParams d = otsh::derive_params(params);

            if (g == otsh::ExperimentGroup::TierCubby)
            {
                for (TierWl tw : {TierWl::InsertOnly, TierWl::DeleteOnly, TierWl::Mix8020,
                                  TierWl::Mix5050})
                {
                    const Workload w = make_workload(d, g, tw, quick);
                    const Result r = run_median(params, d, w, runs, seed);
                    if (!r.ok)
                        all_ok = false;
                    print_row(g, scale, d, tw, r);
                    std::cout.flush();
                }
                return;
            }

            const Workload w = make_workload(d, g, TierWl::InsertOnly, quick);
            const Result r = run_median(params, d, w, runs, seed);
            if (!r.ok)
                all_ok = false;
            print_row(g, scale, d, TierWl::InsertOnly, r);
            std::cout.flush();
        }
    }

} // namespace

int main(int argc, char **argv)
{
    using namespace otsh;

    setup_console_utf8();

    ExperimentGroup group = ExperimentGroup::All;
    Ch4Scale scale = Ch4Scale::E4;
    bool scale_all = true;
    bool scale_explicit = false;
    bool quick = false;
    uint64_t seed = 1;
    int runs = 1;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--quick")
            quick = true;
        else if (a == "--micro")
            group = ExperimentGroup::NMicro;
        else if (a.rfind("--group=", 0) == 0)
            group = experiment_group_from_string(a.substr(8));
        else if (a == "--group" && i + 1 < argc)
            group = experiment_group_from_string(argv[++i]);
        else if (a.rfind("--scale=", 0) == 0)
        {
            scale_explicit = true;
            const std::string s = a.substr(8);
            if (s == "all" || s == "both")
                scale_all = true;
            else
            {
                scale_all = false;
                scale = ch4_scale_from_string(s);
            }
        }
        else if (a == "--scale" && i + 1 < argc)
        {
            scale_explicit = true;
            const std::string s = argv[++i];
            if (s == "all" || s == "both")
                scale_all = true;
            else
            {
                scale_all = false;
                scale = ch4_scale_from_string(s);
            }
        }
        else if (a.rfind("--seed=", 0) == 0)
            seed = std::strtoull(a.c_str() + 7, nullptr, 10);
        else if (a == "--seed" && i + 1 < argc)
            seed = std::strtoull(argv[++i], nullptr, 10);
        else if (a.rfind("--runs=", 0) == 0)
            runs = std::max(1, std::atoi(a.c_str() + 7));
        else if (a == "--runs" && i + 1 < argc)
            runs = std::max(1, std::atoi(argv[++i]));
    }

    if (group == ExperimentGroup::NScale || group == ExperimentGroup::NMicro)
        scale_all = false;
    else if (!scale_explicit && group != ExperimentGroup::All)
        scale_all = true;
    (void)scale_explicit;
    if (group == ExperimentGroup::NMicro)
        runs = 1;
    else if (!quick && runs == 1 && group == ExperimentGroup::All)
        runs = 1;

    std::cout << "=== otsh_experiment (out.log 口径) ===\n"
              << "group="
              << (group == ExperimentGroup::All ? "all"
                                                : experiment_group_name(group))
              << " scale=" << (scale_all ? "all" : ch4_scale_name(scale))
              << " quick=" << (quick ? "true" : "false") << " seed=" << seed
              << " runs=" << runs << "\n";

    bool all_ok = true;
    const auto run_scales = [&]() -> std::vector<Ch4Scale>
    {
        if (scale_all)
            return ch4_all_scales();
        return {scale};
    };

    if (group == ExperimentGroup::All)
    {
        run_group(ExperimentGroup::NScale, Ch4Scale::E4, seed, quick, runs, all_ok);
        for (Ch4Scale s : run_scales())
        {
            run_group(ExperimentGroup::KFixed, s, seed, quick, runs, all_ok);
            run_group(ExperimentGroup::KKickDepth, s, seed, quick, runs, all_ok);
            run_group(ExperimentGroup::TierCubby, s, seed, quick, runs, all_ok);
        }
    }
    else if (group == ExperimentGroup::NScale ||
             group == ExperimentGroup::NMicro)
    {
        run_group(group, Ch4Scale::E4, seed, quick, runs, all_ok);
    }
    else
    {
        for (Ch4Scale s : run_scales())
            run_group(group, s, seed, quick, runs, all_ok);
    }

    std::cout << "--- summary ---\n"
              << "all_ok=" << (all_ok ? "true" : "false") << '\n';
    return all_ok ? 0 : 1;
}