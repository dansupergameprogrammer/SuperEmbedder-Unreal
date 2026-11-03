// SuperEmbedderCore's one verb beyond semb.h. Plain C, no UE header, so the engine-free cores of the
// SuperEmbedderUnreal module and their host tests include it as they include semb.h.
#ifndef SUPEREMBEDDERCORE_EXTRAS_H
#define SUPEREMBEDDERCORE_EXTRAS_H

#include "superembedder/semb.h"

#ifdef __cplusplus
extern "C" {
#endif

// The body-token count of a query, as SuperEmbedder's R17 verb counts it (semb_interim_query_token_count):
// len(Encode(ComposeQuery(text))) - len(Encode(query prefix)), over the encoder's own tokenizer, without the
// forward. R17 is not one of semb.h's frozen verbs and carries no SEMB_API, so a modular build would not
// export it from this module's DLL; this wrapper is compiled into SuperEmbedderCore under the rename header
// and exported with semb.h's own SEMB_API.
//
// SEMB_OK and *out_body_tokens set; SEMB_NULL_ARGUMENT for a null enc or out_body_tokens or a null utf8 with
// a nonzero length; SEMB_INVALID_HANDLE for a handle that names no open encoder; SEMB_ALLOCATION_FAILED.
// At SuperEmbedder b52afd0 the verb counts queries only. There is no document-kind count yet (the proposal's
// section 6 ask, re-audit G-22), so callers that bound documents state which kind they counted.
SEMB_API
semb_status semb_ue_query_body_token_count(semb_encoder enc, const char* utf8, size_t utf8_len,
                                           int64_t* out_body_tokens);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // SUPEREMBEDDERCORE_EXTRAS_H
