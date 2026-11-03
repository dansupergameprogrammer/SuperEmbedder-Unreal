// src/superembedder/certification.h -- the build descriptor, the dispatched CPU tier, and the cell verdict
// that decides certification_epoch at encoder open (plan U2-3 items 1, 8, 9, 11; FORMAT.md sec 3.2, sec 4).
//
// Internal to the library; not part of the public ABI.
#ifndef SUPEREMBEDDER_CERTIFICATION_H
#define SUPEREMBEDDER_CERTIFICATION_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace semb::certification {

// The cell descriptor fields a builds[] entry is matched on, each by string equality.
struct Descriptor {
  std::string os;
  std::string isa;
  std::string toolchain;
  std::string stdlib;
  std::string configuration;
  std::string build_system;
  std::string dispatch;
  std::string compile_flags;
  std::string engine_flags;
};

struct Verdict {
  // The matched cell's id, or "uncertified".
  std::string cell;
  // SHA-256 of the identity file's bytes for a certified cell; SHA-256 of "SEMB-UNCERTIFIED\0" followed by
  // them otherwise.
  std::array<uint8_t, 32> epoch{};
};

inline constexpr char kUncertifiedCell[] = "uncertified";

// The one decision, a pure function of its four inputs: the builds[] entry whose descriptor fields equal
// `descriptor` and whose cpuTier equals `tier`, counted only when the list pairs it with every other cell and
// every such pair carries one rowDigest. Exactly one such entry makes the build that cell; anything else --
// no match, an unpaired cell, an unparseable list -- is uncertified.
Verdict Decide(const Descriptor& descriptor, std::string_view tier, std::string_view list_bytes,
               std::string_view identity_bytes);

// This build's compiled-in descriptor.
const Descriptor& ThisBuild();

// The tier this process dispatches to, read once: a forced engine's tier; the engine's own CPU probe for an
// auto x64 build (sse2, avx2, avx512); scalar on a target without x64 SIMD.
const std::string& DispatchedTier();

// Decide() over this build's descriptor, the dispatched tier and the compiled-in list and identity, computed
// once per process.
const Verdict& ProcessVerdict();

// SHA-256 of the compiled-in list's bytes (the manifest's certified_builds_sha256; provenance only).
const std::array<uint8_t, 32>& CompiledListSha256();

// The certification line this build compiled in: the identity file's `line` member ("" when it carries no
// integer one) and the SHA-256 of the identity file's exact bytes (a certified cell's epoch).
std::string IdentityLine();
const std::array<uint8_t, 32>& IdentitySha256();

}  // namespace semb::certification

#endif  // SUPEREMBEDDER_CERTIFICATION_H
