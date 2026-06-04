#include "otsh/mini_array.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <functional>

namespace otsh
{
    namespace
    {

        uint8_t g_pop8[256];
        bool g_lut_init = false;

        void init_rank_select_lut()
        {
            if (g_lut_init)
                return;
            for (int v = 0; v < 256; ++v)
                g_pop8[v] = static_cast<uint8_t>(std::popcount(static_cast<unsigned>(v)));
            g_lut_init = true;
        }

    } // namespace

    struct MiniArray::Impl
    {
        static constexpr int kMaxFanout = 32;
        struct Entry
        {
            MiniArray::Bits bits;
            uint32_t bitlen = 0;
        };

        struct Leaf
        {
            size_t base = 0;
            size_t count = 0;
            uint64_t bitmap = 0;
            std::vector<uint32_t> bitlens;
            std::vector<MiniArray::Bits> entries;
            std::vector<uint64_t> packed;
            uint64_t packed_bits = 0;

            uint32_t occupied() const;
            void repack();
            MiniArray::Bits read_rel(size_t rel) const;
            void write_rel(size_t rel, const MiniArray::Bits &bits, uint32_t blen);
            void clear_rel(size_t rel);
            int rel_from_logical(uint32_t logical) const;
        };

        struct Node
        {
            bool is_leaf = true;
            Leaf leaf;
            struct Internal
            {
                size_t base = 0;
                size_t count = 0;
                int fanout = 8;
                uint16_t subtree_size[kMaxFanout]{};
                std::unique_ptr<Node> children[kMaxFanout];
            } internal;
        };

        int fanout = 8;
        int node_max_bits = 2;
        size_t max_leaf_slots = 16;
        size_t n = 0;
        std::unique_ptr<Node> root;

        size_t leaf_capacity() const { return max_leaf_slots; }

        void configure(int f, int c)
        {
            init_rank_select_lut();
            fanout = std::clamp(f, 2, kMaxFanout);
            node_max_bits = std::clamp(c, 1, 3);
            max_leaf_slots = static_cast<size_t>(1u)
                             << static_cast<unsigned>(node_max_bits + 4);
            max_leaf_slots = std::max<size_t>(8, std::min<size_t>(64, max_leaf_slots));
        }

        std::unique_ptr<Node> build_leaf(size_t base, size_t count) const
        {
            auto nd = std::make_unique<Node>();
            nd->is_leaf = true;
            nd->leaf.base = base;
            nd->leaf.count = count;
            nd->leaf.bitlens.assign(count, 0);
            nd->leaf.entries.assign(count, MiniArray::Bits{});
            return nd;
        }

        std::unique_ptr<Node> build_tree_range(size_t base, size_t count)
        {
            if (count <= leaf_capacity())
                return build_leaf(base, count);
            auto rt = std::make_unique<Node>();
            rt->is_leaf = false;
            rt->internal.base = base;
            rt->internal.count = count;
            const int need =
                static_cast<int>((count + leaf_capacity() - 1) / leaf_capacity());
            const int nf = std::min(fanout, std::max(2, need));
            size_t pos = 0;
            size_t rem = count;
            int child = 0;
            for (; child < nf && rem > 0; ++child)
            {
                const size_t span = rem / static_cast<size_t>(nf - child);
                rt->internal.children[child] = build_tree_range(base + pos, span);
                rt->internal.subtree_size[child] = static_cast<uint16_t>(span);
                pos += span;
                rem -= span;
            }
            rt->internal.fanout = child;
            return rt;
        }

        void rebuild_tree()
        {
            root.reset();
            if (n == 0)
                return;
            root = build_tree_range(0, n);
            pull_up(root.get());
        }

        uint16_t subtree_span(const Node *node) const
        {
            if (!node)
                return 0;
            if (node->is_leaf)
                return static_cast<uint16_t>(node->leaf.count);
            return static_cast<uint16_t>(node->internal.count);
        }

        void pull_up(Node *node)
        {
            if (!node || node->is_leaf)
                return;
            for (int i = 0; i < node->internal.fanout; ++i)
                pull_up(node->internal.children[i].get());
            size_t total = 0;
            for (int i = 0; i < node->internal.fanout; ++i)
                total += (node->internal.subtree_size[i] =
                              subtree_span(node->internal.children[i].get()));
            node->internal.count = total;
        }

