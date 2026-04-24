#include "ht.h"

#include <algorithm>
#include <stdexcept>

namespace otsh {

static bool table_exists(Db& db, const std::string& name) {
  auto cnt = db.select_one_u64(
      "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() AND table_name='" + name + "'");
  return cnt.has_value() && *cnt > 0;
}

ResizeMeta HashTableDb::load_resize_meta_for_update() {
  // Lock meta row while we mutate resize fields.
  auto rows = db_.query("SELECT total_bins,is_resizing,new_total_bins,resize_progress FROM ht_meta WHERE id=1 FOR UPDATE");
  if (rows.empty() || rows[0].size() < 4) throw std::runtime_error("ht_meta missing resize columns");
  ResizeMeta m;
  m.total_bins = std::stoull(rows[0][0]);
  m.is_resizing = (std::stoull(rows[0][1]) != 0);
  m.new_total_bins = std::stoull(rows[0][2]);
  m.resize_progress = std::stoull(rows[0][3]);
  return m;
}

void HashTableDb::store_resize_meta(const ResizeMeta& m) {
  db_.exec("UPDATE ht_meta SET total_bins=" + std::to_string(m.total_bins) + ", is_resizing=" + std::to_string(m.is_resizing ? 1 : 0) +
           ", new_total_bins=" + std::to_string(m.new_total_bins) + ", resize_progress=" + std::to_string(m.resize_progress) + " WHERE id=1");
}

void HashTableDb::maybe_start_resize(const TableParams& p, double trigger_load) {
  // must be called inside a transaction
  auto m = load_resize_meta_for_update();
  if (m.is_resizing) return;

  auto used = db_.select_one_u64(std::string("SELECT COUNT(*) FROM ") + kActiveTable + " WHERE fp<>0");
  double load = (p.capacity_slots ? (double)(used.value_or(0)) / (double)p.capacity_slots : 0.0);
  if (load < trigger_load) return;

  m.is_resizing = true;
  m.total_bins = p.total_bins;
  m.new_total_bins = p.total_bins * 2;
  m.resize_progress = 0;
  store_resize_meta(m);

  // Create next table if not exists (and initialize slots). We do this outside of the meta lock transaction
  // to keep locks short; callers will call process_resize_step which will verify/create as needed.
}

static void ensure_next_table(Db& db, uint64_t capacity_slots) {
  if (table_exists(db, kNextTable)) return;

  db.exec(std::string("CREATE TABLE ") + kNextTable +
          " ( idx BIGINT PRIMARY KEY,"
          " `key` BIGINT UNSIGNED NULL,"
          " fp SMALLINT UNSIGNED NOT NULL,"
          " UNIQUE KEY uk_key(`key`) )");

  const uint64_t batch = 5000;
  for (uint64_t base = 0; base < capacity_slots; base += batch) {
    uint64_t end = std::min<uint64_t>(capacity_slots, base + batch);
    std::string sql = std::string("INSERT INTO ") + kNextTable + "(idx,`key`,fp) VALUES ";
    for (uint64_t i = base; i < end; i++) {
      sql += "(" + std::to_string(i) + ",NULL,0)";
      if (i + 1 != end) sql += ",";
    }
    db.exec(sql);
  }
}

void HashTableDb::process_resize_step(const TableParams& p) {
  // called inside a transaction (same one as the write op)
  auto m = load_resize_meta_for_update();
  if (!m.is_resizing) return;

  // ensure next table exists
  TableParams np = p;
  np.total_bins = m.new_total_bins;
  np.capacity_slots = np.total_bins * np.bin_size;
  ensure_next_table(db_, np.capacity_slots);

  if (m.resize_progress >= m.total_bins) {
    finish_resize(m);
    return;
  }

  uint64_t bin_idx = m.resize_progress;
  uint64_t start_idx = bin_idx * p.bin_size;
  uint64_t end_idx = start_idx + p.bin_size - 1;

  // Lock old bin rows to avoid concurrent modifications while migrating.
  auto rows = db_.query(std::string("SELECT idx, `key`, fp FROM ") + kActiveTable + " WHERE idx BETWEEN " +
                        std::to_string(start_idx) + " AND " + std::to_string(end_idx) + " AND fp<>0 FOR UPDATE");

  // Move each occupied slot to next table, then clear old slot.
  for (auto& r : rows) {
    if (r.size() < 3) continue;
    uint64_t idx = std::stoull(r[0]);
    if (r[1].empty()) continue;
    uint64_t key = std::stoull(r[1]);
    uint16_t fp = static_cast<uint16_t>(std::stoul(r[2]));

    // Insert into next table using new total_bins.
    // Note: we recompute fp from key inside insert; fp is kept for debug only.
    (void)fp;
    auto ins = insert_key_into_table(kNextTable, np, key, false);
    if (!ins.ok) throw std::runtime_error("resize migrate insert failed: " + ins.error);

    // Clear old slot.
    db_.update_slot(kActiveTable, idx, std::nullopt, 0);
  }

  m.resize_progress++;
  store_resize_meta(m);
}

void HashTableDb::finish_resize(const ResizeMeta& m) {
  // called inside transaction with ht_meta locked
  (void)m;
  // Atomically swap tables: hash_table -> old, hash_table_next -> hash_table
  db_.exec(std::string("DROP TABLE IF EXISTS ") + kOldTable);
  db_.exec(std::string("RENAME TABLE ") + kActiveTable + " TO " + kOldTable + ", " + kNextTable + " TO " + kActiveTable);
  db_.exec(std::string("DROP TABLE IF EXISTS ") + kOldTable);

  ResizeMeta nm = m;
  nm.is_resizing = false;
  nm.total_bins = m.new_total_bins;
  nm.new_total_bins = 0;
  nm.resize_progress = 0;
  store_resize_meta(nm);

  // Keep displayed n in sync with capacity growth (best-effort).
  // We treat `n` as the target scale, so when bins double we double n as well.
  try {
    auto rows = db_.query("SELECT n FROM ht_meta WHERE id=1 FOR UPDATE");
    if (!rows.empty() && !rows[0].empty()) {
      uint64_t n = std::stoull(rows[0][0]);
      db_.exec("UPDATE ht_meta SET n=" + std::to_string(n * 2) + " WHERE id=1");
    }
  } catch (...) {
  }
}

} // namespace otsh

