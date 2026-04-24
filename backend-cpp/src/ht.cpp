#include "ht.h"

#include <chrono>
#include <array>
#include <cmath>
#include <stdexcept>

namespace otsh {

HashTableDb::HashTableDb(Db& db) : db_(db) {}

void HashTableDb::reset_kick_hist(int k) {
  // allocate 0..k plus one extra bucket for "fallback marker" depth==k+1
  size_t n = static_cast<size_t>(std::max(0, k) + 2);
  kick_hist_ = std::make_unique<std::atomic<uint64_t>[]>(n);
  kick_hist_size_ = n;
  for (size_t i = 0; i < n; i++) kick_hist_[i].store(0, std::memory_order_relaxed);
}

std::vector<uint64_t> HashTableDb::kick_hist_snapshot() const {
  std::vector<uint64_t> out;
  out.reserve(kick_hist_size_);
  for (size_t i = 0; i < kick_hist_size_; i++) out.push_back(kick_hist_[i].load(std::memory_order_relaxed));
  return out;
}

uint64_t HashTableDb::ilog_iter(uint64_t n, int iter) {
  double x = static_cast<double>(n);
  for (int i = 0; i < iter; i++) {
    x = std::log(std::max(2.0, x));
  }
  return static_cast<uint64_t>(std::ceil(std::max(2.0, x)));
}

void HashTableDb::derive_sizes(TableParams& p) {
  // Per design doc: mini_bin_size = O(log^(k) n), num_mini_bins = O(log^(k) n)
  // For small n / deep k, clamp to keep bins non-trivial and DB friendly.
  uint64_t base = ilog_iter(p.n, std::max(1, p.k));
  p.mini_bin_size = std::min<uint64_t>(64, std::max<uint64_t>(4, base));
  p.num_mini_bins = std::min<uint64_t>(64, std::max<uint64_t>(4, base));

  p.bin_size = p.mini_bin_size * p.num_mini_bins + p.fallback_size;

  // capacity ~ n / load_factor
  uint64_t target_capacity = static_cast<uint64_t>(std::ceil(static_cast<double>(p.n) / std::max(0.01, p.load_factor)));
  p.total_bins = static_cast<uint64_t>(std::ceil(static_cast<double>(target_capacity) / static_cast<double>(p.bin_size)));
  p.total_bins = std::max<uint64_t>(1, p.total_bins);
  p.capacity_slots = p.total_bins * p.bin_size;
}

HashFunc HashTableDb::make_hash(const TableParams& p) const {
  HashFunc hf;
  hf.seed1 = p.seed1;
  hf.seed2 = p.seed2;
  hf.seed3 = p.seed3;
  return hf;
}

uint64_t HashTableDb::bin_offset(const TableParams& p, uint64_t b) const { return b * p.bin_size; }

uint64_t HashTableDb::mini_range_start(const TableParams& p, uint64_t b, uint64_t m) const {
  return bin_offset(p, b) + m * p.mini_bin_size;
}

uint64_t HashTableDb::mini_range_end(const TableParams& p, uint64_t b, uint64_t m) const {
  return mini_range_start(p, b, m) + p.mini_bin_size - 1;
}

uint64_t HashTableDb::fallback_start(const TableParams& p, uint64_t b) const {
  return bin_offset(p, b) + p.mini_bin_size * p.num_mini_bins;
}

uint64_t HashTableDb::fallback_end(const TableParams& p, uint64_t b) const {
  return fallback_start(p, b) + p.fallback_size - 1;
}

std::optional<uint64_t> HashTableDb::claim_empty_slot_in_range(const std::string& table, uint64_t start, uint64_t end,
                                                               uint64_t* probes) {
  if (probes) (*probes)++; // one range scan on a contiguous idx interval
  return db_.select_one_u64("SELECT idx FROM " + table + " WHERE idx BETWEEN " + std::to_string(start) + " AND " +
                            std::to_string(end) + " AND fp=0 AND `key` IS NULL LIMIT 1 FOR UPDATE");
}

bool HashTableDb::insert_into_fallback(const std::string& table, const TableParams& p, uint64_t key, uint16_t fp, uint64_t b,
                                       std::vector<KickStep>* trace, uint64_t* probes) {
  auto start = fallback_start(p, b);
  auto end = fallback_end(p, b);
  auto idxOpt = claim_empty_slot_in_range(table, start, end, probes);
  if (!idxOpt) return false;
  db_.update_slot(table, *idxOpt, key, fp);
  if (trace) {
    trace->push_back(KickStep{.idx = *idxOpt, .from_fp = 0, .to_fp = fp, .depth = static_cast<int>(p.k + 1), .action = "fallback_add"});
  }
  return true;
}

void HashTableDb::erase_from_fallback_if_present(const TableParams& p, uint64_t key, uint16_t fp, uint64_t b,
                                                 std::vector<KickStep>* trace) {
  auto idxOpt = db_.select_idx_for_key_for_update(key);
  if (!idxOpt) return;
  uint64_t start = fallback_start(p, b);
  uint64_t end = fallback_end(p, b);
  if (*idxOpt < start || *idxOpt > end) return;
  db_.update_slot(*idxOpt, std::nullopt, 0);
  if (trace) trace->push_back(KickStep{.idx = *idxOpt, .from_fp = fp, .to_fp = 0, .depth = 0, .action = "fallback_remove"});
}

bool HashTableDb::insert_with_kick(const std::string& table, const TableParams& p, const HashFunc& hf, uint64_t key, uint16_t fp,
                                  uint64_t depth, std::vector<KickStep>* trace, uint64_t* probes) {
  if (depth > static_cast<uint64_t>(p.k)) return false;

  uint64_t b = hf.h_bin(key, p.total_bins);
  uint64_t m = hf.h_pref(key, p.num_mini_bins);

  uint64_t start = mini_range_start(p, b, m);
  uint64_t end = mini_range_end(p, b, m);

  // empty slot
  auto idxOpt = claim_empty_slot_in_range(table, start, end, probes);
  if (idxOpt) {
    erase_from_fallback_if_present(p, key, fp, b, trace);
    db_.update_slot(table, *idxOpt, key, fp);
    if (kick_hist_ && depth < kick_hist_size_) kick_hist_[depth].fetch_add(1, std::memory_order_relaxed);
    if (trace) trace->push_back(KickStep{.idx = *idxOpt, .from_fp = 0, .to_fp = fp, .depth = static_cast<int>(depth), .action = "place"});
    return true;
  }

  // kick: choose a slot deterministically (DB-friendly, avoids ORDER BY RAND())
  uint64_t slot = hf.kick_slot(fp, depth, p.mini_bin_size);
  uint64_t idx = start + slot;

  auto cur = db_.select_slot_for_update(table, idx);
  if (!cur || cur->fp == 0) {
    erase_from_fallback_if_present(p, key, fp, b, trace);
    db_.update_slot(table, idx, key, fp);
    if (kick_hist_ && depth < kick_hist_size_) kick_hist_[depth].fetch_add(1, std::memory_order_relaxed);
    if (trace) trace->push_back(KickStep{.idx = idx, .from_fp = 0, .to_fp = fp, .depth = static_cast<int>(depth), .action = "place"});
    return true;
  }

  uint16_t evicted = cur->fp;
  if (!cur->key.has_value()) throw std::runtime_error("invariant violated: occupied slot missing key");
  uint64_t evicted_key = *cur->key;

  erase_from_fallback_if_present(p, key, fp, b, trace);
  db_.update_slot(table, idx, key, fp);
  if (kick_hist_ && depth < kick_hist_size_) kick_hist_[depth].fetch_add(1, std::memory_order_relaxed);
  if (trace) trace->push_back(KickStep{.idx = idx, .from_fp = evicted, .to_fp = fp, .depth = static_cast<int>(depth), .action = "kick"});

  // evicted goes to fallback first (as per design doc), then recurse
  uint64_t ev_b = hf.h_bin(evicted_key, p.total_bins);
  if (!insert_into_fallback(table, p, evicted_key, evicted, ev_b, trace, probes)) {
    // Can't safely proceed: we'd lose the evicted element (and violate persistence).
    return false;
  }

  bool ok = insert_with_kick(table, p, hf, evicted_key, evicted, depth + 1, trace, probes);
  if (ok) {
    erase_from_fallback_if_present(p, evicted_key, evicted, ev_b, trace);
  }
  return ok;
}

InsertResult HashTableDb::insert_key_into_table(const std::string& table, const TableParams& p, uint64_t key, bool with_trace) {
  InsertResult r;
  try {
    auto hf = make_hash(p);
    uint16_t fp = hf.fingerprint(key);
    uint64_t b = hf.h_bin(key, p.total_bins);
    uint64_t m = hf.h_pref(key, p.num_mini_bins);
    r.fp = fp;
    r.bin = b;
    r.mini = m;

    db_.begin();
    if (db_.select_idx_for_key_for_update(table, key).has_value()) {
      db_.commit();
      r.ok = true;
      return r;
    }

    bool ok = insert_with_kick(table, p, hf, key, fp, 0, with_trace ? &r.trace : nullptr, &r.probes);
    if (ok) {
      db_.commit();
      r.ok = true;
      return r;
    }

    bool fb_ok = insert_into_fallback(table, p, key, fp, b, with_trace ? &r.trace : nullptr, &r.probes);
    db_.commit();
    if (fb_ok) {
      r.ok = true;
      r.error = "placed_in_fallback_only";
      return r;
    }

    r.ok = false;
    r.error = "fallback_full_or_insert_failed";
    return r;
  } catch (const std::exception& e) {
    try { db_.rollback(); } catch (...) {}
    r.ok = false;
    r.error = e.what();
    return r;
  }
}

TableParams HashTableDb::load_or_init_meta(uint64_t n, int k, double load_factor, std::optional<uint64_t> seed1,
                                          std::optional<uint64_t> seed2, std::optional<uint64_t> seed3) {
  db_.exec(
      "CREATE TABLE IF NOT EXISTS ht_meta ("
      " id INT PRIMARY KEY,"
      " n BIGINT NOT NULL,"
      " k INT NOT NULL,"
      " load_factor DOUBLE NOT NULL,"
      " seed1 BIGINT UNSIGNED NOT NULL,"
      " seed2 BIGINT UNSIGNED NOT NULL,"
      " seed3 BIGINT UNSIGNED NOT NULL,"
      " total_bins BIGINT NOT NULL,"
      " is_resizing TINYINT NOT NULL DEFAULT 0,"
      " new_total_bins BIGINT NOT NULL DEFAULT 0,"
      " resize_progress BIGINT NOT NULL DEFAULT 0"
      ")");

  // Best-effort schema migration for existing ht_meta created by older versions.
  // MySQL/MariaDB might reject ADD COLUMN if it already exists; ignore those errors.
  try { db_.exec("ALTER TABLE ht_meta ADD COLUMN total_bins BIGINT NOT NULL DEFAULT 0"); } catch (...) {}
  try { db_.exec("ALTER TABLE ht_meta ADD COLUMN is_resizing TINYINT NOT NULL DEFAULT 0"); } catch (...) {}
  try { db_.exec("ALTER TABLE ht_meta ADD COLUMN new_total_bins BIGINT NOT NULL DEFAULT 0"); } catch (...) {}
  try { db_.exec("ALTER TABLE ht_meta ADD COLUMN resize_progress BIGINT NOT NULL DEFAULT 0"); } catch (...) {}

  auto rows = db_.query("SELECT n,k,load_factor,seed1,seed2,seed3,total_bins,is_resizing,new_total_bins,resize_progress FROM ht_meta WHERE id=1");

  TableParams p;
  if (!rows.empty()) {
    p.n = std::stoull(rows[0][0]);
    p.k = std::stoi(rows[0][1]);
    p.load_factor = std::stod(rows[0][2]);
    p.seed1 = std::stoull(rows[0][3]);
    p.seed2 = std::stoull(rows[0][4]);
    p.seed3 = std::stoull(rows[0][5]);
    derive_sizes(p);
    // Override derived bins with persisted bins (allows online resize without changing n/lf).
    p.total_bins = std::stoull(rows[0][6]);
    p.total_bins = std::max<uint64_t>(1, p.total_bins);
    p.capacity_slots = p.total_bins * p.bin_size;
  } else {
    p.n = n;
    p.k = k;
    p.load_factor = load_factor;

    auto now = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    p.seed1 = seed1.value_or(splitmix64(now ^ 0xA5A5A5A5A5A5A5A5ULL));
    p.seed2 = seed2.value_or(splitmix64(now ^ 0x0123456789ABCDEFULL));
    p.seed3 = seed3.value_or(splitmix64(now ^ 0xF0F0F0F0F0F0F0F0ULL));

    derive_sizes(p);
    db_.exec("INSERT INTO ht_meta(id,n,k,load_factor,seed1,seed2,seed3,total_bins,is_resizing,new_total_bins,resize_progress) "
             "VALUES(1," + std::to_string(p.n) + "," + std::to_string(p.k) + "," + std::to_string(p.load_factor) + "," +
             std::to_string(p.seed1) + "," + std::to_string(p.seed2) + "," + std::to_string(p.seed3) + "," +
             std::to_string(p.total_bins) + ",0,0,0)");
  }
  return p;
}

OpResult HashTableDb::init_table(const TableParams& p) {
  OpResult r;
  try {
    // Reset any in-progress resize artifacts.
    db_.exec(std::string("DROP TABLE IF EXISTS ") + kNextTable);
    db_.exec(std::string("DROP TABLE IF EXISTS ") + kOldTable);

    // Build new tables first, then atomically swap them in.
    // This avoids losing existing data if initialization fails midway (e.g. due to max_allowed_packet, timeouts).
    db_.exec("DROP TABLE IF EXISTS hash_table_new");
    // No key_slots in simplified schema.

    db_.exec(
        "CREATE TABLE hash_table_new ("
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
        if (i + 1 != end) sql += ",";
      }
      db_.exec(sql);
    }

    // Swap in (atomic in MySQL): rename tables.
    // We must handle 3 states robustly:
    // - both live tables exist
    // - neither exists (fresh DB)
    // - only one exists (partial/corrupted state). In that case, drop the existing one then swap in.
    auto table_exists = [&](const char* name) -> bool {
      auto cnt = db_.select_one_u64(
          "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() AND table_name='" +
          std::string(name) + "'");
      return cnt.has_value() && *cnt > 0;
    };

    bool has_ht = table_exists("hash_table");
    bool has_ks = table_exists("key_slots");

    db_.exec("DROP TABLE IF EXISTS hash_table_old");
    db_.exec("DROP TABLE IF EXISTS key_slots_old");

    // We no longer use key_slots at all; if it exists, drop it as part of init.
    if (has_ks) {
      db_.exec("DROP TABLE IF EXISTS key_slots");
    }

    if (has_ht) {
      db_.exec("RENAME TABLE hash_table TO hash_table_old, hash_table_new TO hash_table");
      db_.exec("DROP TABLE IF EXISTS hash_table_old");
    } else {
      db_.exec("RENAME TABLE hash_table_new TO hash_table");
    }

    // Clear resize flags and persist current total_bins.
    db_.exec("UPDATE ht_meta SET total_bins=" + std::to_string(p.total_bins) +
             ", is_resizing=0, new_total_bins=0, resize_progress=0 WHERE id=1");

    r.ok = true;
    return r;
  } catch (const std::exception& e) {
    r.ok = false;
    r.error = e.what();
    return r;
  }
}

