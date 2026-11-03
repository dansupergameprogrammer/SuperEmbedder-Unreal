// src/superembedder/engine_link_check.cpp -- the linked engine's identity check (plan U2-3 item 14; FORMAT.md
// sec 5.3). The first statement of OpenEncoder and of the bank's open: read the build identity of the engine
// this process actually LINKED (sslm_build_identity, a constant object in the engine library), and refuse by
// name before any artifact byte is read and before any engine code runs when that engine is not one this host
// can run, or not the one configure verified.
//
// Order, each a distinct status:
//   1. identity unreadable or too small              -> SEMB_ENGINE_BUILD_UNRECORDED
//   2. engine needs an ISA extension the host lacks  -> SEMB_CPU_TIER_UNSUPPORTED
//   3. engine compiler / standard library != ours    -> SEMB_ENGINE_TOOLCHAIN_MISMATCH
//   4. engine carries no build record               -> SEMB_ENGINE_BUILD_UNRECORDED
//   5. engine build record != configure's record    -> SEMB_ENGINE_LINK_MISMATCH
// Safety first: step 2 runs before any comparison, so an engine that would fault is refused as that. The
// reference is a STRONG undefined symbol: an engine library that does not define it fails the consumer's
// link, naming sslm_build_identity.
#include "engine_link_check.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#if defined(__has_include)
#if __has_include(<version>)
#include <version>
#endif
#endif

#include "semb_engine_pin.h"  // SEMB_ENGINE_BUILD_RECORD, compiled in at configure
#include "engine_identity/superslm_build_identity.h"

#if defined(_M_X64) || defined(__x86_64__)
#define SEMB_HOST_X64 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#else
#define SEMB_HOST_X64 0
#endif

