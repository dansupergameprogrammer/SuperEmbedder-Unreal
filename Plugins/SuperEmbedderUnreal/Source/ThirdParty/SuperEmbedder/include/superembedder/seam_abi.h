// include/superembedder/seam_abi.h -- TE-63 (U0 slice B): the C ABI verbs
// for the SEAM -- the encoder's per-position {codes, m, e} output and its
// serialisation (`Claude/Plans/SuperEmbedder_Plan.md` sec 4.4b).
//
// This header's symbol names, enum values and function signatures satisfy
// `tests/semb_seam_abi_contract.h` exactly, for the same reason
// `pool_abi.h` states.
#ifndef SEMB_SEAM_ABI_H
#define SEMB_SEAM_ABI_H

#include <cstddef>
#include <cstdint>

enum SembSerializeStatus : int32_t {
  kSembSerializeOk = 0,
  kSembSerializeBufferTooSmall = 1,
};

enum SembEncodeStatus : int32_t {
  kSembEncodeOk = 0,
  kSembEncodeEngineRefused = 1,
  // TE-79 F-7 (Significant, TE-76 review): the real hidden_size, known
  // only after the model opens, exceeds kSembWitnessMaxHidden -- refused
  // BEFORE codes_out is written. See semb_encode_document_for_witness's
  // own doc comment.
  kSembEncodeHiddenTooLarge = 2,
  // U2-10 Builds item 3: memory could not be allocated.
  kSembEncodeAllocationFailed = 3,
};

// TE-79 F-7: semb_encode_document_for_witness cannot take a real
// capacity parameter for codes_out without breaking every existing
// caller's own fixed signature (extern "C", no default arguments, and
// the callers under tests/ are outside this seat's writable scope) --
// so the domain is closed the other direction: a fixed, documented
// ceiling on hidden_size, enforced INSIDE the verb once the real value
// is known (after the model opens, before any write), refusing by name
// rather than writing past what any caller could safely have allocated.
//
// RAISED, TE-147 (U1 slice 2 build): this ceiling's own comment named it
// owed at U1 ("both are owed together at U1, when this witness-only verb
// retires in favour of the real product entry point") -- due now that the
// real W-B witness leg must run against the real candidate artifact
// (`Claude/Plans/SuperEmbedder_Plan.md` sec 9 dim 4: `hidden_size` 1024).
// 1024 matches the real candidate exactly (the current committed W-B
// candidate's own `hidden_size`, TE-145 casebook sec 7); every consumer
// that sizes a buffer as `n * kSembWitnessMaxHidden`
// (tests/c39_seam_digest_test.cpp, tests/c40_chunk_budget_inertness_test.cpp,
// tests/w_b_corpus_witness_test.cpp) tracks the new value with no further
// change (TE-145 casebook sec 7's own consumer sweep). A future artifact
// whose own hidden_size exceeds 1024 still needs both halves raised again
// together, per this same rule.
constexpr size_t kSembWitnessMaxHidden = 1024;

extern "C" {

// sec 4.4b's per-document seam serialisation, VERBATIM byte layout:
//   u64(n) || u64(hidden_size)
//   || for p in 0..n-1: codes[p][0..hidden-1] (raw int8, dimension order)
//                       || u64((uint64)m[p]) || u64((uint64)e[p])
// little-endian throughout. Writes into `out_buf` (capacity `out_buf_cap`
// bytes) and reports the exact length written (or required, on
// kSembSerializeBufferTooSmall) via `out_len`.
SembSerializeStatus semb_seam_serialize_document(const int8_t* codes, const int64_t* m,
                                                  const int64_t* e, size_t n, size_t hidden,
                                                  uint8_t* out_buf, size_t out_buf_cap,
                                                  size_t* out_len);

// The one encoder TU's real, timed seam call over a whole document (C39):
// runs the actual forward (EmbedEntry + RunLayerLoopChunkBatched) on
// `token_ids` and writes the per-position codes/scales. `chunk_budget` is
// sec 4.3's pinned chunk-budget parameter (0 or >= n means one
// whole-document call).
//
// CAPACITY (TE-79 F-7): `codes_out` must have room for at least
// `n * kSembWitnessMaxHidden` int8 values; `m_out`/`e_out` each need `n`
// int64_t values (independent of hidden_size -- always safe at any `n` a
// caller has itself allocated for). The real `hidden_size` is not known
// until the model opens; if it exceeds `kSembWitnessMaxHidden` this verb
// returns `kSembEncodeHiddenTooLarge` BEFORE writing anything to
// `codes_out`, `m_out`, `e_out`, or `*hidden_out`.
SembEncodeStatus semb_encode_document_for_witness(const char* model_path,
                                                   const int32_t* token_ids, size_t n,
                                                   size_t chunk_budget, int8_t* codes_out,
                                                   int64_t* m_out, int64_t* e_out,
                                                   size_t* hidden_out);

}  // extern "C"

#endif  // SEMB_SEAM_ABI_H
