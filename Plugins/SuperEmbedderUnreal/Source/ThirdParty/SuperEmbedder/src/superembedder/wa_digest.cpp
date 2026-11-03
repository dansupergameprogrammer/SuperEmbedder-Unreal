// src/superembedder/wa_digest.cpp -- TE-78: W-A's own digest, driven over
// sec 4.4's named edge sets crossed with the full legal parameter range
// (`Claude/Plans/SuperEmbedder_Plan.md` sec 4.4b), calling the library's
// own semb_wa_pool_run (ps1_pool.cpp) over the population and folding
// every case's PARAMETER TUPLE (hidden, n, spread, ebase, the m-edge
// rotation index), status, and (on SEMB_OK) emitted bits into one
// SHA-256 -- the engine's own portable routine (superslm/sha256.h), sec
// 4.4b's own "one hash function, not two."
//
// TE-79 F-23 (Observation, TE-76 review) -- CORRECTED CLAIM, was "every
// case's inputs": the actual `codes`/`m`/`e` arrays built per case are
// NOT digested, only the five scalars that parameterise their
// construction plus the outcome. For the 1,029 refused cases only the
// status STRING is digested (per sec 4.4's own "every refusal's STATUS
// digested"), so a change to the construction (the code-edge placement,
// the m-rotation, the e-alternation) that leaves every case's status
// unchanged is invisible to this digest -- it would still move the
// oracle's own w_a_population.py comparison (tests/, outside this seat's
// scope), but not this witness component. The population is transcribed
// independently in two places (this file and w_a_population.py) with
// nothing comparing them at the construction level; both were read
// side by side this round and agree (crossing order, m rotation, e
// alternation, code-edge placement, the shared splitmix64 stream).
//
// THE POPULATION IS SEC 4.4'S OWN PROSE, TRANSCRIBED DIRECTLY -- not
// derived from any test file or oracle: "hidden in {1,2,7,64}, n in
// {1,2,3,5,17,41}, exponent spread in {0,1,2,9,10,63,64,73,74} crossed
// with a base exponent in {-33,-160,+80}, canonical-m edges at 2^30,
// 2^30+1, 2^31-1 and mid-range, and the int8 code edge set {-128,-127,-1,
// 0,1,126,127} on the leading positions -- 2,592 cases; every refusal's
// STATUS digested." The crossing order and the per-case construction
// (edge codes on the leading positions, a seeded splitmix64 filling the
// rest; m rotating through the edge set per position; e alternating
// between the base exponent and the base-plus-spread so both e_min and
// e_max are actually attained) mirror the withdrawn fold-round-11 probe's
// own driver shape (`Claude/Vitruvius/t2217-fold4-probe/cost/
// t2217_fold11_pooling_wa_cell.cpp`, read as population/construction
// precedent per this seat's own brief, never as an oracle) -- that
// driver's own POOLING ARITHMETIC was withdrawn (D-SLM4079, no corner
// refusals); its POPULATION CONSTRUCTION is not, and this file drives the
// SAME population through THIS project's own corrected epilogue
// (semb_wa_pool_run, which does carry both corner refusals).
#include "superembedder/semb.h"
#include "superembedder/witness_abi.h"
//
// TE-79 F-9 (Significant, TE-76 review) -- QUARANTINE NOTICE
// (StandardsDocument.md sec 5.4: "an instrument is commissioned before
// its verdicts are load-bearing -- and never by its own maker"). W-A's
// digest is a DECIDING instrument (it decides, via witness_sha, whether
// C36(b)'s epilogue-edit construction is caught) and this file -- AND
// both constructions shown to move it (C36(b)'s kExpBias mutant,
// C36(a)'s engine-side perturbation, D-SLM6072/D-SLM6075) -- were
// authored by the same seat (TE-78/TE-79's own builder). Its readings
// are QUARANTINED: recorded, exercised, and real, but not yet
// independently commissioned. The commissioning constructions (a
// must-accept and a must-reject, producible by this verb's own real
// data path) are owed to a seat independent of this one -- routed to the
// coverage auditor's rung, per this round's own brief.

#include <cstdint>
#include <cstring>
#include <vector>

#include "c_boundary.h"
#include "superembedder/pool_abi.h"
#include "superslm/sha256.h"

