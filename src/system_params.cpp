#include "config.h"
#include "otsh/system_params.h"

#include <algorithm>
#include <cmath>

namespace otsh
{
    namespace
    {

        uint64_t next_pow2(uint64_t x)
        {
            if (x <= 1)
                return 1;
            x--;
            x |= x >> 1;
            x |= x >> 2;
            x |= x >> 4;
            x |= x >> 8;
            x |= x >> 16;
            x |= x >> 32;
            return x + 1;
        }

        uint64_t floor_pow2(uint64_t x)
        {
            if (x <= 1)
                return 1;
            uint64_t p2 = 1;
            while ((p2 << 1) <= x)
                p2 <<= 1;
            return p2;
        }

        double iter_log2(double x, int t)
        {
            x = std::max(2.0, x);
            for (int i = 0; i < t; ++i)
                x = std::log2(std::max(2.0, x));
            return x;
        }

        uint64_t choose_K(uint64_t N, int polylog_exp)
        {
            if (N <= 1)
                return 1;
            const double lg = std::log2(static_cast<double>(N));
            const int exp =
                std::clamp(polylog_exp, 2, 4); // §6.1: log^2 n ~ log^4 n
            double Kd = std::pow(lg, static_cast<double>(exp));
            uint64_t K = static_cast<uint64_t>(std::ceil(Kd));
            K = std::max<uint64_t>(64, std::min<uint64_t>(4096, K));
            K = std::min(K, N);
            return std::max<uint64_t>(1, floor_pow2(K));
        }

        int fanout_of(uint64_t N, int override_fanout)
        {
            if (override_fanout > 0)
                return override_fanout;
            const double lg = std::log2(std::max<uint64_t>(2, N));
            const double lglg = std::log2(std::max(2.0, lg));
            const int f = static_cast<int>(std::floor(lg / (2.0 * lglg)));
            return std::max(4, f);
        }

        TableParams make_preset(const char *id, uint64_t n, int k_kick, int k_exp,
                                int node_bits, int max_tier, double lf,
                                uint64_t k_override = 0)
        {
            TableParams p;
            p.preset_id = id;
            p.n = n;
            p.k = k_kick;
            p.k_polylog_exp = k_exp;
            p.K_override = k_override;
            p.node_max_bits = node_bits;
            p.max_tier = max_tier;
            p.load_factor = lf;
            p.seed1 = 0x1111111111111111ULL;
            p.seed2 = 0x2222222222222222ULL;
            p.seed3 = 0x3333333333333333ULL;
            return p;
        }

    } // namespace

    DerivedParams derive_params(const TableParams &p)
    {
        DerivedParams d;
        d.n_hint = std::max<uint64_t>(2, p.n);
        d.N = next_pow2(d.n_hint);
        d.k_polylog_exp = std::clamp(p.k_polylog_exp, 2, 4);
        if (p.K_override > 0)
        {
            d.K = floor_pow2(p.K_override);
            d.K = std::min(d.K, d.N);
        }
        else
        {
            d.K = choose_K(d.N, d.k_polylog_exp);
        }
        d.k_kick = std::clamp(p.k, 3, 7);
        d.node_max_bits = std::clamp(p.node_max_bits, 1, 3);
        d.fanout = fanout_of(d.N, p.fanout_override);
        d.max_tier = std::max(1, p.max_tier);
        d.tier_use_canon = p.tier_use_canon;
        d.tier_target_divisor = std::max(1, p.tier_target_divisor);
        d.load_factor =
            (p.load_factor > 0.0 && p.load_factor < 1.0) ? p.load_factor : 0.90;
        d.preset_id = p.preset_id;
        return d;
    }

    TableParams apply_ch4_mode(TableParams p)
    {
        p.validation_mode = true;
        p.enable_rebuild_up = true;
        p.enable_rebuild_down = true;
        // §3.1 论文公式 r_j / t_j；§6.2 规范表可显式设 tier_use_canon=true
        p.tier_use_canon = false;
        p.tier_target_divisor = 1;
        return p;
    }

    TableParams apply_derived(const TableParams &base, const DerivedParams &d)
    {
        TableParams p = base;
        p.n = d.n_hint;
        p.k = d.k_kick;
        p.k_polylog_exp = d.k_polylog_exp;
        p.node_max_bits = d.node_max_bits;
        p.fanout_override = d.fanout;
        p.max_tier = d.max_tier;
        p.tier_use_canon = d.tier_use_canon;
        p.tier_target_divisor = d.tier_target_divisor;
        p.load_factor = d.load_factor;
        p.preset_id = d.preset_id;
        return p;
    }

