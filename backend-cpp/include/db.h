#pragma once

#include "config.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct st_mysql;

namespace otsh {

class Db {
public:
  explicit Db(DbConfig cfg);
  ~Db();

  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;
  Db(Db&&) = delete;
  Db& operator=(Db&&) = delete;

  void connect();

  void exec(const std::string& sql);

  // Simple query helpers (text protocol). Values are returned as strings.
  std::vector<std::vector<std::string>> query(const std::string& sql);

  void begin();
  void commit();
  void rollback();

  // Locking helper for transactional updates
  std::optional<uint64_t> select_one_u64(const std::string& sql);
  struct SlotRow {
    uint16_t fp = 0;
    std::optional<uint64_t> key;
  };
  std::optional<SlotRow> select_slot_for_update(const std::string& table, uint64_t idx);
  std::optional<SlotRow> select_slot_for_update(uint64_t idx) { return select_slot_for_update("hash_table", idx); }
  // fp==0 means empty slot
  // When fp==0, we will store NULL key.
  void update_slot(const std::string& table, uint64_t idx, std::optional<uint64_t> key, uint16_t fp);
  void update_slot(uint64_t idx, std::optional<uint64_t> key, uint16_t fp) { update_slot("hash_table", idx, key, fp); }

  // Find slot index by key (requires UNIQUE index on hash_table.key).
  std::optional<uint64_t> select_idx_for_key_for_update(const std::string& table, uint64_t key);
  std::optional<uint64_t> select_idx_for_key_for_update(uint64_t key) { return select_idx_for_key_for_update("hash_table", key); }
  std::optional<uint64_t> select_idx_for_key(const std::string& table, uint64_t key);
  std::optional<uint64_t> select_idx_for_key(uint64_t key) { return select_idx_for_key("hash_table", key); }

  const DbConfig& config() const { return cfg_; }

private:
  DbConfig cfg_;
  st_mysql* conn_{nullptr};
};

} // namespace otsh

