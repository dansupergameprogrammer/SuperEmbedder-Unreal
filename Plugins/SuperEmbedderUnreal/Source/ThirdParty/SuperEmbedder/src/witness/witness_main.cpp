// src/witness/witness_main.cpp -- TE-63/TE-78 (U0 slice B): the witness
// executable (`Claude/Plans/SuperEmbedder_Plan.md` sec 4.4b, "the
// structural law": "1. The witness is a target of the build being
// fingerprinted... 4. The witness's own translation unit contains no
// arithmetic on the reproducible path -- it is a loop and a serialiser").
//
// This TU calls the library's own exported C ABI verbs
// (semb_wa_digest, semb_encode_document_for_witness,
// semb_seam_serialize_document) and digests their output with the
// engine's own portable SHA-256 (superslm/sha256.h, sec 4.4b: "the same
// routine model_sha uses, so the fingerprint contains one hash function
// and not two"). It performs no arithmetic of its own on the
// reproducible path -- clause 4's own requirement, and the reason W-A's
// driver (ps1_pool.cpp, wa_digest.cpp) and the interim adapter
// (engine_adapter_interim.cpp) live in the LIBRARY rather than here (C38:
// an `inline` engine primitive called from this TU would compile under
// the WITNESS's own invocation, not the library's, and a definition on
// the library target would move nothing).
//
// TE-78: `witness_sha` COMPOSED per sec 4.4b -- SHA-256 over W-A's digest
// || W-B's digest || W-Q's digest, three fixed-width 32-byte components
// concatenated (no length prefix needed; sec 4.4b's own text: "three
// fixed-width (32-byte) SHA-256 digests concatenated need no length-
// prefix to stay injective"). W-A is `semb_wa_digest` (library code, the
// plan's own population). W-B is this TU's existing seam digest -- SHA-256
// over `semb_seam_serialize_document`'s own output bytes, unchanged from
// TE-63. **W-Q's own leg is not yet buildable at U0** (sec 4.4c: W-Q
// executes `semb_query_encode`, which does not exist until a real,
// converted artifact exists post Ask 5/E0b) -- its 32-byte slot is filled
// with a FIXED, DOCUMENTED PLACEHOLDER digest (SHA-256 of the literal
// string below), never zero bytes (indistinguishable from an
// uninitialized buffer) and never omitted from the concatenation (which
// would silently change witness_sha's own byte WIDTH the day W-Q lands,
// rather than only its VALUE). Decision recorded: D-SLM6073 (see this
// seat's own build log) -- U1's builder replaces this placeholder with
// W-Q's real digest and does not need to change witness_sha's shape to do
// it.
//
// The printed key `witness_seam_sha256=` is preserved BYTE-FOR-BYTE from
// TE-63 (outside this seat's writable scope to rename:
// `tests/witness_structural_matrix.py`'s own `run_witness()` parses this
// exact token) and now carries the FULL COMPOSED witness_sha, not only
// W-B's own seam digest -- the old name undersells what the value now is;
// a second, correctly-named `witness_sha=` field is also printed, byte-
// identical, for any future reader who wants the honestly-named key.
//
// WHAT THIS ROUND BUILDS AND WHAT IT DOES NOT. This executable is a
// target of this build, linking the real `superembedder` library and the
// real installed `superslm` library, calling the library's own entry
// points for W-A's digest, the seam, and its digest, with zero arithmetic
// in its own translation unit. The full C31 second-configure/detached-
// link/foreign-reading machinery remains outside this seat's own build
// (see this seat's build log's own disposition, D-SLM6074) --
// `tests/witness_structural_matrix.py` (outside this seat's writable
// scope) is where those constructions are driven and graded, and it now
// gives two of its ten legs (C31(iii), C31(v)) real, executed evaluation,
// with C36(b) now able to evaluate for real too now that W-A is wired in.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "superembedder/semb.h"  // semb_status, for witness_abi.h
#include "superembedder/seam_abi.h"
#include "superslm/sha256.h"

