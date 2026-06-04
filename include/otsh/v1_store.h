#pragma once

#include "otsh/keystore.h"

namespace otsh
{

    // V1：直接包装现有 HashTable（OTSH-Tiered + π Feistel + 多层 cubby）。
    class V1Store : public IKeyStore
    {
    public:
        V1Store();
        ~V1Store() override;

        OpResult init(const StoreParams &p) override;
        InsertResult insert(uint64_t key) override;
        QueryResult query(uint64_t key) const override;
        DeleteResult erase(uint64_t key) override;
        OpResult bulk_load(const std::vector<uint64_t> &keys) override;
        void drain_background_work() override;
        StoreStats stats() const override;
        StoreVariant variant() const override { return StoreVariant::V1_TieredCubby; }

    private:
        HashTable ht_;
        mutable uint64_t background_drain_ns_ = 0;
    };

} // namespace otsh