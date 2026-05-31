#pragma once

#include <cstdint>
#include <string>

namespace otsh {

struct TableParams {
  uint64_t n = 10000;        // 规模参数（N 会取 2^p，使 N <= n <= 2N）
  int k = 4;                 // k-kick 最大深度（§6.1：3~7，默认 4）
  int k_polylog_exp = 3;     // K ≈ (log2 N)^k_polylog_exp（2~4，默认 3）
  uint64_t K_override = 0;   // >0 时固定 Facility 规模 K（论文 Ch4：256/4096/65536）
  int node_max_bits = 2;     // MiniArray 叶子常数 c（§6.1：1~3）
  int fanout_override = 0;   // 0=自动 ⌊log n / (2 log log n)⌋
  int max_tier = 3;          // 最高 j-tiered 层级（§3.1 从 1 起，默认 3-tiered）
  bool tier_use_canon = false; // false=§3.1 论文公式；true=§6.2 实验规范表
  int tier_target_divisor = 1; // tier_use_canon=false 时对公式 t_j 的除数
  // §6 验证模式：暂停未完成子系统，只测 CRUD / k-kick / rebuild 指标
  bool validation_mode = false;
  bool enable_resize = true;       // false=不做双表迁移
  bool enable_rebuild_up = true;   // false=仅测 rebuild_down（合并）
  bool enable_rebuild_down = true; // false=跳过 cubby 合并
  double load_factor = 0.90;
  std::string preset_id;     // 实验预设名（experiment_presets / preset_by_name）

  // 用于可逆置换 pi 以及其他哈希/随机选择的种子
  uint64_t seed1 = 0;
  uint64_t seed2 = 0;
  uint64_t seed3 = 0;
};

} // namespace otsh
