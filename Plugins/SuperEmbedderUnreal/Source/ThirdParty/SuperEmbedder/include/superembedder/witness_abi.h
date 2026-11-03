// include/superembedder/witness_abi.h -- TE-78: the C ABI verb for W-A's
// own digest (`Claude/Plans/SuperEmbedder_Plan.md` sec 4.4b: "W-A digests
// the arithmetic primitives on the reproducible path, PS-1's epilogue
// included -- driven over named edge sets crossed with full legal
// parameter ranges... every refusal STATUS digested").
//
// Compiled into the LIBRARY, not the witness executable's own TU (sec
// 4.4's structural law, clause 3): the driver calls semb_wa_pool_run
// (itself library code) over the plan's own population and folds every
// case's PARAMETER TUPLE, status, and (on SEMB_OK) emitted bits into one
// SHA-256, using the engine's own portable SHA-256 routine (sec 4.4b:
// "the same routine model_sha uses, so the fingerprint contains one hash
// function and not two"). TE-79 F-23: the constructed `codes`/`m`/`e`
// arrays themselves are not digested, only the tuple that parameterises
// their construction -- see wa_digest.cpp's own header comment for the
// exact scope this leaves open.
//
// TE-89 (TE-88 review O-6): both verbs in this header are WITNESS/BUILD
// MACHINERY -- consumed by semb_witness and by this project's own test
// matrix, never by an application encoding text. §3.1's twelve frozen
// verbs do not include either of them, and CMakeLists.txt declares no
// install() rules at all today, so neither reaches a consumer yet. A
// future installer's own include-directory/export list is a DELIBERATE,
// curated set -- it must not become "every symbol this header happens to
// declare" by default, or it ships a build-generation accessor and a
// synthetic self-test digest to a downstream consumer by accident.
#ifndef SEMB_WITNESS_ABI_H
#define SEMB_WITNESS_ABI_H

#include <cstdint>

// `semb_status` is NOT declared here: an includer declares it first, from
// superembedder/semb.h (every product TU) or from the test suite's own
// contract header, which mirrors it and cannot be included beside semb.h.

extern "C" {

// Writes the 32-byte SHA-256 digest of W-A's own population run to
// out_digest[0..31]. Deterministic: the population, the seed (splitmix64,
// 0x0F01D11, sec 4.4's own named discipline) and the driver are all fixed
// by this function's own source. Returns SEMB_OK with the digest written;
// SEMB_ALLOCATION_FAILED when memory cannot be allocated, and
// SEMB_ENGINE_REFUSED on any other internal failure, with out_digest
// unspecified (U2-10: a C-linkage boundary reports, it never throws).
semb_status semb_wa_digest(uint8_t out_digest[32]);

// sec 8 C31(ii) (SEMB_WITNESS_DETACHED_LINK, sec 4.4d item 2): writes this
// LIBRARY's own compiled-in copy of the per-configure build-generation
// token (32 ASCII hex characters, no terminator needed -- the token is a
// fixed 32-hex-char/128-bit value, so this always writes exactly 32 bytes)
// to out_token[0..31]. The bytes come from the library's own compiled
// object -- never a file read -- so the witness executable can compare
// them against its OWN compiled-in copy of the identical token
// (cmake/templates/semb_witness_build_generation.h.in) at process start,
// before computing anything: the two agree if and only if the witness and
// the library it links were configured together.
void semb_library_build_generation(char out_token[32]);

}  // extern "C"

#endif  // SEMB_WITNESS_ABI_H
