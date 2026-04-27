#include "ht.h"

#include <array>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace otsh {

HashTableDb::HashTableDb(Db &db) : db_(db) {}

void HashTableDb::reset_kick_hist(int k) {
  // 分配 0..k 加上一个额外的桶，用于 "fallback marker" depth==k+1
  size_t n = static_cast<size_t>(std::max(0, k) + 2);
  kick_hist_ = std::make_unique<std::atomic<uint64_t>[]>(n);
  kick_hist_size_ = n;
  for (size_t i = 0; i < n; i++)
    kick_hist_[i].store(0, std::memory_order_relaxed);
}

// 获取踢出历史快照
std::vector<uint64_t> HashTableDb::kick_hist_snapshot() const {
  std::vector<uint64_t> out;
  out.reserve(kick_hist_size_);
  for (size_t i = 0; i < kick_hist_size_; i++)
    out.push_back(kick_hist_[i].load(std::memory_order_relaxed));
  return out;
}

// 计算迭代对数
uint64_t HashTableDb::ilog_iter(uint64_t n, int iter) {
  double x = static_cast<double>(n);
  for (int i = 0; i < iter; i++) {
    x = std::log(std::max(2.0, x));
  }
  return static_cast<uint64_t>(std::ceil(std::max(2.0, x)));
}

void HashTableDb::derive_sizes(TableParams &p) {
  // 根据设计文档：mini_bin_size = O(log^(k) n), num_mini_bins = O(log^(k) n)
  // 对于小 n / 深 k，clamp 以保持 bins 非平凡且 DB 友好。
  uint64_t base = ilog_iter(p.n, std::max(1, p.k));
  p.mini_bin_size = std::min<uint64_t>(64, std::max<uint64_t>(4, base));
  p.num_mini_bins = std::min<uint64_t>(64, std::max<uint64_t>(4, base));

  p.bin_size = p.mini_bin_size * p.num_mini_bins + p.fallback_size;

  // 约定：n 表示“空间/槽位规模”（capacity_slots 期望与 n 同阶/近似）。
  // load_factor 仅作为扩容阈值（触发在线 resize），不参与容量规划。
  uint64_t target_capacity = std::max<uint64_t>(1, p.n);
  p.total_bins = static_cast<uint64_t>(std::ceil(
      static_cast<double>(target_capacity) / static_cast<double>(p.bin_size)));
  p.total_bins = std::max<uint64_t>(1, p.total_bins);
  p.capacity_slots = p.total_bins * p.bin_size;
}

// 创建哈希函数
HashFunc HashTableDb::make_hash(const TableParams &p) const {
  HashFunc hf;
  hf.seed1 = p.seed1;
  hf.seed2 = p.seed2;
  hf.seed3 = p.seed3;
  return hf;
}

// 计算 bin 偏移
uint64_t HashTableDb::bin_offset(const TableParams &p, uint64_t b) const {
  return b * p.bin_size;
}

// 计算 mini-bin 范围起始
uint64_t HashTableDb::mini_range_start(const TableParams &p, uint64_t b,
                                       uint64_t m) const {
  return bin_offset(p, b) + m * p.mini_bin_size;
}

// 计算 mini-bin 范围结束
uint64_t HashTableDb::mini_range_end(const TableParams &p, uint64_t b,
                                     uint64_t m) const {
  return mini_range_start(p, b, m) + p.mini_bin_size - 1;
}

// 计算 fallback 范围起始
uint64_t HashTableDb::fallback_start(const TableParams &p, uint64_t b) const {
  return bin_offset(p, b) + p.mini_bin_size * p.num_mini_bins;
}

// 计算 fallback 范围结束
uint64_t HashTableDb::fallback_end(const TableParams &p, uint64_t b) const {
  return fallback_start(p, b) + p.fallback_size - 1;
}

// 在范围内查找空槽
std::optional<uint64_t>
HashTableDb::claim_empty_slot_in_range(const std::string &table, uint64_t start,
                                       uint64_t end, uint64_t *probes) {
  if (probes)
    (*probes)++; // 在一个连续的 idx 区间上进行一次范围扫描
  return db_.select_one_u64("SELECT idx FROM " + table + " WHERE idx BETWEEN " +
                            std::to_string(start) + " AND " +
                            std::to_string(end) +
                            " AND fp=0 AND `key` IS NULL LIMIT 1 FOR UPDATE");
}

