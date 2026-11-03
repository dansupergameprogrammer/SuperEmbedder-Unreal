// src/superembedder/certification.cpp -- the build descriptor, the dispatched tier, and the cell verdict
// (plan U2-3 items 1, 8, 9, 11). See certification.h.
#include "certification.h"

#include "engine_link_check.h"
#include "json_lite.h"
#include "semb_build_descriptor.h"

// Generated at configure time: the list's and the identity file's exact bytes (cmake/CertifiedBuilds.cmake),
// and the engine-build record configure verified (cmake/EngineGuards.cmake).
#include "semb_certified_builds.h"
#include "semb_engine_pin.h"
// Generated on every build, before any library translation unit compiles, from the build's own
// compile_commands.json (tools/semb_build_step.py): the digest of the product's compile-flag record.
#include "semb_compile_flags.h"

#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

#include "superslm/matmul.h"
#include "superslm/sha256.h"

#ifndef SEMB_BUILD_CONFIGURATION
#define SEMB_BUILD_CONFIGURATION ""
#endif
#ifndef SEMB_BUILD_SYSTEM
#define SEMB_BUILD_SYSTEM ""
#endif

namespace semb::certification {
namespace {

constexpr char kUncertifiedPrefix[] = "SEMB-UNCERTIFIED";  // followed by one NUL byte in the hashed input

std::array<uint8_t, 32> Sha256Of(const void* data, size_t size) {
  std::array<uint8_t, 32> out{};
  superslm::Sha256 hash;
  hash.Update(static_cast<const uint8_t*>(data), size);
  hash.Final(out.data());
  return out;
}

std::string_view Bytes(const uint8_t* data, size_t size) {
  return std::string_view(reinterpret_cast<const char*>(data), size);
}

std::string StringMember(const json::Value& object, const char* key) {
  const json::Value* v = object.Find(key);
  return v && v->IsString() ? v->as_string : std::string();
}

bool DescriptorMatches(const json::Value& cell, const Descriptor& d) {
  const std::pair<const char*, const std::string*> fields[] = {
      {"os", &d.os},
      {"isa", &d.isa},
      {"toolchain", &d.toolchain},
      {"stdlib", &d.stdlib},
      {"configuration", &d.configuration},
      {"buildSystem", &d.build_system},
      {"dispatch", &d.dispatch},
      {"compileFlags", &d.compile_flags},
      {"engineFlags", &d.engine_flags},
  };
  for (const auto& [key, value] : fields) {
    const json::Value* v = cell.Find(key);
    if (!v || !v->IsString() || v->as_string != *value) return false;
  }
  return true;
}

// The pair-completeness half of SEMB_CERTIFIED_BUILDS_UNPROVEN (item 11), from the compiled-in bytes alone:
// `cell` has exactly one pair with every other cell, and every such pair carries one rowDigest.
bool FullyPaired(const std::string& cell, const std::set<std::string>& cells, const json::Value& proofs) {
  std::map<std::string, int> partners;
  std::string row_digest;
  for (const json::Value& proof : proofs.as_array) {
    if (!proof.IsObject()) return false;
    const std::string a = StringMember(proof, "a");
    const std::string b = StringMember(proof, "b");
    if (a != cell && b != cell) continue;
    const std::string other = a == cell ? b : a;
    if (other == cell || cells.count(other) == 0) return false;
    const std::string digest = StringMember(proof, "rowDigest");
    if (digest.empty() || (!row_digest.empty() && digest != row_digest)) return false;
    row_digest = digest;
    ++partners[other];
  }
  for (const std::string& other : cells) {
    if (other == cell) continue;
    auto it = partners.find(other);
    if (it == partners.end() || it->second != 1) return false;
  }
  return true;
}

std::array<uint8_t, 32> UncertifiedEpoch(std::string_view identity_bytes) {
  std::string input(kUncertifiedPrefix, sizeof(kUncertifiedPrefix));  // the prefix and its NUL
  input.append(identity_bytes);
  return Sha256Of(input.data(), input.size());
}

}  // namespace

Verdict Decide(const Descriptor& descriptor, std::string_view tier, std::string_view list_bytes,
               std::string_view identity_bytes) {
  Verdict verdict;
  verdict.cell = kUncertifiedCell;
  verdict.epoch = UncertifiedEpoch(identity_bytes);
  json::Value list;
  try {
    list = json::Parse(std::string(list_bytes));
  } catch (const std::exception&) {
    return verdict;
  }
  const json::Value* builds = list.IsObject() ? list.Find("builds") : nullptr;
  const json::Value* proofs = list.IsObject() ? list.Find("pairwiseProofs") : nullptr;
  if (!builds || !builds->IsArray() || !proofs || !proofs->IsArray()) return verdict;

  std::set<std::string> cells;
  for (const json::Value& cell : builds->as_array) {
    if (cell.IsObject()) cells.insert(StringMember(cell, "id"));
  }
  std::vector<std::string> matched;
  for (const json::Value& cell : builds->as_array) {
    if (!cell.IsObject()) continue;
    const std::string id = StringMember(cell, "id");
    if (id.empty() || StringMember(cell, "cpuTier") != tier || !DescriptorMatches(cell, descriptor)) continue;
    if (!FullyPaired(id, cells, *proofs)) continue;
    matched.push_back(id);
  }
  if (matched.size() != 1) return verdict;
  verdict.cell = matched.front();
  verdict.epoch = Sha256Of(identity_bytes.data(), identity_bytes.size());
  return verdict;
}

const Descriptor& ThisBuild() {
  static const Descriptor descriptor{
      SEMB_DESCRIPTOR_OS,
      SEMB_DESCRIPTOR_ISA,
      SEMB_DESCRIPTOR_TOOLCHAIN,
      SEMB_DESCRIPTOR_STDLIB,
      SEMB_BUILD_CONFIGURATION,
      SEMB_BUILD_SYSTEM,
      // The engine's dispatch as the LINKED engine reports it (sslm_build_identity's forced_tier, item 14).
      // An open that reaches this has passed the link check, so it equals the record configure verified.
      semb::engine_link::LinkedDispatch(),
      SEMB_COMPILE_FLAGS_SHA,
      SEMB_ENGINE_FLAGS_SHA,
  };
  return descriptor;
}

const std::string& DispatchedTier() {
  static const std::string tier = [] {
    const std::string forced = semb::engine_link::LinkedDispatch();
    if (forced != "auto") return forced;
#if SUPERSLM_MATMUL_HAVE_SIMD_X64
    switch (superslm::DetectBestDotRowTierForCpu()) {
      case 0:
        return std::string("sse2");
      case 1:
        return std::string("avx2");
      case 2:
        return std::string("avx512");
      default:
        return std::string("unknown");
    }
#else
    return std::string("scalar");
#endif
  }();
  return tier;
}

const Verdict& ProcessVerdict() {
  static const Verdict verdict =
      Decide(ThisBuild(), DispatchedTier(), Bytes(kCertifiedBuildsDocument, kCertifiedBuildsDocumentSize),
             Bytes(kCertificationIdentity, kCertificationIdentitySize));
  return verdict;
}

const std::array<uint8_t, 32>& CompiledListSha256() {
  static const std::array<uint8_t, 32> digest = Sha256Of(kCertifiedBuildsDocument, kCertifiedBuildsDocumentSize);
  return digest;
}

std::string IdentityLine() {
  try {
    const json::Value identity =
        json::Parse(std::string(Bytes(kCertificationIdentity, kCertificationIdentitySize)));
    const json::Value* line = identity.IsObject() ? identity.Find("line") : nullptr;
    return line && line->kind == json::Kind::kInt ? std::to_string(line->as_int) : std::string();
  } catch (const std::exception&) {
    return std::string();
  }
}

const std::array<uint8_t, 32>& IdentitySha256() {
  static const std::array<uint8_t, 32> digest = Sha256Of(kCertificationIdentity, kCertificationIdentitySize);
  return digest;
}

}  // namespace semb::certification