namespace semb {
namespace {

struct SplitMix64 {
  uint64_t state;
  uint64_t Next() {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
};

// sec 4.4's own named edge sets.
const int8_t kCodeEdges[] = {-128, -127, -1, 0, 1, 126, 127};
constexpr size_t kNumCodeEdges = sizeof(kCodeEdges) / sizeof(kCodeEdges[0]);
const int64_t kMEdges[] = {(int64_t{1} << 30), (int64_t{1} << 30) + 1, (int64_t{1} << 31) - 1,
                           (int64_t{1} << 30) + (int64_t{1} << 29)};
constexpr size_t kNumMEdges = sizeof(kMEdges) / sizeof(kMEdges[0]);
const int64_t kSpreads[] = {0, 1, 2, 9, 10, 63, 64, 73, 74};
const size_t kNs[] = {1, 2, 3, 5, 17, 41};
const size_t kHiddens[] = {1, 2, 7, 64};
const int64_t kEBases[] = {-33, -160, 80};

const char* StatusName(SembWaPoolStatus s) {
  switch (s) {
    case kSembOk: return "SEMB_OK";
    case kSembInputTooShort: return "SEMB_INPUT_TOO_SHORT";
    case kSembAccumulatorOverflow: return "SEMB_ACCUMULATOR_OVERFLOW";
    case kSembPooledSubnormal: return "SEMB_POOLED_SUBNORMAL";
    case kSembPooledOverflow: return "SEMB_POOLED_OVERFLOW";
    case kSembWaPoolAllocationFailed:
    case kSembWaPoolInternalFailure: break;
  }
  return "SEMB_UNKNOWN";
}

// A small streaming-digest helper over superslm::Sha256, matching the
// withdrawn probe's own Digest struct shape (little-endian u64s,
// length-prefixed strings, raw float32 bits) so every digested value has
// an unambiguous, injective encoding.
struct Digest {
  superslm::Sha256 h;
  void Bytes(const void* p, size_t n) { h.Update(static_cast<const uint8_t*>(p), n); }
  void U64(uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    Bytes(b, 8);
  }
  void Str(const char* s) {
    const size_t len = std::strlen(s);
    U64(len);
    Bytes(s, len);
  }
  void F32(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    U64(bits);
  }
};

// The verb's body (U2-10: the C-linkage function below is its boundary).
semb_status WaDigestBody(uint8_t out_digest[32]) {
  Digest d;
  d.Str("SEMB-W-A-1");  // a domain-separation tag, so this digest's own
                        // namespace never collides with W-B's or W-Q's.
  SplitMix64 rng{0x0F01D11ULL};  // sec 4.4's own named seed.

  for (size_t hidden : kHiddens) {
    for (size_t n : kNs) {
      for (int64_t spread : kSpreads) {
        for (int64_t ebase : kEBases) {
          for (size_t me = 0; me < kNumMEdges; ++me) {
            std::vector<int8_t> codes(n * hidden);
            std::vector<int64_t> m(n), e(n);
            for (size_t t = 0; t < n; ++t) {
              for (size_t i = 0; i < hidden; ++i) {
                const size_t k = t * hidden + i;
                codes[k] = (k < kNumCodeEdges) ? kCodeEdges[k]
                                                : static_cast<int8_t>(rng.Next() & 0xFF);
              }
              m[t] = kMEdges[(me + t) % kNumMEdges];
              // Alternate the extreme exponents so e_min and e_max are
              // both attained, exercising the full spread rather than an
              // average of it -- the same reason the withdrawn probe's
              // own construction alternates.
              e[t] = (t % 2 == 0) ? ebase : (ebase + spread);
            }

            std::vector<float> out(hidden, 0.0f);
            const SembWaPoolStatus status =
                semb_wa_pool_run(codes.data(), m.data(), e.data(), n, hidden, out.data());
            // A callee's allocation status is this verb's own; its internal
            // failure is this verb's catch-all. Neither is a digested
            // refusal of the population.
            if (status == kSembWaPoolAllocationFailed) return SEMB_ALLOCATION_FAILED;
            if (status == kSembWaPoolInternalFailure) return SEMB_ENGINE_REFUSED;

            d.U64(hidden);
            d.U64(n);
            d.U64(static_cast<uint64_t>(spread));
            d.U64(static_cast<uint64_t>(ebase));
            d.U64(me);
            d.Str(StatusName(status));  // every refusal's STATUS digested
            if (status == kSembOk) {
              for (size_t i = 0; i < hidden; ++i) d.F32(out[i]);
            }
          }
        }
      }
    }
  }

  d.h.Final(out_digest);
  return SEMB_OK;
}

}  // namespace
}  // namespace semb

extern "C" semb_status semb_wa_digest(uint8_t out_digest[32]) {
  return semb::c_boundary::Status([&] { return semb::WaDigestBody(out_digest); });
}
