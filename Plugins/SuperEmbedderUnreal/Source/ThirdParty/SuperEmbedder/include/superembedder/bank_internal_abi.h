// include/superembedder/bank_internal_abi.h -- TE-191 (U1 slice 4, F4): an
// internal, non-frozen accessor for what an already-open `semb_bank`
// handle retained at open, matching this project's own
// one-declaration-per-machinery-concern convention
// (`manifest_labels_abi.h`, `bake_abi.h`, `wb_digest_abi.h`). NOT one of
// the frozen twelve verbs (sec 3.1, D-SLM4646); not installed as part of
// the public ABI surface promise.
//
// F4's own finding: `semb_bank_opaque` was empty, so nothing an open
// handle validated could be read back from the handle itself, and
// Slice 5's frozen checked path (`semb_query_open` reads the expected
// fingerprint from the ALREADY-OPEN bank, sec 3.1) had no field to read.
// This header exists so that claim is checkable without re-reading the
// manifest from disk: `semb_bank_debug_state` reads exactly what the
// handle retained at open, nothing else -- a scratch probe (or Slice 5's
// own build) can mutate or delete the manifest/pair on disk after open
// and confirm the values returned here do not move.
#ifndef SEMB_BANK_INTERNAL_ABI_H
#define SEMB_BANK_INTERNAL_ABI_H

#include <cstddef>
#include <cstdint>

#include "superembedder/semb.h"

extern "C" {

// Reads the fingerprint (32 bytes, the SAME value semb_query_open's own
// frozen comparison would read) and the geometry (count, dims) `bank`
// retained at open -- never re-read from disk. `bank == NULL` ->
// SEMB_NULL_ARGUMENT; a non-NULL value naming no live handle ->
// SEMB_INVALID_HANDLE (sec 3.1 property 7's own NULL-vs-INVALID_HANDLE
// distinction, applied here as it is everywhere else in this surface).
// `out_fingerprint`, `out_count`, and `out_dims` are all required
// (NULL -> SEMB_NULL_ARGUMENT); nothing is written to any of them on a
// non-SEMB_OK return.
semb_status semb_bank_debug_state(semb_bank bank, uint8_t out_fingerprint[32],
                                   int64_t* out_count, int64_t* out_dims);

// Views ids and decoded float32 rows retained by `bank` at checked open.
// This is the validated snapshot itself: it performs no file I/O and its
// returned storage remains owned by the handle.  `ids[row]` has exactly
// `id_lengths[row]` bytes (it need not be NUL-terminated); `rows` is row-major
// with `count * dims` floats.  The view remains valid while the caller keeps
// the bank live (a checked query retains that lifetime).  `bank == NULL` or
// `out == NULL` -> SEMB_NULL_ARGUMENT; no live handle -> SEMB_INVALID_HANDLE.
struct semb_bank_ranked_view {
  const char* const* ids;
  const size_t* id_lengths;
  const float* rows;
  int64_t count;
  int64_t dims;
};

semb_status semb_bank_ranked_state(semb_bank bank, semb_bank_ranked_view* out);

}  // extern "C"

#endif  // SEMB_BANK_INTERNAL_ABI_H