OpResult HashTableDb::find_key(const TableParams& p, uint64_t key) {
  OpResult r;
  try {
    // During resizing: check next table first, then active table.
    // We intentionally avoid FOR UPDATE here; find is read-only.
    auto meta_rows = db_.query("SELECT is_resizing,new_total_bins FROM ht_meta WHERE id=1");
    bool resizing = false;
    uint64_t new_bins = 0;
    if (!meta_rows.empty() && meta_rows[0].size() >= 2) {
      resizing = (std::stoull(meta_rows[0][0]) != 0);
      new_bins = std::stoull(meta_rows[0][1]);
    }

    auto hf = make_hash(p);
    uint16_t fp = hf.fingerprint(key);
    auto find_in_table = [&](const std::string& table, uint64_t total_bins) -> bool {
      uint64_t b = hf.h_bin(key, total_bins);
      uint64_t m = hf.h_pref(key, p.num_mini_bins);

      auto verify_by_range = [&](uint64_t start, uint64_t end) -> bool {
        auto rows = db_.query("SELECT 1 FROM " + table + " WHERE idx BETWEEN " + std::to_string(start) + " AND " +
                              std::to_string(end) + " AND `key`=" + std::to_string(key) + " AND fp=" + std::to_string(fp) +
                              " LIMIT 1");
        return !rows.empty();
      };

      // Probe 1: preferred mini-bin scan
      {
        r.probes++;
        uint64_t start = mini_range_start(p, b, m);
        uint64_t end = mini_range_end(p, b, m);
        if (verify_by_range(start, end)) return true;
      }
      // Probe 2: fallback scan
      {
        r.probes++;
        uint64_t start = fallback_start(p, b);
        uint64_t end = fallback_end(p, b);
        if (verify_by_range(start, end)) return true;
      }
      return false;
    };

    auto table_exists2 = [&](const std::string& name) -> bool {
      auto cnt = db_.select_one_u64(
          "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() AND table_name='" + name + "'");
      return cnt.has_value() && *cnt > 0;
    };

    if (resizing && table_exists2(kNextTable) && new_bins > 0) {
      if (find_in_table(kNextTable, new_bins)) {
        r.ok = true;
        return r;
      }
    }
    r.ok = find_in_table(kActiveTable, p.total_bins);
    return r;
  } catch (const std::exception& e) {
    r.ok = false;
    r.error = e.what();
    return r;
  }
}

