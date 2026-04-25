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

  Db(const Db &) = delete;
  Db &operator=(const Db &) = delete;
  Db(Db &&) = delete;
  Db &operator=(Db &&) = delete;

  void connect();

  void exec(const std::string &sql);

  // 简单的查询助手 (文本协议)。值作为字符串返回。
  std::vector<std::vector<std::string>> query(const std::string &sql) const;

  void begin();
  void commit();
  void rollback();

  // 锁定助手用于事务更新
  std::optional<uint64_t> select_one_u64(const std::string &sql);
  struct SlotRow {
    uint16_t fp = 0;
    std::optional<uint64_t> key;
  };
  std::optional<SlotRow> select_slot_for_update(const std::string &table,
                                                uint64_t idx);
  std::optional<SlotRow> select_slot_for_update(uint64_t idx) {
    return select_slot_for_update("hash_table", idx);
  }
  // fp==0 表示空槽
  // 当 fp==0 时, 我们将存储 NULL 键。
  void update_slot(const std::string &table, uint64_t idx,
                   std::optional<uint64_t> key, uint16_t fp);
  void update_slot(uint64_t idx, std::optional<uint64_t> key, uint16_t fp) {
    update_slot("hash_table", idx, key, fp);
  }

  // 按 key 查找槽索引 (需要 UNIQUE 索引 on hash_table.key)。
  std::optional<uint64_t>
  select_idx_for_key_for_update(const std::string &table, uint64_t key);
  std::optional<uint64_t> select_idx_for_key_for_update(uint64_t key) {
    return select_idx_for_key_for_update("hash_table", key);
  }
  std::optional<uint64_t> select_idx_for_key(const std::string &table,
                                             uint64_t key);
  std::optional<uint64_t> select_idx_for_key(uint64_t key) {
    return select_idx_for_key("hash_table", key);
  }

  const DbConfig &config() const { return cfg_; }

private:
  DbConfig cfg_;
  st_mysql *conn_{nullptr};
};

} // namespace otsh