namespace semb::engine_link {
namespace {

#if SEMB_HOST_X64
void Cpuid(int leaf, int sub, unsigned r[4]) {
#if defined(_MSC_VER)
  int v[4];
  __cpuidex(v, leaf, sub);
  for (int i = 0; i < 4; ++i) r[i] = static_cast<unsigned>(v[i]);
#else
  __cpuid_count(static_cast<unsigned>(leaf), static_cast<unsigned>(sub), r[0], r[1], r[2], r[3]);
#endif
}
unsigned long long Xcr0() {
#if defined(_MSC_VER)
  return _xgetbv(0);
#else
  unsigned lo = 0, hi = 0;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  return (static_cast<unsigned long long>(hi) << 32) | lo;
#endif
}
#endif

// The identity when it is readable: non-null, at least schema 1's size and version, with a record pointer.
const SslmBuildIdentity* Readable(const SslmBuildIdentity* id) {
  if (!id || id->struct_size < offsetof(SslmBuildIdentity, build_record) + sizeof(const char*) ||
      id->schema_version < 1 || !id->build_record) {
    return nullptr;
  }
  return id;
}

}  // namespace

// The host's usable extensions: CPUID feature bits, gated on the OS saving the state each
// family needs (XCR0 bits 1-2 for AVX, 5-7 for AVX-512), as the engine's own resolver gates.
uint64_t HostIsa() {
#if SEMB_HOST_X64
  unsigned r0[4], r1[4], r7[4] = {0, 0, 0, 0}, r71[4] = {0, 0, 0, 0}, rx[4] = {0, 0, 0, 0};
  Cpuid(0, 0, r0);
  const unsigned max_leaf = r0[0];
  Cpuid(1, 0, r1);
  if (max_leaf >= 7) {
    Cpuid(7, 0, r7);
    if (r7[0] >= 1) Cpuid(7, 1, r71);
  }
  Cpuid(static_cast<int>(0x80000000u), 0, rx);
  unsigned ext1_ecx = 0;
  if (rx[0] >= 0x80000001u) {
    unsigned re[4];
    Cpuid(static_cast<int>(0x80000001u), 0, re);
    ext1_ecx = re[2];
  }
  const unsigned c1 = r1[2], b7 = r7[1], c7 = r7[2], d7 = r7[3], a71 = r71[0], d71 = r71[3];
  const bool osxsave = (c1 & (1u << 27)) != 0;
  const unsigned long long xcr0 = osxsave ? Xcr0() : 0;
  const bool ymm = (xcr0 & 0x6ull) == 0x6ull;
  const bool zmm = ymm && (xcr0 & 0xE0ull) == 0xE0ull;
  auto bit = [](unsigned reg, int b) { return (reg & (1u << b)) != 0; };
  uint64_t h = 0;
  if (bit(c1, 0)) h |= SSLM_ISA_SSE3;
  if (bit(c1, 9)) h |= SSLM_ISA_SSSE3;
  if (bit(c1, 19)) h |= SSLM_ISA_SSE4_1;
  if (bit(c1, 20)) h |= SSLM_ISA_SSE4_2;
  if (bit(c1, 23)) h |= SSLM_ISA_POPCNT;
  if (bit(c1, 22)) h |= SSLM_ISA_MOVBE;
  if (bit(b7, 3)) h |= SSLM_ISA_BMI1;
  if (bit(b7, 8)) h |= SSLM_ISA_BMI2;
  if (bit(ext1_ecx, 5)) h |= SSLM_ISA_LZCNT;
  if (ymm) {
    if (bit(c1, 28)) h |= SSLM_ISA_AVX;
    if (bit(c1, 12)) h |= SSLM_ISA_FMA;
    if (bit(c1, 29)) h |= SSLM_ISA_F16C;
    if (max_leaf >= 7 && bit(b7, 5)) h |= SSLM_ISA_AVX2;
    if (bit(a71, 4)) h |= SSLM_ISA_AVXVNNI;
  }
  if (zmm && max_leaf >= 7) {
    if (bit(b7, 16)) h |= SSLM_ISA_AVX512F;
    if (bit(b7, 30)) h |= SSLM_ISA_AVX512BW;
    if (bit(b7, 17)) h |= SSLM_ISA_AVX512DQ;
    if (bit(b7, 31)) h |= SSLM_ISA_AVX512VL;
    if (bit(b7, 28)) h |= SSLM_ISA_AVX512CD;
    if (bit(b7, 21)) h |= SSLM_ISA_AVX512IFMA;
    if (bit(c7, 11)) h |= SSLM_ISA_AVX512VNNI;
    if (bit(c7, 1)) h |= SSLM_ISA_AVX512VBMI;
    if (bit(d7, 23)) h |= SSLM_ISA_AVX512FP16;
    if (bit(a71, 5)) h |= SSLM_ISA_AVX512BF16;
    if (bit(d71, 19)) h |= SSLM_ISA_AVX10;
  }
  return h;
#else
  return 0;
#endif
}

Verdict CheckLinkedEngine() {
  Verdict v{};
  const SslmBuildIdentity* id = Readable(sslm_build_identity());
  if (!id) {
    v.status = SEMB_ENGINE_BUILD_UNRECORDED;
    return v;
  }
  v.identity = id;
  v.host_isa = HostIsa();
  v.missing_isa = id->required_isa & ~v.host_isa;
  if (v.missing_isa != 0) {
    v.status = SEMB_CPU_TIER_UNSUPPORTED;
    return v;
  }
#if defined(_MSC_FULL_VER)
  const uint32_t msc = _MSC_FULL_VER;
#else
  const uint32_t msc = 0;
#endif
#if defined(__clang__)
  const uint32_t clang = __clang_major__ * 10000 + __clang_minor__ * 100 + __clang_patchlevel__;
#else
  const uint32_t clang = 0;
#endif
#if defined(__GNUC__) && !defined(__clang__)
  const uint32_t gnuc = __GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__;
#else
  const uint32_t gnuc = 0;
#endif
#if defined(_MSVC_STL_UPDATE)
  const uint32_t stl = static_cast<uint32_t>(_MSVC_STL_UPDATE);
#else
  const uint32_t stl = 0;
#endif
#if defined(__GLIBCXX__)
  const uint32_t glibcxx = static_cast<uint32_t>(__GLIBCXX__);
#else
  const uint32_t glibcxx = 0;
#endif
#if defined(_LIBCPP_VERSION)
  const uint32_t libcpp = static_cast<uint32_t>(_LIBCPP_VERSION);
#else
  const uint32_t libcpp = 0;
#endif
  if (id->msc_full_ver != msc || id->clang_version != clang || id->gnuc_version != gnuc ||
      id->msvc_stl_update != stl || id->glibcxx != glibcxx || id->libcpp_version != libcpp) {
    v.status = SEMB_ENGINE_TOOLCHAIN_MISMATCH;
    return v;
  }
  if (id->build_record[0] == '\0') {
    v.status = SEMB_ENGINE_BUILD_UNRECORDED;
    return v;
  }
  if (std::strcmp(id->build_record, SEMB_ENGINE_BUILD_RECORD) != 0) {
    v.status = SEMB_ENGINE_LINK_MISMATCH;
    return v;
  }
  v.status = SEMB_OK;
  return v;
}

std::string LinkedBuildRecord() {
  const SslmBuildIdentity* id = Readable(sslm_build_identity());
  return id ? std::string(id->build_record) : std::string();
}

std::string IsaNames(uint64_t bits) {
  static const std::pair<uint64_t, const char*> kNames[] = {
      {SSLM_ISA_SSE3, "SSE3"},           {SSLM_ISA_SSSE3, "SSSE3"},         {SSLM_ISA_SSE4_1, "SSE4_1"},
      {SSLM_ISA_SSE4_2, "SSE4_2"},       {SSLM_ISA_POPCNT, "POPCNT"},       {SSLM_ISA_AVX, "AVX"},
      {SSLM_ISA_AVX2, "AVX2"},           {SSLM_ISA_FMA, "FMA"},             {SSLM_ISA_F16C, "F16C"},
      {SSLM_ISA_BMI1, "BMI1"},           {SSLM_ISA_BMI2, "BMI2"},           {SSLM_ISA_LZCNT, "LZCNT"},
      {SSLM_ISA_MOVBE, "MOVBE"},         {SSLM_ISA_AVX512F, "AVX512F"},     {SSLM_ISA_AVX512BW, "AVX512BW"},
      {SSLM_ISA_AVX512DQ, "AVX512DQ"},   {SSLM_ISA_AVX512VL, "AVX512VL"},   {SSLM_ISA_AVX512CD, "AVX512CD"},
      {SSLM_ISA_AVX512VNNI, "AVX512VNNI"}, {SSLM_ISA_AVX512VBMI, "AVX512VBMI"},
      {SSLM_ISA_AVX512IFMA, "AVX512IFMA"}, {SSLM_ISA_AVX512BF16, "AVX512BF16"},
      {SSLM_ISA_AVX512FP16, "AVX512FP16"}, {SSLM_ISA_AVXVNNI, "AVXVNNI"}, {SSLM_ISA_AVX10, "AVX10"},
  };
  std::string out;
  uint64_t named = 0;
  for (const auto& [bit, name] : kNames) {
    named |= bit;
    if ((bits & bit) == 0) continue;
    if (!out.empty()) out += ',';
    out += name;
  }
  if ((bits & ~named) != 0) {
    if (!out.empty()) out += ',';
    out += "0x";
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    for (uint64_t rest = bits & ~named; rest != 0; rest >>= 4) hex.insert(hex.begin(), kHex[rest & 0xF]);
    out += hex;
  }
  return out.empty() ? std::string("none") : out;
}

std::string LinkedDispatch() {
  const SslmBuildIdentity* id = Readable(sslm_build_identity());
  if (!id) return "unknown";
  switch (id->forced_tier) {
    case SSLM_FORCED_TIER_NONE:
      return "auto";
    case SSLM_FORCED_TIER_SCALAR:
      return "scalar";
    case SSLM_FORCED_TIER_SSE2:
      return "sse2";
    case SSLM_FORCED_TIER_AVX2:
      return "avx2";
    case SSLM_FORCED_TIER_AVX512:
      return "avx512";
    default:
      return "unknown";
  }
}

}  // namespace semb::engine_link
