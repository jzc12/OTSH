#pragma once

#include "otsh/keystore.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace otsh
{

    struct PermutationHash;

    // V2：Pure-SQLite。
    // 与 V3 对齐：按 facility r 分表 facility_<r>(key INTEGER PRIMARY KEY) WITHOUT ROWID，
    // π Feistel 计算 r。WAL + synchronous=NORMAL。
    // V2 vs V3 唯一差异：V2 没有 hot tail 缓存，每个 op 直达 SQLite。
    class V2SqliteStore : public IKeyStore
    {
    public:
        V2SqliteStore();
        ~V2SqliteStore() override;

        OpResult init(const StoreParams &p) override;
        InsertResult insert(uint64_t key) override;
        QueryResult query(uint64_t key) const override;
        DeleteResult erase(uint64_t key) override;
        OpResult bulk_load(const std::vector<uint64_t> &keys) override;
        void drain_background_work() override;
        StoreStats stats() const override;
        StoreVariant variant() const override { return StoreVariant::V2_SqliteOnly; }

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace otsh