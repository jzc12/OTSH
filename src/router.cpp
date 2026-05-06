#include "otsh/router.h"

#include <algorithm>
#include <cstdint>
#include <functional>

namespace otsh {

namespace {

class BitWriter {
public:
  void write_bit(bool b) {
    const uint64_t pos = bits_;
    const size_t w = static_cast<size_t>(pos / 64);
    const uint64_t o = pos % 64;
    if (w >= buf_.size())
      buf_.resize(w + 1, 0);
    if (b)
      buf_[w] |= (1ULL << o);
    bits_++;
  }

  void write_bits_u64(uint64_t v, int nbits) {
    for (int i = 0; i < nbits; i++) {
      write_bit(((v >> i) & 1ULL) != 0);
    }
  }

  uint64_t bits_written() const { return bits_; }

private:
  std::vector<uint64_t> buf_;
  uint64_t bits_ = 0;
};

} // namespace

bool Router::contains(uint64_t key) const {
  for (const auto &e : entries_) {
    if (e.key == key)
      return true;
  }
  return false;
}

std::pair<std::optional<Router::Value>, uint64_t> Router::locate(uint64_t key) const {
  // steps：访问 trie 的节点数（Patricia 深度 <= 64）。
  uint64_t steps = 0;
  uint64_t bits = key_bits(key);
  int node = root_;
  while (node >= 0 && node < static_cast<int>(nodes_.size())) {
    steps++;
    const Node &nd = nodes_[node];
    if (nd.bucket_idx >= 0) {
      const auto &bucket = buckets_[static_cast<size_t>(nd.bucket_idx)];
      for (int idx : bucket) {
        steps++;
        const Entry &e = entries_[static_cast<size_t>(idx)];
        if (e.key == key)
          return {e.v, steps};
      }
      return {std::nullopt, steps};
    }
    if (nd.entry_idx >= 0) {
      const Entry &e = entries_[static_cast<size_t>(nd.entry_idx)];
      if (e.key == key)
        return {e.v, steps};
      return {std::nullopt, steps};
    }
    const bool dir = (nd.split_bit >= 64) ? false : bit_at(bits, nd.split_bit);
    node = dir ? nd.right : nd.left;
  }
  // fallback scan（理论上不该走到这里）
  for (const auto &e : entries_) {
    steps++;
    if (e.key == key)
      return {e.v, steps};
  }
  return {std::nullopt, steps};
}

uint64_t Router::insert(uint64_t key, Value v) {
  // overwrite semantics
  for (auto &e : entries_) {
    if (e.key == key) {
      e.v = v;
      rebuild();
      return 1;
    }
  }
  entries_.push_back(Entry{key, v, key_bits(key)});
  rebuild();
  return 1;
}

uint64_t Router::erase(uint64_t key) {
  auto it = std::remove_if(entries_.begin(), entries_.end(),
                           [&](const Entry &e) { return e.key == key; });
  if (it != entries_.end()) {
    entries_.erase(it, entries_.end());
    rebuild();
  }
  return 1;
}

void Router::clear() {
  entries_.clear();
  nodes_.clear();
  buckets_.clear();
  root_ = -1;
  encoded_bits_ = 0;
}

int Router::first_diff_bit(uint64_t a, uint64_t b) {
  if (a == b)
    return -1;
  // find MSB differing position as bit index [0..63] (0 is highest-order)
  uint64_t x = a ^ b;
  int msb = 63;
  while (msb >= 0 && ((x >> msb) & 1ULL) == 0)
    msb--;
  // our bit_at uses (63-bitpos), so bitpos = 63 - msb
  return 63 - msb;
}

int Router::build_range(std::vector<int> &idxs, int l, int r, int bit_lo) {
  if (l >= r)
    return -1;
  if (r - l == 1) {
    Node leaf;
    leaf.entry_idx = idxs[l];
    nodes_.push_back(leaf);
    return static_cast<int>(nodes_.size() - 1);
  }

  // check if all bits identical => bucket
  const uint64_t base = entries_[static_cast<size_t>(idxs[l])].bits;
  bool all_same = true;
  for (int i = l + 1; i < r; i++) {
    if (entries_[static_cast<size_t>(idxs[i])].bits != base) {
      all_same = false;
      break;
    }
  }
  if (all_same) {
    Node nd;
    nd.bucket_idx = static_cast<int>(buckets_.size());
    buckets_.push_back(std::vector<int>{});
    buckets_.back().reserve(static_cast<size_t>(r - l));
    for (int i = l; i < r; i++)
      buckets_.back().push_back(idxs[i]);
    nodes_.push_back(nd);
    return static_cast<int>(nodes_.size() - 1);
  }

  // find a split bit that separates the set; use first differing bit from base
  int split = -1;
  for (int i = l + 1; i < r; i++) {
    int d = first_diff_bit(base, entries_[static_cast<size_t>(idxs[i])].bits);
    if (d >= 0) {
      split = d;
      break;
    }
  }
  if (split < 0)
    split = bit_lo;
  if (split < bit_lo)
    split = bit_lo;
  if (split > 64)
    split = 64;

  // partition by split bit
  int mid = l;
  for (int i = l; i < r; i++) {
    if (!bit_at(entries_[static_cast<size_t>(idxs[i])].bits, split)) {
      std::swap(idxs[static_cast<size_t>(mid)], idxs[static_cast<size_t>(i)]);
      mid++;
    }
  }
  if (mid == l || mid == r) {
    // fallback: advance split
    return build_range(idxs, l, r, split + 1);
  }

  Node nd;
  nd.split_bit = static_cast<uint8_t>(split);
  nodes_.push_back(nd);
  const int self = static_cast<int>(nodes_.size() - 1);
  const int left = build_range(idxs, l, mid, split + 1);
  const int right = build_range(idxs, mid, r, split + 1);
  nodes_[self].left = left;
  nodes_[self].right = right;
  return self;
}

void Router::encode_node(int node) {
  // Encoding (simple but real bitstream):
  // - 1 bit: is_leaf
  // - if internal: 6 bits split_bit, then encode left, encode right
  // - if leaf: 1 bit bucket_flag; if bucket => 8 bits count + count*(64 bits key)
  //           else 64 bits key
  static thread_local BitWriter bw; // reuse per recompute call
  // not used; bw is managed by recompute_encoded_bits
  (void)bw;
  (void)node;
}

void Router::recompute_encoded_bits() {
  BitWriter bw;
  // local lambda recursion
  std::function<void(int)> enc = [&](int n) {
    if (n < 0) {
      // null child marker: encode as leaf+bucket+count=0 (cheap, deterministic)
      bw.write_bit(true);  // is_leaf
      bw.write_bit(true);  // is_bucket
      bw.write_bits_u64(0, 8);
      return;
    }
    const Node &nd = nodes_[n];
    const bool is_leaf = (nd.entry_idx >= 0) || (nd.bucket_idx >= 0);
    bw.write_bit(is_leaf);
    if (!is_leaf) {
      // internal
      bw.write_bits_u64(static_cast<uint64_t>(nd.split_bit), 6);
      enc(nd.left);
      enc(nd.right);
      return;
    }
    // leaf
    const bool is_bucket = (nd.bucket_idx >= 0);
    bw.write_bit(is_bucket);
    if (is_bucket) {
      const auto &bucket = buckets_[static_cast<size_t>(nd.bucket_idx)];
      const uint64_t cnt = static_cast<uint64_t>(std::min<size_t>(255, bucket.size()));
      bw.write_bits_u64(cnt, 8);
      for (size_t i = 0; i < cnt; i++) {
        const uint64_t k = entries_[static_cast<size_t>(bucket[i])].key;
        bw.write_bits_u64(k, 64);
      }
    } else {
      const uint64_t k = entries_[static_cast<size_t>(nd.entry_idx)].key;
      bw.write_bits_u64(k, 64);
    }
  };
  enc(root_);
  encoded_bits_ = static_cast<size_t>(bw.bits_written());
}

void Router::rebuild() {
  nodes_.clear();
  buckets_.clear();
  root_ = -1;

  // refresh hashed bits for all entries (stable)
  for (auto &e : entries_) {
    e.bits = key_bits(e.key);
  }

  std::vector<int> idxs;
  idxs.reserve(entries_.size());
  for (size_t i = 0; i < entries_.size(); i++)
    idxs.push_back(static_cast<int>(i));

  root_ = build_range(idxs, 0, static_cast<int>(idxs.size()), 0);
  recompute_encoded_bits();
}

} // namespace otsh

