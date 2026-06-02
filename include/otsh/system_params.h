#pragma once

#include "config.h"

#include <cstdint>
#include <string>
#include <vector>

namespace otsh
{

    // 由《系统重构方案 v1》§6 派生的运行时结构参数（N/K/tier/k-kick/bin 等）。
    struct DerivedParams
    {
        uint64_t n_hint = 1'000'000;
        uint64_t N = 0;
        uint64_t K = 0;
        int k_kick = 4;
        int k_polylog_exp = 3; // K ≈ (log2 N)^k_polylog_exp
        int node_max_bits = 2; // MiniArray 叶子 NODE_MAX_BITS 常数 c
        int fanout = 8;
        int max_tier = 3;
        bool tier_use_canon = false;
        int tier_target_divisor = 1;
        double load_factor = 0.90;
        std::string preset_id;
    };

    // §6.2 实验规范：每层目标数量 t_j 与 cubby 容量 r_j（j 从 1 起，1-tiered 为 tail）。
    struct TierCanonSpec
    {
        uint64_t t = 1;
        size_t r = 16;
    };

    TierCanonSpec tier_canon_spec(int j, uint64_t n_hint, uint64_t K);

    // 从 TableParams 计算 N、K、fanout、max_tier 等。
    DerivedParams derive_params(const TableParams &p);

    // 将派生结果写回 TableParams（保留种子等用户字段）。
    TableParams apply_derived(const TableParams &base, const DerivedParams &d);

    // 论文第四章实验：关闭 resize，启用 rebuild_up/down（out.log 口径）。
    TableParams apply_ch4_mode(TableParams p);

    // 实验用参数组（§6.1–6.3）：基准 + 多组对照。
    std::vector<TableParams> experiment_presets();

    // 实验分组（对齐 experiments/out.log 四组表）。
    enum class ExperimentGroup
    {
        All,        // n + Kfixed + k + tier
        NScale,     // 表1：n ∈ {10^3,10^4,10^5}，K=log^3 n，k=4
        NMicro,     // 渐进验证：n ∈ {200…5000}，全量填充，runs=1
        KFixed,     // 表2：K 三档，n 由 Ch4Scale 决定
        KKickDepth, // 表3：k ∈ {3,4,5}，n/K 由 Ch4Scale 决定
        TierCubby,  // 表4：四档 workload，n/K 由 Ch4Scale 决定
    };

    // K/k/tier 固定 n 档位：10^3 / 10^4 / 10^5。
    enum class Ch4Scale
    {
        E3 = 1'000,
        E4 = 10'000,
        E5 = 100'000,
    };

    Ch4Scale ch4_scale_from_string(const std::string &name);
    const char *ch4_scale_name(Ch4Scale scale);  // "1e3" … "1e5"
    const char *ch4_scale_label(Ch4Scale scale); // "10^3" … "10^5"
    std::vector<Ch4Scale> ch4_all_scales();

    // 按分组返回预设列表（NScale/NMicro 忽略 scale）。
    std::vector<TableParams> presets_for_group(ExperimentGroup group,
                                               Ch4Scale scale = Ch4Scale::E4);

    // 解析 CLI 分组名（n / Kfixed / k / tier / all）。
    ExperimentGroup experiment_group_from_string(const std::string &name);

    const char *experiment_group_name(ExperimentGroup group);

    // 按名称取预设；未知名返回基准组。
    TableParams preset_by_name(const std::string &name);

    // k-kick 第 i 层名义 bin 宽 s_i：s_0=log^3 n，s_i=(log^(i) n)^6（§4.2 / §6.3）。
    size_t kkick_bin_size(int depth_i, uint64_t K, uint64_t n_hint);

    // j-tiered cubby 目标数量 t_j（§3.1–3.2 / §6.2；j 从 1 起）。
    uint64_t tier_target_count(int j, uint64_t n_hint, uint64_t K, bool use_canon,
                               int divisor = 1);
    // j-tiered cubby 容量 r_j（§3.1；j 从 1 起）。
    size_t tier_cubby_capacity(uint64_t K, uint64_t n_hint, int tier,
                               bool use_canon = true);

} // namespace otsh