// TE-86's own C31(ii) leg (tests/witness_structural_matrix.py::eval_c31_ii)
// patches a scratch copy of this file directly ahead of the witness ABI
// header's own include line below, at an anchor that also spans the
// namespace-opening brace that follows it -- the new generated-header
// include is placed BEFORE that header's own include, not after, so the
// anchor's own text stays byte-identical. (TE-89 S-1: this comment used to
// quote the witness ABI header's own include line verbatim, which made
// this comment a SECOND occurrence of eval_c31_i's own anchor text --
// patch_text() replaced the FIRST occurrence, landing that leg's mutation
// here instead of on the real include below. Described in prose rather
// than quoted now, and patch_text() itself refuses a non-unique anchor.)
#include "semb_witness_build_generation.h"
#include "superembedder/witness_abi.h"

namespace {

void PrintUsage(const char* argv0) {
  std::fprintf(stderr,
                "usage: %s --model <path.sslm> --tokens <comma-separated token ids> "
                "[--chunk-budget N]\n",
                argv0);
}

std::vector<int32_t> ParseTokens(const std::string& csv) {
  std::vector<int32_t> out;
  size_t start = 0;
  while (start <= csv.size()) {
    size_t comma = csv.find(',', start);
    if (comma == std::string::npos) comma = csv.size();
    if (comma > start) out.push_back(std::atoi(csv.substr(start, comma - start).c_str()));
    start = comma + 1;
  }
  return out;
}

// sec 4.4c: W-Q is not yet buildable at U0 (no semb_query_encode entry
// point exists until a real, converted artifact lands post Ask 5/E0b).
// A FIXED, DOCUMENTED placeholder -- SHA-256 of this literal sentinel
// string, computed by the same engine SHA-256 routine every other
// component uses -- fills W-Q's own 32-byte slot in the concatenation so
// witness_sha keeps sec 4.4b's own three-fixed-width-component SHAPE from
// U0 forward; only its VALUE changes once U1 wires the real W-Q digest
// in, which is the expected, ordinary way witness_sha is allowed to move.
constexpr char kWQPlaceholderSentinel[] = "SEMB-W-Q-ABSENT-AT-U0-D-SLM6073-PLACEHOLDER";

void ComputeWQPlaceholderDigest(uint8_t out[32]) {
  superslm::Sha256 h;
  h.Update(reinterpret_cast<const uint8_t*>(kWQPlaceholderSentinel),
           sizeof(kWQPlaceholderSentinel) - 1);  // exclude the trailing NUL
  h.Final(out);
}

}  // namespace

