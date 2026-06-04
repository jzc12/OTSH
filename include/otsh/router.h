#pragma once

#include "hash.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace otsh
{

    class Cubby;

    // Facility-level router: key -> (cubby, slot).
    // Entries are organized as a Patricia trie over stable 64-bit key hashes; the
    // encoded length is tracked for metadata accounting.
    class Router
    {
    public:
        using Value = std::pair<Cubby *, size_t>;

        std::pair<std::optional<Value>, uint64_t> locate(uint64_t key) const;
        bool contains(uint64_t key) const;
        uint64_t insert(uint64_t key, Value v);
        uint64_t erase(uint64_t key);
        // 移除所有指向 cb 的路由项（cubby 销毁 / rebuild 前调用）。
        void erase_cubby(const Cubby *cb);
        void clear();

        size_t entry_count() const { return entries_.size(); }

        // 编码后的 bitstream 大小（用于空间统计）。
        uint64_t bits_total() const { return static_cast<uint64_t>(encoded_bits_); }
        uint64_t fingerprint() const;

    private:
        struct Entry
        {
            uint64_t key = 0;
            Value v{};
            uint64_t bits = 0; // hashed bits used by trie
        };
        struct Node
        {
            int left = -1;
            int right = -1;
            int entry_idx = -1;    // leaf: index into entries_
            int bucket_idx = -1;   // hash-collision bucket (split_bit==64)
            uint8_t split_bit = 0; // internal node: branching bit position [0,64]
        };

        std::vector<Entry> entries_;
        std::vector<Node> nodes_;
        std::vector<std::vector<int>> buckets_;
        int root_ = -1;

        // 真实 bitstream 编码长度（通过 bit-writer 计算）。
        size_t encoded_bits_ = 0;

        static uint64_t key_bits(uint64_t key)
        {
            // 与设计文档一致：router 对键使用随机 bit 串；这里用 splitmix64 作为稳定
            // hash。
            return splitmix64(key ^ 0x9e3779b97f4a7c15ULL);
        }

        void rebuild();
        static bool bit_at(uint64_t bits, int bitpos)
        {
            return ((bits >> (63 - bitpos)) & 1ULL) != 0;
        }

        // Patricia build helpers
        int build_range(std::vector<int> &idxs, int l, int r, int bit_lo);
        static int first_diff_bit(uint64_t a, uint64_t b);

        void recompute_encoded_bits();
    };

} // namespace otsh