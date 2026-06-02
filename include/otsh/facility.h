#pragma once

#include "otsh/mini_array.h"
#include "otsh/router.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace otsh
{

    struct Cubby;

    struct FacilityBucketMeta
    {
        uint32_t entry_count = 0;
        uint32_t encoded_bits = 0;
        uint64_t fingerprint = 0;
    };

    inline MiniArray::Bits encode_facility_bucket_meta(const FacilityBucketMeta &m)
    {
        return MiniArray::Bits{
            (static_cast<uint64_t>(m.entry_count) & 0xffffffffULL) |
                (static_cast<uint64_t>(m.encoded_bits) << 32),
            m.fingerprint};
    }

    inline std::optional<FacilityBucketMeta>
    decode_facility_bucket_meta(const MiniArray::Bits &bits, uint32_t bitlen)
    {
        if (bitlen != 128 || bits.size() < 2)
            return std::nullopt;
        FacilityBucketMeta m;
        m.entry_count = static_cast<uint32_t>(bits[0] & 0xffffffffULL);
        m.encoded_bits = static_cast<uint32_t>(bits[0] >> 32);
        m.fingerprint = bits[1];
        return m;
    }

    // §1.2 / §2 Facility：聚合多 tier cubbies + §2.1 facility mini-array。
    struct Facility
    {
        std::vector<std::vector<std::unique_ptr<Cubby>>> tiers;
        int max_tier = 0;

        int tail_tier = 0; // tiers[tail_tier] 为 1-tiered cubby 集合（恒为 tiers[0]）
        Cubby *tail = nullptr;
        std::unique_ptr<Cubby> tail_owned;

        // §2.1：K 桶 facility mini-array（每桶路由表项数 + 编码位宽摘要）
        MiniArray ma;
        // D[b]：g*(x) mod K = b 时 key 的 (cubby, slot) 定位
        std::vector<Router> D;
    };

} // namespace otsh
