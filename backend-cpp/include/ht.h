#pragma once

#include "config.h"
#include "db.h"
#include "hash.h"
#include "resize.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace otsh {

struct OpResult {
  bool ok = false;
  uint64_t probes = 0; // number of SQL range scans (mini-bin + fallback)
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
  std::string action; // "place" | "kick" | "fallback_add" | "fallback_remove"
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

  // Best-effort: for batch inserts, pre-check projected load and start resize
  // early so the batch can directly write to `hash_table_next`.
  // `projected_inserts` is an estimate; duplicates are fine.
  void prepare_batch_insert(const TableParams &p, uint64_t projected_inserts);

  // Synchronous resize: when load reaches trigger, pause the caller's work,
  // run resize steps until finished, then return.
  // This matches a simple single-threaded control flow:
  //   insert... -> detect threshold -> resize_to_completion() -> resume insert...
  void resize_to_completion(const TableParams &p);

  // Kick depth histogram (in-memory only, cleared on init/rebuild).
  void reset_kick_hist(int k);
  std::vector<uint64_t> kick_hist_snapshot() const;

  Stats stats(const TableParams &p);
  std::vector<BinStats> bin_stats(const TableParams &p, uint64_t bin_start,
                                  uint64_t bin_count);

  // Snapshot for Canvas grid visualization
  // returns rows: [idx, fp, reserved]
  std::vector<std::array<uint64_t, 3>>
  snapshot_bins(const TableParams &p, uint64_t bin_start, uint64_t bin_count);

  // Read-only resize metadata from `ht_meta` (no row lock; best-effort for UI
  // and cooperative waits).
  std::optional<ResizeMeta> read_resize_state_snapshot() const;

private:
  Db &db_;

  // depth -> count (0..k). We also keep one extra bucket for depth==k+1
  // (fallback marker). We avoid std::vector<std::atomic<...>> because atomics
  // are non-movable.
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

  // --- resize meta/state (persisted in ht_meta) ---
  ResizeMeta load_resize_meta_for_update();
  void store_resize_meta(const ResizeMeta &m);
  void maybe_start_resize(const TableParams &p, double trigger_load);
  void process_resize_step(const TableParams &p);
  void finish_resize(const ResizeMeta &m);

  // Transactional operations (all assume caller runs in a transaction)
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

  // Insert implementation targeting a specific physical table name.
  // If `manage_txn` is false, caller must already be in a transaction.
  InsertResult insert_key_into_table(const std::string &table,
                                     const TableParams &p, uint64_t key,
                                     bool with_trace, bool manage_txn);
};

} // namespace otsh
