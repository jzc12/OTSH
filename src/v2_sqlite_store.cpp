#include "otsh/v2_sqlite_store.h"

#include "hash.h"
#include "ht.h"
#include "otsh/system_params.h"
#include "sqlite3.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace otsh
{
    namespace
    {
        uint64_t file_size_safe(const std::string &p)
        {
            if (p.empty() || p == ":memory:")
                return 0;
            std::error_code ec;
            const auto sz = std::filesystem::file_size(p, ec);
            return ec ? 0 : static_cast<uint64_t>(sz);
        }
    } // namespace

    class V2SqliteStore::Impl
    {
    public:
        Impl() = default;
        ~Impl() { close(); }

        OpResult init(const StoreParams &p)
        {
            close();
            std::lock_guard<std::mutex> lk(mu_);

            params_ = p;
            derived_ = derive_params(p.table);
            N_ = derived_.N;
            K_ = derived_.K;
            n_facilities_ = std::max<uint64_t>(1, N_ / std::max<uint64_t>(1, K_));

            uint64_t seed = static_cast<uint64_t>(
                std::chrono::high_resolution_clock::now().time_since_epoch().count());
            seed ^= p.table.seed1 ^ (p.table.seed2 << 1) ^ (p.table.seed3 << 2);
            pi_.k1 = splitmix64(seed ^ 0x1111111111111111ULL);
            pi_.k2 = splitmix64(seed ^ 0x2222222222222222ULL);
            pi_.k3 = splitmix64(seed ^ 0x3333333333333333ULL);
            pi_.k4 = splitmix64(seed ^ 0x4444444444444444ULL);

            n_.store(0);
            sqlite_hits_.store(0);
            background_drain_ns_.store(0);

            db_path_ = p.sqlite_path.empty() ? ":memory:" : p.sqlite_path;
            if (db_path_ != ":memory:")
            {
                std::error_code ec;
                std::filesystem::remove(db_path_, ec);
                std::filesystem::remove(db_path_ + "-wal", ec);
                std::filesystem::remove(db_path_ + "-shm", ec);
                std::filesystem::remove(db_path_ + "-journal", ec);
                if (auto par = std::filesystem::path(db_path_).parent_path();
                    !par.empty())
                    std::filesystem::create_directories(par, ec);
            }
            const int rc = sqlite3_open_v2(
                db_path_.c_str(), &db_,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
            if (rc != SQLITE_OK)
                return {false, std::string("sqlite3_open: ") + sqlite3_errstr(rc)};

            const char *pragmas =
                "PRAGMA journal_mode = WAL;"
                "PRAGMA synchronous  = NORMAL;"
                "PRAGMA cache_size   = -65536;"
                "PRAGMA temp_store   = MEMORY;"
                "PRAGMA mmap_size    = 268435456;"
                "PRAGMA wal_autocheckpoint = 1000;";
            std::string err;
            if (!exec_(pragmas, err))
                return {false, "pragma: " + err};

            // 按 facility r 建表，一次性事务
            if (!exec_("BEGIN IMMEDIATE", err))
                return {false, "begin ddl: " + err};
            for (size_t r = 0; r < n_facilities_; ++r)
            {
                char sql[160];
                std::snprintf(sql, sizeof(sql),
                              "CREATE TABLE IF NOT EXISTS facility_%zu "
                              "(key INTEGER PRIMARY KEY) WITHOUT ROWID;",
                              r);
                if (!exec_(sql, err))
                {
                    exec_("ROLLBACK", err);
                    return {false, "create: " + err};
                }
            }
            if (!exec_("COMMIT", err))
                return {false, "commit ddl: " + err};

            st_insert_.assign(n_facilities_, nullptr);
            st_query_.assign(n_facilities_, nullptr);
            st_delete_.assign(n_facilities_, nullptr);
            for (size_t r = 0; r < n_facilities_; ++r)
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "INSERT OR IGNORE INTO facility_%zu(key) VALUES(?1)", r);
                if (!prepare_(buf, &st_insert_[r], err))
                    return {false, "prep ins: " + err};
                std::snprintf(buf, sizeof(buf),
                              "SELECT 1 FROM facility_%zu WHERE key=?1", r);
                if (!prepare_(buf, &st_query_[r], err))
                    return {false, "prep qry: " + err};
                std::snprintf(buf, sizeof(buf),
                              "DELETE FROM facility_%zu WHERE key=?1", r);
                if (!prepare_(buf, &st_delete_[r], err))
                    return {false, "prep del: " + err};
            }
            return {true, ""};
        }

        InsertResult insert(uint64_t key)
        {
            InsertResult r;
            if (!db_)
            {
                r.ok = false;
                r.error = "not_initialized";
                return r;
            }
            const size_t fi = facility_of(pi_.pi(key));
            std::lock_guard<std::mutex> lk(mu_);
            sqlite3_stmt *s = st_insert_[fi];
            sqlite3_reset(s);
            sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(key));
            const int rc = sqlite3_step(s);
            sqlite3_reset(s);
            if (rc != SQLITE_DONE)
            {
                r.ok = false;
                r.error = sqlite3_errmsg(db_);
                return r;
            }
            r.ok = true;
            r.inserted = (sqlite3_changes(db_) > 0);
            if (r.inserted)
                n_.fetch_add(1, std::memory_order_relaxed);
            r.router_probe_steps = 1;
            r.kick_count = 0;
            r.cubby_tier = -1;
            return r;
        }

        QueryResult query(uint64_t key) const
        {
            QueryResult r;
            if (!db_)
            {
                r.ok = false;
                r.error = "not_initialized";
                return r;
            }
            const size_t fi = facility_of(pi_.pi(key));
            std::lock_guard<std::mutex> lk(mu_);
            sqlite3_stmt *s = st_query_[fi];
            sqlite3_reset(s);
            sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(key));
            const int rc = sqlite3_step(s);
            r.ok = true;
            r.router_probe_steps = 1;
            if (rc == SQLITE_ROW)
            {
                r.found = true;
                sqlite_hits_.fetch_add(1, std::memory_order_relaxed);
            }
            else if (rc != SQLITE_DONE)
            {
                r.ok = false;
                r.error = sqlite3_errmsg(db_);
            }
            sqlite3_reset(s);
            return r;
        }

        DeleteResult erase(uint64_t key)
        {
            DeleteResult r;
            if (!db_)
            {
                r.ok = false;
                r.error = "not_initialized";
                return r;
            }
            const size_t fi = facility_of(pi_.pi(key));
            std::lock_guard<std::mutex> lk(mu_);
            sqlite3_stmt *s = st_delete_[fi];
            sqlite3_reset(s);
            sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(key));
            const int rc = sqlite3_step(s);
            sqlite3_reset(s);
            if (rc != SQLITE_DONE)
            {
                r.ok = false;
                r.error = sqlite3_errmsg(db_);
                return r;
            }
            r.ok = true;
            r.deleted = (sqlite3_changes(db_) > 0);
            if (r.deleted && n_.load() > 0)
                n_.fetch_sub(1, std::memory_order_relaxed);
            r.router_probe_steps = 1;
            r.cubby_tier = -1;
            return r;
        }

        OpResult bulk_load(const std::vector<uint64_t> &keys)
        {
            if (!db_)
                return {false, "not_initialized"};
            // 简化：把 keys 按 r 分桶后一桶一个事务。
            std::vector<std::vector<uint64_t>> by_r(n_facilities_);
            for (uint64_t k : keys)
                by_r[facility_of(pi_.pi(k))].push_back(k);

            std::lock_guard<std::mutex> lk(mu_);
            std::string err;
            for (size_t r = 0; r < n_facilities_; ++r)
            {
                if (by_r[r].empty())
                    continue;
                if (!exec_("BEGIN IMMEDIATE", err))
                    return {false, "begin: " + err};
                sqlite3_stmt *s = st_insert_[r];
                uint64_t added = 0;
                for (uint64_t k : by_r[r])
                {
                    sqlite3_reset(s);
                    sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(k));
                    if (sqlite3_step(s) != SQLITE_DONE)
                    {
                        std::string e = sqlite3_errmsg(db_);
                        exec_("ROLLBACK", err);
                        return {false, "step: " + e};
                    }
                    if (sqlite3_changes(db_) > 0)
                        ++added;
                }
                sqlite3_reset(s);
                if (!exec_("COMMIT", err))
                    return {false, "commit: " + err};
                n_.fetch_add(added, std::memory_order_relaxed);
            }
            return {true, ""};
        }

        void drain_background_work()
        {
            if (!db_)
                return;
            const auto t0 = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lk(mu_);
            sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_TRUNCATE,
                                      nullptr, nullptr);
            const auto t1 = std::chrono::steady_clock::now();
            background_drain_ns_.fetch_add(
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()),
                std::memory_order_relaxed);
        }

        StoreStats stats() const
        {
            StoreStats s;
            s.n = n_.load();
            s.mem_meta_bits = 0;
            s.sqlite_hits = sqlite_hits_.load();
            s.background_drain_ns = background_drain_ns_.load();
            // 仅主 DB 计入持久化字节；WAL+SHM 是运行时开销，单独报告。
            s.disk_file_bytes = file_size_safe(db_path_);
            s.runtime_overhead_bytes = file_size_safe(db_path_ + "-wal") +
                                       file_size_safe(db_path_ + "-shm");
            return s;
        }

        void close()
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto &p : st_insert_)
                if (p)
                    sqlite3_finalize(p);
            for (auto &p : st_query_)
                if (p)
                    sqlite3_finalize(p);
            for (auto &p : st_delete_)
                if (p)
                    sqlite3_finalize(p);
            st_insert_.clear();
            st_query_.clear();
            st_delete_.clear();
            if (db_)
            {
                sqlite3_close(db_);
                db_ = nullptr;
            }
        }

    private:
        bool exec_(const char *sql, std::string &err)
        {
            char *emsg = nullptr;
            const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &emsg);
            if (rc != SQLITE_OK)
            {
                err = emsg ? emsg : sqlite3_errstr(rc);
                if (emsg)
                    sqlite3_free(emsg);
                return false;
            }
            return true;
        }
        bool prepare_(const char *sql, sqlite3_stmt **stmt, std::string &err)
        {
            const int rc = sqlite3_prepare_v2(db_, sql, -1, stmt, nullptr);
            if (rc != SQLITE_OK)
            {
                err = sqlite3_errmsg(db_);
                return false;
            }
            return true;
        }
        size_t facility_of(uint64_t gx) const
        {
            if (n_facilities_ == 0 || K_ == 0 || N_ < 2)
                return 0;
            const uint64_t g = gx & (N_ - 1);
            return static_cast<size_t>((g / K_) % n_facilities_);
        }

        StoreParams params_{};
        DerivedParams derived_{};
        PermutationHash pi_{};
        uint64_t N_ = 0, K_ = 0, n_facilities_ = 0;

        sqlite3 *db_ = nullptr;
        std::string db_path_;
        std::vector<sqlite3_stmt *> st_insert_;
        std::vector<sqlite3_stmt *> st_query_;
        std::vector<sqlite3_stmt *> st_delete_;

        mutable std::mutex mu_;
        std::atomic<uint64_t> n_{0};
        mutable std::atomic<uint64_t> sqlite_hits_{0};
        std::atomic<uint64_t> background_drain_ns_{0};
    };

    V2SqliteStore::V2SqliteStore() : impl_(std::make_unique<Impl>()) {}
    V2SqliteStore::~V2SqliteStore() = default;

    OpResult V2SqliteStore::init(const StoreParams &p) { return impl_->init(p); }
    InsertResult V2SqliteStore::insert(uint64_t key) { return impl_->insert(key); }
    QueryResult V2SqliteStore::query(uint64_t key) const { return impl_->query(key); }
    DeleteResult V2SqliteStore::erase(uint64_t key) { return impl_->erase(key); }
    OpResult V2SqliteStore::bulk_load(const std::vector<uint64_t> &keys)
    {
        return impl_->bulk_load(keys);
    }
    void V2SqliteStore::drain_background_work() { impl_->drain_background_work(); }
    StoreStats V2SqliteStore::stats() const { return impl_->stats(); }

} // namespace otsh