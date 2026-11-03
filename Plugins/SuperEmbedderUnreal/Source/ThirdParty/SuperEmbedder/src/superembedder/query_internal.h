#pragma once

#include <array>
#include <string_view>
#include <vector>

struct semb_encoder_s;

namespace semb {

// The compiled query prefix has one definition, in query.cpp.  Both the
// query composition and encoder fingerprint digest consume these bytes.
extern const char kQueryPrefix[];

// The one query encode (U2-7 item 2): composes `text` with the query prefix
// (ComposeQuery) and encodes the composition (EncodeQueryComposed).
// semb_query_encode and semb_encode_query both call it; neither composes on
// its own. Defined in query.cpp.
semb_status EncodeQuery(semb_encoder enc, std::string_view text, std::vector<float>* out_vector);

// The token count of an already-composed query under `enc`'s tokenizer, by
// the tokenization EncodeQueryComposed runs. Defined in encoder.cpp.
semb_status ComposedQueryTokenCount(semb_encoder enc, std::string_view composed, size_t* out_count);

}  // namespace semb

// W-Q is computed from the query entry point's committed composition corpus.
// It is private build machinery, called during encoder open before any
// ABI-facing fingerprint is derived.
// Returns SEMB_OK, SEMB_ALLOCATION_FAILED when an encode could not allocate
// (U2-10 Builds item 3), or SEMB_ENGINE_REFUSED on any other refusal.
semb_status semb_compute_wq_digest(semb_encoder_s* encoder, std::array<uint8_t, 32>* out_digest);
