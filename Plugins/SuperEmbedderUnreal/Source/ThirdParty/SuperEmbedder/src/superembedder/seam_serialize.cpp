// src/superembedder/seam_serialize.cpp -- TE-63 (U0 slice B): the seam
// serialisation, byte-for-byte per `Claude/Plans/SuperEmbedder_Plan.md`
// sec 4.4b ("THE SEAM SERIALISATION"): `u64(n) || u64(hidden_size) || for
// p in 0..n-1: codes[p][0..hidden-1] (raw int8) || u64((uint64)m_p) ||
// u64((uint64)e_p)`, little-endian throughout, position 0 INCLUDED (unlike
// PS-1's own pooling, which excludes it -- W-B must NOT inherit that
// convention). One serialisation; every digest (W-B, C39, C40) reads these
// same bytes.
#include "superembedder/seam_abi.h"

#include <cstring>

namespace {

void PutU64LE(uint8_t* buf, uint64_t v) {
  for (int i = 0; i < 8; ++i) buf[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

}  // namespace

extern "C" SembSerializeStatus semb_seam_serialize_document(const int8_t* codes, const int64_t* m,
                                                              const int64_t* e, size_t n,
                                                              size_t hidden, uint8_t* out_buf,
                                                              size_t out_buf_cap, size_t* out_len) {
  const size_t needed = 16 + n * (hidden + 16);
  if (out_len) *out_len = needed;
  if (out_buf_cap < needed) return kSembSerializeBufferTooSmall;

  size_t off = 0;
  PutU64LE(out_buf + off, static_cast<uint64_t>(n));
  off += 8;
  PutU64LE(out_buf + off, static_cast<uint64_t>(hidden));
  off += 8;
  for (size_t p = 0; p < n; ++p) {
    for (size_t i = 0; i < hidden; ++i)
      out_buf[off++] = static_cast<uint8_t>(codes[p * hidden + i]);
    PutU64LE(out_buf + off, static_cast<uint64_t>(m[p]));
    off += 8;
    PutU64LE(out_buf + off, static_cast<uint64_t>(e[p]));
    off += 8;
  }
  return kSembSerializeOk;
}