OpResult HashTableDb::erase_key(const TableParams& p, uint64_t key) {
  OpResult r;
  try {
    db_.begin();

    // If resizing, migrate one bin and try erase in next then active.
    auto m = load_resize_meta_for_update();
    if (m.is_resizing) {
      process_resize_step(p);
      if (m.new_total_bins > 0) {
        auto idx2 = db_.select_idx_for_key_for_update(kNextTable, key);
        if (idx2) {
          db_.update_slot(kNextTable, *idx2, std::nullopt, 0);
          db_.commit();
          r.ok = true;
          return r;
        }
      }
    }

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
  } catch (const std::exception& e) {
    try { db_.rollback(); } catch (...) {}
    r.ok = false;
    r.error = e.what();
    return r;
  }
}

InsertResult HashTableDb::insert_key(const TableParams& p, uint64_t key, bool with_trace) {
  // Online resize: during resizing we always write into next table and migrate 1 bin per write op.
  try {
    db_.begin();
    auto m = load_resize_meta_for_update();
    if (!m.is_resizing) {
      // maybe start resize based on load
      maybe_start_resize(p, /*trigger_load=*/0.99);
      db_.commit();
      return insert_key_into_table(kActiveTable, p, key, with_trace);
    }
    // migrate one step (inside same transaction)
    process_resize_step(p);
    // write to next table using new_total_bins
    TableParams np = p;
    np.total_bins = m.new_total_bins;
    np.capacity_slots = np.total_bins * np.bin_size;
    db_.commit();
    return insert_key_into_table(kNextTable, np, key, with_trace);
  } catch (const std::exception& e) {
    try { db_.rollback(); } catch (...) {}
    InsertResult r;
    r.ok = false;
    r.error = e.what();
    return r;
  }
}

