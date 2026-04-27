#pragma once

#include "config.h"
#include "db.h"
#include "hash.h"
#include "resize.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace otsh {

struct OpResult {
  bool ok = false;
  uint64_t probes = 0; // SQL 范围扫描次数（mini-bin + fallback）
  std::string error;
};

struct Stats {
  uint64_t used_slots = 0;
  uint64_t fallback_used = 0;
};

struct BinStats {
  uint64_t bin = 0;
  uint32_t used_slots = 0;
  uint32_t fallback_used = 0;
};

struct KickStep {
  uint64_t idx = 0;
  uint16_t from_fp = 0;
  uint16_t to_fp = 0;
  int depth = 0;
  std::string action; // "place" | "kick" | "fallback_add" |
                      // "fallback_remove"（动作类型）
};

struct InsertResult : OpResult {
  uint64_t bin = 0;
  uint64_t mini = 0;
  uint16_t fp = 0;
  std::vector<KickStep> trace;
};

class HashTableDb {
public:
  explicit HashTableDb(Db &db);

  TableParams load_or_init_meta(uint64_t n, int k, double load_factor,
                                std::optional<uint64_t> seed1,
                                std::optional<uint64_t> seed2,
                                std::optional<uint64_t> seed3);

  OpResult init_table(const TableParams &p);
  InsertResult insert_key(const TableParams &p, uint64_t key, bool with_trace);
  OpResult find_key(const TableParams &p, uint64_t key);
  OpResult erase_key(const TableParams &p, uint64_t key);

  // 尽力而为：批量插入前，预检查预计负载并提前触发扩容，
  // 使该批次可以直接写入 `hash_table_next`。
  // `projected_inserts` 为估算值；包含重复 key 也没问题。
  void prepare_batch_insert(const TableParams &p, uint64_t projected_inserts);

  // 同步扩容：当负载达到触发阈值时，暂停调用方当前工作，
  // 执行扩容步骤直到完成后再返回。
  // 这对应简单的单线程控制流：
  //   insert... -> 检测阈值 -> resize_to_completion() -> 继续
  //   insert...
  void resize_to_completion(const TableParams &p);

  // Kick 深度直方图（仅内存；init/rebuild 时清空）。
  void reset_kick_hist(int k);
  std::vector<uint64_t> kick_hist_snapshot() const;

  Stats stats(const TableParams &p);
  std::vector<BinStats> bin_stats(const TableParams &p, uint64_t bin_start,
                                  uint64_t bin_count);

  // 用于 Canvas 网格可视化的快照
  // 返回行格式：[idx, fp, reserved]
  std::vector<std::array<uint64_t, 3>>
  snapshot_bins(const TableParams &p, uint64_t bin_start, uint64_t bin_count);

  // 扩容状态（仅进程内，用于前端展示进度；不写入数据库）。
  std::optional<ResizeMeta> read_resize_state_snapshot() const;

private:
  Db &db_;

  // --- 扩容状态（仅进程内；不持久化）---
  mutable std::mutex resize_mu_;
  ResizeMeta resize_state_;

  // depth -> count（0..k）。另外保留一个桶用于 depth==k+1（fallback 标记）。
  // 避免使用 std::vector<std::atomic<...>>，因为 atomic 不可移动。
  std::unique_ptr<std::atomic<uint64_t>[]> kick_hist_;
  size_t kick_hist_size_ = 0;

  static uint64_t ilog_iter(uint64_t n, int iter);
  static void derive_sizes(TableParams &p);

  HashFunc make_hash(const TableParams &p) const;

  uint64_t bin_offset(const TableParams &p, uint64_t b) const;
  uint64_t mini_range_start(const TableParams &p, uint64_t b, uint64_t m) const;
  uint64_t mini_range_end(const TableParams &p, uint64_t b, uint64_t m) const;
  uint64_t fallback_start(const TableParams &p, uint64_t b) const;
  uint64_t fallback_end(const TableParams &p, uint64_t b) const;

  // --- 扩容状态（仅进程内；不持久化）---
  void maybe_start_resize(const TableParams &p, double trigger_load);
  void process_resize_step(const TableParams &p);
  void finish_resize(const TableParams &p, const ResizeMeta &m);

  // 事务性操作
  std::optional<uint64_t> claim_empty_slot_in_range(const std::string &table,
                                                    uint64_t start,
                                                    uint64_t end,
                                                    uint64_t *probes);
  bool insert_into_fallback(const std::string &table, const TableParams &p,
                            uint64_t key, uint16_t fp, uint64_t b,
                            std::vector<KickStep> *trace, uint64_t *probes);
  void erase_from_fallback_if_present(const TableParams &p, uint64_t key,
                                      uint16_t fp, uint64_t b,
                                      std::vector<KickStep> *trace);

  bool insert_with_kick(const std::string &table, const TableParams &p,
                        const HashFunc &hf, uint64_t key, uint16_t fp,
                        uint64_t depth, std::vector<KickStep> *trace,
                        uint64_t *probes);

  // 面向指定物理表名的插入实现。
  // 若 `manage_txn` 为 false，则调用方必须已处于事务中。
  InsertResult insert_key_into_table(const std::string &table,
                                     const TableParams &p, uint64_t key,
                                     bool with_trace, bool manage_txn);
};

} // namespace otsh