// 插入到 fallback 区域
bool HashTableDb::insert_into_fallback(const std::string &table,
                                       const TableParams &p, uint64_t key,
                                       uint16_t fp, uint64_t b,
                                       std::vector<KickStep> *trace,
                                       uint64_t *probes) {
  auto start = fallback_start(p, b);
  auto end = fallback_end(p, b);
  auto idxOpt = claim_empty_slot_in_range(table, start, end, probes);
  if (!idxOpt)
    return false;
  db_.update_slot(table, *idxOpt, key, fp);
  if (trace) {
    trace->push_back(KickStep{.idx = *idxOpt,
                              .from_fp = 0,
                              .to_fp = fp,
                              .depth = static_cast<int>(p.k + 1),
                              .action = "fallback_add"});
  }
  return true;
}

// 从 fallback 区域删除
void HashTableDb::erase_from_fallback_if_present(const TableParams &p,
                                                 uint64_t key, uint16_t fp,
                                                 uint64_t b,
                                                 std::vector<KickStep> *trace) {
  auto idxOpt = db_.select_idx_for_key_for_update(key);
  if (!idxOpt)
    return;
  uint64_t start = fallback_start(p, b);
  uint64_t end = fallback_end(p, b);
  if (*idxOpt < start || *idxOpt > end)
    return;
  db_.update_slot(*idxOpt, std::nullopt, 0);
  if (trace)
    trace->push_back(KickStep{.idx = *idxOpt,
                              .from_fp = fp,
                              .to_fp = 0,
                              .depth = 0,
                              .action = "fallback_remove"});
}

// 插入并踢出
bool HashTableDb::insert_with_kick(const std::string &table,
                                   const TableParams &p, const HashFunc &hf,
                                   uint64_t key, uint16_t fp, uint64_t depth,
                                   std::vector<KickStep> *trace,
                                   uint64_t *probes) {
  if (depth > static_cast<uint64_t>(p.k))
    return false;

  uint64_t b = hf.h_bin(key, p.total_bins);
  uint64_t m = hf.h_pref(key, p.num_mini_bins);

  uint64_t start = mini_range_start(p, b, m);
  uint64_t end = mini_range_end(p, b, m);

  // 空槽
  auto idxOpt = claim_empty_slot_in_range(table, start, end, probes);
  if (idxOpt) {
    erase_from_fallback_if_present(p, key, fp, b, trace);
    db_.update_slot(table, *idxOpt, key, fp);
    if (kick_hist_ && depth < kick_hist_size_)
      kick_hist_[depth].fetch_add(1, std::memory_order_relaxed);
    if (trace)
      trace->push_back(KickStep{.idx = *idxOpt,
                                .from_fp = 0,
                                .to_fp = fp,
                                .depth = static_cast<int>(depth),
                                .action = "place"});
    return true;
  }

  // 踢出: 选择一个槽确定性地 (DB-friendly, 避免 ORDER BY RAND() 的随机性)
  uint64_t slot = hf.kick_slot(fp, depth, p.mini_bin_size);
  uint64_t idx = start + slot;

  auto cur = db_.select_slot_for_update(table, idx);
  if (!cur || cur->fp == 0) {
    erase_from_fallback_if_present(p, key, fp, b, trace);
    db_.update_slot(table, idx, key, fp);
    if (kick_hist_ && depth < kick_hist_size_)
      kick_hist_[depth].fetch_add(1, std::memory_order_relaxed);
    if (trace)
      trace->push_back(KickStep{.idx = idx,
                                .from_fp = 0,
                                .to_fp = fp,
                                .depth = static_cast<int>(depth),
                                .action = "place"});
    return true;
  }

  uint16_t evicted = cur->fp;
  if (!cur->key.has_value())
    throw std::runtime_error("invariant violated: occupied slot missing key");
  uint64_t evicted_key = *cur->key;

  erase_from_fallback_if_present(p, key, fp, b, trace);
  db_.update_slot(table, idx, key, fp);
  if (kick_hist_ && depth < kick_hist_size_)
    kick_hist_[depth].fetch_add(1, std::memory_order_relaxed);
  if (trace)
    trace->push_back(KickStep{.idx = idx,
                              .from_fp = evicted,
                              .to_fp = fp,
                              .depth = static_cast<int>(depth),
                              .action = "kick"});

  // 被踢出的元素首先插入到 fallback 区域 然后递归
  uint64_t ev_b = hf.h_bin(evicted_key, p.total_bins);
  if (!insert_into_fallback(table, p, evicted_key, evicted, ev_b, trace,
                            probes)) {
    // 不能安全继续: 我们会丢失被踢出的元素 违反持久性.
    return false;
  }

  // 递归插入
  bool ok = insert_with_kick(table, p, hf, evicted_key, evicted, depth + 1,
                             trace, probes);
  if (ok) {
    // 从 fallback 区域删除
    erase_from_fallback_if_present(p, evicted_key, evicted, ev_b, trace);
  }
  return ok;
}

