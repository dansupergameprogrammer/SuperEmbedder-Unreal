// Private diagnostic ABI for the encoder-scoped W-Q witness digest.
#ifndef SEMB_WQ_DIGEST_ABI_H
#define SEMB_WQ_DIGEST_ABI_H

#include <cstdint>

#include "superembedder/semb.h"

extern "C" {

// Returns the W-Q digest derived when `enc` opened: SHA-256 over the
// length-prefixed vectors emitted for the configure-verified committed W-Q
// fixture. `enc` is NULL -> SEMB_NULL_ARGUMENT; an invalid handle ->
// SEMB_INVALID_HANDLE; `out_digest` is NULL -> SEMB_NULL_ARGUMENT.
// This diagnostic exposes the already-derived digest only. It neither exposes
// query composition state nor adds a thirteenth public `semb_*` verb.
semb_status semb_wq_digest(semb_encoder enc, uint8_t out_digest[32]);

}  // extern "C"

#endif  // SEMB_WQ_DIGEST_ABI_H
