#include "otsh/mini_array.h"

#include <algorithm>
#include <functional>

namespace otsh {
namespace {

uint8_t g_pop8[256];
bool g_lut_init = false;

void init_rank_select_lut() {
  if (g_lut_init)
    return;
  for (int v = 0; v < 256; ++v)
    g_pop8[v] = static_cast<uint8_t>(__builtin_popcount(v));
  g_lut_init = true;
}

} // namespace

struct MiniArray::Impl {
  static constexpr int kMaxFanout = 32;

  struct Leaf {
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

  struct Node {
    bool is_leaf = true;
    Leaf leaf;
    struct Internal {
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

  void configure(int f, int c) {
    init_rank_select_lut();
    fanout = std::clamp(f, 2, kMaxFanout);
    node_max_bits = std::clamp(c, 1, 3);
    max_leaf_slots = static_cast<size_t>(1u)
                       << static_cast<unsigned>(node_max_bits + 4);
    max_leaf_slots = std::max<size_t>(8, std::min<size_t>(64, max_leaf_slots));
  }

  std::unique_ptr<Node> build_leaf(size_t base, size_t count) const {
    auto nd = std::make_unique<Node>();
    nd->is_leaf = true;
    nd->leaf.base = base;
    nd->leaf.count = count;
    nd->leaf.bitlens.assign(count, 0);
    nd->leaf.entries.assign(count, MiniArray::Bits{});
    return nd;
  }

  std::unique_ptr<Node> build_tree_range(size_t base, size_t count) {
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
    for (; child < nf && rem > 0; ++child) {
      const size_t span = rem / static_cast<size_t>(nf - child);
      rt->internal.children[child] = build_tree_range(base + pos, span);
      rt->internal.subtree_size[child] = static_cast<uint16_t>(span);
      pos += span;
      rem -= span;
    }
    rt->internal.fanout = child;
    return rt;
  }

  void rebuild_tree() {
    root.reset();
    if (n == 0)
      return;
    root = build_tree_range(0, n);
    pull_up(root.get());
  }

  uint16_t subtree_span(const Node *node) const {
    if (!node)
      return 0;
    if (node->is_leaf)
      return static_cast<uint16_t>(node->leaf.count);
    return static_cast<uint16_t>(node->internal.count);
  }

  void pull_up(Node *node) {
    if (!node || node->is_leaf)
      return;
    for (int i = 0; i < node->internal.fanout; ++i)
      pull_up(node->internal.children[i].get());
    for (int i = 0; i < node->internal.fanout; ++i)
      node->internal.subtree_size[i] =
          subtree_span(node->internal.children[i].get());
  }

