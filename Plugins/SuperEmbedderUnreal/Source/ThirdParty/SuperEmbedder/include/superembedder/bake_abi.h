// include/superembedder/bake_abi.h -- TE-166 (U1 slice 4): the product-side
// declaration of the internal bake primitive
// (`tests/semb_bake_abi_contract.h` is the suite's own concrete pin;
// `Claude/Plans/te140-u1-remainder-sequencing.md` sec 4.4). Declared in its
// own header rather than added to `semb.h`, matching `wb_digest_abi.h`'s
// own precedent (that header's comment states the reason: a TU that also
// includes a test contract header declaring its own independent
// `semb_status`/`semb_encoder` types must not have a product header pull
// those real types in a second time). Included only where the real
// product types are already in scope (src/superembedder/encoder.cpp,
// after superembedder/semb.h).
//
// NOT one of the frozen twelve verbs (sec 3.1, D-SLM4646) -- Slice 6's own
// `semb bake <corpus> --out <name>` CLI wraps this primitive; it exists at
// this slice only so a bank pair can be written at all, for
// `semb_bank_open`'s own red suite to open (and mutate copies of).
#ifndef SEMB_BAKE_ABI_H
#define SEMB_BAKE_ABI_H

#include <cstddef>

#include "superembedder/semb.h"  // semb_status, semb_encoder, semb_document

extern "C" {

// Bakes `n` documents through `enc` (a live, open handle) and writes the
// bank pair + manifest sidecar to the three sibling paths named by
// `path_stem` (sized UTF-8 span, `path_stem_len` bytes, never
// NUL-terminated). Computes the rows through the SAME validated call
// `semb_encode_batch` itself is; every refusal that verb can produce
// propagates unchanged and nothing is written.
//
// On any non-SEMB_OK return from a live, uninterrupted call -- a
// validation refusal, or a caught I/O exception during the write -- the
// rollback leaves none of the three sibling files in place (TE-191 F6:
// this claim covers a RETURN; it says nothing about a crash that never
// returns at all). The three-file REPLACEMENT itself is NOT atomic as a
// set: each of the three renames is individually atomic, but a process
// termination or power loss between them is not undone by any in-process
// handler. The manifest is renamed LAST (`AtomicTripleWriter::Commit`,
// encoder.cpp), so a termination before it completes leaves either the
// pre-bake triple entirely untouched, or the new header+payload paired
// with the OLD manifest -- never a new manifest paired with an old header
// or payload. `semb_bank_open`'s own manifest cross-checks (count, dims,
// and the row digest recomputed over the payload) refuse that hybrid by
// name (SEMB_BANK_SIZE_MISMATCH / SEMB_BANK_DIGEST_MISMATCH) rather than
// silently accepting it.
semb_status semb_bake_to_path(semb_encoder enc, const semb_document* docs, size_t n,
                               const char* path_stem, size_t path_stem_len);

}  // extern "C"

#endif  // SEMB_BAKE_ABI_H
