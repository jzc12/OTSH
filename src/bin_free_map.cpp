#include "otsh/bin_free_map.h"

#include <algorithm>
#include <limits>

namespace otsh
{

    namespace
    {

        constexpr size_t kNoFreeSlot = std::numeric_limits<size_t>::max();

    } // namespace

    void BinFreeMap::reset(const KKickGeometry * /*geom*/, size_t cubby_capacity)
    {
        cap_ = cubby_capacity;
        slot_ma_.configure(8, 2);
        slot_ma_.reset(cap_);
        free_slots_.assign(cap_, 1);
        bin_states_.clear();
        slot_bins_.assign(cap_, {});
        bin_index_.clear();
        const MiniArray::Bits one{1ULL};
        for (size_t s = 0; s < cap_; ++s)
        {
            slot_ma_.update(s, one, 1);
        }
    }

    bool BinFreeMap::slot_flag(size_t slot) const
    {
        if (slot >= cap_ || slot >= free_slots_.size())
            return false;
        return free_slots_[slot] != 0;
    }

    void BinFreeMap::mark_used(size_t slot)
    {
        if (slot >= cap_ || !slot_flag(slot))
            return;
        free_slots_[slot] = 0;
        slot_ma_.erase(slot);
        for (const size_t id : slot_bins_[slot])
        {
            BinState &state = bin_states_[id];
            if (state.free_count > 0)
                --state.free_count;
            if (state.first_free == slot)
            {
                state.first_free = kNoFreeSlot;
                for (size_t s = slot + 1; s < state.range.end && s < cap_; ++s)
                {
                    if (slot_flag(s))
                    {
                        state.first_free = s;
                        break;
                    }
                }
            }
        }
    }

    void BinFreeMap::mark_free(size_t slot)
    {
        if (slot >= cap_ || slot_flag(slot))
            return;
        free_slots_[slot] = 1;
        MiniArray::Bits one{1ULL};
        slot_ma_.update(slot, one, 1);
        for (const size_t id : slot_bins_[slot])
        {
            BinState &state = bin_states_[id];
            ++state.free_count;
            if (state.first_free == kNoFreeSlot || slot < state.first_free)
                state.first_free = slot;
        }
    }

    bool BinFreeMap::is_free(size_t slot) const
    {
        return slot_flag(slot);
    }

    bool BinFreeMap::bin_has_free(const BinRange &bin) const
    {
        const BinState *state = ensure_bin(bin);
        if (state)
            return state->free_count > 0;
        return false;
    }

    std::optional<size_t> BinFreeMap::first_free_in_bin(const BinRange &bin) const
    {
        const BinState *state = ensure_bin(bin);
        if (state)
        {
            if (state->free_count == 0 || state->first_free == kNoFreeSlot)
                return std::nullopt;
            return state->first_free;
        }
        return std::nullopt;
    }

    uint64_t BinFreeMap::bin_key(const BinRange &bin)
    {
        return (static_cast<uint64_t>(bin.start) << 32) ^
               static_cast<uint64_t>(bin.end);
    }

    const BinFreeMap::BinState *BinFreeMap::ensure_bin(const BinRange &bin) const
    {
        const size_t start = std::min(bin.start, cap_);
        const size_t end = std::min(bin.end, cap_);
        if (start >= end)
            return nullptr;
        const BinRange clipped{start, end};
        const uint64_t key = bin_key(clipped);
        const auto found = bin_index_.find(key);
        if (found != bin_index_.end())
            return &bin_states_[found->second];

        const size_t id = bin_states_.size();
        bin_index_[key] = id;
        size_t free_count = 0;
        size_t first_free = kNoFreeSlot;
        for (size_t slot = start; slot < end; ++slot)
        {
            slot_bins_[slot].push_back(id);
            if (slot_flag(slot))
            {
                ++free_count;
                if (first_free == kNoFreeSlot)
                    first_free = slot;
            }
        }
        bin_states_.push_back(BinState{clipped, free_count, first_free});
        return &bin_states_.back();
    }

} // namespace otsh