  Leaf *find_leaf_mut(size_t idx, size_t *rel_out = nullptr) {
    if (!root || idx >= n)
      return nullptr;
    Node *cur = root.get();
    while (cur && !cur->is_leaf) {
      auto &in = cur->internal;
      size_t acc = 0;
      int chosen = -1;
      for (int i = 0; i < in.fanout; ++i) {
        const size_t sz = in.subtree_size[i];
        if (idx < acc + sz) {
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

  const Leaf *find_leaf(size_t idx, size_t *rel_out = nullptr) const {
    return const_cast<Impl *>(this)->find_leaf_mut(idx, rel_out);
  }

  uint64_t total_occupied() const {
    uint64_t t = 0;
    std::function<void(const Node *)> walk = [&](const Node *nd) {
      if (!nd)
        return;
      if (nd->is_leaf) {
        t += nd->leaf.occupied();
        return;
      }
      for (int i = 0; i < nd->internal.fanout; ++i)
        walk(nd->internal.children[i].get());
    };
    walk(root.get());
    return t;
  }

  static uint64_t node_occupied(const Node *nd) {
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
                              uint64_t *local_logical) const {
    if (!nd)
      return nullptr;
    if (nd->is_leaf) {
      if (logical >= nd->leaf.occupied())
        return nullptr;
      if (local_logical)
        *local_logical = logical;
      return &nd->leaf;
    }
    uint64_t acc = 0;
    for (int i = 0; i < nd->internal.fanout; ++i) {
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
                            uint64_t *local_logical) {
    return const_cast<Leaf *>(
        leaf_at_logical(nd, logical, local_logical));
  }
};

uint32_t MiniArray::Impl::Leaf::occupied() const {
  if (count == 0)
    return 0;
  if (count >= 64)
    return static_cast<uint32_t>(__builtin_popcountll(bitmap));
  return static_cast<uint32_t>(
      __builtin_popcountll(bitmap & ((1ULL << count) - 1)));
}

int MiniArray::Impl::Leaf::rel_from_logical(uint32_t logical) const {
  return MiniArray::select_u64(bitmap, logical);
}

uint32_t MiniArray::rank_u64(uint64_t bitmap, uint32_t i) {
  init_rank_select_lut();
  if (i == 0)
    return 0;
  const uint32_t lim = std::min(i, 64u);
  uint32_t c = 0;
  for (uint32_t b = 0; b < lim; ++b) {
    if ((bitmap >> b) & 1ULL)
      ++c;
  }
  return c;
}

int MiniArray::select_u64(uint64_t bitmap, uint32_t k) {
  init_rank_select_lut();
  uint32_t seen = 0;
  for (int b = 0; b < 64; ++b) {
    if ((bitmap >> b) & 1ULL) {
      if (seen == k)
        return b;
      ++seen;
    }
  }
  return -1;
}

void MiniArray::Impl::Leaf::repack() {
  packed_bits = 0;
  packed.clear();
  for (size_t rel = 0; rel < count; ++rel) {
    if (((bitmap >> rel) & 1ULL) == 0)
      continue;
    const uint32_t bl = bitlens[rel];
    const uint64_t start = packed_bits;
    packed_bits += bl;
    const size_t words = static_cast<size_t>((packed_bits + 63) / 64);
    if (packed.size() < words)
      packed.resize(words, 0);
    for (uint32_t b = 0; b < bl; ++b) {
      const uint32_t w = b / 64;
      const uint32_t o = b % 64;
      if (((entries[rel][w] >> o) & 1ULL) != 0) {
        const uint64_t pos = start + b;
        packed[pos / 64] |= (1ULL << (pos % 64));
      }
    }
  }
}

MiniArray::Bits MiniArray::Impl::Leaf::read_rel(size_t rel) const {
  MiniArray::Bits out;
  if (rel >= count || ((bitmap >> rel) & 1ULL) == 0)
    return out;
  const uint32_t blen = bitlens[rel];
  out.assign((blen + 63) / 64, 0);
  uint64_t start = 0;
  for (size_t i = 0; i < rel; ++i) {
    if ((bitmap >> i) & 1ULL)
      start += bitlens[i];
  }
  for (uint32_t b = 0; b < blen; ++b) {
    const uint64_t pos = start + b;
    const uint64_t w = pos / 64;
    const uint64_t o = pos % 64;
    if (w < packed.size() && ((packed[w] >> o) & 1ULL))
      out[b / 64] |= (1ULL << (b % 64));
  }
  return out;
}

void MiniArray::Impl::Leaf::write_rel(size_t rel, const MiniArray::Bits &bits,
                                      uint32_t blen) {
  if (rel >= count)
    return;
  if (blen == 0) {
    clear_rel(rel);
    return;
  }
  bitmap |= (1ULL << rel);
  bitlens[rel] = blen;
  entries[rel] = bits;
  repack();
}

void MiniArray::Impl::Leaf::clear_rel(size_t rel) {
  if (rel >= count)
    return;
  bitmap &= ~(1ULL << rel);
  bitlens[rel] = 0;
  entries[rel].clear();
  repack();
}

MiniArray::MiniArray() : impl_(std::make_unique<Impl>()) {
  init_rank_select_lut();
}

MiniArray::~MiniArray() = default;
MiniArray::MiniArray(MiniArray &&) noexcept = default;
MiniArray &MiniArray::operator=(MiniArray &&) noexcept = default;

MiniArray::MiniArray(size_t n) : impl_(std::make_unique<Impl>()) {
  init_rank_select_lut();
  reset(n);
}

void MiniArray::configure(int fanout, int node_max_bits) {
  if (!impl_)
    impl_ = std::make_unique<Impl>();
  impl_->configure(fanout, node_max_bits);
}

void MiniArray::reset(size_t n) {
  if (!impl_)
    impl_ = std::make_unique<Impl>();
  impl_->n = n;
  impl_->rebuild_tree();
}

size_t MiniArray::size() const { return impl_ ? impl_->n : 0; }

uint64_t MiniArray::occupied_count() const {
  return impl_ ? impl_->total_occupied() : 0;
}

uint64_t MiniArray::bits_total() const {
  if (!impl_ || !impl_->root)
    return 0;
  uint64_t t = 0;
  std::function<void(const Impl::Node *)> walk = [&](const Impl::Node *nd) {
    if (!nd)
      return;
    if (nd->is_leaf) {
      t += nd->leaf.packed_bits;
      return;
    }
    for (int i = 0; i < nd->internal.fanout; ++i)
      walk(nd->internal.children[i].get());
  };
  walk(impl_->root.get());
  return t;
}

uint32_t MiniArray::bitlen(size_t i) const {
  if (!impl_)
    return 0;
  size_t rel = 0;
  const Impl::Leaf *lf = impl_->find_leaf(i, &rel);
  if (!lf)
    return 0;
  return lf->bitlens[rel];
}

MiniArray::Bits MiniArray::access(size_t i) const {
  if (!impl_)
    return Bits{};
  size_t rel = 0;
  const Impl::Leaf *lf = impl_->find_leaf(i, &rel);
  if (!lf)
    return Bits{};
  return lf->read_rel(rel);
}

void MiniArray::update(size_t i, const Bits &bits, uint32_t blen) {
  if (!impl_ || i >= impl_->n)
    throw std::out_of_range("MiniArray::update");
  size_t rel = 0;
  Impl::Leaf *lf = impl_->find_leaf_mut(i, &rel);
  if (!lf)
    throw std::out_of_range("MiniArray::update leaf");
  lf->write_rel(rel, bits, blen);
  impl_->pull_up(impl_->root.get());
}

void MiniArray::erase(size_t i) {
  if (!impl_ || i >= impl_->n)
    return;
  size_t rel = 0;
  Impl::Leaf *lf = impl_->find_leaf_mut(i, &rel);
  if (!lf)
    return;
  lf->clear_rel(rel);
  impl_->pull_up(impl_->root.get());
}

std::optional<MiniArray::Bits> MiniArray::access_logical(size_t logical_idx) const {
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
                               uint32_t blen) {
  if (!impl_ || blen == 0 || !impl_->root)
    return;
  uint64_t local = 0;
  Impl::Leaf *lf =
      impl_->leaf_at_logical_mut(impl_->root.get(), logical_idx, &local);
  if (!lf) {
    for (size_t i = 0; i < impl_->n; ++i) {
      if (bitlen(i) == 0) {
        update(i, bits, blen);
        return;
      }
    }
    return;
  }
  int rel = lf->rel_from_logical(static_cast<uint32_t>(local));
  if (rel < 0) {
    for (size_t r = 0; r < lf->count; ++r) {
      if (((lf->bitmap >> r) & 1ULL) == 0) {
        rel = static_cast<int>(r);
        break;
      }
    }
    if (rel < 0)
      return;
  }
  lf->write_rel(static_cast<size_t>(rel), bits, blen);
  impl_->pull_up(impl_->root.get());
}

void MiniArray::delete_logical(size_t logical_idx) {
  if (!impl_ || !impl_->root)
    return;
  uint64_t local = 0;
  Impl::Leaf *lf =
      impl_->leaf_at_logical_mut(impl_->root.get(), logical_idx, &local);
  if (!lf)
    return;
  const int rel = lf->rel_from_logical(static_cast<uint32_t>(local));
  if (rel < 0)
    return;
  lf->clear_rel(static_cast<size_t>(rel));
  impl_->pull_up(impl_->root.get());
}

} // namespace otsh
