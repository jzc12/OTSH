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
// 以上函数均要求调用方已处于合适的事务/锁上下文中，与写路径一致。
// ---------------------------------------------------------------------------

// 检查给定名字的数据表是否已存在于当前数据库
static bool table_exists(Db &db, const std::string &name) {
  auto cnt =
      db.select_one_u64("SELECT COUNT(*) FROM information_schema.tables WHERE "
                        "table_schema=DATABASE() AND table_name='" +
                        name + "'");
  return cnt.has_value() && *cnt > 0;
}

// 从 ht_meta 读出一行并加行锁（FOR UPDATE），供后续更新扩容相关字段
ResizeMeta HashTableDb::load_resize_meta_for_update() {
  // 锁定元数据行，与 store_resize_meta 成对使用，避免并发读写扩容状态
  auto rows =
      db_.query("SELECT total_bins,is_resizing,new_total_bins,resize_progress,"
                "resize_started_at_ms,resize_migrated_keys,resize_last_step_ms,"
                "resize_elapsed_ms,resize_finished_total_ms "
                "FROM ht_meta WHERE id=1 FOR UPDATE");
  if (rows.empty() || rows[0].size() < 9)
    throw std::runtime_error("ht_meta missing resize columns");
  ResizeMeta m;
  m.total_bins = std::stoull(rows[0][0]);
  m.is_resizing = (std::stoull(rows[0][1]) != 0);
  m.new_total_bins = std::stoull(rows[0][2]);
  m.resize_progress = std::stoull(rows[0][3]);
  m.started_at_ms = std::stoull(rows[0][4]);
  m.migrated_keys = std::stoull(rows[0][5]);
  m.last_step_ms = std::stoull(rows[0][6]);
  m.elapsed_ms = std::stoull(rows[0][7]);
  m.finished_total_ms = std::stoull(rows[0][8]);
  return m;
}

std::optional<ResizeMeta> HashTableDb::read_resize_state_snapshot() const {
  try {
    auto rows =
        db_.query("SELECT total_bins,is_resizing,new_total_bins,resize_progress,"
                  "resize_started_at_ms,resize_migrated_keys,resize_last_step_ms,"
                  "resize_elapsed_ms,resize_finished_total_ms "
                  "FROM ht_meta WHERE id=1");
    if (rows.empty() || rows[0].size() < 9)
      return std::nullopt;
    ResizeMeta m;
    m.total_bins = std::stoull(rows[0][0]);
    m.is_resizing = (std::stoull(rows[0][1]) != 0);
    m.new_total_bins = std::stoull(rows[0][2]);
    m.resize_progress = std::stoull(rows[0][3]);
    m.started_at_ms = std::stoull(rows[0][4]);
    m.migrated_keys = std::stoull(rows[0][5]);
    m.last_step_ms = std::stoull(rows[0][6]);
    m.elapsed_ms = std::stoull(rows[0][7]);
    m.finished_total_ms = std::stoull(rows[0][8]);
    return m;
  } catch (...) {
    return std::nullopt;
  }
}

// 存储扩容元数据
void HashTableDb::store_resize_meta(const ResizeMeta &m) {
  db_.exec("UPDATE ht_meta SET total_bins=" + std::to_string(m.total_bins) +
           ", is_resizing=" + std::to_string(m.is_resizing ? 1 : 0) +
           ", new_total_bins=" + std::to_string(m.new_total_bins) +
           ", resize_progress=" + std::to_string(m.resize_progress) +
           ", resize_started_at_ms=" + std::to_string(m.started_at_ms) +
           ", resize_migrated_keys=" + std::to_string(m.migrated_keys) +
           ", resize_last_step_ms=" + std::to_string(m.last_step_ms) +
           ", resize_elapsed_ms=" + std::to_string(m.elapsed_ms) +
           ", resize_finished_total_ms=" + std::to_string(m.finished_total_ms) +
           " WHERE id=1");
}

static uint64_t now_ms() {
  using namespace std::chrono;
  return (uint64_t)duration_cast<milliseconds>(
             system_clock::now().time_since_epoch())
      .count();
}