// 插入到表中
InsertResult HashTableDb::insert_key_into_table(const std::string &table,
                                                const TableParams &p,
                                                uint64_t key, bool with_trace,
                                                bool manage_txn) {
  InsertResult r;
  try {
    auto hf = make_hash(p);
    uint16_t fp = hf.fingerprint(key);
    uint64_t b = hf.h_bin(key, p.total_bins);
    uint64_t m = hf.h_pref(key, p.num_mini_bins);
    r.fp = fp;
    r.bin = b;
    r.mini = m;

    if (manage_txn)
      db_.begin();
    if (db_.select_idx_for_key_for_update(table, key).has_value()) {
      if (manage_txn)
        db_.commit();
      r.ok = true;
      return r;
    }

    // --- Local simulation path (minimize SQL round-trips) ---
    // Lock the whole bin once, simulate kick chain in-memory, then batch-write
    // changed slots back with a single UPDATE ... CASE.
    const uint64_t bin_start = bin_offset(p, b);
    const uint64_t bin_end = bin_start + p.bin_size - 1;
    auto rows =
        db_.query("SELECT idx, `key`, fp FROM " + table +
                  " WHERE idx BETWEEN " + std::to_string(bin_start) + " AND " +
                  std::to_string(bin_end) + " ORDER BY idx FOR UPDATE");
    r.probes += 1; // one contiguous range scan for the whole bin

    struct Slot {
      std::optional<uint64_t> key;
      uint16_t fp = 0;
    };
    std::vector<Slot> slots;
    slots.resize(static_cast<size_t>(p.bin_size));
    for (const auto &rr : rows) {
      if (rr.size() < 3)
        continue;
      uint64_t idx = std::stoull(rr[0]);
      if (idx < bin_start || idx > bin_end)
        continue;
      size_t off = static_cast<size_t>(idx - bin_start);
      Slot s;
      if (!rr[1].empty())
        s.key = std::stoull(rr[1]);
      s.fp = static_cast<uint16_t>(std::stoul(rr[2]));
      slots[off] = s;
    }

    auto write_back = [&](const std::vector<size_t> &changed) {
      if (changed.empty())
        return;
      std::string sql = "UPDATE " + table + " SET ";

      // key CASE
      sql += "`key` = CASE idx ";
      for (size_t off : changed) {
        uint64_t idx = bin_start + static_cast<uint64_t>(off);
        sql += "WHEN " + std::to_string(idx) + " THEN ";
        if (slots[off].fp == 0 || !slots[off].key.has_value())
          sql += "NULL ";
        else
          sql += std::to_string(*slots[off].key) + " ";
      }
      sql += "ELSE `key` END, ";

      // fp CASE
      sql += "fp = CASE idx ";
      for (size_t off : changed) {
        uint64_t idx = bin_start + static_cast<uint64_t>(off);
        sql += "WHEN " + std::to_string(idx) + " THEN " +
               std::to_string(slots[off].fp) + " ";
      }
      sql += "ELSE fp END ";

      // WHERE idx IN (...)
      sql += "WHERE idx IN (";
      for (size_t i = 0; i < changed.size(); i++) {
        if (i)
          sql += ",";
        sql += std::to_string(bin_start + static_cast<uint64_t>(changed[i]));
      }
      sql += ")";
      db_.exec(sql);
    };

    auto mark_trace = [&](uint64_t idx, uint16_t from_fp, uint16_t to_fp,
                          int depth, const char *action) {
      if (!with_trace)
        return;
      r.trace.push_back(KickStep{.idx = idx,
                                 .from_fp = from_fp,
                                 .to_fp = to_fp,
                                 .depth = depth,
                                 .action = action});
    };

    const uint64_t mini_total = p.mini_bin_size * p.num_mini_bins;
    const size_t mini_off0 = static_cast<size_t>(m * p.mini_bin_size);
    const size_t mini_off1 = mini_off0 + static_cast<size_t>(p.mini_bin_size);

    std::vector<size_t> changed;
    changed.reserve(static_cast<size_t>(p.k + 4));
    std::vector<uint8_t> changed_flag;
    changed_flag.resize(static_cast<size_t>(p.bin_size), 0);

    auto mark_changed = [&](size_t off) {
      // `changed` must not contain duplicates; otherwise UPDATE ... CASE idx ...
      // may emit duplicated WHEN/IN entries, and (worse) MySQL will take the
      // first matching WHEN, potentially ignoring later in-memory mutations.
      if (off >= changed_flag.size())
        return;
      if (changed_flag[off])
        return;
      changed_flag[off] = 1;
      changed.push_back(off);
    };

    // Case 1: preferred mini-bin has empty slot.
    for (size_t off = mini_off0; off < mini_off1; off++) {
      if (slots[off].fp == 0) {
        uint64_t idx = bin_start + static_cast<uint64_t>(off);
        slots[off].fp = fp;
        slots[off].key = key;
        mark_changed(off);
        if (kick_hist_ && 0 < kick_hist_size_)
          kick_hist_[0].fetch_add(1, std::memory_order_relaxed);
        mark_trace(idx, 0, fp, 0, "place");
        write_back(changed);
        if (manage_txn)
          db_.commit();
        r.ok = true;
        return r;
      }
    }

    // Case 2: run k-kick chain within mini-bin (in-memory), then fallback.
    uint64_t cur_key = key;
    uint16_t cur_fp = fp;

    for (uint64_t depth = 0; depth <= static_cast<uint64_t>(p.k); depth++) {
      uint64_t slot_in_mini = hf.kick_slot(cur_fp, depth, p.mini_bin_size);
      size_t off = mini_off0 + static_cast<size_t>(slot_in_mini);
      if (off >= mini_off1)
        off = mini_off0; // safety

      uint64_t idx = bin_start + static_cast<uint64_t>(off);
      Slot &victim = slots[off];
      if (victim.fp == 0) {
        victim.fp = cur_fp;
        victim.key = cur_key;
        mark_changed(off);
        if (kick_hist_ && depth < kick_hist_size_)
          kick_hist_[depth].fetch_add(1, std::memory_order_relaxed);
        mark_trace(idx, 0, cur_fp, static_cast<int>(depth), "place");
        write_back(changed);
        if (manage_txn)
          db_.commit();
        r.ok = true;
        return r;
      }
      if (!victim.key.has_value())
        throw std::runtime_error(
            "invariant violated: occupied slot missing key");

      uint16_t ev_fp = victim.fp;
      uint64_t ev_key = *victim.key;

      victim.fp = cur_fp;
      victim.key = cur_key;
      mark_changed(off);
      if (kick_hist_ && depth < kick_hist_size_)
        kick_hist_[depth].fetch_add(1, std::memory_order_relaxed);
      mark_trace(idx, ev_fp, cur_fp, static_cast<int>(depth), "kick");

      cur_fp = ev_fp;
      cur_key = ev_key;
    }

    // fallback region: tail slots of the bin
    for (size_t off = static_cast<size_t>(mini_total);
         off < static_cast<size_t>(p.bin_size); off++) {
      if (slots[off].fp == 0) {
        uint64_t idx = bin_start + static_cast<uint64_t>(off);
        slots[off].fp = cur_fp;
        slots[off].key = cur_key;
        mark_changed(off);
        if (kick_hist_ && (static_cast<size_t>(p.k + 1) < kick_hist_size_))
          kick_hist_[static_cast<size_t>(p.k + 1)].fetch_add(
              1, std::memory_order_relaxed);
        mark_trace(idx, 0, cur_fp, static_cast<int>(p.k + 1), "fallback_add");
        write_back(changed);
        if (manage_txn)
          db_.commit();
        r.ok = true;
        r.error = "placed_in_fallback_only";
        return r;
      }
    }

    // Local simulation failed: keep old slow path as fallback (correctness).
    if (!manage_txn) {
      throw std::runtime_error(
          "local_insert_simulation_failed_in_existing_txn");
    }
    db_.rollback();
    db_.begin();

    bool ok = insert_with_kick(table, p, hf, key, fp, 0,
                               with_trace ? &r.trace : nullptr, &r.probes);
    if (ok) {
      db_.commit();
      r.ok = true;
      return r;
    }

    bool fb_ok = insert_into_fallback(
        table, p, key, fp, b, with_trace ? &r.trace : nullptr, &r.probes);
    db_.commit();
    if (fb_ok) {
      r.ok = true;
      r.error = "placed_in_fallback_only";
      return r;
    }

    r.ok = false;
    r.error = "fallback_full_or_insert_failed";
    return r;
  } catch (const std::exception &e) {
    try {
      if (manage_txn)
        db_.rollback();
    } catch (...) {
    }
    r.ok = false;
    r.error = e.what();
    return r;
  }
}

