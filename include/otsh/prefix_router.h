#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace otsh {

// §5 Local Query Router：二叉前缀树，key → 探测序列下标 j（O(log U) 位，工程用 64-bit）。
// 实现细节在 .cpp（pimpl），避免头文件依赖 std::vector 以利 IDE/clangd 解析。
class PrefixRouter {
public:
  PrefixRouter();
  PrefixRouter(const PrefixRouter &);
  PrefixRouter &operator=(const PrefixRouter &);
  ~PrefixRouter();
  PrefixRouter(PrefixRouter &&) noexcept;
  PrefixRouter &operator=(PrefixRouter &&) noexcept;

  bool insert(uint64_t key_bits, uint32_t j);
  bool erase(uint64_t key_bits);
  std::optional<uint32_t> query(uint64_t key_bits) const;
  void clear();
  size_t node_count() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace otsh
