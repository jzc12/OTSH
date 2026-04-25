#include "ht.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>

namespace otsh {

// ---------------------------------------------------------------------------
// 哈希表 MySQL 持久化层：表扩容（resize）流程
//
// 触发：负载超过阈值时 maybe_start_resize 把元数据标为 is_resizing，并记下
//      new_total_bins = 旧 total_bins * 2。
// 步进：process_resize_step 每次在事务中迁移一个旧 bin 的已占用槽到
//      hash_table_next，进度记在 resize_progress。
// 完成：当所有旧 bin 迁完，finish_resize 用 RENAME 交换主表与 next 表，
//      丢弃中间命名，并尝试把 ht_meta.n 同步为翻倍。
//
// 扩容进度用本地变量管理（不写入数据库）。
// ---------------------------------------------------------------------------

// 检查给定名字的数据表是否已存在于当前数据库
static bool table_exists(Db &db, const std::string &name) {
  auto cnt =
      db.select_one_u64("SELECT COUNT(*) FROM information_schema.tables WHERE "
                        "table_schema=DATABASE() AND table_name='" +
                        name + "'");
  return cnt.has_value() && *cnt > 0;
}

static uint64_t now_ms() {
  using namespace std::chrono;
  return (uint64_t)duration_cast<milliseconds>(
             system_clock::now().time_since_epoch())
      .count();
}

ResizeMeta HashTableDb::load_resize_meta_for_update() {
  // In-process state only.
  std::lock_guard<std::mutex> lk(resize_mu_);
  return resize_state_;
}

std::optional<ResizeMeta> HashTableDb::read_resize_state_snapshot() const {
  try {
    std::lock_guard<std::mutex> lk(resize_mu_);
    if (!resize_state_.is_resizing)
      return std::nullopt;
    return resize_state_;
  } catch (...) {
    return std::nullopt;
  }
}

void HashTableDb::store_resize_meta(const ResizeMeta &m) {
  std::lock_guard<std::mutex> lk(resize_mu_);
  resize_state_ = m;
}

void HashTableDb::maybe_start_resize(const TableParams &p,
                                     double trigger_load) {
  // `resize_mu_` must be held by caller.
  if (resize_state_.is_resizing)
    return;

  auto used = db_.select_one_u64(std::string("SELECT COUNT(*) FROM ") +
                                 kActiveTable + " WHERE fp<>0");
  const double load =
      p.capacity_slots ? (double)used.value_or(0) / (double)p.capacity_slots
                       : 0.0;
  if (load < trigger_load)
    return;

  resize_state_ = ResizeMeta{};
  resize_state_.is_resizing = true;
  resize_state_.total_bins = p.total_bins;
  resize_state_.new_total_bins = std::max<uint64_t>(1, p.total_bins * 2);
  resize_state_.resize_progress = 0;
  resize_state_.started_at_ms = now_ms();
}

// 若 hash_table_next 不存在则创建，并按 batch 大小批量插入空槽（key NULL,
// fp=0）， 行数 = new_total_bins * bin_size（由调用方通过 capacity_slots 传入）
static void ensure_next_table(Db &db, uint64_t capacity_slots) {
  if (table_exists(db, kNextTable))
    return;

  db.exec(std::string("CREATE TABLE ") + kNextTable +
          " ( idx BIGINT PRIMARY KEY,"
          " `key` BIGINT UNSIGNED NULL,"
          " fp SMALLINT UNSIGNED NOT NULL,"
          " UNIQUE KEY uk_key(`key`) )");

  const uint64_t batch = 5000;
  for (uint64_t base = 0; base < capacity_slots; base += batch) {
    uint64_t end = std::min<uint64_t>(capacity_slots, base + batch);
    std::string sql =
        std::string("INSERT INTO ") + kNextTable + "(idx,`key`,fp) VALUES ";
    for (uint64_t i = base; i < end; i++) {
      sql += "(" + std::to_string(i) + ",NULL,0)";
      if (i + 1 != end)
        sql += ",";
    }
    db.exec(sql);
  }
}

// 扩容主循环中的一步：迁移一个旧桶（bin）内所有已占用槽到 next 表，并推进
// resize_progress。全部 bin 处理完后会调用 finish_resize。须与写操作同事务。
void HashTableDb::process_resize_step(const TableParams &p) {
  // `resize_mu_` must be held by caller.
  if (!resize_state_.is_resizing)
    return;
  if (resize_state_.resize_progress >= resize_state_.total_bins)
    return;

  TableParams np = p;
  np.total_bins = resize_state_.new_total_bins;
  np.capacity_slots = np.total_bins * np.bin_size;
  ensure_next_table(db_, np.capacity_slots);

  const uint64_t bin_idx = resize_state_.resize_progress;
  const uint64_t start_idx = bin_idx * p.bin_size;
  const uint64_t end_idx = start_idx + p.bin_size - 1;

  const uint64_t t0 = now_ms();

  // Migrate one bin in a short transaction.
  db_.begin();
  auto rows =
      db_.query(std::string("SELECT `key` FROM ") + kActiveTable +
                " WHERE idx BETWEEN " + std::to_string(start_idx) + " AND " +
                std::to_string(end_idx) + " AND fp<>0 FOR UPDATE");
  uint64_t migrated = 0;
  for (auto &r : rows) {
    if (r.empty() || r[0].empty())
      continue;
    uint64_t key = std::stoull(r[0]);
    auto ins = insert_key_into_table(kNextTable, np, key, false, false);
    if (!ins.ok) {
      db_.rollback();
      throw std::runtime_error("resize migrate insert failed: " + ins.error);
    }
    migrated++;
  }
  db_.commit();

  const uint64_t t1 = now_ms();
  resize_state_.migrated_keys += migrated;
  resize_state_.last_step_ms = (t1 >= t0) ? (t1 - t0) : 0;
  if (resize_state_.started_at_ms) {
    resize_state_.elapsed_ms =
        (t1 >= resize_state_.started_at_ms) ? (t1 - resize_state_.started_at_ms)
                                            : 0;
  }

  resize_state_.resize_progress++;
}

// 双表 RENAME 原子换名：主表 -> 备份名，next -> 主表，再删备份。MySQL 在同一条
// RENAME 里交换，避免中间态无主表。随后把扩容标记清掉、total_bins
// 更新为新的规模。
void HashTableDb::finish_resize(const TableParams &p, const ResizeMeta &m) {
  // `resize_mu_` must be held by caller.
  if (!m.is_resizing)
    return;

  db_.exec(std::string("DROP TABLE IF EXISTS ") + kOldTable);
  db_.exec(std::string("RENAME TABLE ") + kActiveTable + " TO " + kOldTable +
           ", " + kNextTable + " TO " + kActiveTable);
  db_.exec(std::string("DROP TABLE IF EXISTS ") + kOldTable);

  // Persist new scale: total_bins doubles; n tracks nominal slot capacity
  // (total_bins * bin_size, bin layout unchanged).
  const uint64_t new_n = m.new_total_bins * p.bin_size;
  db_.exec("UPDATE ht_meta SET n=" + std::to_string(new_n) + ", total_bins=" +
           std::to_string(m.new_total_bins) + " WHERE id=1");

  uint64_t t = now_ms();
  if (resize_state_.started_at_ms) {
    resize_state_.finished_total_ms =
        (t >= resize_state_.started_at_ms) ? (t - resize_state_.started_at_ms)
                                           : 0;
  }
}

} // namespace otsh
