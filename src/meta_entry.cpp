#include "otsh/meta_entry.h"
#include "hash.h"

#include <algorithm>
#include <optional>

namespace otsh {

namespace {

bool read_bit(const MiniArray::Bits &bits, uint64_t pos, bool &out) {
  const uint32_t w = static_cast<uint32_t>(pos / 64);
  const uint32_t o = static_cast<uint32_t>(pos % 64);
  if (w >= bits.size())
    return false;
  out = ((bits[w] >> o) & 1ULL) != 0;
  return true;
}

void write_bit(MiniArray::Bits &bits, uint64_t pos, bool v) {
  const uint32_t w = static_cast<uint32_t>(pos / 64);
  const uint32_t o = static_cast<uint32_t>(pos % 64);
  if (bits.size() <= w)
    bits.resize(w + 1, 0);
  const uint64_t mask = 1ULL << o;
  if (v)
    bits[w] |= mask;
  else
    bits[w] &= ~mask;
}

void append_u8_bits(MiniArray::Bits &bits, uint64_t &bit_pos, uint8_t byte) {
  for (int i = 0; i < 8; ++i) {
    write_bit(bits, bit_pos, ((byte >> i) & 1) != 0);
    ++bit_pos;
  }
}

bool read_u8_bits(const MiniArray::Bits &bits, uint64_t &bit_pos, uint8_t &byte,
                  uint64_t max_pos) {
  byte = 0;
  for (int i = 0; i < 8; ++i) {
    if (bit_pos >= max_pos)
      return false;
    bool b = false;
    if (!read_bit(bits, bit_pos, b))
      return false;
    if (b)
      byte |= static_cast<uint8_t>(1u << i);
    ++bit_pos;
  }
  return true;
}

void append_fixed(MiniArray::Bits &bits, uint64_t &bit_pos, uint64_t v,
                  uint32_t nbits) {
  for (uint32_t i = 0; i < nbits; ++i)
    write_bit(bits, bit_pos + i, ((v >> i) & 1ULL) != 0);
  bit_pos += nbits;
}

bool read_fixed(const MiniArray::Bits &bits, uint64_t &bit_pos, uint64_t max_pos,
                uint64_t &v, uint32_t nbits) {
  v = 0;
  for (uint32_t i = 0; i < nbits; ++i) {
    if (bit_pos >= max_pos)
      return false;
    bool b = false;
    if (!read_bit(bits, bit_pos, b))
      return false;
    if (b)
      v |= (1ULL << i);
    ++bit_pos;
  }
  return true;
}

void append_varint_zigzag(MiniArray::Bits &bits, uint64_t &bit_pos, int64_t sn) {
  uint64_t u = static_cast<uint64_t>((static_cast<uint64_t>(sn) << 1) ^
                                     static_cast<uint64_t>(sn >> 63));
  for (;;) {
    uint8_t chunk = static_cast<uint8_t>(u & 0x7Fu);
    u >>= 7;
    const bool more = (u != 0);
    append_u8_bits(bits, bit_pos,
                    static_cast<uint8_t>(chunk | (more ? 0x80u : 0u)));
    if (!more)
      break;
  }
}

bool read_varint_zigzag(const MiniArray::Bits &bits, uint64_t &bit_pos,
                        uint64_t max_pos, int64_t &out) {
  uint64_t u = 0;
  int shift = 0;
  for (;;) {
    uint8_t ch = 0;
    if (!read_u8_bits(bits, bit_pos, ch, max_pos))
      return false;
    u |= static_cast<uint64_t>(ch & 0x7Fu) << shift;
    shift += 7;
    if ((ch & 0x80u) == 0)
      break;
    if (shift > 63)
      return false;
  }
  out = (static_cast<int64_t>(u >> 1)) ^
        (-static_cast<int64_t>(u & 1ULL));
  return true;
}

} // namespace

static uint32_t bit_width_u64(uint64_t v) {
  if (v == 0)
    return 0;
  uint32_t s = 0;
  while (v) {
    ++s;
    v >>= 1;
  }
  return s;
}

uint32_t log2_pow2(uint64_t N) {
  if (N <= 1)
    return 0;
  uint32_t s = 0;
  for (uint64_t t = N >> 1; t != 0; t >>= 1)
    ++s;
  return s;
}

MetaCodecLayout meta_layout(uint64_t K, size_t cubby_capacity) {
  MetaCodecLayout ly;
  if (K == 0 || cubby_capacity == 0) {
    ly.mid_w = 0;
    return ly;
  }
  const uint64_t t_max = (K - 1) / static_cast<uint64_t>(cubby_capacity);
  ly.mid_w = std::max<uint32_t>(1u, bit_width_u64(t_max));
  return ly;
}

uint64_t quotient_payload(uint64_t gx_pi, uint32_t logN) {
  if (logN >= 64)
    return 0;
  return gx_pi >> logN;
}

std::optional<uint64_t> reconstruct_gx_pi(uint64_t facility_r, uint64_t N,
                                           uint64_t K, size_t cubby_capacity,
                                           size_t slot_index, uint64_t payload,
                                           const MetaEntry &m, uint32_t logN) {
  if (N < 2 || K == 0 || logN == 0 || logN >= 64)
    return std::nullopt;
  const size_t cap = std::max<size_t>(1, cubby_capacity);
  const int64_t g_i = static_cast<int64_t>(slot_index) + m.delta;
  if (g_i < 0 || static_cast<uint64_t>(g_i) >= cap)
    return std::nullopt;
  const uint64_t g_k =
      static_cast<uint64_t>(m.mid_bits) * cap + static_cast<uint64_t>(g_i);
  if (g_k >= K)
    return std::nullopt;
  const uint64_t g_star = facility_r * K + g_k;
  if (g_star != (g_star & (N - 1)))
    return std::nullopt;
  const uint64_t gx = (payload << logN) | g_star;
  return gx;
}

MetaEntry make_meta_entry(uint64_t key, uint64_t gx_pi, size_t slot_index,
                          uint64_t facility_r, uint64_t N, uint64_t K,
                          size_t cubby_capacity, uint32_t ins_tag) {
  MetaEntry e;
  (void)facility_r;
  (void)N;
  const size_t cap = std::max<size_t>(1, cubby_capacity);
  const uint64_t g_star = gx_pi & (N - 1);
  const uint64_t g_k = g_star & (K - 1);
  const uint64_t g_i = g_k % cap;
  e.delta = static_cast<int64_t>(g_i) - static_cast<int64_t>(slot_index);
  e.mid_bits = static_cast<uint32_t>(g_k / cap);
  const uint64_t h = splitmix64(key ^ gx_pi ^ 0x9e3779b97f4a7c15ULL);
  e.router_bits = static_cast<uint32_t>(h ^ (h >> 32)) & 0xffffu;
  e.insert_bits = ins_tag & 0xffu;
  return e;
}

std::pair<MiniArray::Bits, uint32_t> encode_meta_entry(const MetaEntry &m,
                                                       const MetaCodecLayout &ly) {
  MiniArray::Bits bits;
  uint64_t pos = 0;
  append_varint_zigzag(bits, pos, m.delta);
  append_fixed(bits, pos, m.mid_bits, ly.mid_w);
  append_fixed(bits, pos, m.router_bits, ly.rout_w);
  append_fixed(bits, pos, m.insert_bits, ly.ins_w);
  const uint32_t bl = static_cast<uint32_t>(pos);
  if (bl == 0) {
    bits.assign(1, 0);
    return {bits, 1};
  }
  return {bits, bl};
}

bool decode_meta_entry(const MiniArray::Bits &bits, uint32_t bitlen,
                       const MetaCodecLayout &ly, MetaEntry &out) {
  out = MetaEntry{};
  if (bitlen == 0)
    return false;
  uint64_t p = 0;
  const uint64_t maxp = bitlen;
  if (!read_varint_zigzag(bits, p, maxp, out.delta))
    return false;
  uint64_t midv = 0, rv = 0, iv = 0;
  if (!read_fixed(bits, p, maxp, midv, ly.mid_w))
    return false;
  if (!read_fixed(bits, p, maxp, rv, ly.rout_w))
    return false;
  if (!read_fixed(bits, p, maxp, iv, ly.ins_w))
    return false;
  out.mid_bits = static_cast<uint32_t>(midv);
  out.router_bits = static_cast<uint32_t>(rv);
  out.insert_bits = static_cast<uint32_t>(iv);
  return p == maxp;
}

} // namespace otsh
