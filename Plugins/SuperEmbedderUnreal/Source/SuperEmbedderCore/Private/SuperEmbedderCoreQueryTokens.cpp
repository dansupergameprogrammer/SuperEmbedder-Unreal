// semb_ue_query_body_token_count (Public/SuperEmbedderCoreExtras.h). Plugin source, not a vendored wrapper:
// it lives outside Private/Vendored/, so the wrapper census and the rename list's derivation do not read it.
// It is compiled like the wrappers, under the force-included rename header, and names no engine identifier.
#include "SuperEmbedderCoreExtras.h"

#include "superembedder/encoder_interim_abi.h"

extern "C" semb_status semb_ue_query_body_token_count(semb_encoder enc, const char* utf8, size_t utf8_len,
                                                      int64_t* out_body_tokens)
{
	switch (semb_interim_query_token_count(enc, utf8, utf8_len, out_body_tokens))
	{
	case kSembInterimOk:
		return SEMB_OK;
	case kSembInterimNullArgument:
		return SEMB_NULL_ARGUMENT;
	case kSembInterimAllocationFailed:
		return SEMB_ALLOCATION_FAILED;
	default:
		// The verb's only other outcome is kSembInterimEngineRefused, for a handle that names no open encoder.
		return SEMB_INVALID_HANDLE;
	}
}