    size_t kkick_bin_size(int depth_i, uint64_t K, uint64_t n_hint)
    {
        (void)n_hint;
        const uint64_t base_K = std::max<uint64_t>(1, K);
        // §4.1：s_0 = K；s_i = Θ((log^(i) K)^6)，i ≥ 1。嵌套切分时再按父 bin 截断。
        if (depth_i <= 0)
            return static_cast<size_t>(base_K);
        double logv = static_cast<double>(std::max<uint64_t>(2, base_K));
        for (int t = 0; t < depth_i; ++t)
            logv = std::log2(std::max(2.0, logv));
        return static_cast<size_t>(std::max(1.0, std::floor(std::pow(logv, 6.0))));
    }

    TierCanonSpec tier_canon_spec(int j, uint64_t n_hint, uint64_t K)
    {
        TierCanonSpec out;
        if (j < 1)
            return out;
        // §6.2 实验规范表（按 n 规模档固定 t_j / r_j，模仿论文章节量级）
        struct Row
        {
            uint64_t n_min;
            uint64_t t[3];
            size_t r[3];
        };
        static const Row kTable[] = {
            // n≥1e7:  t0=6,t1=3,t2=1  r0=12,r1=128,r2=512
            {10'000'000, {6, 3, 1}, {12, 128, 512}},
            // n≥1e6:  基准档（§6.1 n=10^6）
            {1'000'000, {4, 2, 1}, {10, 64, 256}},
            // n≥1e5
            {100'000, {4, 2, 1}, {10, 48, 192}},
            // n≥1e4
            {10'000, {3, 2, 1}, {8, 32, 128}},
            {0, {3, 2, 1}, {8, 24, 96}},
        };
        const Row *row = &kTable[sizeof(kTable) / sizeof(kTable[0]) - 1];
        for (const Row &cand : kTable)
        {
            if (n_hint >= cand.n_min)
            {
                row = &cand;
                break;
            }
        }
        const int idx = std::clamp(j, 1, 3) - 1;
        out.t = row->t[idx];
        out.r = row->r[idx];
        out.r = std::min<size_t>(out.r, static_cast<size_t>(std::max<uint64_t>(4, K)));
        return out;
    }

    uint64_t tier_target_count(int j, uint64_t n_hint, uint64_t K, bool use_canon,
                               int divisor)
    {
        if (j < 1)
            return 0;
        if (use_canon)
            return tier_canon_spec(j, n_hint, K).t;
        const int div = std::max(1, divisor);
        const double n = std::max<double>(
            2.0, static_cast<double>(std::max<uint64_t>(1, n_hint)));
        // §3.1：t_j = (log^(j+1) n)^2 / (log^(j) n)^2
        const double a = iter_log2(n, j + 1);
        const double b = iter_log2(n, j);
        const double tj = (a * a) / std::max(1.0, b * b);
        return static_cast<uint64_t>(std::max(1.0, std::floor(tj / div)));
    }

    size_t tier_cubby_capacity(uint64_t K, uint64_t n_hint, int tier,
                               bool use_canon)
    {
        if (tier < 1)
            return 4;
        if (use_canon)
            return tier_canon_spec(tier, n_hint, K).r;
        double x = static_cast<double>(std::max<uint64_t>(2, n_hint));
        for (int i = 0; i < tier; ++i)
            x = std::log2(std::max(2.0, x));
        const double denom = std::max(1.0, x * x);
        const double cap = static_cast<double>(K) / denom;
        size_t out = static_cast<size_t>(std::max(4.0, std::floor(cap)));
        return std::min<size_t>(out, static_cast<size_t>(K));
    }

    std::vector<TableParams> experiment_presets()
    {
        return {
            // 渐进验证（几百～几千键，秒级完成）
            make_preset("n_200", 200, 4, 3, 2, 2, 0.90),
            make_preset("n_500", 500, 4, 3, 2, 2, 0.90),
            make_preset("n_1k", 1'000, 4, 3, 2, 2, 0.90),
            make_preset("n_2k", 2'000, 4, 3, 2, 2, 0.90),
            make_preset("n_5k", 5'000, 4, 3, 2, 2, 0.90),
            // §6.1 / Ch4 基准：n=10^6, K=log^3 n≈4096, k=4
            make_preset("baseline", 1'000'000, 4, 3, 2, 3, 0.90),
            // 表 1：n=10^3…10^5 × K∈{64,128,256}，k=3
            make_preset("n_1e3_K64", 1'000, 3, 3, 2, 2, 0.90, 64),
            make_preset("n_1e3_K128", 1'000, 3, 3, 2, 2, 0.90, 128),
            make_preset("n_1e3_K256", 1'000, 3, 3, 2, 2, 0.90, 256),
            make_preset("n_1e4_K64", 10'000, 3, 3, 2, 2, 0.90, 64),
            make_preset("n_1e4_K128", 10'000, 3, 3, 2, 2, 0.90, 128),
            make_preset("n_1e4_K256", 10'000, 3, 3, 2, 2, 0.90, 256),
            make_preset("n_1e5_K64", 100'000, 3, 3, 2, 3, 0.90, 64),
            make_preset("n_1e5_K128", 100'000, 3, 3, 2, 3, 0.90, 128),
            make_preset("n_1e5_K256", 100'000, 3, 3, 2, 3, 0.90, 256),
            // 论文原始量级（保留备查，不在默认分组）
            make_preset("n_1e2", 100, 4, 3, 2, 2, 0.90),
            make_preset("n_1e6", 1'000'000, 4, 3, 2, 3, 0.90),
            make_preset("n_1e7", 10'000'000, 4, 3, 2, 3, 0.88),
            // tier：n=10^3 / 10^4 / 10^5
            make_preset("tier_ch4_1e3", 1'000, 4, 3, 2, 2, 0.90, 64),
            make_preset("tier_ch4_1e4", 10'000, 4, 3, 2, 2, 0.90, 256),
            make_preset("tier_ch4_1e5", 100'000, 4, 3, 2, 3, 0.90, 4096),
            make_preset("tier_lab", 10'000, 4, 3, 2, 2, 0.90),
            // K 固定三档（n=10^3 / 10^4 / 10^5）
            make_preset("K256_1e3", 1'000, 4, 3, 2, 2, 0.90, 16),
            make_preset("K4096_1e3", 1'000, 4, 3, 2, 2, 0.90, 64),
            make_preset("K65536_1e3", 1'000, 4, 3, 2, 2, 0.90, 256),
            make_preset("K256_1e4", 10'000, 4, 3, 2, 2, 0.90, 64),
            make_preset("K4096_1e4", 10'000, 4, 3, 2, 2, 0.90, 256),
            make_preset("K65536_1e4", 10'000, 4, 3, 2, 2, 0.90, 1024),
            make_preset("K256_1e5", 100'000, 4, 3, 2, 3, 0.90, 256),
            make_preset("K4096_1e5", 100'000, 4, 3, 2, 3, 0.90, 1024),
            make_preset("K65536_1e5", 100'000, 4, 3, 2, 3, 0.90, 4096),
            // K 指数对照（§6）
            make_preset("K_log2n", 1'000'000, 4, 2, 2, 3, 0.90),
            make_preset("K_log4n", 1'000'000, 4, 4, 2, 3, 0.90),
            // k-kick 层数（n=10^3 / 10^4 / 10^5）
            make_preset("k3_1e3", 1'000, 3, 3, 2, 2, 0.90, 64),
            make_preset("k4_1e3", 1'000, 4, 3, 2, 2, 0.90, 64),
            make_preset("k5_1e3", 1'000, 5, 3, 2, 2, 0.90, 64),
            make_preset("k3_1e4", 10'000, 3, 3, 2, 2, 0.90, 256),
            make_preset("k4_1e4", 10'000, 4, 3, 2, 2, 0.90, 256),
            make_preset("k5_1e4", 10'000, 5, 3, 2, 2, 0.90, 256),
            make_preset("k3_1e5", 100'000, 3, 3, 2, 3, 0.90, 4096),
            make_preset("k4_1e5", 100'000, 4, 3, 2, 3, 0.90, 4096),
            make_preset("k5_1e5", 100'000, 5, 3, 2, 3, 0.90, 4096),
            make_preset("k7", 10'000, 7, 3, 2, 2, 0.90, 256),
            make_preset("lf_high", 800'000, 4, 3, 2, 3, 0.95),
            make_preset("lf_low", 800'000, 4, 3, 2, 3, 0.75),
        };
    }

    TableParams preset_by_name(const std::string &name)
    {
        for (const auto &p : experiment_presets())
        {
            if (p.preset_id == name)
                return p;
        }
        return experiment_presets().front();
    }

    std::vector<TableParams> presets_for_group(ExperimentGroup group,
                                               Ch4Scale scale)
    {
        switch (group)
        {
        case ExperimentGroup::NMicro:
            return {preset_by_name("n_200"), preset_by_name("n_500"),
                    preset_by_name("n_1k"), preset_by_name("n_2k"),
                    preset_by_name("n_5k")};
        case ExperimentGroup::NScale:
            return {preset_by_name("n_1e3_K64"), preset_by_name("n_1e3_K128"),
                    preset_by_name("n_1e3_K256"), preset_by_name("n_1e4_K64"),
                    preset_by_name("n_1e4_K128"), preset_by_name("n_1e4_K256"),
                    preset_by_name("n_1e5_K64"), preset_by_name("n_1e5_K128"),
                    preset_by_name("n_1e5_K256")};
        case ExperimentGroup::KFixed:
            switch (scale)
            {
            case Ch4Scale::E3:
                return {preset_by_name("K256_1e3"), preset_by_name("K4096_1e3"),
                        preset_by_name("K65536_1e3")};
            case Ch4Scale::E5:
                return {preset_by_name("K256_1e5"), preset_by_name("K4096_1e5"),
                        preset_by_name("K65536_1e5")};
            default:
                return {preset_by_name("K256_1e4"), preset_by_name("K4096_1e4"),
                        preset_by_name("K65536_1e4")};
            }
        case ExperimentGroup::KKickDepth:
            switch (scale)
            {
            case Ch4Scale::E3:
                return {preset_by_name("k3_1e3"), preset_by_name("k4_1e3"),
                        preset_by_name("k5_1e3")};
            case Ch4Scale::E5:
                return {preset_by_name("k3_1e5"), preset_by_name("k4_1e5"),
                        preset_by_name("k5_1e5")};
            default:
                return {preset_by_name("k3_1e4"), preset_by_name("k4_1e4"),
                        preset_by_name("k5_1e4")};
            }
        case ExperimentGroup::TierCubby:
            switch (scale)
            {
            case Ch4Scale::E3:
                return {preset_by_name("tier_ch4_1e3")};
            case Ch4Scale::E5:
                return {preset_by_name("tier_ch4_1e5")};
            default:
                return {preset_by_name("tier_ch4_1e4")};
            }
        case ExperimentGroup::All:
        default:
            return {};
        }
    }

    Ch4Scale ch4_scale_from_string(const std::string &name)
    {
        if (name == "1e3" || name == "1E3" || name == "1000" || name == "e3")
            return Ch4Scale::E3;
        if (name == "1e5" || name == "1E5" || name == "100000" || name == "e5")
            return Ch4Scale::E5;
        if (name == "1e4" || name == "1E4" || name == "10000" || name == "e4")
            return Ch4Scale::E4;
        return Ch4Scale::E4;
    }

    const char *ch4_scale_name(Ch4Scale scale)
    {
        switch (scale)
        {
        case Ch4Scale::E3:
            return "1e3";
        case Ch4Scale::E5:
            return "1e5";
        default:
            return "1e4";
        }
    }

    const char *ch4_scale_label(Ch4Scale scale)
    {
        switch (scale)
        {
        case Ch4Scale::E3:
            return "10^3";
        case Ch4Scale::E5:
            return "10^5";
        default:
            return "10^4";
        }
    }

    std::vector<Ch4Scale> ch4_all_scales()
    {
        return {Ch4Scale::E3, Ch4Scale::E4, Ch4Scale::E5};
    }

    const char *experiment_group_name(ExperimentGroup group)
    {
        switch (group)
        {
        case ExperimentGroup::NMicro:
            return "micro";
        case ExperimentGroup::NScale:
            return "n";
        case ExperimentGroup::KFixed:
            return "Kfixed";
        case ExperimentGroup::KKickDepth:
            return "k";
        case ExperimentGroup::TierCubby:
            return "tier";
        default:
            return "all";
        }
    }

    ExperimentGroup experiment_group_from_string(const std::string &name)
    {
        if (name == "micro" || name == "smoke" || name == "tiny")
            return ExperimentGroup::NMicro;
        if (name == "n" || name == "N" || name == "nscale")
            return ExperimentGroup::NScale;
        if (name == "Kfixed" || name == "K" || name == "Kfix")
            return ExperimentGroup::KFixed;
        if (name == "k" || name == "kkick")
            return ExperimentGroup::KKickDepth;
        if (name == "tier" || name == "cubby")
            return ExperimentGroup::TierCubby;
        if (name == "all")
            return ExperimentGroup::All;
        return ExperimentGroup::All;
    }

} // namespace otsh