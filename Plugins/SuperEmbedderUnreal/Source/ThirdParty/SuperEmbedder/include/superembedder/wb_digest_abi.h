// include/superembedder/wb_digest_abi.h -- TE-147 (U1 slice 2 build): the
// product-side declaration of W-B's own diagnostic accessor, in the same
// PURPOSE `include/superembedder/witness_abi.h` serves for W-A
// (`semb_status semb_wa_digest(uint8_t out_digest[32]);`) -- but declared in a
// SEPARATE header rather than added to witness_abi.h, because witness_abi.h
// is included by translation units that ALSO include the test suite's own
// contract headers (`tests/semb_public_abi_contract.h`), which declare
// their OWN `semb_status`/`semb_encoder` types independently of this
// product header. Adding a declaration that needs those types to
// witness_abi.h would pull `superembedder/semb.h`'s own real typedefs into
// those translation units too, producing a duplicate-definition conflict
// with the test contract's own types (confirmed:
// `tests/c41_environment_invariance_test.cpp` includes both
// `semb_public_abi_contract.h` and `superembedder/witness_abi.h` in one
// TU). This header is included only where the real product types are
// already in scope (src/superembedder/encoder.cpp, after
// `superembedder/semb.h`), never by a TU that also carries the test
// suite's own duplicate contract types.
//
// Like `semb_wa_digest`, this verb is WITNESS/BUILD MACHINERY -- sec 3.1's
// twelve frozen verbs do not include it, and it reaches no consumer beyond
// this project's own test matrix.
#ifndef SEMB_WB_DIGEST_ABI_H
#define SEMB_WB_DIGEST_ABI_H

#include <cstdint>

#include "superembedder/semb.h"  // semb_status, semb_encoder -- this verb's own types

extern "C" {

// W-B's own digest for the artifacts `enc` was opened on (TE-147; the
// contract of record is `tests/semb_wb_witness_abi_contract.h`, AMENDED
// TE-151 to this handle-scoped shape). SHA-256 over the committed
// sixteen-document corpus's seam serialisation -- u64(document_count)
// first, then each document's own semb_seam_serialize_document stream in
// committed order -- each document tokenized through `enc`'s OWN
// tokenizer with `enc`'s OWN resolved end-token id appended
// (`Claude/Plans/SuperEmbedder_Plan.md` sec 4.4b). Unlike W-A, W-B is NOT
// model-independent, so this accessor is handle-scoped rather than bare:
//
// TE-155 (D-SLM6341, ruled by the conductor under delegation, subject to
// Dan's veto): EACH DOCUMENT'S OWN TOKENIZED CONTENT IS CUT TO
// `enc`'s context_cap MINUS ONE ids BEFORE the resolved end token is
// appended, so the per-document position count never exceeds context_cap
// on any artifact `enc` was legally opened on. `semb_encoder_open` never
// refuses solely because the compiled-in W-B corpus cannot fit inside a
// small artifact's own context window -- the digest this accessor returns
// is over what the forward actually executed (the cut content), never
// over the full, untokenized document text. Against an artifact whose
// context_cap comfortably exceeds every committed document's own
// tokenized length (the real candidate included), no document is cut and
// this digest is unaffected. The cut is a pure, deterministic function of
// `enc`'s own context_cap and the compiled-in corpus's own tokenized
// content, so it does not disturb this accessor's own determinism
// (property b), cross-handle agreement (property c), or id-inertness
// (property d) -- see `tests/w_b_corpus_witness_test.cpp`.
//
//   enc == NULL                  -> SEMB_NULL_ARGUMENT, out_digest untouched
//   enc names no live handle     -> SEMB_INVALID_HANDLE, out_digest untouched
//   out_digest == NULL           -> SEMB_NULL_ARGUMENT
//   else                         -> the 32 bytes written, SEMB_OK
//
// The same value this accessor returns for a live `enc` is what
// `ComputeManifestFingerprintComponents` (src/superembedder/encoder.cpp)
// folds into that handle's own `witness_sha`, and so into its encoder
// fingerprint.
//
// TE-165 (D-SLM6390 items 1-3, re-opening TE-159's C-2/D-SLM6354 on
// TE-161's own measurement, N-3/D-SLM6385): this digest is derived by
// calling `ComposeBatch` (src/superembedder/encoder.cpp) with the seam
// tapped -- the ONE internal batch composition `semb_encode_batch` itself
// validates the caller's buffers and calls: per-document tokenization,
// the document frame, the `>= context_cap` refusal, the appended
// terminator, the workspace acquisition(s) (TE-350: at most one per worker
// thread), the chunk-batched
// forward. TE-159's own remedy shared only the interim function
// (`semb_interim_encode_batch_ps_n`) and kept a second, independent
// tokenize-and-cut inside `ComputeWbDigest`, so a change to the batch
// verb's OWN tokenize call, document frame or truncation rule did not
// move this digest -- TE-161's review proved it by executing exactly that
// perturbation. This digest now reaches the whole composition: a change
// to the batch verb's own composition (special-token handling, the
// document frame once PS-1 lands, the tokenizer call, truncation
// ordering) moves this digest -- proven by execution, both the
// composition's own appended-token perturbation (unchanged from TE-159)
// and the document-frame perturbation TE-161 added (see this round's own
// build record for both quoted digest pairs). W-B's own sixteen
// documents are prepared as TEXT before the call (`CutWbDocumentText`),
// the longest UTF-8-boundary byte prefix whose tokenized length is at
// most `context_cap - 1` -- an input rule, not a second copy of the
// composition's own tokenize-and-cut.
semb_status semb_wb_digest(semb_encoder enc, uint8_t out_digest[32]);

}  // extern "C"

#endif  // SEMB_WB_DIGEST_ABI_H
