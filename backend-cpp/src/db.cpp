#include "db.h"

#include <mysql.h>

#include <stdexcept>

namespace otsh {

// 抛出 MySQL 错误
static void throw_mysql(st_mysql *conn, const char *prefix) {
  std::string msg = prefix;
  msg += ": ";
  msg += mysql_error(conn);
  throw std::runtime_error(msg);
}

Db::Db(DbConfig cfg) : cfg_(std::move(cfg)) {}

Db::~Db() {
  if (conn_)
    mysql_close(conn_);
}

void Db::connect() {
  conn_ = mysql_init(nullptr);
  if (!conn_)
    throw std::runtime_error("mysql_init failed");

  mysql_options(conn_, MYSQL_SET_CHARSET_NAME, "utf8mb4");

  if (!mysql_real_connect(conn_, cfg_.host.c_str(), cfg_.user.c_str(),
                          cfg_.password.c_str(), cfg_.database.c_str(),
                          cfg_.port, nullptr, 0)) {
    throw_mysql(conn_, "mysql_real_connect failed");
  }
}

void Db::exec(const std::string &sql) {
  if (mysql_query(conn_, sql.c_str()) != 0) {
    throw_mysql(conn_, ("query failed: " + sql).c_str());
  }
  // 丢弃结果（如果有的话）
  MYSQL_RES *res = mysql_store_result(conn_);
  if (res)
    mysql_free_result(res);
}

// 执行查询并返回结果
std::vector<std::vector<std::string>> Db::query(const std::string &sql) {
  if (mysql_query(conn_, sql.c_str()) != 0) {
    throw_mysql(conn_, ("query failed: " + sql).c_str());
  }
  MYSQL_RES *res = mysql_store_result(conn_);
  if (!res)
    return {};

  int nfields = mysql_num_fields(res);
  std::vector<std::vector<std::string>> rows;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res)) != nullptr) {
    unsigned long *lengths = mysql_fetch_lengths(res);
    std::vector<std::string> r;
    r.reserve(nfields);
    for (int i = 0; i < nfields; i++) {
      if (!row[i])
        r.emplace_back("");
      else
        r.emplace_back(row[i], row[i] + lengths[i]);
    }
    rows.emplace_back(std::move(r));
  }
  mysql_free_result(res);
  return rows;
}

void Db::begin() { exec("START TRANSACTION"); }
void Db::commit() { exec("COMMIT"); }
void Db::rollback() { exec("ROLLBACK"); }

// 执行查询并返回一个 uint64_t 类型的结果
std::optional<uint64_t> Db::select_one_u64(const std::string &sql) {
  auto rows = query(sql);
  if (rows.empty() || rows[0].empty())
    return std::nullopt;
  return static_cast<uint64_t>(std::stoull(rows[0][0]));
}

// 执行查询并返回一个 SlotRow 类型的结果
std::optional<Db::SlotRow> Db::select_slot_for_update(const std::string &table,
                                                      uint64_t idx) {
  auto rows = query("SELECT `key`, fp FROM " + table +
                    " WHERE idx=" + std::to_string(idx) + " FOR UPDATE");
  if (rows.empty())
    return std::nullopt;
  SlotRow r;
  if (rows[0].size() >= 1 && !rows[0][0].empty())
    r.key = static_cast<uint64_t>(std::stoull(rows[0][0]));
  r.fp = static_cast<uint16_t>(std::stoul(rows[0][1]));
  return r;
}

// 更新一个槽
void Db::update_slot(const std::string &table, uint64_t idx,
                     std::optional<uint64_t> key, uint16_t fp) {
  std::string sql =
      "UPDATE " + table + " SET fp=" + std::to_string(fp) + ", `key`=";
  if (key.has_value())
    sql += std::to_string(*key);
  else
    sql += "NULL";
  sql += " WHERE idx=" + std::to_string(idx);
  exec(sql);
}

// 执行查询并返回一个 uint64_t 类型的结果
std::optional<uint64_t>
Db::select_idx_for_key_for_update(const std::string &table, uint64_t key) {
  auto rows =
      query("SELECT idx FROM " + table + " WHERE `key`=" + std::to_string(key) +
            " LIMIT 1 FOR UPDATE");
  if (rows.empty() || rows[0].empty())
    return std::nullopt;
  return static_cast<uint64_t>(std::stoull(rows[0][0]));
}

// 执行查询并返回一个 uint64_t 类型的结果
std::optional<uint64_t> Db::select_idx_for_key(const std::string &table,
                                               uint64_t key) {
  auto rows = query("SELECT idx FROM " + table +
                    " WHERE `key`=" + std::to_string(key) + " LIMIT 1");
  if (rows.empty() || rows[0].empty())
    return std::nullopt;
  return static_cast<uint64_t>(std::stoull(rows[0][0]));
}

} // namespace otsh