// 加载或初始化元数据
TableParams HashTableDb::load_or_init_meta(uint64_t n, int k,
                                           double load_factor,
                                           std::optional<uint64_t> seed1,
                                           std::optional<uint64_t> seed2,
                                           std::optional<uint64_t> seed3) {
  db_.exec("CREATE TABLE IF NOT EXISTS ht_meta ("
           " id INT PRIMARY KEY,"
           " n BIGINT NOT NULL,"
           " k INT NOT NULL,"
           " load_factor DOUBLE NOT NULL,"
           " seed1 BIGINT UNSIGNED NOT NULL,"
           " seed2 BIGINT UNSIGNED NOT NULL,"
           " seed3 BIGINT UNSIGNED NOT NULL,"
           " total_bins BIGINT NOT NULL"
           ")");

  // Best-effort schema migration for older databases (only keep total_bins).
  // MySQL/MariaDB may reject ADD COLUMN if it already exists; ignore those.
  try {
    db_.exec(
        "ALTER TABLE ht_meta ADD COLUMN total_bins BIGINT NOT NULL DEFAULT 0");
  } catch (...) {
  }

  auto rows = db_.query("SELECT "
                        "n,k,load_factor,seed1,seed2,seed3,total_bins "
                        "FROM ht_meta WHERE id=1");

  TableParams p;
  if (!rows.empty()) {
    p.n = std::stoull(rows[0][0]);
    p.k = std::stoi(rows[0][1]);
    p.load_factor = std::stod(rows[0][2]);
    p.seed1 = std::stoull(rows[0][3]);
    p.seed2 = std::stoull(rows[0][4]);
    p.seed3 = std::stoull(rows[0][5]);
    derive_sizes(p);
    // 覆盖派生的 bins 与持久化的 bins (允许在线 resize 而不改变 n/lf)。
    p.total_bins = std::stoull(rows[0][6]);
    p.total_bins = std::max<uint64_t>(1, p.total_bins);
    p.capacity_slots = p.total_bins * p.bin_size;
  } else {
    p.n = n;
    p.k = k;
    p.load_factor = load_factor;

    auto now = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    p.seed1 = seed1.value_or(splitmix64(now ^ 0xA5A5A5A5A5A5A5A5ULL));
    p.seed2 = seed2.value_or(splitmix64(now ^ 0x0123456789ABCDEFULL));
    p.seed3 = seed3.value_or(splitmix64(now ^ 0xF0F0F0F0F0F0F0F0ULL));

    derive_sizes(p);
    db_.exec("INSERT INTO "
             "ht_meta(id,n,k,load_factor,seed1,seed2,seed3,total_bins) "
             "VALUES(1," +
             std::to_string(p.n) + "," + std::to_string(p.k) + "," +
             std::to_string(p.load_factor) + "," + std::to_string(p.seed1) +
             "," + std::to_string(p.seed2) + "," + std::to_string(p.seed3) +
             "," + std::to_string(p.total_bins) + ")");
  }
  return p;
}

