#pragma once

#include "superembedder/encoder_interim_abi.h"

#include <cstdint>
#include <vector>

namespace superslm {
struct SslmModelView;
}

namespace semb {

// Private raw-text open: validate the tokenizer/model vocabulary join after
// engine load and marshal, before sizing or allocating the workspace pool.
// A zero tokenizer vocabulary preserves the interim token-id open behavior.
// On a join refusal, returns kSembInterimEngineRefused with vocab_mismatch set;
// the public wrapper maps that distinct refusal to its public status.
SembInterimStatus OpenEncoderOwnedWithVocabJoin(
    std::vector<uint8_t>&& model_bytes, int32_t appended_token_id,
    uint32_t max_concurrency, uint32_t tokenizer_vocab_size,
    SembInterimEncoder** out_encoder, const char** out_engine_status_name,
    bool& vocab_mismatch);

// U3 (te266 sec 4.2): PS-N's selected triple, as dequantize reads it.
struct InterimTriple {
  std::vector<int8_t> codes;
  int64_t m = 0;
  int64_t e = 0;
};

// semb_interim_encode_ps_n that also returns the selected triple. The GPU open
// path's W-Q agreement-check triples come from the witness's own query
// forward through it; nothing else calls it.
SembInterimStatus EncodePsNWithTriple(SembInterimEncoder* encoder, const int32_t* content_ids,
                                      size_t n_content, size_t chunk_budget, float* out_vector,
                                      size_t out_capacity_bytes, size_t* out_required_bytes,
                                      SembWaPoolStatus* out_dequantize_status,
                                      const char** out_engine_status_name, InterimTriple* out_triple);

// The interim encoder's loaded model view, which the GPU adapter maps.
const superslm::SslmModelView* InterimModelView(const SembInterimEncoder* encoder);

}  // namespace semb
