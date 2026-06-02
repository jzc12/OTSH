#pragma once

#include "otsh/bin_free_map.h"
#include "otsh/free_slot_tree.h"
#include "otsh/mini_array.h"
#include "otsh/prefix_router.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace otsh
{

    class KKickGeometry;

    // §3.3 Cubby：storage + k-kick + mini-array A/M/B。
    struct Cubby
    {

        int tier = 1; // j-tiered 层级（§3.1 从 1 起：1=tail/最小 cubby）
        size_t capacity = 0;
        size_t size = 0;
        std::vector<std::optional<uint64_t>> slots; // storage：quotient payload
        std::vector<size_t> occupied;

        FreeSlotTree free_slots;

        // A[i]：g_I(x)=i 的 Local Query Router（§5.2）
        std::vector<PrefixRouter> array_a;
        // M：各 k-kick 深度 bin 的空闲槽位图（§3.3 / §4）
        BinFreeMap array_m;
        // B[i]：槽位 meta（delta + 中间位 + 插入信息），§3.3
        MiniArray array_b;

        std::unique_ptr<KKickGeometry> kick_geom;
    };

} // namespace otsh