int main(int argc, char** argv) {
  // sec 8 C31(ii) (SEMB_WITNESS_DETACHED_LINK, sec 4.4d item 2): at
  // process start, before anything else runs, compare this TU's own
  // compiled-in build-generation token against the LINKED LIBRARY's own
  // compiled-in copy, read through its C ABI accessor rather than a file.
  // The two are identical bytes if and only if both were configured
  // together -- linking this witness executable against a superembedder
  // library built from a SEPARATE configure (a detached link) makes the
  // accessor return a different token than this TU's own, caught here
  // before any digest, before argument parsing, before anything else.
  {
    const char* const kThisBuildGeneration = SEMB_WITNESS_BUILD_GENERATION_TOKEN;
    char library_build_generation[32];
    semb_library_build_generation(library_build_generation);
    // TE-89 (TE-88 review S-6): sizeof(...) - 1 on the MACRO reads exactly
    // the literal's own hex-character width, guarded at compile time by
    // the static_assert in semb_witness_build_generation.h.in -- see
    // witness_build_generation.cpp's own identical comment on its
    // memcpy. The %.*s precision arguments below carry the same width, so
    // a shorter literal cannot make the diagnostic print past the buffer
    // either.
    constexpr int kTokenLen = static_cast<int>(sizeof(SEMB_WITNESS_BUILD_GENERATION_TOKEN) - 1);
    if (std::memcmp(kThisBuildGeneration, library_build_generation, kTokenLen) != 0) {
      std::fprintf(stderr,
                    "witness: SEMB_WITNESS_DETACHED_LINK -- this build's own compiled-in "
                    "build-generation token does not match the linked library's own copy "
                    "(this=%.*s, library=%.*s); the witness executable and the "
                    "superembedder library it links were not configured together\n",
                    kTokenLen, kThisBuildGeneration, kTokenLen, library_build_generation);
      return 1;
    }
  }

  std::string model_path;
  std::string tokens_csv;
  size_t chunk_budget = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--model" && i + 1 < argc) {
      model_path = argv[++i];
    } else if (a == "--tokens" && i + 1 < argc) {
      tokens_csv = argv[++i];
    } else if (a == "--chunk-budget" && i + 1 < argc) {
      chunk_budget = static_cast<size_t>(std::atoll(argv[++i]));
    } else {
      PrintUsage(argv[0]);
      return 2;
    }
  }
  if (model_path.empty() || tokens_csv.empty()) {
    PrintUsage(argv[0]);
    return 2;
  }

  const std::vector<int32_t> tokens = ParseTokens(tokens_csv);
  const size_t n = tokens.size();
  if (n == 0) {
    std::fprintf(stderr, "witness: --tokens produced an empty sequence\n");
    return 1;
  }

  // Hidden size is not known until the model is open, inside the library's
  // own entry point -- this TU allocates for seam_abi.h's own documented
  // ceiling (TE-79 F-7: semb_encode_document_for_witness refuses
  // kSembEncodeHiddenTooLarge, before writing anything, if the real
  // hidden_size exceeds it) and lets the library report the real value
  // via hidden_out.
  std::vector<int8_t> codes(n * kSembWitnessMaxHidden);
  std::vector<int64_t> m(n), e(n);
  size_t hidden = 0;

  const SembEncodeStatus st = semb_encode_document_for_witness(
      model_path.c_str(), tokens.data(), n, chunk_budget, codes.data(), m.data(), e.data(),
      &hidden);
  if (st != kSembEncodeOk) {
    std::fprintf(stderr, "witness: encode refused (status=%d)\n", static_cast<int>(st));
    return 1;
  }

  // TE-79 F-8 (TE-76 review): sized EXACTLY per seam_serialize.cpp's own
  // formula (16 + n * (hidden + 16): the two u64 header fields, then per
  // position hidden code bytes plus two u64 scale fields) rather than a
  // heuristic "+4096" pad, which silently ran out above n=255 (16 + 16n >
  // 4096) and made every document above that length an unconditional
  // kSembSerializeBufferTooSmall.
  std::vector<uint8_t> buf(16 + n * (hidden + 16));
  size_t len = 0;
  const SembSerializeStatus sst = semb_seam_serialize_document(codes.data(), m.data(), e.data(), n,
                                                                 hidden, buf.data(), buf.size(),
                                                                 &len);
  if (sst != kSembSerializeOk) {
    std::fprintf(stderr, "witness: serialize returned status %d\n", static_cast<int>(sst));
    return 1;
  }

  // W-B's own digest: SHA-256 over the seam bytes, unchanged from TE-63.
  uint8_t wb_digest[32];
  {
    superslm::Sha256 h;
    h.Update(buf.data(), len);
    h.Final(wb_digest);
  }

  // W-A's own digest: the library's own driver over sec 4.4's population
  // (wa_digest.cpp) -- compiled into the library, not this TU, per C38.
  uint8_t wa_digest[32];
  const semb_status wa_status = semb_wa_digest(wa_digest);
  if (wa_status != SEMB_OK) {
    std::fprintf(stderr, "witness: W-A digest refused (%s)\n", semb_status_name(wa_status));
    return 1;
  }

  // W-Q's own slot: not yet buildable at U0 -- a fixed, documented
  // placeholder (see the header comment and D-SLM6073).
  uint8_t wq_digest[32];
  ComputeWQPlaceholderDigest(wq_digest);

  // witness_sha = SHA-256(W-A || W-B || W-Q), sec 4.4b's own formula.
  uint8_t witness_sha[32];
  {
    superslm::Sha256 h;
    h.Update(wa_digest, 32);
    h.Update(wb_digest, 32);
    h.Update(wq_digest, 32);
    h.Final(witness_sha);
  }

  // The printed key `witness_seam_sha256=` is preserved for
  // `tests/witness_structural_matrix.py`'s own parsing (outside this
  // seat's writable scope to rename) and now carries the FULL composed
  // witness_sha; `witness_sha=` is the same value under its honest name.
  std::printf("witness: n=%zu hidden=%zu seam_bytes=%zu witness_seam_sha256=%s witness_sha=%s\n",
              n, hidden, len, superslm::ToHex(witness_sha).c_str(),
              superslm::ToHex(witness_sha).c_str());
  return 0;
}