        Leaf *find_leaf_mut(size_t idx, size_t *rel_out = nullptr)
        {
            if (!root || idx >= n)
                return nullptr;
            Node *cur = root.get();
            while (cur && !cur->is_leaf)
            {
                auto &in = cur->internal;
                size_t acc = 0;
                int chosen = -1;
                for (int i = 0; i < in.fanout; ++i)
                {
                    const size_t sz = in.subtree_size[i];
                    if (idx < acc + sz)
                    {
                        chosen = i;
                        break;
                    }
                    acc += sz;
                }
                if (chosen < 0)
                    return nullptr;
                idx -= acc;
                cur = in.children[chosen].get();
            }
            if (!cur || !cur->is_leaf)
                return nullptr;
            Leaf *lf = &cur->leaf;
            if (idx >= lf->count)
                return nullptr;
            if (rel_out)
                *rel_out = idx;
            return lf;
        }

        const Leaf *find_leaf(size_t idx, size_t *rel_out = nullptr) const
        {
            return const_cast<Impl *>(this)->find_leaf_mut(idx, rel_out);
        }

        uint64_t total_occupied() const
        {
            uint64_t t = 0;
            std::function<void(const Node *)> walk = [&](const Node *nd)
            {
                if (!nd)
                    return;
                if (nd->is_leaf)
                {
                    t += nd->leaf.occupied();
                    return;
                }
                for (int i = 0; i < nd->internal.fanout; ++i)
                    walk(nd->internal.children[i].get());
            };
            walk(root.get());
            return t;
        }

        static std::vector<Entry> leaf_entries(const Leaf &leaf)
        {
            std::vector<Entry> out;
            out.reserve(leaf.occupied());
            for (size_t i = 0; i < leaf.count; ++i)
            {
                if (i >= leaf.bitlens.size() || leaf.bitlens[i] == 0)
                    continue;
                out.push_back(Entry{leaf.read_rel(i), leaf.bitlens[i]});
            }
            return out;
        }

        static uint64_t low_bits(size_t count)
        {
            if (count == 0)
                return 0;
            if (count >= 64)
                return ~0ULL;
            return (1ULL << count) - 1ULL;
        }

        static void write_leaf_entries(Leaf &leaf, const std::vector<Entry> &entries)
        {
            leaf.count = entries.size();
            leaf.bitmap = low_bits(entries.size());
            leaf.bitlens.assign(entries.size(), 0);
            leaf.entries.assign(entries.size(), MiniArray::Bits{});
            leaf.packed.clear();
            leaf.packed_bits = 0;
            for (size_t i = 0; i < entries.size(); ++i)
            {
                leaf.bitlens[i] = entries[i].bitlen;
                leaf.entries[i] = entries[i].bits;
            }
            leaf.repack();
        }

        std::unique_ptr<Node> make_leaf_from_entries(const std::vector<Entry> &entries) const
        {
            auto n = std::make_unique<Node>();
            n->is_leaf = true;
            write_leaf_entries(n->leaf, entries);
            return n;
        }

        std::unique_ptr<Node> split_leaf_insert(Leaf &leaf, size_t idx,
                                                const Entry &entry) const
        {
            auto entries = leaf_entries(leaf);
            entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(
                                                 std::min(idx, entries.size())),
                           entry);
            const size_t mid = entries.size() / 2;
            std::vector<Entry> left(entries.begin(), entries.begin() + mid);
            std::vector<Entry> right(entries.begin() + mid, entries.end());
            write_leaf_entries(leaf, left);
            return make_leaf_from_entries(right);
        }

        std::unique_ptr<Node> split_internal_insert(Node &node, int child_pos,
                                                    std::unique_ptr<Node> extra)
        {
            std::vector<std::unique_ptr<Node>> children;
            children.reserve(static_cast<size_t>(node.internal.fanout + 1));
            for (int i = 0; i < node.internal.fanout; ++i)
            {
                children.push_back(std::move(node.internal.children[i]));
                if (i == child_pos)
                    children.push_back(std::move(extra));
            }
            const size_t mid = children.size() / 2;
            auto right = std::make_unique<Node>();
            right->is_leaf = false;
            node.internal.fanout = static_cast<int>(mid);
            for (int i = 0; i < kMaxFanout; ++i)
                node.internal.children[i].reset();
            for (size_t i = 0; i < mid; ++i)
                node.internal.children[i] = std::move(children[i]);
            right->internal.fanout = static_cast<int>(children.size() - mid);
            for (size_t i = mid; i < children.size(); ++i)
                right->internal.children[i - mid] = std::move(children[i]);
            pull_up(&node);
            pull_up(right.get());
            return right;
        }

