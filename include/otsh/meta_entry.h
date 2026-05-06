#pragma once

#include "otsh/mini_array.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace otsh {

// 设计文档 MetaEntry：B_I（商压缩）+ A_I（路由指纹）+ M_I（插入过程）。
struct MetaEntry {
  int64_t delta = 0;       // g_I - i（g_I = g_K mod |I|）
  uint32_t mid_bits = 0; // g_K / |I|
  uint32_t router_bits = 0;
  uint32_t insert_bits = 0;
};

// 编解码时与 (K, |I|) 绑定的位宽派生参数。
struct MetaCodecLayout {
  uint32_t mid_w = 0;
  uint32_t rout_w = 16;
  uint32_t ins_w = 8;
};

MetaCodecLayout meta_layout(uint64_t K, size_t cubby_capacity);

// log2(N)，N 为 >=2 的 2 幂。
uint32_t log2_pow2(uint64_t N);

// g* = π(x) mod N 的高位商：π(x) 去掉低 logN 位后的部分（设计文档 3.5）。
uint64_t quotient_payload(uint64_t gx_pi, uint32_t logN);

// 由设施 r、槽位、Meta、payload 恢复 π(x)（64 位像）。
std::optional<uint64_t> reconstruct_gx_pi(uint64_t facility_r, uint64_t N,
                                         uint64_t K, size_t cubby_capacity,
                                         size_t slot_index, uint64_t payload,
                                         const MetaEntry &m, uint32_t logN);

MetaEntry make_meta_entry(uint64_t key, uint64_t gx_pi, size_t slot_index,
                          uint64_t facility_r, uint64_t N, uint64_t K,
                          size_t cubby_capacity, uint32_t ins_tag);

std::pair<MiniArray::Bits, uint32_t> encode_meta_entry(const MetaEntry &m,
                                                       const MetaCodecLayout &ly);

bool decode_meta_entry(const MiniArray::Bits &bits, uint32_t bitlen,
                       const MetaCodecLayout &ly, MetaEntry &out);

} // namespace otsh
