#pragma once

#include "config.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace otsh {

// 操作结果
struct OpResult {
  bool ok = false;
  std::string error;
};

// 插入结果
struct InsertResult : OpResult {
  bool inserted = false; // true=新插入，false=已存在
  uint64_t router_probe_steps = 0;
  uint64_t kick_count = 0;
  int cubby_tier = -1;
};

// 查询结果
struct QueryResult : OpResult {
  bool found = false;
  uint64_t router_probe_steps = 0;
  int cubby_tier = -1;
};

// 删除结果
struct DeleteResult : OpResult {
  bool deleted = false;
  uint64_t router_probe_steps = 0;
  uint64_t kick_count = 0;
  int cubby_tier = -1;
};

// 哈希表状态
struct HashTableState {
  uint64_t n = 0;
  uint64_t N = 0;
  uint64_t K = 0;
  uint64_t facilities = 0;
};

// 只读遍历当前内存结构（用于 snapshot_meta / cubby / slot_snapshot 落库）。
struct CubbyStructureView {
  int facility_id = 0;
  int tier = 0;
  int capacity = 0;
  int size = 0;
  bool is_tail = false;
  std::vector<std::optional<uint64_t>> slot_keys;
};

// 设计文档：3 轮 Feistel + F（可逆）；k1–k3 参与轮函数，k4 保留作种子扩展。
struct PermutationHash {
  uint64_t k1 = 0, k2 = 0, k3 = 0, k4 = 0;
  uint64_t pi(uint64_t x) const;
  uint64_t inverse(uint64_t y) const;
};

class HashTable {
public:
  HashTable();
  ~HashTable();
  HashTable(const HashTable &) = delete;
  HashTable &operator=(const HashTable &) = delete;
  HashTable(HashTable &&) noexcept;
  HashTable &operator=(HashTable &&) noexcept;

  // 初始化
  OpResult init(const TableParams &p);

  // 插入
  InsertResult insert(uint64_t key);

  // 查询
  QueryResult query(uint64_t key) const;

  // 删除
  DeleteResult erase(uint64_t key);

  // 批量加载 key，不再写回存储。
  OpResult bulk_load(const std::vector<uint64_t> &keys);

  // 状态
  HashTableState state() const;

  // 遍历结构
  void visit_structure(
      const std::function<void(const CubbyStructureView &)> &fn) const;

  // 设计文档中的 π(x)，用于 slot_snapshot.key_hash。
  uint64_t pi_of(uint64_t key) const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace otsh
