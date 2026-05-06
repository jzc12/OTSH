#include "storage.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>

#if OTSH_HAVE_MYSQL
#include <mysql.h>
#include <mysqld_error.h>
#endif

namespace otsh {

namespace {

#if OTSH_HAVE_MYSQL
std::string sql_escape_tag(MYSQL *conn, const std::string &s) {
  if (!conn || s.empty())
    return "''";
  std::string out(1, '\'');
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '\'')
      out += "''";
    else if (c == '\\')
      out += "\\\\";
    else
      out += c;
  }
  out += '\'';
  return out;
}
#endif

} // namespace

class MySqlStorage final : public IStorage {
public:
  ~MySqlStorage() override {
    std::lock_guard<std::mutex> lk(mu_);
#if OTSH_HAVE_MYSQL
    if (conn_) {
      mysql_close(conn_);
      conn_ = nullptr;
    }
#endif
  }

  StorageResult open(const StorageOpenOptions &opt) override {
#if !OTSH_HAVE_MYSQL
    (void)opt;
    return {false, "mysql_not_available"};
#else
    std::lock_guard<std::mutex> lk(mu_);
    host_ = opt.mysql_host;
    port_ = opt.mysql_port;
    user_ = opt.mysql_user;
    pass_ = opt.mysql_password;
    db_ = opt.mysql_database;
    table_ = opt.mysql_table.empty() ? "otsh_keys" : opt.mysql_table;

    if (conn_) {
      mysql_close(conn_);
      conn_ = nullptr;
    }

    conn_ = mysql_init(nullptr);
    if (!conn_)
      return {false, "mysql_init_failed"};

    mysql_options(conn_, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    auto connect = [&](const char *db) -> bool {
      return mysql_real_connect(conn_, host_.c_str(), user_.c_str(),
                                pass_.c_str(), db, port_, nullptr,
                                0) != nullptr;
    };

    if (!connect(db_.c_str())) {
      unsigned int err_no = mysql_errno(conn_);
      if (err_no == ER_BAD_DB_ERROR) {
        mysql_close(conn_);
        conn_ = mysql_init(nullptr);
        if (!conn_)
          return {false, "mysql_init_failed"};
        mysql_options(conn_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
        if (!connect(nullptr)) {
          std::string err = mysql_error(conn_);
          mysql_close(conn_);
          conn_ = nullptr;
          return {false, "mysql_connect_failed: " + err};
        }
        std::string cdb =
            "CREATE DATABASE IF NOT EXISTS `" + db_ + "` CHARACTER SET utf8mb4";
        auto r1 = exec(cdb);
        if (!r1.ok)
          return r1;
        mysql_close(conn_);
        conn_ = mysql_init(nullptr);
        if (!conn_)
          return {false, "mysql_init_failed"};
        mysql_options(conn_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
        if (!connect(db_.c_str())) {
          std::string err = mysql_error(conn_);
          mysql_close(conn_);
          conn_ = nullptr;
          return {false, "mysql_connect_failed: " + err};
        }
      } else {
        std::string err = mysql_error(conn_);
        mysql_close(conn_);
        conn_ = nullptr;
        return {false, "mysql_connect_failed: " + err};
      }
    }

    std::string sql = "CREATE TABLE IF NOT EXISTS `" + table_ +
                      "`("
                      " `k` BIGINT UNSIGNED PRIMARY KEY"
                      ") ENGINE=InnoDB";
    auto r2 = exec(sql);
    if (!r2.ok)
      return r2;
    return ensure_analytics_ddl();
#endif
  }

  StorageResult clear() override {
#if !OTSH_HAVE_MYSQL
    return {false, "mysql_not_available"};
#else
    std::lock_guard<std::mutex> lk(mu_);
    if (!conn_)
      return {false, "not_open"};
    (void)exec("SET FOREIGN_KEY_CHECKS=0");
    // 使用 DELETE 代替 TRUNCATE：部分环境对 TRUNCATE 权限更严；缺表时忽略 1146。
    const char *analytics_tables[] = {"otsh_op_metrics", "slot_snapshot", "tier_stat",
                                      "facility",        "cubby",         "snapshot_meta",
                                      "metrics"};
    for (const char *t : analytics_tables) {
      const std::string sql = std::string("DELETE FROM `") + t + "`";
      if (mysql_query(conn_, sql.c_str()) != 0) {
        const unsigned err = mysql_errno(conn_);
        MYSQL_RES *res = mysql_store_result(conn_);
        if (res)
          mysql_free_result(res);
#if defined(ER_NO_SUCH_TABLE)
        if (err != ER_NO_SUCH_TABLE)
#else
        if (err != 1146)
#endif
        {
          (void)exec("SET FOREIGN_KEY_CHECKS=1");
          return {false, "mysql_clear_failed: " + std::string(mysql_error(conn_))};
        }
      } else {
        MYSQL_RES *res = mysql_store_result(conn_);
        if (res)
          mysql_free_result(res);
      }
    }
    StorageResult r = exec("TRUNCATE TABLE `" + table_ + "`");
    if (!r.ok) {
      r = exec("DELETE FROM `" + table_ + "`");
      if (!r.ok) {
        (void)exec("SET FOREIGN_KEY_CHECKS=1");
        return r;
      }
    }
    (void)exec("SET FOREIGN_KEY_CHECKS=1");
    metrics_buf_.clear();
    return {true, ""};
#endif
  }

  StorageResult for_each_key(const std::function<void(uint64_t)> &cb) override {
#if !OTSH_HAVE_MYSQL
    (void)cb;
    return {false, "mysql_not_available"};
#else
    std::lock_guard<std::mutex> lk(mu_);
    if (!conn_)
      return {false, "not_open"};

    std::string sql = "SELECT `k` FROM `" + table_ + "`";
    if (mysql_query(conn_, sql.c_str()) != 0) {
      return {false, "mysql_query_failed: " + std::string(mysql_error(conn_))};
    }
    MYSQL_RES *res = mysql_store_result(conn_);
    if (!res)
      return {false,
              "mysql_store_result_failed: " + std::string(mysql_error(conn_))};

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr) {
      if (!row[0])
        continue;
      uint64_t k = static_cast<uint64_t>(std::strtoull(row[0], nullptr, 10));
      cb(k);
    }
    mysql_free_result(res);
    return {true, ""};
#endif
  }

  StorageResult put(uint64_t key) override {
#if !OTSH_HAVE_MYSQL
    (void)key;
    return {false, "mysql_not_available"};
#else
    std::lock_guard<std::mutex> lk(mu_);
    if (!conn_)
      return {false, "not_open"};
    std::string sql = "INSERT IGNORE INTO `" + table_ + "`(`k`) VALUES(" +
                      std::to_string(key) + ")";
    return exec(sql);
#endif
  }

  StorageResult erase(uint64_t key) override {
#if !OTSH_HAVE_MYSQL
    (void)key;
    return {false, "mysql_not_available"};
#else
    std::lock_guard<std::mutex> lk(mu_);
    if (!conn_)
      return {false, "not_open"};
    std::string sql =
        "DELETE FROM `" + table_ + "` WHERE `k`=" + std::to_string(key);
    return exec(sql);
#endif
  }

  int64_t analytics_create_snapshot(const std::string &snapshot_tag, uint64_t n,
                                    uint64_t N, uint64_t K) override {
#if !OTSH_HAVE_MYSQL
    (void)snapshot_tag;
    (void)n;
    (void)N;
    (void)K;
    return -1;
#else
    std::lock_guard<std::mutex> lk(mu_);
    if (!conn_)
      return -1;
    const std::string tag_sql = sql_escape_tag(conn_, snapshot_tag);
    std::string sql =
        "INSERT INTO `snapshot_meta`(`snapshot_tag`,`init_param_n`,`table_n`,`table_k`)"
        " VALUES(" +
        tag_sql + "," + std::to_string(n) + "," + std::to_string(N) + "," +
        std::to_string(K) + ")";
    if (!exec(sql).ok)
      return -1;
    return static_cast<int64_t>(mysql_insert_id(conn_));
#endif
  }

  void analytics_enqueue_metric(const SqlMetricRow &row) override {
#if !OTSH_HAVE_MYSQL
    (void)row;
#else
    std::lock_guard<std::mutex> lk(mu_);
    metrics_buf_.push_back(row);
    flush_metrics_locked(false);
#endif
  }

  void analytics_flush_metrics(bool force) override {
#if !OTSH_HAVE_MYSQL
    (void)force;
#else
    std::lock_guard<std::mutex> lk(mu_);
    flush_metrics_locked(force);
#endif
  }

  StorageResult analytics_replace_structure(
      int64_t snapshot_id, const std::vector<SqlFacilityRow> &facilities,
      const std::vector<SqlCubbyRow> &cubbies,
      const std::vector<SqlSlotRow> &slots,
      const std::vector<SqlTierStatRow> &tier_stats) override {
#if !OTSH_HAVE_MYSQL
    (void)snapshot_id;
    (void)facilities;
    (void)cubbies;
    (void)slots;
    (void)tier_stats;
    return {false, "mysql_not_available"};
#else
    std::lock_guard<std::mutex> lk(mu_);
    if (!conn_)
      return {false, "not_open"};
    const std::string sid = std::to_string(snapshot_id);
    auto r0 = exec("DELETE FROM `slot_snapshot` WHERE `snapshot_id`=" + sid);
    if (!r0.ok)
      return r0;
    auto r1 = exec("DELETE FROM `cubby` WHERE `snapshot_id`=" + sid);
    if (!r1.ok)
      return r1;
    auto r2 = exec("DELETE FROM `facility` WHERE `snapshot_id`=" + sid);
    if (!r2.ok)
      return r2;
    auto r3 = exec("DELETE FROM `tier_stat` WHERE `snapshot_id`=" + sid);
    if (!r3.ok)
      return r3;

    for (const auto &f : facilities) {
      std::string sql =
          "INSERT INTO `facility`(`id`,`snapshot_id`,`tail_cubby_id`) VALUES(" +
          std::to_string(f.id) + "," + sid + "," +
          std::to_string(f.tail_cubby_id) + ")";
      auto rx = exec(sql);
      if (!rx.ok)
        return rx;
    }
    for (const auto &c : cubbies) {
      std::string sql =
          "INSERT INTO `cubby`(`id`,`snapshot_id`,`facility_id`,`tier`,`capacity`,"
          "`size`,`is_tail`) VALUES(" +
          std::to_string(c.id) + "," + sid + "," + std::to_string(c.facility_id) +
          "," + std::to_string(c.tier) + "," + std::to_string(c.capacity) + "," +
          std::to_string(c.size) + "," + std::string(c.is_tail ? "1" : "0") +
          ")";
      auto rx = exec(sql);
      if (!rx.ok)
        return rx;
    }

    constexpr size_t kBatch = 400;
    for (size_t off = 0; off < slots.size(); off += kBatch) {
      const size_t end = std::min(slots.size(), off + kBatch);
      std::ostringstream oss;
      oss << "INSERT INTO `slot_snapshot`(`snapshot_id`,`cubby_id`,`slot_index`,"
             "`occupied`,`key_hash`,`probe_length`) VALUES ";
      for (size_t i = off; i < end; i++) {
        if (i > off)
          oss << ",";
        const auto &s = slots[i];
        oss << "(" << sid << "," << s.cubby_id << "," << s.slot_index << ","
            << (s.occupied ? 1 : 0) << "," << s.key_hash << ","
            << s.probe_length << ")";
      }
      auto rx = exec(oss.str());
      if (!rx.ok)
        return rx;
    }

    for (const auto &t : tier_stats) {
      std::string sql =
          "INSERT INTO `tier_stat`(`snapshot_id`,`facility_id`,`tier`,`cubby_count`)"
          " VALUES(" +
          sid + "," + std::to_string(t.facility_id) + "," +
          std::to_string(t.tier) + "," + std::to_string(t.cubby_count) + ")";
      auto rx = exec(sql);
      if (!rx.ok)
        return rx;
    }
    return {true, ""};
#endif
  }

  std::string analytics_list_snapshots_json() override {
#if !OTSH_HAVE_MYSQL
    return "[]";
#else
    std::lock_guard<std::mutex> lk(mu_);
    if (!conn_)
      return "[]";
    const char *sql =
        "SELECT `id`,`snapshot_tag`,UNIX_TIMESTAMP(`created_at`),`init_param_n`,`table_n`,`table_k` "
        "FROM `snapshot_meta` ORDER BY `id` DESC LIMIT 128";
    if (mysql_query(conn_, sql) != 0)
      return "[]";
    MYSQL_RES *res = mysql_store_result(conn_);
    if (!res)
      return "[]";
    std::ostringstream out;
    out << "[";
    MYSQL_ROW row;
    bool first = true;
    while ((row = mysql_fetch_row(res)) != nullptr) {
      if (!row[0])
        continue;
      if (!first)
        out << ",";
      first = false;
      out << "{\"id\":" << (row[0] ? row[0] : "0") << ",\"snapshot_tag\":";
      if (row[1]) {
        out << "\"";
        for (const char *p = row[1]; *p; ++p) {
          if (*p == '"' || *p == '\\')
            out << '\\';
          out << *p;
        }
        out << "\"";
      } else {
        out << "null";
      }
      out << ",\"created_at\":" << (row[2] ? row[2] : "0")
          << ",\"n\":" << (row[3] ? row[3] : "0")
          << ",\"N\":" << (row[4] ? row[4] : "0")
          << ",\"K\":" << (row[5] ? row[5] : "0") << "}";
    }
    mysql_free_result(res);
    out << "]";
    return out.str();
#endif
  }

  std::string analytics_summary_json(int64_t snapshot_id) override {
#if !OTSH_HAVE_MYSQL
    (void)snapshot_id;
    return "{}";
#else
    std::lock_guard<std::mutex> lk(mu_);
    if (!conn_ || snapshot_id <= 0)
      return "{}";
    const std::string sid = std::to_string(snapshot_id);
    std::ostringstream out;
    out << "{\"snapshot_id\":" << sid;

    auto q1 = [&](const char *sql, const char *key) {
      if (mysql_query(conn_, sql) != 0) {
        out << ",\"" << key << "\":null";
        return;
      }
      MYSQL_RES *res = mysql_store_result(conn_);
      if (!res || mysql_num_rows(res) == 0) {
        if (res)
          mysql_free_result(res);
        out << ",\"" << key << "\":null";
        return;
      }
      MYSQL_ROW row = mysql_fetch_row(res);
      out << ",\"" << key << "\":" << (row && row[0] ? row[0] : "null");
      mysql_free_result(res);
    };

    std::string s1 = "SELECT AVG(`probe_count`) FROM `otsh_op_metrics` WHERE "
                     "`snapshot_id`=" +
                     sid;
    q1(s1.c_str(), "avg_probe");

    std::string s2 =
        "SELECT `cubby_tier`,AVG(`probe_count`) AS a FROM `otsh_op_metrics` WHERE "
        "`snapshot_id`=" +
        sid + " GROUP BY `cubby_tier` ORDER BY `cubby_tier`";
    if (mysql_query(conn_, s2.c_str()) != 0) {
      out << ",\"probe_by_tier\":[]";
    } else {
      MYSQL_RES *res = mysql_store_result(conn_);
      out << ",\"probe_by_tier\":[";
      if (res) {
        MYSQL_ROW row;
        bool first = true;
        while ((row = mysql_fetch_row(res)) != nullptr) {
          if (!first)
            out << ",";
          first = false;
          out << "{\"cubby_tier\":" << (row[0] ? row[0] : "0")
              << ",\"avg_probe\":" << (row[1] ? row[1] : "0") << "}";
        }
        mysql_free_result(res);
      }
      out << "]";
    }

    std::string s3 = "SELECT `tier`,AVG(`size`*1.0/`capacity`) AS lf FROM "
                     "`cubby` WHERE `snapshot_id`=" +
                     sid + " AND `capacity`>0 GROUP BY `tier` ORDER BY `tier`";
    if (mysql_query(conn_, s3.c_str()) != 0) {
      out << ",\"cubby_load_by_tier\":[]";
    } else {
      MYSQL_RES *res = mysql_store_result(conn_);
      out << ",\"cubby_load_by_tier\":[";
      if (res) {
        MYSQL_ROW row;
        bool first = true;
        while ((row = mysql_fetch_row(res)) != nullptr) {
          if (!first)
            out << ",";
          first = false;
          out << "{\"tier\":" << (row[0] ? row[0] : "0")
              << ",\"avg_load_factor\":" << (row[1] ? row[1] : "0") << "}";
        }
        mysql_free_result(res);
      }
      out << "]";
    }

    std::string s4 = "SELECT `probe_count`,COUNT(*) AS c FROM `otsh_op_metrics` WHERE "
                     "`snapshot_id`=" +
                     sid + " GROUP BY `probe_count` ORDER BY `probe_count` LIMIT 64";
    if (mysql_query(conn_, s4.c_str()) != 0) {
      out << ",\"probe_hist\":[]";
    } else {
      MYSQL_RES *res = mysql_store_result(conn_);
      out << ",\"probe_hist\":[";
      if (res) {
        MYSQL_ROW row;
        bool first = true;
        while ((row = mysql_fetch_row(res)) != nullptr) {
          if (!first)
            out << ",";
          first = false;
          out << "{\"probe_count\":" << (row[0] ? row[0] : "0")
              << ",\"count\":" << (row[1] ? row[1] : "0") << "}";
        }
        mysql_free_result(res);
      }
      out << "]";
    }

    std::string s5 = "SELECT `kick_count`,COUNT(*) AS c FROM `otsh_op_metrics` WHERE "
                     "`snapshot_id`=" +
                     sid + " GROUP BY `kick_count` ORDER BY `kick_count` LIMIT 64";
    if (mysql_query(conn_, s5.c_str()) != 0) {
      out << ",\"kick_hist\":[]";
    } else {
      MYSQL_RES *res = mysql_store_result(conn_);
      out << ",\"kick_hist\":[";
      if (res) {
        MYSQL_ROW row;
        bool first = true;
        while ((row = mysql_fetch_row(res)) != nullptr) {
          if (!first)
            out << ",";
          first = false;
          out << "{\"kick_count\":" << (row[0] ? row[0] : "0")
              << ",\"count\":" << (row[1] ? row[1] : "0") << "}";
        }
        mysql_free_result(res);
      }
      out << "]";
    }

    out << "}";
    return out.str();
#endif
  }

  StorageResult append_test_session_log(const std::string &json_line) override {
#if !OTSH_HAVE_MYSQL
    (void)json_line;
    return {false, "mysql_not_available"};
#else
    std::lock_guard<std::mutex> lk(mu_);
    if (!conn_)
      return {false, "not_open"};
    const std::string esc = sql_escape_tag(conn_, json_line);
    const std::string sql =
        "INSERT INTO `otsh_test_session_log`(`body`) VALUES (" + esc + ")";
    return exec(sql);
#endif
  }

private:
#if OTSH_HAVE_MYSQL
  StorageResult ensure_analytics_ddl() {
    const char *stmts[] = {
        "CREATE TABLE IF NOT EXISTS `snapshot_meta`("
        " `id` BIGINT NOT NULL AUTO_INCREMENT,"
        " `snapshot_tag` VARCHAR(64),"
        " `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        // Windows 上 lower_case_table_names 常使 `n` 与 `N` 被视为重复列名。
        " `init_param_n` BIGINT NOT NULL,"
        " `table_n` BIGINT UNSIGNED NOT NULL,"
        " `table_k` BIGINT UNSIGNED NOT NULL,"
        " PRIMARY KEY (`id`)"
        ") ENGINE=InnoDB",
        "CREATE TABLE IF NOT EXISTS `facility`("
        " `id` INT NOT NULL,"
        " `snapshot_id` BIGINT NOT NULL,"
        " `tail_cubby_id` INT NOT NULL,"
        " PRIMARY KEY (`id`,`snapshot_id`),"
        " KEY `ix_fac_snap` (`snapshot_id`)"
        ") ENGINE=InnoDB",
        "CREATE TABLE IF NOT EXISTS `cubby`("
        " `id` INT NOT NULL,"
        " `snapshot_id` BIGINT NOT NULL,"
        " `facility_id` INT NOT NULL,"
        " `tier` INT NOT NULL,"
        " `capacity` INT NOT NULL,"
        " `size` INT NOT NULL,"
        " `is_tail` TINYINT(1) NOT NULL,"
        " PRIMARY KEY (`id`,`snapshot_id`),"
        " KEY `ix_cub_snap` (`snapshot_id`)"
        ") ENGINE=InnoDB",
        "CREATE TABLE IF NOT EXISTS `slot_snapshot`("
        " `id` BIGINT NOT NULL AUTO_INCREMENT,"
        " `snapshot_id` BIGINT NOT NULL,"
        " `cubby_id` INT NOT NULL,"
        " `slot_index` INT NOT NULL,"
        " `occupied` TINYINT(1) NOT NULL,"
        " `key_hash` BIGINT UNSIGNED NULL,"
        " `probe_length` INT NOT NULL,"
        " PRIMARY KEY (`id`),"
        " KEY `ix_slot_snap` (`snapshot_id`),"
        " KEY `ix_slot_cubby` (`snapshot_id`,`cubby_id`)"
        ") ENGINE=InnoDB",
        "CREATE TABLE IF NOT EXISTS `tier_stat`("
        " `snapshot_id` BIGINT NOT NULL,"
        " `facility_id` INT NOT NULL,"
        " `tier` INT NOT NULL,"
        " `cubby_count` INT NOT NULL,"
        " PRIMARY KEY (`snapshot_id`,`facility_id`,`tier`)"
        ") ENGINE=InnoDB",
        "CREATE TABLE IF NOT EXISTS `otsh_op_metrics`("
        " `id` BIGINT NOT NULL AUTO_INCREMENT,"
        " `snapshot_id` BIGINT NOT NULL,"
        " `operation_type` ENUM('insert','query','delete') NOT NULL,"
        " `probe_count` INT NOT NULL,"
        " `kick_count` INT NOT NULL,"
        " `latency_ns` BIGINT NOT NULL,"
        " `cubby_tier` INT NOT NULL,"
        " `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        " PRIMARY KEY (`id`),"
        " KEY `ix_met_snap` (`snapshot_id`)"
        ") ENGINE=InnoDB",
        "CREATE TABLE IF NOT EXISTS `otsh_test_session_log`("
        " `id` BIGINT NOT NULL AUTO_INCREMENT,"
        " `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        " `body` MEDIUMTEXT NOT NULL,"
        " PRIMARY KEY (`id`)"
        ") ENGINE=InnoDB",
    };
    for (const char *s : stmts) {
      auto r = exec(s);
      if (!r.ok)
        return r;
    }
    return {true, ""};
  }

  void flush_metrics_locked(bool force) {
    if (!conn_)
      return;
    if (!force && metrics_buf_.size() < 1000)
      return;
    if (metrics_buf_.empty())
      return;
    constexpr size_t kChunk = 500;
    std::vector<SqlMetricRow> batch = std::move(metrics_buf_);
    metrics_buf_.clear();
    for (size_t off = 0; off < batch.size(); off += kChunk) {
      const size_t end = std::min(batch.size(), off + kChunk);
      std::ostringstream oss;
      oss << "INSERT INTO `otsh_op_metrics`(`snapshot_id`,`operation_type`,`probe_count`,"
             "`kick_count`,`latency_ns`,`cubby_tier`) VALUES ";
      for (size_t i = off; i < end; i++) {
        if (i > off)
          oss << ",";
        const SqlMetricRow &m = batch[i];
        oss << "(" << m.snapshot_id << ",'" << m.operation_type << "',"
            << m.probe_count << "," << m.kick_count << "," << m.latency_ns << ","
            << m.cubby_tier << ")";
      }
      if (!exec(oss.str()).ok) {
        for (size_t i = off; i < batch.size(); ++i)
          metrics_buf_.push_back(batch[i]);
        return;
      }
    }
  }

  StorageResult exec(const std::string &sql) {
    if (!conn_)
      return {false, "not_open"};
    if (mysql_query(conn_, sql.c_str()) != 0) {
      return {false, "mysql_query_failed: " + std::string(mysql_error(conn_))};
    }
    MYSQL_RES *res = mysql_store_result(conn_);
    if (res)
      mysql_free_result(res);
    return {true, ""};
  }
#endif

  std::mutex mu_;
  std::string host_;
  uint16_t port_ = 3306;
  std::string user_;
  std::string pass_;
  std::string db_;
  std::string table_;

#if OTSH_HAVE_MYSQL
  MYSQL *conn_ = nullptr;
  std::vector<SqlMetricRow> metrics_buf_;
#endif
};

std::unique_ptr<IStorage> make_storage_mysql() {
  return std::make_unique<MySqlStorage>();
}

} // namespace otsh
