#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace otsh {

// 极简指标采集：用于后续 OTSH 复杂度/空间验收。
// 目前只记录可稳定定义的计数与均值；随着模块落地再逐步细化。
class Metrics {
public:
  struct LatencySummary {
    uint64_t count = 0;                    // 样本数
    uint64_t total_ns = 0;                 // 总耗时（纳秒）
    uint64_t max_ns = 0;                   // 最大耗时（纳秒）
    uint64_t p50_ns = 0;                   // 50% 分位数（纳秒） 中位数
    uint64_t p99_ns = 0;                   // 99% 分位数（纳秒）
    std::array<uint64_t, 32> buckets = {}; // 耗时分布（纳秒）
  };

  struct EventSummary {
    uint64_t rebuild_down = 0;  // 重建下采样次数
    uint64_t rebuild_up = 0;    // 重建上采样次数
    uint64_t resize_start = 0;  // 扩容开始次数
    uint64_t resize_finish = 0; // 扩容完成次数
  };

  struct Snapshot {
    uint64_t ops_init = 0;   // 初始化次数
    uint64_t ops_insert = 0; // 插入次数
    uint64_t ops_query = 0;  // 查询次数
    uint64_t ops_delete = 0; // 删除次数

    uint64_t insert_moved_total = 0; // 插入移动次数
    uint64_t insert_moved_max = 0;   // 插入移动最大次数
    uint64_t delete_moved_total = 0; // 删除移动次数
    uint64_t delete_moved_max = 0;   // 删除移动最大次数

    uint64_t router_steps_total = 0; // 路由步数总和
    uint64_t router_steps_max = 0;   // 路由步数最大值

    uint64_t meta_bits_total = 0; // 元数据位数总和
    uint64_t meta_bits_max = 0;   // 元数据位数最大值

    LatencySummary ht_init;   // 初始化耗时
    LatencySummary ht_insert; // 插入耗时
    LatencySummary ht_query;  // 查询耗时
    LatencySummary ht_delete; // 删除耗时

    EventSummary events; // 事件统计
  };

  void on_init();
  void on_insert(uint64_t moved, uint64_t router_steps, uint64_t meta_bits);
  void on_query(uint64_t router_steps);
  void on_delete(uint64_t moved, uint64_t router_steps, uint64_t meta_bits);

  void on_ht_init_ns(uint64_t ns);
  void on_ht_insert_ns(uint64_t ns);
  void on_ht_query_ns(uint64_t ns);
  void on_ht_delete_ns(uint64_t ns);

  void on_rebuild_down();
  void on_rebuild_up();
  void on_resize_start();
  void on_resize_finish();

  Snapshot snapshot() const;

private:
  static void atomic_max(std::atomic<uint64_t> &x, uint64_t v);
  static size_t bucket_idx(uint64_t ns);
  static uint64_t
  estimate_quantile_from_buckets(const std::array<uint64_t, 32> &b,
                                 uint64_t count, double q);

  std::atomic<uint64_t> ops_init_{0};
  std::atomic<uint64_t> ops_insert_{0};
  std::atomic<uint64_t> ops_query_{0};
  std::atomic<uint64_t> ops_delete_{0};

  std::atomic<uint64_t> insert_moved_total_{0};
  std::atomic<uint64_t> insert_moved_max_{0};
  std::atomic<uint64_t> delete_moved_total_{0};
  std::atomic<uint64_t> delete_moved_max_{0};

  std::atomic<uint64_t> router_steps_total_{0};
  std::atomic<uint64_t> router_steps_max_{0};

  std::atomic<uint64_t> meta_bits_total_{0};
  std::atomic<uint64_t> meta_bits_max_{0};

  // HT-only latency histograms (ns), log2 buckets.
  std::atomic<uint64_t> ht_init_count_{0}, ht_init_total_{0}, ht_init_max_{0};
  std::array<std::atomic<uint64_t>, 32> ht_init_b_{};

  std::atomic<uint64_t> ht_insert_count_{0}, ht_insert_total_{0},
      ht_insert_max_{0};
  std::array<std::atomic<uint64_t>, 32> ht_insert_b_{};

  std::atomic<uint64_t> ht_query_count_{0}, ht_query_total_{0},
      ht_query_max_{0};
  std::array<std::atomic<uint64_t>, 32> ht_query_b_{};

  std::atomic<uint64_t> ht_delete_count_{0}, ht_delete_total_{0},
      ht_delete_max_{0};
  std::array<std::atomic<uint64_t>, 32> ht_delete_b_{};

  // Events
  std::atomic<uint64_t> ev_rebuild_down_{0};
  std::atomic<uint64_t> ev_rebuild_up_{0};
  std::atomic<uint64_t> ev_resize_start_{0};
  std::atomic<uint64_t> ev_resize_finish_{0};
};

// 进程级 metrics（后续可改为注入依赖）。
Metrics &global_metrics();

} // namespace otsh
