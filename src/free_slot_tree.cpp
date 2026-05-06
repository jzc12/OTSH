#include "otsh/free_slot_tree.h"

#include <cstddef>
#include <cstdint>

namespace otsh {

namespace {

uint32_t get_cnt(const std::vector<uint64_t> &t, int i) {
  return static_cast<uint32_t>(t[static_cast<size_t>(i)] & 0xffffffffu);
}

void set_cnt(std::vector<uint64_t> &t, int i, uint32_t v) {
  t[static_cast<size_t>(i)] = static_cast<uint64_t>(v);
}

int padded_leaves(int cap) {
  int p = 1;
  while (p < cap)
    p <<= 1;
  return p;
}

} // namespace

void FreeSlotTree::pull_up(int p) {
  if (p < 1 || bitmaps.empty())
    return;
  set_cnt(bitmaps, p, get_cnt(bitmaps, 2 * p) + get_cnt(bitmaps, 2 * p + 1));
}

void FreeSlotTree::build() {
  if (capacity <= 0) {
    n = 0;
    bitmaps.clear();
    return;
  }
  n = padded_leaves(capacity);
  bitmaps.assign(static_cast<size_t>(2 * n + 2), 0);
  for (int i = 0; i < n; ++i) {
    const uint32_t v = (i < capacity) ? 1u : 0u;
    set_cnt(bitmaps, n + i, v);
  }
  for (int p = n - 1; p >= 1; --p)
    pull_up(p);
}

int FreeSlotTree::find_free() const {
  if (capacity <= 0 || n == 0 || bitmaps.empty())
    return -1;
  if (get_cnt(bitmaps, 1) == 0)
    return -1;
  int p = 1;
  while (p < n) {
    if (get_cnt(bitmaps, 2 * p) > 0)
      p = 2 * p;
    else
      p = 2 * p + 1;
  }
  const int idx = p - n;
  if (idx < 0 || idx >= capacity)
    return -1;
  return idx;
}

void FreeSlotTree::mark_used(int i) {
  if (!valid_idx(i) || n == 0 || bitmaps.empty())
    return;
  const int leaf = n + i;
  if (get_cnt(bitmaps, leaf) == 0)
    return;
  set_cnt(bitmaps, leaf, 0);
  for (int p = leaf / 2; p >= 1; p /= 2)
    pull_up(p);
}

void FreeSlotTree::mark_free(int i) {
  if (!valid_idx(i) || n == 0 || bitmaps.empty())
    return;
  const int leaf = n + i;
  if (get_cnt(bitmaps, leaf) != 0)
    return;
  set_cnt(bitmaps, leaf, 1);
  for (int p = leaf / 2; p >= 1; p /= 2)
    pull_up(p);
}

} // namespace otsh
