#pragma once

#include "otsh/keystore.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace otsh
{

    struct Cubby;
    struct PermutationHash;
    class KKickGeometry;

    // V3：每 facility 一个 hot tail（k-kick 进出）+ frozen tails 队列 + SQLite 按 r 分表。
    // 设计要点：
    //   - tail 满或 k-kick 失败 → rotate：当前 tail 入 frozen 队列，新建 hot tail
    //   - 后台线程把 frozen tail 的 key 反推（pi^-1）+ 批量写入 facility_<r> 表
    //   - frozen 队列长度上限 = StoreParams.frozen_queue_max（默认 4），超出阻塞 insert
    //   - erase 命中 SQLite：同步 DELETE
    //   - 不再有 active/old 双表、不再有多 tier rebuild
    class V3HotTailStore : public IKeyStore
    {
    public:
        V3HotTailStore();
        ~V3HotTailStore() override;

        OpResult init(const StoreParams &p) override;
        InsertResult insert(uint64_t key) override;
        QueryResult query(uint64_t key) const override;
        DeleteResult erase(uint64_t key) override;
        OpResult bulk_load(const std::vector<uint64_t> &keys) override;
        void drain_background_work() override;
        StoreStats stats() const override;
        StoreVariant variant() const override { return StoreVariant::V3_HotTailSqlite; }

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace otsh