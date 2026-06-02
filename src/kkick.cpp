#include "otsh/kkick.h"
#include "otsh/bin_free_map.h"
#include "hash.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace otsh
{
    namespace
    {

        static bool slot_occupied(KKickReadSlot &read, size_t p)
        {
            std::optional<uint64_t> k;
            return read(p, &k).occupied;
        }

        static bool bin_is_saturated(const BinRange &bin, int depth_d,
                                     KKickReadSlot &read)
        {
            bool any = false;
            for (size_t p = bin.start; p < bin.end; ++p)
            {
                std::optional<uint64_t> k;
                const auto v = read(p, &k);
                if (!v.occupied)
                    return false;
                any = true;
                if (v.insert_depth < static_cast<uint32_t>(depth_d))
                    return false;
            }
            return any;
        }

        static int max_usable_depth(const KKickGeometry &geom, uint64_t gx,
                                    KKickReadSlot &read)
        {
            for (int d = geom.k(); d >= 0; --d)
            {
                const BinRange b = geom.preference_bin(gx, d);
                if (!bin_is_saturated(b, d, read))
                    return d;
            }
            return 0;
        }

        static std::optional<size_t> first_free_in_bin(const BinRange &bin,
                                                       KKickReadSlot &read,
                                                       BinFreeMap *free_map)
        {
            if (free_map)
            {
                if (auto p = free_map->first_free_in_bin(bin))
                    return p;
                return std::nullopt;
            }
            for (size_t p = bin.start; p < bin.end; ++p)
            {
                if (!slot_occupied(read, p))
                    return p;
            }
            return std::nullopt;
        }

        static std::optional<std::pair<size_t, uint32_t>>
        min_depth_occupant(const BinRange &bin, KKickReadSlot &read)
        {
            std::optional<std::pair<size_t, uint32_t>> best;
            for (size_t p = bin.start; p < bin.end; ++p)
            {
                std::optional<uint64_t> k;
                const auto v = read(p, &k);
                if (!v.occupied)
                    continue;
                if (!best || v.insert_depth < best->second)
                    best = std::make_pair(p, v.insert_depth);
            }
            return best;
        }

        struct OriginalSlotState
        {
            size_t slot = 0;
            bool occupied = false;
            uint64_t key = 0;
            uint32_t depth = 0;
        };

        static void remember_original(std::vector<OriginalSlotState> &originals,
                                      size_t slot, KKickReadSlot &read_slot)
        {
            for (const auto &s : originals)
            {
                if (s.slot == slot)
                    return;
            }
            std::optional<uint64_t> key;
            const auto v = read_slot(slot, &key);
            OriginalSlotState st;
            st.slot = slot;
            st.occupied = v.occupied && key.has_value();
            st.key = key.value_or(0);
            st.depth = v.insert_depth;
            originals.push_back(st);
        }

        static std::optional<uint32_t> probe_for_existing_slot(
            const KKickGeometry &geom, uint64_t gx, uint32_t depth, size_t slot)
        {
            const BinRange bin = geom.preference_bin(gx, static_cast<int>(depth));
            if (slot < bin.start || slot >= bin.end)
                return std::nullopt;
            return static_cast<uint32_t>(geom.probe_index(gx, static_cast<int>(depth),
                                                          slot - bin.start));
        }

        static void rollback_originals(
            const KKickGeometry &geom, const std::vector<OriginalSlotState> &originals,
            KKickClearSlot &clear_slot, KKickWriteSlot &write_slot,
            const std::function<uint64_t(uint64_t)> &gx_of_key)
        {
            for (auto it = originals.rbegin(); it != originals.rend(); ++it)
            {
                clear_slot(it->slot);
                if (!it->occupied)
                    continue;
                const uint64_t gx = gx_of_key ? gx_of_key(it->key) : it->key;
                const uint32_t pj =
                    probe_for_existing_slot(geom, gx, it->depth, it->slot).value_or(0);
                (void)write_slot(it->slot, it->key, it->depth, pj);
            }
        }

        // §4.4 ReInsert：被踢元素从原深度向上寻找位置；若遇到未饱和但已满
        // 的 bin，继续踢出其中 insert_depth 最小的元素，最多执行 k 次搬移。
        static bool reinsert_chain(const KKickGeometry &geom, uint64_t key,
                                   uint64_t gx_pi, uint32_t victim_depth,
                                   KKickReadSlot &read_slot, KKickWriteSlot &write_slot,
                                   KKickClearSlot &clear_slot,
                                   const std::function<uint64_t(uint64_t)> &gx_of_key,
                                   BinFreeMap *free_map, uint64_t &kick_count,
                                   std::vector<OriginalSlotState> &originals)
        {
            uint64_t cur_key = key;
            uint64_t cur_gx = gx_pi;
            int start = static_cast<int>(victim_depth) - 1;
            if (start < 0)
                start = 0;

            while (kick_count <= static_cast<uint64_t>(geom.k()))
            {
                bool advanced = false;
                for (int d = start; d >= 0; --d)
                {
                    const BinRange bin = geom.preference_bin(cur_gx, d);
                    if (bin_is_saturated(bin, d, read_slot))
                        continue;

                    if (const auto pos = first_free_in_bin(bin, read_slot, free_map))
                    {
                        remember_original(originals, *pos, read_slot);
                        const uint32_t pj =
                            static_cast<uint32_t>(geom.probe_index(cur_gx, d, *pos - bin.start));
                        return write_slot(*pos, cur_key, static_cast<uint32_t>(d), pj);
                    }

                    if (kick_count >= static_cast<uint64_t>(geom.k()))
                        return false;

                    const auto victim = min_depth_occupant(bin, read_slot);
                    if (!victim)
                        continue;

                    std::optional<uint64_t> evicted_key;
                    const auto evicted_view = read_slot(victim->first, &evicted_key);
                    if (!evicted_key)
                        return false;

                    remember_original(originals, victim->first, read_slot);
                    clear_slot(victim->first);
                    const uint32_t pj = static_cast<uint32_t>(
                        geom.probe_index(cur_gx, d, victim->first - bin.start));
                    if (!write_slot(victim->first, cur_key, static_cast<uint32_t>(d), pj))
                        return false;

                    ++kick_count;
                    cur_key = *evicted_key;
                    cur_gx = gx_of_key ? gx_of_key(cur_key) : cur_gx;
                    start = static_cast<int>(evicted_view.insert_depth) - 1;
                    if (start < 0)
                        start = 0;
                    advanced = true;
                    break;
                }
                if (!advanced)
                    return false;
            }
            return false;
        }

        static KKickInsertResult insert_kick_once(
            const KKickGeometry &geom, uint64_t key, uint64_t gx_pi,
            KKickReadSlot read_slot, KKickWriteSlot write_slot,
            KKickClearSlot clear_slot,
            const std::function<uint32_t(int max_d, uint64_t gx)> &random_depth,
            const std::function<uint64_t(uint64_t)> &gx_of_key, BinFreeMap *free_map)
        {
            KKickInsertResult out;
            if (!read_slot || !write_slot || !clear_slot)
                return out;

            const int max_d = max_usable_depth(geom, gx_pi, read_slot);
            const uint32_t s_x = random_depth ? random_depth(max_d, gx_pi) : 0;
            const int depth =
                static_cast<int>(std::min<uint32_t>(static_cast<uint32_t>(max_d), s_x));

            const BinRange target = geom.preference_bin(gx_pi, depth);
            const auto free_pos = first_free_in_bin(target, read_slot, free_map);
            if (free_pos)
            {
                const size_t off = *free_pos - target.start;
                out.probe_j =
                    static_cast<uint32_t>(geom.probe_index(gx_pi, depth, off));
                if (!write_slot(*free_pos, key, static_cast<uint32_t>(depth), out.probe_j))
                    return out;
                out.ok = true;
                out.slot = *free_pos;
                out.insert_depth = static_cast<uint32_t>(depth);
                return out;
            }

            const auto victim = min_depth_occupant(target, read_slot);
            if (!victim)
                return out;

            std::vector<OriginalSlotState> originals;
            std::optional<uint64_t> evicted_key;
            (void)read_slot(victim->first, &evicted_key);
            if (!evicted_key)
                return out;

            remember_original(originals, victim->first, read_slot);
            clear_slot(victim->first);
            const size_t off = victim->first - target.start;
            out.probe_j =
                static_cast<uint32_t>(geom.probe_index(gx_pi, depth, off));
            if (!write_slot(victim->first, key, static_cast<uint32_t>(depth), out.probe_j))
                return out;

            out.ok = true;
            out.slot = victim->first;
            out.insert_depth = static_cast<uint32_t>(depth);
            out.kick_count = 1;

            const uint64_t ev_gx = gx_of_key ? gx_of_key(*evicted_key) : gx_pi;
            if (!reinsert_chain(geom, *evicted_key, ev_gx, victim->second, read_slot,
                                write_slot, clear_slot, gx_of_key, free_map,
                                out.kick_count, originals))
            {
                rollback_originals(geom, originals, clear_slot, write_slot, gx_of_key);
                out.ok = false;
                out.kick_count = 0;
                out.slot = 0;
                return out;
            }
            return out;
        }

    } // namespace

    KKickGeometry::KKickGeometry(int k_depth, size_t cubby_capacity, uint64_t K,
                                 uint64_t n_hint)
    {
        k_ = std::max(0, k_depth);
        cap_ = std::max<size_t>(1, cubby_capacity);
        s_.resize(static_cast<size_t>(k_ + 1));
        for (int i = 0; i <= k_; ++i)
            s_[static_cast<size_t>(i)] = kkick_bin_size(i, K, n_hint);

        // §4.2：探测序下标按 k→0 每层贡献 s_i 个槽位
        probe_base_.assign(static_cast<size_t>(k_ + 1), 0);
        size_t acc = 0;
        for (int i = k_; i >= 0; --i)
        {
            probe_base_[static_cast<size_t>(i)] = acc;
            acc += s_[static_cast<size_t>(i)];
        }
    }

    size_t KKickGeometry::split_size(int depth, size_t parent_span) const
    {
        if (depth <= 0)
            return parent_span;
        if (depth > k_)
            return 1;
        const size_t nominal = s_[static_cast<size_t>(depth)];
        return std::max<size_t>(1, std::min(nominal, parent_span));
    }

    BinRange KKickGeometry::preference_bin(uint64_t gx_pi, int depth) const
    {
        // g_0(x) = 整个 cubby [0, I)
        BinRange r{0, cap_};
        if (depth <= 0)
            return r;
        // g_d 在 g_{d-1} 内按 s_d 均匀切分（§4.1 / §4.2）
        for (int d = 1; d <= depth; ++d)
        {
            const size_t span = r.size();
            const size_t sz = split_size(d, span);
            const size_t num_bins = std::max<size_t>(1, span / sz);
            const size_t idx =
                static_cast<size_t>(splitmix64(gx_pi ^ static_cast<uint64_t>(d) * 0x9e37ULL) %
                                    num_bins);
            const size_t start = r.start + idx * sz;
            const size_t end = std::min(start + sz, r.end);
            r = BinRange{start, end};
        }
        return r;
    }

    size_t KKickGeometry::probe_index(uint64_t gx_pi, int depth,
                                      size_t offset_in_bin) const
    {
        (void)gx_pi;
        return probe_base_[static_cast<size_t>(depth)] + offset_in_bin;
    }

    size_t KKickGeometry::probe_sequence_length() const
    {
        size_t t = 0;
        for (size_t s : s_)
            t += s;
        return t;
    }

    size_t KKickGeometry::probe_base_at(int depth) const
    {
        if (depth < 0 || depth > k_)
            return 0;
        return probe_base_[static_cast<size_t>(depth)];
    }

    std::optional<size_t> probe_j_to_slot(const KKickGeometry &geom, uint64_t gx_pi,
                                          uint32_t j)
    {
        for (int d = geom.k(); d >= 0; --d)
        {
            const size_t base = geom.probe_base_at(d);
            const size_t sz = geom.bin_sizes()[static_cast<size_t>(d)];
            if (j < base || j >= base + sz)
                continue;
            const size_t off = static_cast<size_t>(j) - base;
            const BinRange bin = geom.preference_bin(gx_pi, d);
            if (off >= bin.size())
                return std::nullopt;
            return bin.start + off;
        }
        return std::nullopt;
    }

    KKickInsertResult kkick_insert_cubby(
        const KKickGeometry &geom, uint64_t key, uint64_t gx_pi,
        KKickReadSlot read_slot, KKickWriteSlot write_slot,
        KKickClearSlot clear_slot,
        const std::function<uint32_t(int max_d, uint64_t gx)> &random_depth,
        const std::function<uint64_t(uint64_t key)> &gx_of_key,
        BinFreeMap *free_map)
    {
        auto gx_of = [&](uint64_t k) -> uint64_t
        {
            return gx_of_key ? gx_of_key(k) : gx_pi;
        };
        return insert_kick_once(geom, key, gx_pi, read_slot, write_slot, clear_slot,
                                random_depth, gx_of, free_map);
    }

} // namespace otsh