OpResult HashTableDb::init_table(const TableParams &p) {
  OpResult r;
  try {
    // 重置任何正在进行的 resize 工件。
    db_.exec(std::string("DROP TABLE IF EXISTS ") + kNextTable);
    db_.exec(std::string("DROP TABLE IF EXISTS ") + kOldTable);

    // 首先构建新的表，然后原子地交换它们。
    // 这避免了在初始化失败中途 (例如由于 max_allowed_packet, 超时)
    // 丢失现有数据。 由于 max_allowed_packet, 超时)。
    db_.exec("DROP TABLE IF EXISTS hash_table_new");
    // 简化模式中没有 key_slots。

    db_.exec("CREATE TABLE hash_table_new ("
             " idx BIGINT PRIMARY KEY,"
             " `key` BIGINT UNSIGNED NULL,"
             " fp SMALLINT UNSIGNED NOT NULL,"
             " UNIQUE KEY uk_key(`key`)"
             ")");

    // bulk insert slots in batches
    const uint64_t batch = 5000;
    for (uint64_t base = 0; base < p.capacity_slots; base += batch) {
      uint64_t end = std::min<uint64_t>(p.capacity_slots, base + batch);
      std::string sql = "INSERT INTO hash_table_new(idx,`key`,fp) VALUES ";
      for (uint64_t i = base; i < end; i++) {
        sql += "(" + std::to_string(i) + ",NULL,0)";
        if (i + 1 != end)
          sql += ",";
      }
      db_.exec(sql);
    }

    // 交换 in (atomic in MySQL): 重命名表。
    // 我们必须稳健地处理 3 种状态:
    // - 两个 live 表都存在
    // - 都不存在 (fresh DB)
    // - 只有一个存在 (部分/损坏的状态)。在这种情况下，删除现有的一个然后交换
    // in。
    auto table_exists = [&](const char *name) -> bool {
      auto cnt =
          db_.select_one_u64("SELECT COUNT(*) FROM information_schema.tables "
                             "WHERE table_schema=DATABASE() AND table_name='" +
                             std::string(name) + "'");
      return cnt.has_value() && *cnt > 0;
    };

    bool has_ht = table_exists("hash_table");
    bool has_ks = table_exists("key_slots");

    db_.exec("DROP TABLE IF EXISTS hash_table_old");
    db_.exec("DROP TABLE IF EXISTS key_slots_old");

    if (has_ks) {
      db_.exec("DROP TABLE IF EXISTS key_slots");
    }

    if (has_ht) {
      db_.exec("RENAME TABLE hash_table TO hash_table_old, hash_table_new TO "
               "hash_table");
      db_.exec("DROP TABLE IF EXISTS hash_table_old");
    } else {
      db_.exec("RENAME TABLE hash_table_new TO hash_table");
    }

    // 清除 resize 标志并持久化当前 total_bins。
    db_.exec("UPDATE ht_meta SET total_bins=" + std::to_string(p.total_bins) +
             " WHERE id=1");

    r.ok = true;
    return r;
  } catch (const std::exception &e) {
    r.ok = false;
    r.error = e.what();
    return r;
  }
}

