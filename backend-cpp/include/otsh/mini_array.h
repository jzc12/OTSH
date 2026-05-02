#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace otsh {

// Phase4: MiniArray（可工作的正确版）
// - 变长 bit entry，紧凑打包在一个 bit buffer 中
// - 为了先把系统跑通：更新时允许 O(m) 重打包（m 为该 MiniArray 的 entry 数）
//   设计文档中的均摊 O(1) 与查表加速会在后续优化。
class MiniArray {
public:
  using Bits = std::vector<uint64_t>; // little-endian 64-bit words

  MiniArray() = default;
  explicit MiniArray(size_t n) { reset(n); }

  void reset(size_t n) {
    bitlens_.assign(n, 0);
    offsets_.assign(n + 1, 0);
    entries_.assign(n, Bits{});
    buf_.clear();
  }

  size_t size() const { return bitlens_.size(); }

  // 返回总 bit 数（用于 metrics / 空间估计）。
  uint64_t bits_total() const { return offsets_.empty() ? 0 : offsets_.back(); }

  uint32_t bitlen(size_t i) const { return bitlens_.at(i); }

  // 读取 entry i 的 bits（按 64-bit words 返回；多余高位为 0）。
  Bits access(size_t i) const {
    const uint32_t bl = bitlens_.at(i);
    const uint64_t start = offsets_.at(i);
    Bits out((bl + 63) / 64, 0);
    for (uint32_t b = 0; b < bl; b++) {
      if (get_bit(start + b))
        out[b / 64] |= (1ULL << (b % 64));
    }
    return out;
  }

  void update(size_t i, const Bits &bits, uint32_t bitlen) {
    if (i >= bitlens_.size())
      throw std::out_of_range("MiniArray::update");
    bitlens_[i] = bitlen;
    entries_[i] = bits;
    repack();
  }

private:
  std::vector<uint32_t> bitlens_;
  std::vector<uint64_t> offsets_; // prefix sum in bits, size=n+1
  std::vector<Bits> entries_;
  std::vector<uint64_t> buf_; // packed bits

  bool get_bit(uint64_t pos) const {
    const uint64_t w = pos / 64;
    const uint64_t o = pos % 64;
    if (w >= buf_.size())
      return false;
    return ((buf_[w] >> o) & 1ULL) != 0;
  }

  static bool get_bit_from(const Bits &src, uint32_t b) {
    const uint32_t w = b / 64;
    const uint32_t o = b % 64;
    if (w >= src.size())
      return false;
    return ((src[w] >> o) & 1ULL) != 0;
  }

  void set_bit(uint64_t pos, bool v) {
    const uint64_t w = pos / 64;
    const uint64_t o = pos % 64;
    if (w >= buf_.size())
      buf_.resize(w + 1, 0);
    const uint64_t mask = (1ULL << o);
    if (v)
      buf_[w] |= mask;
    else
      buf_[w] &= ~mask;
  }

  void recompute_offsets() {
    offsets_[0] = 0;
    for (size_t i = 0; i < bitlens_.size(); i++) {
      offsets_[i + 1] = offsets_[i] + static_cast<uint64_t>(bitlens_[i]);
    }
  }

  void repack() {
    recompute_offsets();
    const uint64_t total = offsets_.back();
    buf_.assign(static_cast<size_t>((total + 63) / 64), 0);

    for (size_t i = 0; i < bitlens_.size(); i++) {
      const uint32_t bl = bitlens_[i];
      if (bl == 0)
        continue;
      const uint64_t start = offsets_[i];
      for (uint32_t b = 0; b < bl; b++) {
        set_bit(start + b, get_bit_from(entries_[i], b));
      }
    }
  }
};

} // namespace otsh
