#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace otsh
{

    // §2 MiniArray：O(1) 深度 B 树 + Packed Leaf + Bitmap + Rank/Select。
    class MiniArray
    {
    public:
        using Bits = std::vector<uint64_t>;

        MiniArray();
        ~MiniArray();
        MiniArray(const MiniArray &) = delete;
        MiniArray &operator=(const MiniArray &) = delete;
        MiniArray(MiniArray &&) noexcept;
        MiniArray &operator=(MiniArray &&) noexcept;

        explicit MiniArray(size_t n);

        void configure(int fanout, int node_max_bits);
        void reset(size_t n);

        size_t size() const;
        uint64_t occupied_count() const;
        uint64_t bits_total() const;

        // 物理槽位 i（§3.3 B[i] 与 storage 一一对应）
        uint32_t bitlen(size_t i) const;
        Bits access(size_t i) const;
        void update(size_t i, const Bits &bits, uint32_t bitlen);
        void erase(size_t i);

        // 逻辑下标 idx ∈ [0, occupied_count)：§2.2 Access/Insert/Delete + Select/Rank
        std::optional<Bits> access_logical(size_t logical_idx) const;
        void insert_logical(size_t logical_idx, const Bits &bits, uint32_t bitlen);
        void delete_logical(size_t logical_idx);

        // §2.2 Rank / Select（论文必备 LUT）
        static uint32_t rank_u64(uint64_t bitmap, uint32_t i);
        static int select_u64(uint64_t bitmap, uint32_t k);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace otsh
