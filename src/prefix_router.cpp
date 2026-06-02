#include "otsh/prefix_router.h"

#include <algorithm>
#include <vector>

namespace otsh
{
    namespace
    {

        struct Entry
        {
            uint64_t key_bits = 0;
            uint32_t j_value = 0;
        };

        struct Node
        {
            int left = -1;
            int right = -1;
            int entry_idx = -1;
            uint8_t split_bit = 0;
        };

        bool bit_at(uint64_t bits, int bitpos)
        {
            return ((bits >> (63 - bitpos)) & 1ULL) != 0;
        }

    } // namespace

    struct PrefixRouter::Impl
    {
        std::vector<Entry> entries;
        std::vector<Node> nodes;
        int root = -1;

        int build_range(std::vector<int> &idxs, int l, int r, int bit_lo)
        {
            if (l >= r)
                return -1;
            if (r - l == 1)
            {
                Node leaf;
                leaf.entry_idx = idxs[l];
                nodes.push_back(leaf);
                return static_cast<int>(nodes.size() - 1);
            }

            int split = -1;
            int mid = l;
            for (int bit = bit_lo; bit < 64; ++bit)
            {
                mid = l;
                for (int i = l; i < r; ++i)
                {
                    if (!bit_at(entries[static_cast<size_t>(idxs[i])].key_bits, bit))
                    {
                        std::swap(idxs[static_cast<size_t>(mid)], idxs[static_cast<size_t>(i)]);
                        ++mid;
                    }
                }
                if (mid != l && mid != r)
                {
                    split = bit;
                    break;
                }
            }
            if (split < 0)
            {
                Node leaf;
                leaf.entry_idx = idxs[l];
                nodes.push_back(leaf);
                return static_cast<int>(nodes.size() - 1);
            }

            Node nd;
            nd.split_bit = static_cast<uint8_t>(split);
            nodes.push_back(nd);
            const int self = static_cast<int>(nodes.size() - 1);
            nodes[static_cast<size_t>(self)].left = build_range(idxs, l, mid, split + 1);
            nodes[static_cast<size_t>(self)].right = build_range(idxs, mid, r, split + 1);
            return self;
        }

        void rebuild()
        {
            nodes.clear();
            root = -1;
            std::vector<int> idxs;
            idxs.reserve(entries.size());
            for (size_t i = 0; i < entries.size(); ++i)
                idxs.push_back(static_cast<int>(i));
            root = build_range(idxs, 0, static_cast<int>(idxs.size()), 0);
        }
    };

    PrefixRouter::PrefixRouter() : impl_(std::make_unique<Impl>()) {}

    PrefixRouter::PrefixRouter(const PrefixRouter &o)
        : impl_(o.impl_ ? std::make_unique<Impl>(*o.impl_) : nullptr) {}

    PrefixRouter &PrefixRouter::operator=(const PrefixRouter &o)
    {
        if (this == &o)
            return *this;
        impl_ = o.impl_ ? std::make_unique<Impl>(*o.impl_) : nullptr;
        return *this;
    }

    PrefixRouter::~PrefixRouter() = default;

    PrefixRouter::PrefixRouter(PrefixRouter &&) noexcept = default;

    PrefixRouter &PrefixRouter::operator=(PrefixRouter &&) noexcept = default;

    bool PrefixRouter::insert(uint64_t key_bits, uint32_t j)
    {
        if (!impl_)
            impl_ = std::make_unique<Impl>();
        for (auto &e : impl_->entries)
        {
            if (e.key_bits == key_bits)
            {
                e.j_value = j;
                impl_->rebuild();
                return true;
            }
        }
        impl_->entries.push_back(Entry{key_bits, j});
        impl_->rebuild();
        return true;
    }

    bool PrefixRouter::erase(uint64_t key_bits)
    {
        if (!impl_)
            return false;
        auto it = std::remove_if(impl_->entries.begin(), impl_->entries.end(),
                                 [&](const Entry &e)
                                 { return e.key_bits == key_bits; });
        const bool erased = it != impl_->entries.end();
        if (erased)
        {
            impl_->entries.erase(it, impl_->entries.end());
            impl_->rebuild();
        }
        return erased;
    }

    std::optional<uint32_t> PrefixRouter::query(uint64_t key_bits) const
    {
        if (!impl_ || impl_->root < 0)
            return std::nullopt;
        int cur = impl_->root;
        while (cur >= 0)
        {
            const Node &n = impl_->nodes[static_cast<size_t>(cur)];
            if (n.entry_idx >= 0)
            {
                const Entry &e = impl_->entries[static_cast<size_t>(n.entry_idx)];
                if (e.key_bits == key_bits)
                    return e.j_value;
                return std::nullopt;
            }
            cur = bit_at(key_bits, n.split_bit) ? n.right : n.left;
        }
        return std::nullopt;
    }

    void PrefixRouter::clear()
    {
        if (!impl_)
            return;
        impl_->entries.clear();
        impl_->nodes.clear();
        impl_->root = -1;
    }

    size_t PrefixRouter::node_count() const
    {
        return impl_ ? impl_->nodes.size() : 0;
    }

} // namespace otsh