        std::unique_ptr<Node> insert_logical_rec(Node &node, size_t idx,
                                                 const Entry &entry)
        {
            if (node.is_leaf)
            {
                if (node.leaf.occupied() < leaf_capacity())
                {
                    auto entries = leaf_entries(node.leaf);
                    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(
                                                         std::min(idx, entries.size())),
                                   entry);
                    write_leaf_entries(node.leaf, entries);
                    return nullptr;
                }
                return split_leaf_insert(node.leaf, idx, entry);
            }

            size_t acc = 0;
            int child_pos = node.internal.fanout - 1;
            for (int i = 0; i < node.internal.fanout; ++i)
            {
                const size_t span = subtree_span(node.internal.children[i].get());
                if (idx <= acc + span)
                {
                    child_pos = i;
                    break;
                }
                acc += span;
            }
            auto extra = insert_logical_rec(*node.internal.children[child_pos],
                                            idx > acc ? idx - acc : 0, entry);
            if (extra)
            {
                if (node.internal.fanout < fanout)
                {
                    for (int i = node.internal.fanout; i > child_pos + 1; --i)
                        node.internal.children[i] = std::move(node.internal.children[i - 1]);
                    node.internal.children[child_pos + 1] = std::move(extra);
                    ++node.internal.fanout;
                }
                else
                {
                    return split_internal_insert(node, child_pos, std::move(extra));
                }
            }
            pull_up(&node);
            return nullptr;
        }

        bool merge_leaf_children(Node &node, int left_pos)
        {
            if (left_pos < 0 || left_pos + 1 >= node.internal.fanout)
                return false;
            Node *left = node.internal.children[left_pos].get();
            Node *right = node.internal.children[left_pos + 1].get();
            if (!left || !right || !left->is_leaf || !right->is_leaf)
                return false;
            auto entries = leaf_entries(left->leaf);
            auto rhs = leaf_entries(right->leaf);
            if (entries.size() + rhs.size() > leaf_capacity())
                return false;
            entries.insert(entries.end(), rhs.begin(), rhs.end());
            write_leaf_entries(left->leaf, entries);
            for (int i = left_pos + 1; i + 1 < node.internal.fanout; ++i)
                node.internal.children[i] = std::move(node.internal.children[i + 1]);
            node.internal.children[node.internal.fanout - 1].reset();
            --node.internal.fanout;
            pull_up(&node);
            return true;
        }

        bool delete_logical_rec(Node &node, size_t idx)
        {
            if (node.is_leaf)
            {
                auto entries = leaf_entries(node.leaf);
                if (idx >= entries.size())
                    return false;
                entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(idx));
                write_leaf_entries(node.leaf, entries);
                return true;
            }

            size_t acc = 0;
            int child_pos = -1;
            for (int i = 0; i < node.internal.fanout; ++i)
            {
                const size_t span = node_occupied(node.internal.children[i].get());
                if (idx < acc + span)
                {
                    child_pos = i;
                    break;
                }
                acc += span;
            }
            if (child_pos < 0)
                return false;
            const bool removed =
                delete_logical_rec(*node.internal.children[child_pos], idx - acc);
            if (!removed)
                return false;
            Node *child = node.internal.children[child_pos].get();
            if (child && child->is_leaf &&
                child->leaf.occupied() < std::max<size_t>(1, leaf_capacity() / 2))
            {
                if (!merge_leaf_children(node, child_pos) && child_pos > 0)
                    (void)merge_leaf_children(node, child_pos - 1);
            }
            pull_up(&node);
            return true;
        }

        static uint64_t node_occupied(const Node *nd)
        {
            if (!nd)
                return 0;
            if (nd->is_leaf)
                return nd->leaf.occupied();
            uint64_t s = 0;
            for (int i = 0; i < nd->internal.fanout; ++i)
                s += node_occupied(nd->internal.children[i].get());
            return s;
        }

        const Leaf *leaf_at_logical(const Node *nd, uint64_t logical,
                                    uint64_t *local_logical) const
        {
            if (!nd)
                return nullptr;
            if (nd->is_leaf)
            {
                if (logical >= nd->leaf.occupied())
                    return nullptr;
                if (local_logical)
                    *local_logical = logical;
                return &nd->leaf;
            }
            uint64_t acc = 0;
            for (int i = 0; i < nd->internal.fanout; ++i)
            {
                const Node *ch = nd->internal.children[i].get();
                if (!ch)
                    break;
                const uint64_t sub = node_occupied(ch);
                if (logical < acc + sub)
                    return leaf_at_logical(ch, logical - acc, local_logical);
                acc += sub;
            }
            return nullptr;
        }

        Leaf *leaf_at_logical_mut(Node *nd, uint64_t logical,
                                  uint64_t *local_logical)
        {
            return const_cast<Leaf *>(
                leaf_at_logical(nd, logical, local_logical));
        }
    };

    uint32_t MiniArray::Impl::Leaf::occupied() const
    {
        if (count == 0)
            return 0;
        if (count >= 64)
            return static_cast<uint32_t>(std::popcount(bitmap));
        return static_cast<uint32_t>(
            std::popcount(bitmap & ((1ULL << count) - 1)));
    }

    int MiniArray::Impl::Leaf::rel_from_logical(uint32_t logical) const
    {
        return MiniArray::select_u64(bitmap, logical);
    }

    uint32_t MiniArray::rank_u64(uint64_t bitmap, uint32_t i)
    {
        init_rank_select_lut();
        if (i == 0)
            return 0;
        const uint32_t lim = std::min(i, 64u);
        uint32_t c = 0;
        for (uint32_t b = 0; b < lim; ++b)
        {
            if ((bitmap >> b) & 1ULL)
                ++c;
        }
        return c;
    }

    int MiniArray::select_u64(uint64_t bitmap, uint32_t k)
    {
        init_rank_select_lut();
        uint32_t seen = 0;
        for (int b = 0; b < 64; ++b)
        {
            if ((bitmap >> b) & 1ULL)
            {
                if (seen == k)
                    return b;
                ++seen;
            }
        }
        return -1;
    }

    void MiniArray::Impl::Leaf::repack()
    {
        packed_bits = 0;
        packed.clear();
        for (size_t rel = 0; rel < count; ++rel)
        {
            if (((bitmap >> rel) & 1ULL) == 0)
                continue;
            const uint32_t bl = bitlens[rel];
            const uint64_t start = packed_bits;
            packed_bits += bl;
            const size_t words = static_cast<size_t>((packed_bits + 63) / 64);
            if (packed.size() < words)
                packed.resize(words, 0);
            for (uint32_t b = 0; b < bl; ++b)
            {
                const uint32_t w = b / 64;
                const uint32_t o = b % 64;
                if (((entries[rel][w] >> o) & 1ULL) != 0)
                {
                    const uint64_t pos = start + b;
                    packed[pos / 64] |= (1ULL << (pos % 64));
                }
            }
        }
    }

    MiniArray::Bits MiniArray::Impl::Leaf::read_rel(size_t rel) const
    {
        MiniArray::Bits out;
        if (rel >= count || ((bitmap >> rel) & 1ULL) == 0)
            return out;
        const uint32_t blen = bitlens[rel];
        out.assign((blen + 63) / 64, 0);
        uint64_t start = 0;
        for (size_t i = 0; i < rel; ++i)
        {
            if ((bitmap >> i) & 1ULL)
                start += bitlens[i];
        }
        for (uint32_t b = 0; b < blen; ++b)
        {
            const uint64_t pos = start + b;
            const uint64_t w = pos / 64;
            const uint64_t o = pos % 64;
            if (w < packed.size() && ((packed[w] >> o) & 1ULL))
                out[b / 64] |= (1ULL << (b % 64));
        }
        return out;
    }

    void MiniArray::Impl::Leaf::write_rel(size_t rel, const MiniArray::Bits &bits,
                                          uint32_t blen)
    {
        if (rel >= count)
            return;
        if (blen == 0)
        {
            clear_rel(rel);
            return;
        }
        bitmap |= (1ULL << rel);
        bitlens[rel] = blen;
        entries[rel] = bits;
        repack();
    }

    void MiniArray::Impl::Leaf::clear_rel(size_t rel)
    {
        if (rel >= count)
            return;
        bitmap &= ~(1ULL << rel);
        bitlens[rel] = 0;
        entries[rel].clear();
        repack();
    }

    MiniArray::MiniArray() : impl_(std::make_unique<Impl>())
    {
        init_rank_select_lut();
    }

    MiniArray::~MiniArray() = default;
    MiniArray::MiniArray(MiniArray &&) noexcept = default;
    MiniArray &MiniArray::operator=(MiniArray &&) noexcept = default;

    MiniArray::MiniArray(size_t n) : impl_(std::make_unique<Impl>())
    {
        init_rank_select_lut();
        reset(n);
    }

    void MiniArray::configure(int fanout, int node_max_bits)
    {
        if (!impl_)
            impl_ = std::make_unique<Impl>();
        impl_->configure(fanout, node_max_bits);
    }

    void MiniArray::reset(size_t n)
    {
        if (!impl_)
            impl_ = std::make_unique<Impl>();
        impl_->n = n;
        impl_->rebuild_tree();
    }

    size_t MiniArray::size() const { return impl_ ? impl_->n : 0; }

    uint64_t MiniArray::occupied_count() const
    {
        return impl_ ? impl_->total_occupied() : 0;
    }

    uint64_t MiniArray::bits_total() const
    {
        if (!impl_ || !impl_->root)
            return 0;
        uint64_t t = 0;
        std::function<void(const Impl::Node *)> walk = [&](const Impl::Node *nd)
        {
            if (!nd)
                return;
            if (nd->is_leaf)
            {
                t += nd->leaf.packed_bits;
                return;
            }
            for (int i = 0; i < nd->internal.fanout; ++i)
                walk(nd->internal.children[i].get());
        };
        walk(impl_->root.get());
        return t;
    }

    uint32_t MiniArray::bitlen(size_t i) const
    {
        if (!impl_)
            return 0;
        size_t rel = 0;
        const Impl::Leaf *lf = impl_->find_leaf(i, &rel);
        if (!lf)
            return 0;
        return lf->bitlens[rel];
    }

    MiniArray::Bits MiniArray::access(size_t i) const
    {
        if (!impl_)
            return Bits{};
        size_t rel = 0;
        const Impl::Leaf *lf = impl_->find_leaf(i, &rel);
        if (!lf)
            return Bits{};
        return lf->read_rel(rel);
    }

    void MiniArray::update(size_t i, const Bits &bits, uint32_t blen)
    {
        if (!impl_ || i >= impl_->n)
            throw std::out_of_range("MiniArray::update");
        size_t rel = 0;
        Impl::Leaf *lf = impl_->find_leaf_mut(i, &rel);
        if (!lf)
            throw std::out_of_range("MiniArray::update leaf");
        lf->write_rel(rel, bits, blen);
        impl_->pull_up(impl_->root.get());
    }

    void MiniArray::erase(size_t i)
    {
        if (!impl_ || i >= impl_->n)
            return;
        size_t rel = 0;
        Impl::Leaf *lf = impl_->find_leaf_mut(i, &rel);
        if (!lf)
            return;
        lf->clear_rel(rel);
        impl_->pull_up(impl_->root.get());
    }

    std::optional<MiniArray::Bits> MiniArray::access_logical(size_t logical_idx) const
    {
        if (!impl_ || !impl_->root)
            return std::nullopt;
        uint64_t local = 0;
        const Impl::Leaf *lf =
            impl_->leaf_at_logical(impl_->root.get(), logical_idx, &local);
        if (!lf)
            return std::nullopt;
        const int rel = lf->rel_from_logical(static_cast<uint32_t>(local));
        if (rel < 0)
            return std::nullopt;
        return lf->read_rel(static_cast<size_t>(rel));
    }

    void MiniArray::insert_logical(size_t logical_idx, const Bits &bits,
                                   uint32_t blen)
    {
        if (!impl_ || blen == 0)
            return;
        if (!impl_->root)
            impl_->root = impl_->make_leaf_from_entries({});
        const size_t pos = std::min<size_t>(logical_idx, impl_->total_occupied());
        auto extra =
            impl_->insert_logical_rec(*impl_->root, pos, Impl::Entry{bits, blen});
        if (extra)
        {
            auto new_root = std::make_unique<Impl::Node>();
            new_root->is_leaf = false;
            new_root->internal.fanout = 2;
            new_root->internal.children[0] = std::move(impl_->root);
            new_root->internal.children[1] = std::move(extra);
            impl_->root = std::move(new_root);
        }
        impl_->pull_up(impl_->root.get());
        impl_->n = static_cast<size_t>(Impl::node_occupied(impl_->root.get()));
    }

    void MiniArray::delete_logical(size_t logical_idx)
    {
        if (!impl_ || !impl_->root)
            return;
        if (logical_idx >= impl_->total_occupied())
            return;
        if (!impl_->delete_logical_rec(*impl_->root, logical_idx))
            return;
        if (!impl_->root->is_leaf && impl_->root->internal.fanout == 1)
            impl_->root = std::move(impl_->root->internal.children[0]);
        impl_->pull_up(impl_->root.get());
        impl_->n = static_cast<size_t>(Impl::node_occupied(impl_->root.get()));
    }

} // namespace otsh