std::vector<BinStats> HashTableDb::bin_stats(const TableParams& p, uint64_t bin_start, uint64_t bin_count) {
  uint64_t mini_total = p.mini_bin_size * p.num_mini_bins;
  uint64_t bin_end = std::min<uint64_t>(p.total_bins, bin_start + bin_count);
  if (bin_start >= bin_end) return {};

  // group-by over whole table, but filtered by bin range
  std::string sql =
      "SELECT (idx DIV " + std::to_string(p.bin_size) + ") AS b,"
      " SUM(fp<>0) AS used_slots,"
      " SUM(CASE WHEN fp<>0 AND (idx % " + std::to_string(p.bin_size) + ") >= " + std::to_string(mini_total) +
      " THEN 1 ELSE 0 END) AS fb_used"
      " FROM hash_table"
      " WHERE (idx DIV " + std::to_string(p.bin_size) + ") BETWEEN " + std::to_string(bin_start) + " AND " +
      std::to_string(bin_end - 1) + " GROUP BY b ORDER BY b";

  auto rows = db_.query(sql);
  std::vector<BinStats> out;
  out.reserve(rows.size());
  for (auto& r : rows) {
    BinStats bs;
    bs.bin = std::stoull(r[0]);
    bs.used_slots = static_cast<uint32_t>(std::stoul(r[1]));
    bs.fallback_used = static_cast<uint32_t>(std::stoul(r[2]));
    out.push_back(bs);
  }
  return out;
}

