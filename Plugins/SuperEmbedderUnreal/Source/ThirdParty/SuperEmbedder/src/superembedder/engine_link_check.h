// src/superembedder/engine_link_check.h -- the linked engine's identity check (plan U2-3 item 14; FORMAT.md
// sec 5.3).
//
// Internal to the library; not part of the public ABI.
#ifndef SUPEREMBEDDER_ENGINE_LINK_CHECK_H
#define SUPEREMBEDDER_ENGINE_LINK_CHECK_H

#include <cstdint>
#include <string>

#include "superembedder/semb.h"

struct SslmBuildIdentity;

namespace semb::engine_link {

struct Verdict {
  semb_status status = SEMB_OK;
  const SslmBuildIdentity* identity = nullptr;  // the linked engine's, when readable
  uint64_t host_isa = 0;
  uint64_t missing_isa = 0;  // required by the engine, absent on the host
};

// The extensions this host can run: the product's own CPUID reading (leaves 0, 1, 7.0, 7.1 and 0x80000001),
// each AVX family gated on its XCR0 state bits. Product code; runs no engine code.
uint64_t HostIsa();

// Reads the linked engine's sslm_build_identity() and returns, in order: SEMB_ENGINE_BUILD_UNRECORDED (no
// readable identity), SEMB_CPU_TIER_UNSUPPORTED (its required_isa holds an extension this host lacks),
// SEMB_ENGINE_TOOLCHAIN_MISMATCH (a compiler or standard-library macro value differs from this library's),
// SEMB_ENGINE_BUILD_UNRECORDED (an empty build record), SEMB_ENGINE_LINK_MISMATCH (a build record other than
// the one configure verified), or SEMB_OK. Reads no artifact byte and runs no engine code.
Verdict CheckLinkedEngine();

// The linked engine's build record as it reports it ("" when it reports none).
std::string LinkedBuildRecord();

// The SSLM_ISA_* bits named, comma-separated, in bit order ("none" for no bit set).
std::string IsaNames(uint64_t bits);

// The linked engine's forced tier as a descriptor `dispatch` string: auto, scalar, sse2, avx2 or avx512;
// "unknown" when the identity is unreadable.
std::string LinkedDispatch();

}  // namespace semb::engine_link

#endif  // SUPEREMBEDDER_ENGINE_LINK_CHECK_H
