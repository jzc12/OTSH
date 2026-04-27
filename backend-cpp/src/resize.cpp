#include "ht.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>

namespace otsh {

// ---------------------------------------------------------------------------
// 在线扩容（仅进程内维护进度，DB 只持久化最终规模）
//
// 目标：当负载超过阈值时，把 `hash_table` 扩展为 2 倍 bins：
// - 物理上创建 `hash_table_next`（容量 = new_total_bins * bin_size）
// - 以 bin 为单位分步迁移（每步一个短事务，便于前端看到 progress）
// - 迁移完成后用 RENAME TABLE 原子换表
// - 最后把最终规模写回 ht_meta（只更新 n 与 total_bins）
//
// 说明：扩容进度/耗时统计全部保存在进程内的 `resize_state_`，不写数据库。
// TODO：缩容
// ---------------------------------------------------------------------------

// 检查给定名字的数据表是否已存在于当前数据库
static bool table_exists(Db &db, const std::string &name) {
  auto cnt =
      db.select_one_u64("SELECT COUNT(*) FROM information_schema.tables WHERE "
                        "table_schema=DATABASE() AND table_name='" +
                        name + "'");
  return cnt.has_value() && *cnt > 0;
}

// 获取当前时间戳
static uint64_t now_ms() {
  using namespace std::chrono;
  return (uint64_t)duration_cast<milliseconds>(
             system_clock::now().time_since_epoch())
      .count();
}

// 读取扩容状态
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

// 可能开始扩容
void HashTableDb::maybe_start_resize(const TableParams &p,
                                     double trigger_load) {
  // `resize_mu_` 必须由调用者持有。
  if (resize_state_.is_resizing) // 如果已经在扩容中，直接返回
    return;

  auto used = db_.select_one_u64(std::string("SELECT COUNT(*) FROM ") +
                                 kActiveTable + " WHERE fp<>0");
  const double load = p.capacity_slots // 如果 capacity_slots 不为 0，则计算负载
                          ? (double)used.value_or(0) / (double)p.capacity_slots
                          : 0.0;
  if (load < trigger_load) // 如果负载小于触发负载，直接返回
    return;

  resize_state_ = ResizeMeta{};            // 初始化扩容状态
  resize_state_.is_resizing = true;        // 设置扩容状态为正在扩容
  resize_state_.total_bins = p.total_bins; // 设置总 bin 数
  resize_state_.new_total_bins =
      std::max<uint64_t>(1, p.total_bins * 2); // 设置新总 bin 数
  resize_state_.resize_progress = 0;           // 设置扩容进度为 0
  resize_state_.started_at_ms = now_ms();      // 设置扩容开始时间
}

// 准备批量插入
void HashTableDb::prepare_batch_insert(const TableParams &p,
                                       uint64_t projected_inserts) {
  // 批量插入是“可控的重操作窗口”，适合在进入循环前触发扩容：
  // - 这样扩容期间不会被每次 insert 的事务反复打断
  // - 扩容完成后，批量插入可继续执行
  const double trigger =
      std::min(0.99, std::max(0.01, p.load_factor)); // 设置触发负载

  std::lock_guard<std::mutex> lk(resize_mu_);

  // 如果已经在扩容中（同进程内），直接继续扩容到完成，避免重复判断。
  if (resize_state_.is_resizing) {
    resize_to_completion(p);
    return;
  }

  // 估算批量插入结束时的负载，决定是否需要扩容。
  auto used = db_.select_one_u64(std::string("SELECT COUNT(*) FROM ") +
                                 kActiveTable + " WHERE fp<>0");
  const uint64_t used_now = used.value_or(0);
  const uint64_t used_proj = used_now + projected_inserts;
  const double load_proj =
      p.capacity_slots ? (double)used_proj / (double)p.capacity_slots : 0.0;
  if (load_proj < trigger)
    return;

  // 关键：这里是“批量插入前的预测触发”。
  // 即使当前 load 尚未达到阈值，也要提前启动扩容；否则 resize_to_completion()
  // 内部按“当前 load”判断会直接返回，导致扩容永远不发生。
  maybe_start_resize(p, /*trigger_load=*/0.0);
  resize_to_completion(p);
}

void HashTableDb::resize_to_completion(const TableParams &p) {
  // 同步扩容：在当前线程完成“创建 next 表 -> 分步迁移 -> 原子换表”。
  // 约束：调用者必须持有 resize_mu_（我们要保证换表期间没有其它写操作）。
  const double trigger = std::min(0.99, std::max(0.01, p.load_factor));

  // 如果不是扩容中，先判断当前负载是否达到触发阈值。
  if (!resize_state_.is_resizing) {
    maybe_start_resize(p, trigger);
    if (!resize_state_.is_resizing)
      return;
  }

  // 逐步迁移：每一步迁移 1 个旧 bin（一个短事务），前端可以看到 progress。
  while (resize_state_.is_resizing &&
         resize_state_.resize_progress < resize_state_.total_bins) {
    process_resize_step(p);
  }

  // 迁移完成：换表 + 持久化新规模。
  if (resize_state_.is_resizing) {
    finish_resize(p, resize_state_);
    resize_state_.is_resizing = false;
  }
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
// resize_progress。全部 bin 处理完后会调用
// finish_resize。须与写操作在同一个事务中。
void HashTableDb::process_resize_step(const TableParams &p) {
  // `resize_mu_` 必须由调用者持有。
  if (!resize_state_.is_resizing) // 如果不在扩容中，直接返回
    return;
  if (resize_state_.resize_progress >=
      resize_state_.total_bins) // 如果扩容进度大于等于总 bin 数，直接返回
    return;

  TableParams np = p; // 设置新的表参数
  np.total_bins = resize_state_.new_total_bins;
  np.capacity_slots = np.total_bins * np.bin_size; // 设置新的容量槽数
  ensure_next_table(db_, np.capacity_slots);

  const uint64_t bin_idx = resize_state_.resize_progress;
  const uint64_t start_idx = bin_idx * p.bin_size;     // 设置开始索引
  const uint64_t end_idx = start_idx + p.bin_size - 1; // 设置结束索引

  const uint64_t t0 = now_ms();

  // 迁移一个 bin 在一个短事务中
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
    resize_state_.elapsed_ms = (t1 >= resize_state_.started_at_ms)
                                   ? (t1 - resize_state_.started_at_ms)
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
  db_.exec("UPDATE ht_meta SET n=" + std::to_string(new_n) +
           ", total_bins=" + std::to_string(m.new_total_bins) + " WHERE id=1");

  uint64_t t = now_ms();
  if (resize_state_.started_at_ms) {
    resize_state_.finished_total_ms = (t >= resize_state_.started_at_ms)
                                          ? (t - resize_state_.started_at_ms)
                                          : 0;
  }
}

} // namespace otsh