std::vector<std::array<uint64_t, 3>> HashTableDb::snapshot_bins(const TableParams& p, uint64_t bin_start, uint64_t bin_count) {
  uint64_t bin_end = std::min<uint64_t>(p.total_bins, bin_start + bin_count);
  if (bin_start >= bin_end) return {};

  uint64_t start_idx = bin_start * p.bin_size;
  uint64_t end_idx = bin_end * p.bin_size - 1;

  auto rows = db_.query("SELECT idx, fp FROM hash_table WHERE idx BETWEEN " + std::to_string(start_idx) + " AND " +
                        std::to_string(end_idx) + " ORDER BY idx");

  std::vector<std::array<uint64_t, 3>> out;
  out.reserve(rows.size());
  for (auto& r : rows) {
    out.push_back({std::stoull(r[0]), std::stoull(r[1]), 0});
  }
  return out;
}

Stats HashTableDb::stats(const TableParams& p) {
  Stats s;
  auto used = db_.query("SELECT COUNT(*) FROM hash_table WHERE fp<>0");
  if (!used.empty()) s.used_slots = std::stoull(used[0][0]);

  // fallback region are the tail slots of each bin
  uint64_t mini_total = p.mini_bin_size * p.num_mini_bins;
  std::string sql =
      "SELECT COUNT(*) FROM hash_table WHERE fp<>0 AND (idx % " + std::to_string(p.bin_size) + ") >= " +
      std::to_string(mini_total);
  auto fb = db_.query(sql);
  if (!fb.empty()) s.fallback_used = std::stoull(fb[0][0]);
  return s;
}

} // namespace otsh