OpResult HashTableDb::find_key(const TableParams &p, uint64_t key) {
  OpResult r;
  try {
    std::lock_guard<std::mutex> lk(resize_mu_);
    auto hf = make_hash(p);
    uint16_t fp = hf.fingerprint(key);
    auto find_in_table = [&](const std::string &table,
                             uint64_t total_bins) -> bool {
      uint64_t b = hf.h_bin(key, total_bins);
      uint64_t m = hf.h_pref(key, p.num_mini_bins);

      auto verify_by_range = [&](uint64_t start, uint64_t end) -> bool {
        auto rows =
            db_.query("SELECT 1 FROM " + table + " WHERE idx BETWEEN " +
                      std::to_string(start) + " AND " + std::to_string(end) +
                      " AND `key`=" + std::to_string(key) +
                      " AND fp=" + std::to_string(fp) + " LIMIT 1");
        return !rows.empty();
      };

      // 探测 1: 偏好 mini-bin 扫描
      {
        r.probes++;
        uint64_t start = mini_range_start(p, b, m);
        uint64_t end = mini_range_end(p, b, m);
        if (verify_by_range(start, end))
          return true;
      }
      // 探测 2: fallback 扫描
      {
        r.probes++;
        uint64_t start = fallback_start(p, b);
        uint64_t end = fallback_end(p, b);
        if (verify_by_range(start, end))
          return true;
      }
      return false;
    };

    auto table_exists2 = [&](const std::string &name) -> bool {
      auto cnt =
          db_.select_one_u64("SELECT COUNT(*) FROM information_schema.tables "
                             "WHERE table_schema=DATABASE() AND table_name='" +
                             name + "'");
      return cnt.has_value() && *cnt > 0;
    };

    (void)table_exists2;
    r.ok = find_in_table(kActiveTable, p.total_bins);
    return r;
  } catch (const std::exception &e) {
    r.ok = false;
    r.error = e.what();
    return r;
  }
}

