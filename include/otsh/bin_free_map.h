#pragma once

#include "otsh/kkick.h"
#include "otsh/mini_array.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace otsh
{

    class KKickGeometry;

    // §3.3 mini-array M：cubby 槽位空闲位图 + bin 级空闲索引。
    class BinFreeMap
    {
    public:
        void reset(const KKickGeometry *geom, size_t cubby_capacity);
        void mark_used(size_t slot);
        void mark_free(size_t slot);

        bool is_free(size_t slot) const;
        std::optional<size_t> first_free_in_bin(const BinRange &bin) const;
        bool bin_has_free(const BinRange &bin) const;

        const MiniArray &slot_bitmap() const { return slot_ma_; }

    private:
        struct BinState
        {
            BinRange range;
            size_t free_count = 0;
            size_t first_free = 0;
        };

        size_t cap_ = 0;
        MiniArray slot_ma_;
        std::vector<uint8_t> free_slots_;
        mutable std::vector<BinState> bin_states_;
        mutable std::vector<std::vector<size_t>> slot_bins_;
        mutable std::unordered_map<uint64_t, size_t> bin_index_;

        bool slot_flag(size_t slot) const;
        const BinState *ensure_bin(const BinRange &bin) const;
        static uint64_t bin_key(const BinRange &bin);
    };

} // namespace otsh
