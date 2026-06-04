#include "otsh/v1_store.h"

#include <chrono>

namespace otsh
{

    V1Store::V1Store() = default;
    V1Store::~V1Store() = default;

    OpResult V1Store::init(const StoreParams &p)
    {
        return ht_.init(p.table);
    }

    InsertResult V1Store::insert(uint64_t key) { return ht_.insert(key); }
    QueryResult  V1Store::query(uint64_t key) const { return ht_.query(key); }
    DeleteResult V1Store::erase(uint64_t key) { return ht_.erase(key); }

    OpResult V1Store::bulk_load(const std::vector<uint64_t> &keys)
    {
        return ht_.bulk_load(keys);
    }

    void V1Store::drain_background_work()
    {
        const auto t0 = std::chrono::steady_clock::now();
        ht_.drain_background_work();
        const auto t1 = std::chrono::steady_clock::now();
        background_drain_ns_ += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    StoreStats V1Store::stats() const
    {
        StoreStats s;
        s.n = ht_.state().n;
        s.mem_meta_bits = ht_.logical_meta_bits();
        s.background_drain_ns = background_drain_ns_;
        return s;
    }

} // namespace otsh