OpResult HashTableDb::erase_key(const TableParams &p, uint64_t key) {
  OpResult r;
  try {
    std::lock_guard<std::mutex> lk(resize_mu_);
    db_.begin();

    auto idxOpt = db_.select_idx_for_key_for_update(kActiveTable, key);
    if (!idxOpt) {
      db_.commit();
      r.ok = false;
      return r;
    }
    db_.update_slot(kActiveTable, *idxOpt, std::nullopt, 0);
    db_.commit();
    r.ok = true;
    return r;
  } catch (const std::exception &e) {
    try {
      db_.rollback();
    } catch (...) {
    }
    r.ok = false;
    r.error = e.what();
    return r;
  }
}

InsertResult HashTableDb::insert_key(const TableParams &p, uint64_t key,
                                     bool with_trace) {
  try {
    std::lock_guard<std::mutex> lk(resize_mu_);
    db_.begin();
    InsertResult out = insert_key_into_table(kActiveTable, p, key, with_trace,
                                             /*manage_txn=*/false);

    if (!out.ok) {
      db_.rollback();
      return out;
    }
    db_.commit();
    return out;
  } catch (const std::exception &e) {
    try {
      db_.rollback();
    } catch (...) {
    }
    InsertResult r;
    r.ok = false;
    r.error = e.what();
    return r;
  }
}

std::vector<BinStats> HashTableDb::bin_stats(const TableParams &p,
                                             uint64_t bin_start,
                                             uint64_t bin_count) {
  uint64_t mini_total = p.mini_bin_size * p.num_mini_bins;
  uint64_t bin_end = std::min<uint64_t>(p.total_bins, bin_start + bin_count);
  if (bin_start >= bin_end)
    return {};

  // group-by over whole table, but filtered by bin range
  std::string sql =
      "SELECT (idx DIV " + std::to_string(p.bin_size) +
      ") AS b,"
      " SUM(fp<>0) AS used_slots,"
      " SUM(CASE WHEN fp<>0 AND (idx % " +
      std::to_string(p.bin_size) + ") >= " + std::to_string(mini_total) +
      " THEN 1 ELSE 0 END) AS fb_used"
      " FROM hash_table"
      " WHERE (idx DIV " +
      std::to_string(p.bin_size) + ") BETWEEN " + std::to_string(bin_start) +
      " AND " + std::to_string(bin_end - 1) + " GROUP BY b ORDER BY b";

  auto rows = db_.query(sql);
  std::vector<BinStats> out;
  out.reserve(rows.size());
  for (auto &r : rows) {
    BinStats bs;
    bs.bin = std::stoull(r[0]);
    bs.used_slots = static_cast<uint32_t>(std::stoul(r[1]));
    bs.fallback_used = static_cast<uint32_t>(std::stoul(r[2]));
    out.push_back(bs);
  }
  return out;
}

std::vector<std::array<uint64_t, 3>>
HashTableDb::snapshot_bins(const TableParams &p, uint64_t bin_start,
                           uint64_t bin_count) {
  uint64_t bin_end = std::min<uint64_t>(p.total_bins, bin_start + bin_count);
  if (bin_start >= bin_end)
    return {};

  uint64_t start_idx = bin_start * p.bin_size;
  uint64_t end_idx = bin_end * p.bin_size - 1;

  auto rows = db_.query("SELECT idx, fp FROM hash_table WHERE idx BETWEEN " +
                        std::to_string(start_idx) + " AND " +
                        std::to_string(end_idx) + " ORDER BY idx");

  std::vector<std::array<uint64_t, 3>> out;
  out.reserve(rows.size());
  for (auto &r : rows) {
    out.push_back({std::stoull(r[0]), std::stoull(r[1]), 0});
  }
  return out;
}

Stats HashTableDb::stats(const TableParams &p) {
  Stats s;
  auto used = db_.query("SELECT COUNT(*) FROM hash_table WHERE fp<>0");
  if (!used.empty())
    s.used_slots = std::stoull(used[0][0]);

  uint64_t mini_total = p.mini_bin_size * p.num_mini_bins;
  std::string sql = "SELECT COUNT(*) FROM hash_table WHERE fp<>0 AND (idx % " +
                    std::to_string(p.bin_size) +
                    ") >= " + std::to_string(mini_total);
  auto fb = db_.query(sql);
  if (!fb.empty())
    s.fallback_used = std::stoull(fb[0][0]);
  return s;
}

} // namespace otsh
