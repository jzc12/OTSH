#include "otsh/keystore.h"
#include "otsh/v1_store.h"
#include "otsh/v2_sqlite_store.h"
#include "otsh/v3_hot_tail_store.h"

namespace otsh
{

    const char *variant_name(StoreVariant v)
    {
        switch (v)
        {
        case StoreVariant::V1_TieredCubby:
            return "V1";
        case StoreVariant::V2_SqliteOnly:
            return "V2";
        case StoreVariant::V3_HotTailSqlite:
            return "V3";
        }
        return "?";
    }

    StoreVariant variant_from_string(const std::string &s)
    {
        if (s == "V1" || s == "v1" || s == "tiered" || s == "1")
            return StoreVariant::V1_TieredCubby;
        if (s == "V2" || s == "v2" || s == "sqlite" || s == "2")
            return StoreVariant::V2_SqliteOnly;
        if (s == "V3" || s == "v3" || s == "hottail" || s == "3")
            return StoreVariant::V3_HotTailSqlite;
        return StoreVariant::V1_TieredCubby;
    }

    OpResult IKeyStore::bulk_load(const std::vector<uint64_t> &keys)
    {
        for (uint64_t k : keys)
            (void)insert(k);
        return {true, ""};
    }

    std::unique_ptr<IKeyStore> make_keystore(StoreVariant v)
    {
        switch (v)
        {
        case StoreVariant::V1_TieredCubby:
            return std::make_unique<V1Store>();
        case StoreVariant::V2_SqliteOnly:
            return std::make_unique<V2SqliteStore>();
        case StoreVariant::V3_HotTailSqlite:
            return std::make_unique<V3HotTailStore>();
        }
        return std::make_unique<V1Store>();
    }

} // namespace otsh