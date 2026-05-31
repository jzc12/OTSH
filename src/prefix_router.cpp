#include "otsh/prefix_router.h"

#include <vector>

namespace otsh {
namespace {

struct Node {
  int child0 = -1;
  int child1 = -1;
  bool is_leaf = false;
  uint32_t j_value = 0;
  uint64_t key_bits = 0;
};

int walk(const std::vector<Node> &nodes, int root, uint64_t key_bits) {
  if (root < 0)
    return -1;
  int cur = root;
  for (int bit = 63; bit >= 0; --bit) {
    const bool b = ((key_bits >> bit) & 1ULL) != 0;
    const Node &n = nodes[static_cast<size_t>(cur)];
    const int child = b ? n.child1 : n.child0;
    if (child < 0)
      return -1;
    cur = child;
  }
  return cur;
}

static int first_diff_bit(uint64_t a, uint64_t b) {
  if (a == b)
    return -1;
  uint64_t x = a ^ b;
  int msb = 63;
  while (msb >= 0 && ((x >> msb) & 1ULL) == 0)
    msb--;
  return msb;
}

static void split_leaf(std::vector<Node> &nodes, int leaf_idx, uint64_t old_key,
                       uint32_t old_j, uint64_t new_key, uint32_t new_j) {
  const int diff = first_diff_bit(old_key, new_key);
  if (diff < 0) {
    nodes[static_cast<size_t>(leaf_idx)].j_value = new_j;
    return;
  }
  nodes[static_cast<size_t>(leaf_idx)].is_leaf = false;
  nodes[static_cast<size_t>(leaf_idx)].j_value = 0;
  nodes[static_cast<size_t>(leaf_idx)].key_bits = 0;
  const size_t base = nodes.size();
  nodes.push_back({-1, -1, true, old_j, old_key});
  nodes.push_back({-1, -1, true, new_j, new_key});
  const int leaf_old = static_cast<int>(base);
  const int leaf_new = static_cast<int>(base + 1);
  const bool old_bit = ((old_key >> diff) & 1ULL) != 0;
  if (old_bit) {
    nodes[static_cast<size_t>(leaf_idx)].child1 = leaf_old;
    nodes[static_cast<size_t>(leaf_idx)].child0 = leaf_new;
  } else {
    nodes[static_cast<size_t>(leaf_idx)].child0 = leaf_old;
    nodes[static_cast<size_t>(leaf_idx)].child1 = leaf_new;
  }
}

} // namespace

struct PrefixRouter::Impl {
  std::vector<Node> nodes;
  int root = -1;
};

PrefixRouter::PrefixRouter() : impl_(std::make_unique<Impl>()) {}

PrefixRouter::PrefixRouter(const PrefixRouter &o)
    : impl_(o.impl_ ? std::make_unique<Impl>(*o.impl_) : nullptr) {}

PrefixRouter &PrefixRouter::operator=(const PrefixRouter &o) {
  if (this == &o)
    return *this;
  impl_ = o.impl_ ? std::make_unique<Impl>(*o.impl_) : nullptr;
  return *this;
}

PrefixRouter::~PrefixRouter() = default;

PrefixRouter::PrefixRouter(PrefixRouter &&) noexcept = default;

PrefixRouter &PrefixRouter::operator=(PrefixRouter &&) noexcept = default;

bool PrefixRouter::insert(uint64_t key_bits, uint32_t j) {
  if (!impl_)
    impl_ = std::make_unique<Impl>();
  auto &nodes = impl_->nodes;
  if (nodes.empty()) {
    nodes.push_back(Node{});
    impl_->root = 0;
  }
  int cur = impl_->root;
  for (int bit = 63; bit >= 0; --bit) {
    const bool b = ((key_bits >> bit) & 1ULL) != 0;
    if (nodes[static_cast<size_t>(cur)].is_leaf) {
      const uint64_t existing = nodes[static_cast<size_t>(cur)].key_bits;
      if (existing == key_bits) {
        nodes[static_cast<size_t>(cur)].j_value = j;
        return true;
      }
      split_leaf(nodes, cur, existing,
                 nodes[static_cast<size_t>(cur)].j_value, key_bits, j);
      return true;
    }
    int child = b ? nodes[static_cast<size_t>(cur)].child1
                  : nodes[static_cast<size_t>(cur)].child0;
    if (child < 0) {
      nodes.push_back(Node{});
      child = static_cast<int>(nodes.size() - 1);
      if (b)
        nodes[static_cast<size_t>(cur)].child1 = child;
      else
        nodes[static_cast<size_t>(cur)].child0 = child;
    }
    cur = child;
  }
  nodes[static_cast<size_t>(cur)].is_leaf = true;
  nodes[static_cast<size_t>(cur)].j_value = j;
  nodes[static_cast<size_t>(cur)].key_bits = key_bits;
  return true;
}

bool PrefixRouter::erase(uint64_t key_bits) {
  if (!impl_)
    return false;
  const int cur = walk(impl_->nodes, impl_->root, key_bits);
  if (cur < 0)
    return false;
  Node &leaf = impl_->nodes[static_cast<size_t>(cur)];
  if (!leaf.is_leaf || leaf.key_bits != key_bits)
    return false;
  leaf.is_leaf = false;
  leaf.j_value = 0;
  leaf.key_bits = 0;
  return true;
}

std::optional<uint32_t> PrefixRouter::query(uint64_t key_bits) const {
  if (!impl_)
    return std::nullopt;
  const int cur = walk(impl_->nodes, impl_->root, key_bits);
  if (cur < 0)
    return std::nullopt;
  const Node &leaf = impl_->nodes[static_cast<size_t>(cur)];
  if (!leaf.is_leaf || leaf.key_bits != key_bits)
    return std::nullopt;
  return leaf.j_value;
}

void PrefixRouter::clear() {
  if (!impl_)
    return;
  impl_->nodes.clear();
  impl_->root = -1;
}

size_t PrefixRouter::node_count() const {
  return impl_ ? impl_->nodes.size() : 0;
}

} // namespace otsh
