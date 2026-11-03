// include/superembedder/manifest_labels_abi.h -- TE-166 (U1 slice 4): C34
// legs (e)/(h) (`Claude/Plans/SuperEmbedder_Plan.md` sec 3.4's label-check
// table, sec 8 C34). Each accessor returns ONE translation unit's own
// compiled-in self-report, read by `semb_bake_to_path` (src/superembedder/
// encoder.cpp) and compared, before the label is written, against an
// INDEPENDENTLY-MAINTAINED literal local to the bake call site -- two
// distinct compiled sources, never one value read twice, which is what
// makes `tests/te163_c34_manifest_honesty_check.py`'s own single-file
// source mutation able to move the comparison's TWO SIDES apart rather
// than moving both together.
//
// WITNESS/BUILD MACHINERY, like `semb_wa_digest`/`semb_wb_digest` --
// sec 3.1's twelve frozen verbs do not include these, and this header is
// included only where the real product types are already in scope
// (`witness_abi.h`'s own header comment states why a shared header is not
// used: this header carries no semb_status/semb_encoder-shaped type, so
// it is safe to include anywhere; kept as its own header purely to match
// this project's one-declaration-per-machinery-concern convention).
#ifndef SEMB_MANIFEST_LABELS_ABI_H
#define SEMB_MANIFEST_LABELS_ABI_H

extern "C" {

// engine_adapter_interim.cpp's own compiled-in adapter identity
// (SEMB_ADAPTER_SELF_ID). Static storage, never NULL.
const char* semb_adapter_self_id();

// ps_n_pool.cpp's own compiled-in pooling-scheme label
// (SEMB_PS_VERSION_LABEL = "PS-N", the ratified scheme, D-SLM4726). Static
// storage, never NULL. Tried first by
// `tests/te163_c34_manifest_honesty_check.py`'s own leg (h) search order.
const char* semb_ps_n_version_label();

// ps1_pool.cpp's own compiled-in pooling-scheme label
// (SEMB_PS_VERSION_LABEL = "PS-1", D-SLM4727's named fallback). Static
// storage, never NULL. Not read by `semb_bake_to_path` today (PS-N is the
// active scheme); present because PS-1's epilogue stays compiled in and
// sec 3.4's label-check table names the pooling epilogue's OWN TU, plural
// in principle.
const char* semb_ps1_version_label();

}  // extern "C"

#endif  // SEMB_MANIFEST_LABELS_ABI_H
