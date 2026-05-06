#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace otsh {

struct StorageOpenOptions {
  // mysql: 连接参数
  std::string mysql_host;
  uint16_t mysql_port = 3306;
  std::string mysql_user;
  std::string mysql_password;
  std::string mysql_database;
  std::string mysql_table;
};

struct StorageResult {
  bool ok = false;
  std::string error;
};

// 单条样本（批量 flush 到 MySQL）。
struct SqlMetricRow {
  int64_t snapshot_id = 0;
  std::string operation_type;
  int probe_count = 0;
  int kick_count = 0;
  int64_t latency_ns = 0;
  int cubby_tier = -1;
};

// 设施行
struct SqlFacilityRow {
  int id = 0;
  int tail_cubby_id = 0;
};

//  cubby 行
struct SqlCubbyRow {
  int id = 0;
  int facility_id = 0;
  int tier = 0;
  int capacity = 0;
  int size = 0;
  bool is_tail = false;
};

// 槽行
struct SqlSlotRow {
  int cubby_id = 0;
  int slot_index = 0;
  bool occupied = false;
  uint64_t key_hash = 0;
  int probe_length = 0;
};

// 层统计行
struct SqlTierStatRow {
  int facility_id = 0;
  int tier = 0;
  int cubby_count = 0;
};

class IStorage {
public:
  virtual ~IStorage() = default;

  // 打开/初始化底层存储。
  virtual StorageResult open(const StorageOpenOptions &opt) = 0;

  // 清空所有数据（用于 /api/init 的“重新初始化”语义）。
  virtual StorageResult clear() = 0;

  // 遍历所有 key（用于启动/重建内存结构）。
  virtual StorageResult
  for_each_key(const std::function<void(uint64_t)> &cb) = 0;

  // 写入操作（幂等）。
  virtual StorageResult put(uint64_t key) = 0;
  virtual StorageResult erase(uint64_t key) = 0;

  // --- 分析库表（默认无操作；MySQL 实现建表并写入）---
  virtual int64_t analytics_create_snapshot(const std::string &snapshot_tag,
                                            uint64_t n, uint64_t N,
                                            uint64_t K) {
    (void)snapshot_tag;
    (void)n;
    (void)N;
    (void)K;
    return -1;
  }

  virtual void analytics_enqueue_metric(const SqlMetricRow &row) { (void)row; }

  virtual void analytics_flush_metrics(bool force) { (void)force; }

  virtual StorageResult
  analytics_replace_structure(int64_t snapshot_id,
                              const std::vector<SqlFacilityRow> &facilities,
                              const std::vector<SqlCubbyRow> &cubbies,
                              const std::vector<SqlSlotRow> &slots,
                              const std::vector<SqlTierStatRow> &tier_stats) {
    (void)snapshot_id;
    (void)facilities;
    (void)cubbies;
    (void)slots;
    (void)tier_stats;
    return {false, "analytics_not_supported"};
  }

  virtual std::string analytics_list_snapshots_json() { return "[]"; }

  virtual std::string analytics_summary_json(int64_t snapshot_id) {
    (void)snapshot_id;
    return "{}";
  }

  // 自动化测试/批跑：追加一行 JSON（无 MySQL 时由测试程序写 txt）。
  virtual StorageResult append_test_session_log(const std::string &json_line) {
    (void)json_line;
    return {false, "not_supported"};
  }
};

std::unique_ptr<IStorage> make_storage_mysql();

} // namespace otsh
