#pragma once

#include "config.h"
#include "ht.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace otsh
{

    // CH5 三方案对比的统一接口（V1 = OTSH-Tiered, V2 = Pure-SQLite, V3 = Hot-Tail+SQLite）。
    // 与 HashTable 既有 OpResult/InsertResult/... 结构保持一致，便于实验直接复用。

    enum class StoreVariant
    {
        V1_TieredCubby = 1,  // 现有 OTSH 多层 cubby
        V2_SqliteOnly = 2,   // 纯 SQLite
        V3_HotTailSqlite = 3 // tail cubby 热缓存 + SQLite 按 facility r 分表
    };

    const char *variant_name(StoreVariant v);
    StoreVariant variant_from_string(const std::string &s);

    // 统一参数：复用 TableParams（V2/V3 只用 n/seed 等部分字段）+ 额外字段。
    struct StoreParams
    {
        TableParams table; // V1/V3 用其完整字段；V2 仅用 n/seed
        // V2/V3 通用：SQLite 文件路径。空串=":memory:"（仅用于内存压力对照）。
        std::string sqlite_path;
        // V3 专用：背压上限（frozen tails 队列长度）。
        int frozen_queue_max = 4;
        // V3 专用：单个 tail 容量；0=沿用 §6.2 tier=1 容量（推荐）。
        int tail_capacity_override = 0;
    };

    struct StoreStats
    {
        uint64_t n = 0;                      // 在库 key 数
        uint64_t mem_meta_bits = 0;          // 内存元数据 bit 数（V2=0）
        uint64_t disk_file_bytes = 0;        // SQLite 主 DB 文件持久化字节（不含 WAL/SHM）
        uint64_t runtime_overhead_bytes = 0; // SQLite WAL+SHM 运行时占用（drain 前可观察；close 后会被清理）
        uint64_t hot_hits = 0;               // V3 hot tail 命中次数
        uint64_t frozen_hits = 0;            // V3 frozen tail 命中次数
        uint64_t sqlite_hits = 0;            // V2/V3 SQLite 命中次数
        uint64_t flush_count = 0;            // V3 frozen cubby flush 次数
        uint64_t flushed_keys = 0;           // V3 已 flush 的 key 数
        uint64_t flush_blocked_ns = 0;       // V3 因背压阻塞的累计纳秒
        uint64_t background_drain_ns = 0;
    };

    class IKeyStore
    {
    public:
        virtual ~IKeyStore() = default;

        virtual OpResult init(const StoreParams &p) = 0;
        virtual InsertResult insert(uint64_t key) = 0;
        virtual QueryResult query(uint64_t key) const = 0;
        virtual DeleteResult erase(uint64_t key) = 0;

        // 部分实现支持批量加载（V2 用事务 batch；V1/V3 退化为逐条 insert）。
        virtual OpResult bulk_load(const std::vector<uint64_t> &keys);

        // 等待后台工作完成（V1=rebuild/resize；V3=frozen flush；V2=WAL checkpoint）。
        virtual void drain_background_work() = 0;

        virtual StoreStats stats() const = 0;

        virtual StoreVariant variant() const = 0;
    };

    std::unique_ptr<IKeyStore> make_keystore(StoreVariant v);

} // namespace otsh