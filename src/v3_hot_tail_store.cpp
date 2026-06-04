#include "otsh/v3_hot_tail_store.h"

#include "hash.h"
#include "otsh/cubby.h"
#include "otsh/kkick.h"
#include "otsh/meta_entry.h"
#include "otsh/system_params.h"
#include "sqlite3.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace otsh
{

    // ====================================================================
    //  Cubby 操作的最小复用集（裁剪自 HashTable::Impl，去掉 facility-level Router 依赖）
    // ====================================================================
    namespace v3_internal
    {
        static void init_free_slots(Cubby &c)
        {
            const size_t cap = std::min(
                c.capacity,
                static_cast<size_t>((std::numeric_limits<int>::max)()));
            c.free_slots.capacity = static_cast<int>(cap);
            c.free_slots.build();
        }

        static std::unique_ptr<Cubby> create_cubby(
            int k_kick, size_t capacity, uint64_t K, uint64_t n_hint,
            int fanout, int node_max_bits)
        {
            auto c = std::make_unique<Cubby>();
            c->tier = 1;
            c->capacity = capacity;
            c->size = 0;
            c->slots.assign(c->capacity, std::nullopt);
            c->occupied.clear();
            c->occupied.reserve(c->capacity);
            c->array_a.assign(c->capacity, PrefixRouter{});
            c->array_b.configure(fanout, node_max_bits);
            c->array_b.reset(c->capacity);
            c->kick_geom = std::make_unique<KKickGeometry>(
                k_kick, c->capacity, K, n_hint);
            c->array_m.reset(c->kick_geom.get(), c->capacity);
            init_free_slots(*c);
            return c;
        }

        static void remove_occupied(Cubby &c, size_t slot)
        {
            auto it = std::find(c.occupied.begin(), c.occupied.end(), slot);
            if (it != c.occupied.end())
            {
                *it = c.occupied.back();
                c.occupied.pop_back();
            }
        }

        static void clear_slot(Cubby &c, size_t slot)
        {
            c.slots[slot].reset();
            c.array_b.erase(slot);
            c.array_m.mark_free(slot);
        }

        static size_t preferred_router_bucket(uint64_t gx, uint64_t K, size_t cap)
        {
            if (K == 0 || cap == 0)
                return 0;
            const uint64_t g_k = gx & (K - 1);
            return static_cast<size_t>(g_k % cap);
        }

        static void local_router_put(Cubby &c, uint64_t gx, uint64_t K, uint32_t probe_j)
        {
            const size_t b = preferred_router_bucket(gx, K, c.capacity);
            if (b < c.array_a.size())
                c.array_a[b].insert(gx, probe_j);
        }

        static void local_router_erase(Cubby &c, uint64_t gx, uint64_t K)
        {
            const size_t b = preferred_router_bucket(gx, K, c.capacity);
            if (b < c.array_a.size())
                c.array_a[b].erase(gx);
        }

        static bool decode_slot_meta(const Cubby &c, size_t slot, uint64_t K, MetaEntry &me)
        {
            const uint32_t bl = c.array_b.bitlen(slot);
            if (bl == 0)
                return false;
            const auto bits = c.array_b.access(slot);
            return decode_meta_entry(bits, bl, meta_layout(K, c.capacity), me);
        }

        static std::optional<uint64_t> recover_key_from_slot(
            const Cubby &c, size_t slot, uint64_t r, uint64_t N, uint64_t K,
            const PermutationHash &pi)
        {
            if (slot >= c.slots.size() || !c.slots[slot].has_value())
                return std::nullopt;
            MetaEntry me;
            if (!decode_slot_meta(c, slot, K, me))
                return std::nullopt;
            const auto gx = reconstruct_gx_pi(
                r, N, K, c.capacity, slot, *c.slots[slot], me, log2_pow2(N));
            if (!gx)
                return std::nullopt;
            return pi.inverse(*gx);
        }

        static bool slot_pi_matches(
            const Cubby &c, size_t slot, uint64_t r, uint64_t N, uint64_t K,
            uint64_t key_gx)
        {
            if (slot >= c.slots.size() || !c.slots[slot].has_value())
                return false;
            MetaEntry me;
            if (!decode_slot_meta(c, slot, K, me))
                return false;
            const auto gx = reconstruct_gx_pi(
                r, N, K, c.capacity, slot, *c.slots[slot], me, log2_pow2(N));
            return gx && *gx == key_gx;
        }

        static void put_quotient_slot(
            Cubby &c, size_t slot, uint64_t key, uint64_t gx_pi,
            uint64_t r, uint64_t N, uint64_t K, uint32_t ins)
        {
            const uint32_t ln = log2_pow2(N);
            const MetaCodecLayout ly = meta_layout(K, c.capacity);
            const MetaEntry me = make_meta_entry(key, gx_pi, slot, r, N, K, c.capacity, ins);
            auto pr = encode_meta_entry(me, ly);
            c.slots[slot] = quotient_payload(gx_pi, ln);
            c.array_b.update(slot, pr.first, pr.second);
        }

        // 在 cubby 内查 key（基于本地 array_a 路由）。命中返回槽位，否则 nullopt。
        static std::optional<size_t> lookup_in_cubby(
            const Cubby &c, uint64_t gx, uint64_t r, uint64_t N, uint64_t K,
            const PermutationHash &pi)
        {
            (void)pi;
            if (!c.kick_geom)
                return std::nullopt;
            const size_t rb = preferred_router_bucket(gx, K, c.capacity);
            if (rb >= c.array_a.size())
                return std::nullopt;
            const auto j_opt = c.array_a[rb].query(gx);
            if (j_opt)
            {
                const auto slot_opt = probe_j_to_slot(*c.kick_geom, gx, *j_opt);
                if (slot_opt && slot_pi_matches(c, *slot_opt, r, N, K, gx))
                    return slot_opt;
            }
            return std::nullopt;
        }

        struct KKickHelpers
        {
            uint64_t N;
            uint64_t K;
            uint64_t r;
            const PermutationHash *pi;

            void wire(Cubby &c, KKickReadSlot &read_slot, KKickWriteSlot &write_slot,
                      KKickClearSlot &clear_slot_cb,
                      std::function<uint32_t(int, uint64_t)> &random_depth,
                      std::function<uint64_t(uint64_t)> &gx_of_key,
                      int k_kick) const
            {
                const uint64_t r_ = r;
                const uint64_t N_ = N;
                const uint64_t K_ = K;
                const PermutationHash *pi_ = pi;

                read_slot = [&c, r_, N_, K_, pi_](size_t slot, std::optional<uint64_t> *kout) -> KKickSlotView
                {
                    KKickSlotView v{};
                    if (slot >= c.slots.size() || !c.slots[slot].has_value())
                    {
                        if (kout)
                            *kout = std::nullopt;
                        return v;
                    }
                    v.occupied = true;
                    if (kout)
                        *kout = recover_key_from_slot(c, slot, r_, N_, K_, *pi_);
                    MetaEntry me;
                    if (decode_slot_meta(c, slot, K_, me))
                        v.insert_depth = me.insert_bits & 0x0fu;
                    return v;
                };

                write_slot = [&c, r_, N_, K_, pi_](size_t slot, uint64_t key,
                                                   uint32_t depth, uint32_t probe_j) -> bool
                {
                    if (slot >= c.capacity)
                        return false;
                    const bool was = c.slots[slot].has_value();
                    if (was)
                    {
                        if (auto oldk = recover_key_from_slot(c, slot, r_, N_, K_, *pi_))
                            local_router_erase(c, pi_->pi(*oldk), K_);
                    }
                    else
                    {
                        c.occupied.push_back(slot);
                        c.size++;
                        c.free_slots.mark_used(static_cast<int>(slot));
                        c.array_m.mark_used(slot);
                    }
                    const uint64_t gxc = pi_->pi(key);
                    const uint32_t ins = (depth & 0x0fu) | ((probe_j & 0x0fffu) << 4);
                    put_quotient_slot(c, slot, key, gxc, r_, N_, K_, ins);
                    local_router_put(c, gxc, K_, probe_j);
                    return true;
                };

                clear_slot_cb = [&c, r_, N_, K_, pi_](size_t slot)
                {
                    if (slot >= c.slots.size() || !c.slots[slot].has_value())
                        return;
                    if (auto oldk = recover_key_from_slot(c, slot, r_, N_, K_, *pi_))
                        local_router_erase(c, pi_->pi(*oldk), K_);
                    c.slots[slot].reset();
                    c.array_b.erase(slot);
                    remove_occupied(c, slot);
                    c.free_slots.mark_free(static_cast<int>(slot));
                    c.array_m.mark_free(slot);
                    c.size--;
                };

                const PermutationHash *pi_for_rand = pi;
                random_depth = [pi_for_rand](int max_d, uint64_t gx_pi) -> uint32_t
                {
                    const uint64_t h = splitmix64(gx_pi ^ pi_for_rand->k4 ^ 0x9e3779b97f4a7c15ULL);
                    return static_cast<uint32_t>(h % static_cast<uint64_t>(max_d + 1));
                };

                gx_of_key = [pi_](uint64_t key)
                { return pi_->pi(key); };
                (void)k_kick;
            }
        };
    } // namespace v3_internal

    using namespace v3_internal;

    // ====================================================================
    //  ColdStore：抽象冷存储后端。
    //  PR-3 用 InMemoryColdStore（per-facility 内存 set）。
    //  PR-4 用 SqliteColdStore（per-facility SQLite 表 facility_<r>）。
    // ====================================================================
    class ColdStore
    {
    public:
        virtual ~ColdStore() = default;
        virtual OpResult open(const StoreParams &p, uint64_t n_facilities) = 0;
        virtual void close() = 0;
        virtual bool contains(size_t r, uint64_t key) = 0;
        virtual bool erase(size_t r, uint64_t key) = 0;
        virtual void bulk_insert(size_t r, const std::vector<uint64_t> &keys) = 0;
        virtual void drain() {}
        // 仅持久化字节（主 DB），不含 WAL/SHM 运行时开销。
        virtual uint64_t disk_bytes() const { return 0; }
        // WAL+SHM 等运行时占用（drain 前可见，close 后会被 SQLite 自动清理）。
        virtual uint64_t runtime_overhead_bytes() const { return 0; }
    };

    class InMemoryColdStore : public ColdStore
    {
    public:
        OpResult open(const StoreParams &, uint64_t n_facilities) override
        {
            sets_.assign(static_cast<size_t>(n_facilities), {});
            return {true, ""};
        }
        void close() override { sets_.clear(); }
        bool contains(size_t r, uint64_t key) override
        {
            return r < sets_.size() && sets_[r].count(key) != 0;
        }
        bool erase(size_t r, uint64_t key) override
        {
            return r < sets_.size() && sets_[r].erase(key) > 0;
        }
        void bulk_insert(size_t r, const std::vector<uint64_t> &keys) override
        {
            if (r >= sets_.size())
                return;
            for (uint64_t k : keys)
                sets_[r].insert(k);
        }

    private:
        std::vector<std::unordered_set<uint64_t>> sets_;
    };

    // ====================================================================
    //  SqliteColdStore：每 facility 一张表 facility_<r>，WAL + NORMAL，
    //  prepared stmt 三件套（select / insert or ignore / delete）。
    // ====================================================================
    class SqliteColdStore : public ColdStore
    {
    public:
        ~SqliteColdStore() override { close(); }

        OpResult open(const StoreParams &p, uint64_t n_facilities) override
        {
            close();
            db_path_ = p.sqlite_path.empty() ? ":memory:" : p.sqlite_path;
            if (db_path_ != ":memory:")
            {
                std::error_code ec;
                std::filesystem::remove(db_path_, ec);
                std::filesystem::remove(db_path_ + "-wal", ec);
                std::filesystem::remove(db_path_ + "-shm", ec);
                std::filesystem::remove(db_path_ + "-journal", ec);
                if (auto par = std::filesystem::path(db_path_).parent_path(); !par.empty())
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
            char *emsg = nullptr;
            if (sqlite3_exec(db_, pragmas, nullptr, nullptr, &emsg) != SQLITE_OK)
            {
                std::string e = emsg ? emsg : "pragma";
                if (emsg)
                    sqlite3_free(emsg);
                return {false, "pragma: " + e};
            }

            n_facilities_ = static_cast<size_t>(n_facilities);
            st_insert_.assign(n_facilities_, nullptr);
            st_query_.assign(n_facilities_, nullptr);
            st_delete_.assign(n_facilities_, nullptr);

            // 一次性 DDL（所有表一起 BEGIN/COMMIT，避免逐表事务开销）
            if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &emsg) != SQLITE_OK)
            {
                std::string e = emsg ? emsg : "begin";
                if (emsg)
                    sqlite3_free(emsg);
                return {false, "begin ddl: " + e};
            }
            for (size_t r = 0; r < n_facilities_; ++r)
            {
                char sql[128];
                std::snprintf(sql, sizeof(sql),
                              "CREATE TABLE IF NOT EXISTS facility_%zu (key INTEGER PRIMARY KEY) WITHOUT ROWID;", r);
                if (sqlite3_exec(db_, sql, nullptr, nullptr, &emsg) != SQLITE_OK)
                {
                    std::string e = emsg ? emsg : "create";
                    if (emsg)
                        sqlite3_free(emsg);
                    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
                    return {false, "create: " + e};
                }
            }
            if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &emsg) != SQLITE_OK)
            {
                std::string e = emsg ? emsg : "commit";
                if (emsg)
                    sqlite3_free(emsg);
                return {false, "commit ddl: " + e};
            }

            for (size_t r = 0; r < n_facilities_; ++r)
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "INSERT OR IGNORE INTO facility_%zu(key) VALUES(?1)", r);
                if (sqlite3_prepare_v2(db_, buf, -1, &st_insert_[r], nullptr) != SQLITE_OK)
                    return {false, std::string("prep ins r=") + std::to_string(r) +
                                       ": " + sqlite3_errmsg(db_)};
                std::snprintf(buf, sizeof(buf),
                              "SELECT 1 FROM facility_%zu WHERE key=?1", r);
                if (sqlite3_prepare_v2(db_, buf, -1, &st_query_[r], nullptr) != SQLITE_OK)
                    return {false, std::string("prep qry r=") + std::to_string(r) +
                                       ": " + sqlite3_errmsg(db_)};
                std::snprintf(buf, sizeof(buf),
                              "DELETE FROM facility_%zu WHERE key=?1", r);
                if (sqlite3_prepare_v2(db_, buf, -1, &st_delete_[r], nullptr) != SQLITE_OK)
                    return {false, std::string("prep del r=") + std::to_string(r) +
                                       ": " + sqlite3_errmsg(db_)};
            }
            return {true, ""};
        }

        void close() override
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto &s : st_insert_)
                if (s)
                    sqlite3_finalize(s);
            for (auto &s : st_query_)
                if (s)
                    sqlite3_finalize(s);
            for (auto &s : st_delete_)
                if (s)
                    sqlite3_finalize(s);
            st_insert_.clear();
            st_query_.clear();
            st_delete_.clear();
            if (db_)
            {
                sqlite3_close(db_);
                db_ = nullptr;
            }
        }

        bool contains(size_t r, uint64_t key) override
        {
            if (!db_ || r >= st_query_.size())
                return false;
            std::lock_guard<std::mutex> lk(mu_);
            sqlite3_stmt *s = st_query_[r];
            sqlite3_reset(s);
            sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(key));
            const int rc = sqlite3_step(s);
            const bool found = (rc == SQLITE_ROW);
            sqlite3_reset(s);
            return found;
        }

        bool erase(size_t r, uint64_t key) override
        {
            if (!db_ || r >= st_delete_.size())
                return false;
            std::lock_guard<std::mutex> lk(mu_);
            sqlite3_stmt *s = st_delete_[r];
            sqlite3_reset(s);
            sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(key));
            const int rc = sqlite3_step(s);
            const bool ok = (rc == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
            sqlite3_reset(s);
            return ok;
        }

        void bulk_insert(size_t r, const std::vector<uint64_t> &keys) override
        {
            if (!db_ || r >= st_insert_.size() || keys.empty())
                return;
            std::lock_guard<std::mutex> lk(mu_);
            char *emsg = nullptr;
            sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &emsg);
            if (emsg)
                sqlite3_free(emsg);
            sqlite3_stmt *s = st_insert_[r];
            for (uint64_t k : keys)
            {
                sqlite3_reset(s);
                sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(k));
                if (sqlite3_step(s) != SQLITE_DONE)
                    break;
            }
            sqlite3_reset(s);
            sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &emsg);
            if (emsg)
                sqlite3_free(emsg);
        }

        void drain() override
        {
            if (!db_)
                return;
            std::lock_guard<std::mutex> lk(mu_);
            sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_TRUNCATE,
                                      nullptr, nullptr);
        }

        uint64_t disk_bytes() const override
        {
            if (db_path_.empty() || db_path_ == ":memory:")
                return 0;
            std::error_code ec;
            const auto s = std::filesystem::file_size(db_path_, ec);
            return ec ? 0 : static_cast<uint64_t>(s);
        }
        uint64_t runtime_overhead_bytes() const override
        {
            if (db_path_.empty() || db_path_ == ":memory:")
                return 0;
            auto sz = [](const std::string &p) -> uint64_t
            {
                std::error_code ec;
                const auto s = std::filesystem::file_size(p, ec);
                return ec ? 0 : static_cast<uint64_t>(s);
            };
            return sz(db_path_ + "-wal") + sz(db_path_ + "-shm");
        }

    private:
        sqlite3 *db_ = nullptr;
        std::string db_path_;
        std::vector<sqlite3_stmt *> st_insert_;
        std::vector<sqlite3_stmt *> st_query_;
        std::vector<sqlite3_stmt *> st_delete_;
        size_t n_facilities_ = 0;
        mutable std::mutex mu_;
    };

    std::unique_ptr<ColdStore> make_cold_store(const StoreParams &p);

    // ====================================================================
    //  V3HotTailStore::Impl
    // ====================================================================
    class V3HotTailStore::Impl
    {
    public:
        Impl() = default;

        OpResult init(const StoreParams &p)
        {
            std::lock_guard<std::mutex> lk(global_mu_);
            params_ = p;
            derived_ = derive_params(p.table);

            N_ = derived_.N;
            K_ = derived_.K;
            n_facilities_ = std::max<uint64_t>(1, N_ / std::max<uint64_t>(1, K_));
            fanout_ = derived_.fanout;
            node_max_bits_ = derived_.node_max_bits;
            k_kick_ = derived_.k_kick;
            frozen_queue_max_ = std::max(1, p.frozen_queue_max);

            if (p.tail_capacity_override > 0)
            {
                tail_capacity_ = static_cast<size_t>(p.tail_capacity_override);
            }
            else
            {
                // 默认 tail 容量：取较大值（V3 中 tail 是热缓存，不是 1-tiered 微 cubby）
                // 选 max(64, K/16)，并以 K 为上界
                size_t cap = std::max<size_t>(64, static_cast<size_t>(K_ / 16));
                cap = std::min<size_t>(cap, static_cast<size_t>(K_));
                tail_capacity_ = cap;
            }

            uint64_t seed = static_cast<uint64_t>(
                std::chrono::high_resolution_clock::now().time_since_epoch().count());
            seed ^= p.table.seed1 ^ (p.table.seed2 << 1) ^ (p.table.seed3 << 2);
            pi_.k1 = splitmix64(seed ^ 0x1111111111111111ULL);
            pi_.k2 = splitmix64(seed ^ 0x2222222222222222ULL);
            pi_.k3 = splitmix64(seed ^ 0x3333333333333333ULL);
            pi_.k4 = splitmix64(seed ^ 0x4444444444444444ULL);

            facs_.clear();
            facs_.reserve(static_cast<size_t>(n_facilities_));
            for (uint64_t i = 0; i < n_facilities_; ++i)
            {
                auto fp = std::make_unique<PerFacility>();
                fp->hot = create_cubby(k_kick_, tail_capacity_, K_, derived_.n_hint,
                                       fanout_, node_max_bits_);
                facs_.push_back(std::move(fp));
            }

            cold_ = make_cold_store(p);
            if (auto cr = cold_->open(p, n_facilities_); !cr.ok)
                return cr;

            n_.store(0);
            hot_hits_.store(0);
            frozen_hits_.store(0);
            cold_hits_.store(0);
            flush_count_.store(0);
            flushed_keys_.store(0);
            flush_blocked_ns_.store(0);
            background_drain_ns_.store(0);

            return {true, ""};
        }

        InsertResult insert(uint64_t key)
        {
            InsertResult r;
            if (facs_.empty())
            {
                r.ok = false;
                r.error = "not_initialized";
                return r;
            }
            r.ok = true;
            r.cubby_tier = 1;

            const uint64_t gx = pi_.pi(key);
            const size_t r_idx = facility_of(gx);
            PerFacility &f = *facs_[r_idx];

            std::unique_lock<std::mutex> lk(f.mu);

            // dedup: hot → frozen → cold (filtering tombstones)
            if (lookup_in_cubby(*f.hot, gx, r_idx, N_, K_, pi_))
            {
                r.inserted = false;
                return r;
            }
            for (auto &fc : f.frozen)
            {
                if (lookup_in_cubby(*fc, gx, r_idx, N_, K_, pi_))
                {
                    if (f.tombstones.count(key) == 0)
                    {
                        r.inserted = false;
                        return r;
                    }
                }
            }
            if (cold_contains_locked(r_idx, key))
            {
                if (f.tombstones.count(key) == 0)
                {
                    r.inserted = false;
                    return r;
                }
            }

            // 重新插入或新插入：先清 tombstone（如有）
            f.tombstones.erase(key);

            // 尝试塞入 hot；若失败 → rotate
            uint64_t kicks = 0;
            if (!try_kkick_insert(f.hot.get(), r_idx, key, gx, kicks))
            {
                rotate_locked(f, r_idx);
                if (!try_kkick_insert(f.hot.get(), r_idx, key, gx, kicks))
                {
                    r.ok = false;
                    r.error = "tail_full_after_rotate";
                    return r;
                }
            }
            r.inserted = true;
            r.kick_count = kicks;
            n_.fetch_add(1, std::memory_order_relaxed);
            return r;
        }

        QueryResult query(uint64_t key) const
        {
            QueryResult r;
            if (facs_.empty())
            {
                r.ok = false;
                r.error = "not_initialized";
                return r;
            }
            r.ok = true;
            const uint64_t gx = pi_.pi(key);
            const size_t r_idx = facility_of(gx);
            const PerFacility &f = *facs_[r_idx];

            std::lock_guard<std::mutex> lk(f.mu);
            r.router_probe_steps = 1;

            if (lookup_in_cubby(*f.hot, gx, r_idx, N_, K_, pi_))
            {
                r.found = true;
                r.cubby_tier = 1;
                hot_hits_.fetch_add(1, std::memory_order_relaxed);
                return r;
            }

            for (auto &fc : f.frozen)
            {
                r.router_probe_steps++;
                if (lookup_in_cubby(*fc, gx, r_idx, N_, K_, pi_))
                {
                    if (f.tombstones.count(key) == 0)
                    {
                        r.found = true;
                        r.cubby_tier = 1;
                        frozen_hits_.fetch_add(1, std::memory_order_relaxed);
                        return r;
                    }
                    // tombstoned → fall through to cold
                }
            }

            if (cold_contains_locked(r_idx, key) && f.tombstones.count(key) == 0)
            {
                r.found = true;
                r.cubby_tier = 0; // 标记 cold 命中
                cold_hits_.fetch_add(1, std::memory_order_relaxed);
                return r;
            }
            r.found = false;
            return r;
        }

        DeleteResult erase(uint64_t key)
        {
            DeleteResult r;
            if (facs_.empty())
            {
                r.ok = false;
                r.error = "not_initialized";
                return r;
            }
            r.ok = true;
            const uint64_t gx = pi_.pi(key);
            const size_t r_idx = facility_of(gx);
            PerFacility &f = *facs_[r_idx];

            std::lock_guard<std::mutex> lk(f.mu);

            // hot 命中：直接清，不需要 tombstone（hot 没有进入 cold）
            if (auto slot = lookup_in_cubby(*f.hot, gx, r_idx, N_, K_, pi_))
            {
                clear_in_cubby_locked(*f.hot, *slot, r_idx);
                f.tombstones.erase(key);
                n_.fetch_sub(1, std::memory_order_relaxed);
                r.deleted = true;
                r.cubby_tier = 1;
                return r;
            }

            // frozen 命中：写 tombstone（cubby 只读，不能就地清；flush 时会跳过）
            for (auto &fc : f.frozen)
            {
                if (lookup_in_cubby(*fc, gx, r_idx, N_, K_, pi_))
                {
                    if (f.tombstones.insert(key).second)
                    {
                        n_.fetch_sub(1, std::memory_order_relaxed);
                        r.deleted = true;
                        r.cubby_tier = 1;
                    }
                    return r;
                }
            }

            // cold 命中：同步删除
            if (cold_erase_locked(r_idx, key))
            {
                f.tombstones.erase(key);
                n_.fetch_sub(1, std::memory_order_relaxed);
                r.deleted = true;
                r.cubby_tier = 0;
                return r;
            }

            r.deleted = false;
            return r;
        }

        OpResult bulk_load(const std::vector<uint64_t> &keys)
        {
            for (uint64_t k : keys)
                (void)insert(k);
            return {true, ""};
        }

        void drain_background_work()
        {
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t r = 0; r < facs_.size(); ++r)
            {
                PerFacility &f = *facs_[r];
                std::unique_lock<std::mutex> lk(f.mu);
                while (!f.frozen.empty())
                    flush_oldest_locked(f, r);
            }
            // 让 cold store 也收尾（SQLite: WAL checkpoint TRUNCATE）
            if (cold_)
                cold_->drain();
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
            s.hot_hits = hot_hits_.load();
            s.frozen_hits = frozen_hits_.load();
            s.sqlite_hits = cold_hits_.load();
            s.flush_count = flush_count_.load();
            s.flushed_keys = flushed_keys_.load();
            s.flush_blocked_ns = flush_blocked_ns_.load();
            s.background_drain_ns = background_drain_ns_.load();
            s.disk_file_bytes = cold_ ? cold_->disk_bytes() : 0;
            s.runtime_overhead_bytes = cold_ ? cold_->runtime_overhead_bytes() : 0;
            std::lock_guard<std::mutex> lk(global_mu_);
            uint64_t bits = 0;
            for (auto &fp : facs_)
            {
                std::lock_guard<std::mutex> flk(fp->mu);
                auto add = [&](const Cubby *c)
                {
                    if (!c)
                        return;
                    for (size_t i = 0; i < c->slots.size(); ++i)
                        if (c->slots[i].has_value())
                            bits += c->array_b.bitlen(i);
                };
                add(fp->hot.get());
                for (auto &fc : fp->frozen)
                    add(fc.get());
            }
            s.mem_meta_bits = bits;
            return s;
        }

    public:
        struct PerFacility
        {
            std::unique_ptr<Cubby> hot;
            std::deque<std::unique_ptr<Cubby>> frozen;
            std::unordered_set<uint64_t> tombstones;
            mutable std::mutex mu;
        };

    private:
        bool cold_contains_locked(size_t r, uint64_t key) const
        {
            return cold_ ? cold_->contains(r, key) : false;
        }
        bool cold_erase_locked(size_t r, uint64_t key)
        {
            return cold_ ? cold_->erase(r, key) : false;
        }
        void cold_bulk_insert_locked(size_t r, const std::vector<uint64_t> &keys)
        {
            if (cold_)
                cold_->bulk_insert(r, keys);
        }

        size_t facility_of(uint64_t gx) const
        {
            if (n_facilities_ == 0 || K_ == 0 || N_ < 2)
                return 0;
            const uint64_t g = gx & (N_ - 1);
            const uint64_t r = g / K_;
            return static_cast<size_t>(r % n_facilities_);
        }

        bool try_kkick_insert(Cubby *c, size_t r_idx, uint64_t key, uint64_t gx,
                              uint64_t &kicks_out)
        {
            if (!c || !c->kick_geom)
                return false;
            KKickReadSlot read_slot;
            KKickWriteSlot write_slot;
            KKickClearSlot clear_slot_cb;
            std::function<uint32_t(int, uint64_t)> random_depth;
            std::function<uint64_t(uint64_t)> gx_of_key;
            KKickHelpers h{N_, K_, r_idx, &pi_};
            h.wire(*c, read_slot, write_slot, clear_slot_cb, random_depth, gx_of_key, k_kick_);
            const auto kr = kkick_insert_cubby(*c->kick_geom, key, gx, read_slot,
                                               write_slot, clear_slot_cb,
                                               random_depth, gx_of_key, &c->array_m);
            kicks_out = kr.kick_count;
            return kr.ok;
        }

        void clear_in_cubby_locked(Cubby &c, size_t slot, size_t r_idx)
        {
            if (slot >= c.slots.size() || !c.slots[slot].has_value())
                return;
            if (auto oldk = recover_key_from_slot(c, slot, r_idx, N_, K_, pi_))
                local_router_erase(c, pi_.pi(*oldk), K_);
            clear_slot(c, slot);
            remove_occupied(c, slot);
            c.free_slots.mark_free(static_cast<int>(slot));
            c.size--;
        }

        // rotate：hot → frozen 队列；新建 hot；如队列超出 frozen_queue_max，inline flush 直到队列降到上限内（背压）。
        void rotate_locked(PerFacility &f, size_t r_idx)
        {
            f.frozen.push_back(std::move(f.hot));
            f.hot = create_cubby(k_kick_, tail_capacity_, K_, derived_.n_hint,
                                 fanout_, node_max_bits_);
            // 背压：超过上限就在前台 flush 到上限内
            const auto t0 = std::chrono::steady_clock::now();
            while (static_cast<int>(f.frozen.size()) > frozen_queue_max_)
                flush_oldest_locked(f, r_idx);
            const auto t1 = std::chrono::steady_clock::now();
            flush_blocked_ns_.fetch_add(
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()),
                std::memory_order_relaxed);
        }

        void flush_oldest_locked(PerFacility &f, size_t r_idx)
        {
            if (f.frozen.empty())
                return;
            auto cubby = std::move(f.frozen.front());
            f.frozen.pop_front();
            if (!cubby)
                return;

            std::vector<uint64_t> keys;
            keys.reserve(cubby->size);
            for (size_t i = 0; i < cubby->slots.size(); ++i)
            {
                if (!cubby->slots[i].has_value())
                    continue;
                auto k = recover_key_from_slot(*cubby, i, r_idx, N_, K_, pi_);
                if (!k)
                    continue;
                // 过滤 tombstone
                auto it = f.tombstones.find(*k);
                if (it != f.tombstones.end())
                {
                    f.tombstones.erase(it);
                    continue;
                }
                keys.push_back(*k);
            }
            cold_bulk_insert_locked(r_idx, keys);
            flush_count_.fetch_add(1, std::memory_order_relaxed);
            flushed_keys_.fetch_add(keys.size(), std::memory_order_relaxed);
        }

    private:
        StoreParams params_{};
        DerivedParams derived_{};
        PermutationHash pi_{};
        uint64_t N_ = 0, K_ = 0, n_facilities_ = 0;
        int frozen_queue_max_ = 4;
        size_t tail_capacity_ = 0;
        int fanout_ = 8, node_max_bits_ = 2, k_kick_ = 4;

        mutable std::mutex global_mu_;
        std::vector<std::unique_ptr<PerFacility>> facs_;

        std::atomic<uint64_t> n_{0};
        mutable std::atomic<uint64_t> hot_hits_{0};
        mutable std::atomic<uint64_t> frozen_hits_{0};
        mutable std::atomic<uint64_t> cold_hits_{0};
        std::atomic<uint64_t> flush_count_{0};
        std::atomic<uint64_t> flushed_keys_{0};
        std::atomic<uint64_t> flush_blocked_ns_{0};
        std::atomic<uint64_t> background_drain_ns_{0};

        std::unique_ptr<ColdStore> cold_;

        friend class V3HotTailStore;
    };

    // sqlite_path 非空 → SQLite 后端；空 → in-memory（实验对照线）。
    std::unique_ptr<ColdStore> make_cold_store(const StoreParams &p)
    {
        if (p.sqlite_path.empty())
            return std::make_unique<InMemoryColdStore>();
        return std::make_unique<SqliteColdStore>();
    }

    // ====================================================================
    //  V3HotTailStore 公共接口转发
    // ====================================================================
    V3HotTailStore::V3HotTailStore() : impl_(std::make_unique<Impl>()) {}
    V3HotTailStore::~V3HotTailStore() = default;

    OpResult V3HotTailStore::init(const StoreParams &p) { return impl_->init(p); }
    InsertResult V3HotTailStore::insert(uint64_t key) { return impl_->insert(key); }
    QueryResult V3HotTailStore::query(uint64_t key) const { return impl_->query(key); }
    DeleteResult V3HotTailStore::erase(uint64_t key) { return impl_->erase(key); }
    OpResult V3HotTailStore::bulk_load(const std::vector<uint64_t> &keys)
    {
        return impl_->bulk_load(keys);
    }
    void V3HotTailStore::drain_background_work() { impl_->drain_background_work(); }
    StoreStats V3HotTailStore::stats() const { return impl_->stats(); }

} // namespace otsh