// 若当前负载达到 trigger_load 且未在扩容中，则把元数据标为“正在扩容”并
// 记录目标桶数 new_total_bins = 2 * total_bins；不在这里建表，建表在首步
// process_resize_step 中完成。必须在写路径同一事务中调用。
void HashTableDb::maybe_start_resize(const TableParams &p,
                                     double trigger_load) {
  auto m = load_resize_meta_for_update();
  if (m.is_resizing)
    return;

  auto used = db_.select_one_u64(std::string("SELECT COUNT(*) FROM ") +
                                 kActiveTable + " WHERE fp<>0");
  double load =
      (p.capacity_slots ? (double)(used.value_or(0)) / (double)p.capacity_slots
                        : 0.0);
  if (load < trigger_load)
    return;

  m.is_resizing = true;
  m.total_bins = p.total_bins;
  m.new_total_bins = p.total_bins * 2;
  m.resize_progress = 0;
  m.started_at_ms = now_ms();
  m.migrated_keys = 0;
  m.last_step_ms = 0;
  m.elapsed_ms = 0;
  m.finished_total_ms = 0;
  store_resize_meta(m);
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
  auto m = load_resize_meta_for_update();
  if (!m.is_resizing)
    return;

  TableParams np = p; // np：按新桶数重算后的参数，用于在 next 表里定位槽位
  np.total_bins = m.new_total_bins;
  np.capacity_slots = np.total_bins * np.bin_size;
  ensure_next_table(db_, np.capacity_slots);

  if (m.resize_progress >= m.total_bins) {
    finish_resize(m);
    return;
  }

  const uint64_t t0 = now_ms();

  // 当前要迁移的 bin 下标；对应主表中一段连续的 idx 区间
  uint64_t bin_idx = m.resize_progress;
  uint64_t start_idx = bin_idx * p.bin_size;
  uint64_t end_idx = start_idx + p.bin_size - 1;

  // 锁住该段内 fp≠0 的行，防止迁移时其它事务改同一槽
  auto rows =
      db_.query(std::string("SELECT idx, `key`, fp FROM ") + kActiveTable +
                " WHERE idx BETWEEN " + std::to_string(start_idx) + " AND " +
                std::to_string(end_idx) + " AND fp<>0 FOR UPDATE");

  for (auto &r : rows) {
    if (r.size() < 3)
      continue;
    uint64_t idx = std::stoull(r[0]);
    if (r[1].empty())
      continue;
    uint64_t key = std::stoull(r[1]);
    uint16_t fp = static_cast<uint16_t>(std::stoul(r[2]));

    // 按新桶数把 key 插入 next 表；fp 在插入路径里会按 key 重算，这里仅占位
    (void)fp;
    auto ins = insert_key_into_table(kNextTable, np, key, false, false);
    if (!ins.ok)
      throw std::runtime_error("resize migrate insert failed: " + ins.error);

    db_.update_slot(kActiveTable, idx, std::nullopt, 0);
  }

  const uint64_t t1 = now_ms();
  m.last_step_ms = (t1 >= t0) ? (t1 - t0) : 0;
  m.migrated_keys += static_cast<uint64_t>(rows.size());
  if (m.started_at_ms != 0 && t1 >= m.started_at_ms)
    m.elapsed_ms = t1 - m.started_at_ms;

  m.resize_progress++;
  store_resize_meta(m);
}

// 双表 RENAME 原子换名：主表 -> 备份名，next -> 主表，再删备份。MySQL 在同一条
// RENAME 里交换，避免中间态无主表。随后把扩容标记清掉、total_bins
// 更新为新的规模。
void HashTableDb::finish_resize(const ResizeMeta &m) {
  (void)m;
  db_.exec(std::string("DROP TABLE IF EXISTS ") + kOldTable);
  db_.exec(std::string("RENAME TABLE ") + kActiveTable + " TO " + kOldTable +
           ", " + kNextTable + " TO " + kActiveTable);
  db_.exec(std::string("DROP TABLE IF EXISTS ") + kOldTable);

  ResizeMeta nm = m;
  nm.is_resizing = false;
  nm.total_bins = m.new_total_bins;
  nm.new_total_bins = 0;
  nm.resize_progress = 0;
  const uint64_t t = now_ms();
  if (nm.started_at_ms != 0 && t >= nm.started_at_ms) {
    nm.elapsed_ms = t - nm.started_at_ms;
    nm.finished_total_ms = nm.elapsed_ms;
  }
  store_resize_meta(nm);
}

} // namespace otsh
