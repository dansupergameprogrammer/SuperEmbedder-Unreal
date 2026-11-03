// src/superembedder/ps1_pool.cpp -- TE-63 (U0 slice B): PS-1's pooling
// epilogue, W-A's own primitive-tier entry point (`Claude/Plans/
// SuperEmbedder_Plan.md` sec 3.2 items 1, 2, 4; sec 4.4: "The pooling
// driver is an ENTRY POINT... pool({codes, m, e} over n positions) ->
// hidden float32, plus its refusals").
//
// INTEGER-EXACT END TO END, per sec 3.2 item 4's own specification: the
// pooled sum S_i is exact wide-integer accumulation (BigUInt, unbounded --
// generalizing the withdrawn fold-round-11 probe's fixed 128-bit I128 to
// whatever width the caller's spread/positions actually need); the emitted
// binary32 bit pattern is CONSTRUCTED from the exact rational
// v_i = S_i * 2^e_min / P by integer sign/exponent/significand arithmetic,
// with round-to-nearest-ties-to-even applied ONCE to the exact quotient.
// No floating-point operation appears anywhere on this path -- no ldexp,
// no scalbnf, no float divide, matching item 4's own prohibition (the
// withdrawn probe cell's `ldexp`+float-divide construction is exactly what
// D-SLM4079 withdrew it for: it depends on the ambient FP environment).
//
// Independently authored against the plan's prose and against
// `tests/oracle/ps1_reference.py`'s own documented algorithm (its module
// docstring, read as the specification's restatement -- this file's own
// arithmetic was derived from sec 3.2 item 4 directly, then checked for
// agreement against the oracle's documented rounding rule, never against
// its Python source line by line).
#include "superembedder/pool_abi.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "big_uint.h"
#include "binary32_construct.h"
#include "c_boundary.h"
#include "superembedder/manifest_labels_abi.h"

namespace semb {
namespace {

constexpr int64_t kAccumulatorSpreadBits = 73;

// A signed arbitrary-precision integer: a BigUInt magnitude plus a sign.
// `neg` is meaningless when `mag` is zero (both signs denote the same
// value); every accumulation below treats a zero magnitude as sign-free.
struct SignedBig {
  bool neg = false;
  BigUInt mag;
};

SignedBig AddSignedTerm(SignedBig acc, bool term_neg, const BigUInt& term_mag) {
  if (term_mag.IsZero()) return acc;
  if (acc.mag.IsZero()) {
    acc.neg = term_neg;
    acc.mag = term_mag;
    return acc;
  }
  if (acc.neg == term_neg) {
    acc.mag = acc.mag.Add(term_mag);
    return acc;
  }
  if (acc.mag.Compare(term_mag) >= 0) {
    acc.mag = acc.mag.Sub(term_mag);
  } else {
    acc.mag = term_mag.Sub(acc.mag);
    acc.neg = term_neg;
  }
  return acc;
}

// The overflow-safe spread computation, checked BEFORE any alignment
// shift, shared by semb_wa_pool_run and semb_wa_pooled_spread. `e_max` and
// `e_min` are cast to uint64_t and subtracted in that width -- their exact
// mathematical difference over int64_t's own range is always in
// [0, 2^64 - 2] (e_max >= e_min by construction), which fits a uint64_t
// exactly, so the unsigned subtraction (well-defined modulo 2^64, never
// UB) recovers the true magnitude with no width in which it can overflow
// -- sec 3.2 item 4's own "computed in a width in which it cannot
// overflow" applied literally, rather than casting an already-computed
// (and, for extreme inputs, already-UB) signed difference.
bool ComputeSpread(const int64_t* e, size_t n, int64_t* out_e_min, uint64_t* out_spread) {
  if (n < 2) return false;
  int64_t e_min = e[1], e_max = e[1];
  for (size_t t = 1; t < n; ++t) {
    if (e[t] < e_min) e_min = e[t];
    if (e[t] > e_max) e_max = e[t];
  }
  const uint64_t spread_u = static_cast<uint64_t>(e_max) - static_cast<uint64_t>(e_min);
  if (out_e_min) *out_e_min = e_min;
  if (out_spread) *out_spread = spread_u;
  return true;
}

}  // namespace
}  // namespace semb

// semb_wa_pool_run's body (U2-10: the C-linkage function below is its
// boundary).
static SembWaPoolStatus WaPoolRunBody(const int8_t* codes, const int64_t* m, const int64_t* e,
                                      size_t n, size_t hidden, float* out) {
  using semb::AddSignedTerm;
  using semb::BigUInt;
  using semb::ComputeSpread;
  using semb::detail::RoundPositiveRationalToBinary32;
  using semb::detail::RoundResult;
  using semb::SignedBig;

  int64_t e_min = 0;
  uint64_t spread = 0;
  if (!ComputeSpread(e, n, &e_min, &spread)) return kSembInputTooShort;
  if (spread > static_cast<uint64_t>(semb::kAccumulatorSpreadBits)) return kSembAccumulatorOverflow;
  // TE-79 F-2 (Critical, TE-76 review): e_min's own domain, checked in
  // int64_t -- the width e_min already lives in, so this comparison
  // itself cannot overflow -- BEFORE either ShiftLeft call below casts it
  // to `unsigned` (32 bits). Unchecked, that cast truncates modulo 2^32:
  // at e_min = 2^32 the 2^e_min factor is dropped entirely (`unsigned(2^32)
  // == 0`, so ShiftLeft(0) is a no-op), and the pooling epilogue returns
  // kSembOk with a vector that is wrong by a factor of 2^e_min -- a silent
  // wrong answer with a success status, executed and confirmed by the
  // review (e_min = 2^32 returned 5*2^30 in place of the true 5*2^(2^32)).
  if (e_min > kSembWaPoolMaxAbsEMin || e_min < -kSembWaPoolMaxAbsEMin) {
    return kSembEMinDomainExceeded;
  }

  const size_t first = 1;
  const size_t P = n - first;  // the POOLED count, never n

  std::vector<uint32_t> bits(hidden, 0);
  for (size_t i = 0; i < hidden; ++i) {
    SignedBig acc;
    for (size_t t = first; t < n; ++t) {
      const int8_t c = codes[t * hidden + i];
      if (c == 0) continue;
      const int64_t mt = m[t];
      if (mt == 0) continue;
      const unsigned shift = static_cast<unsigned>(e[t] - e_min);
      const bool term_neg = (c < 0) != (mt < 0);
      const uint64_t abs_c =
          c < 0 ? static_cast<uint64_t>(-static_cast<int32_t>(c)) : static_cast<uint64_t>(c);
      // Two's-complement negation via unsigned arithmetic -- safe even for
      // mt == INT64_MIN, where -mt is itself UB in signed arithmetic.
      const uint64_t abs_m =
          mt < 0 ? (~static_cast<uint64_t>(mt) + 1u) : static_cast<uint64_t>(mt);
      const BigUInt term_mag = BigUInt(abs_c).MulU64(abs_m).ShiftLeft(shift);
      acc = AddSignedTerm(acc, term_neg, term_mag);
    }

    if (acc.mag.IsZero()) {
      bits[i] = 0x00000000u;  // exact zero: always +0.0f, no sign carried
      continue;
    }

    BigUInt A, D;
    if (e_min >= 0) {
      A = acc.mag.ShiftLeft(static_cast<unsigned>(e_min));
      D = BigUInt(static_cast<uint64_t>(P));
    } else {
      A = acc.mag;
      D = BigUInt(static_cast<uint64_t>(P)).ShiftLeft(static_cast<unsigned>(-e_min));
    }
    const RoundResult rr = RoundPositiveRationalToBinary32(acc.neg, A, D);
    switch (rr.kind) {
      case RoundResult::kSubnormal:
        return kSembPooledSubnormal;
      case RoundResult::kInfinity:
        return kSembPooledOverflow;
      case RoundResult::kZero:
      case RoundResult::kNormal:
        bits[i] = rr.bits;
        break;
    }
  }

  for (size_t i = 0; i < hidden; ++i) std::memcpy(&out[i], &bits[i], sizeof(float));
  return kSembOk;
}

extern "C" SembWaPoolStatus semb_wa_pool_run(const int8_t* codes, const int64_t* m,
                                              const int64_t* e, size_t n, size_t hidden,
                                              float* out) {
  return semb::c_boundary::WaPool([&] { return WaPoolRunBody(codes, m, e, n, hidden, out); });
}

extern "C" SembWaPoolStatus semb_wa_pooled_spread(const int64_t* e, size_t n,
                                                   int64_t* out_spread) {
  using semb::ComputeSpread;

  int64_t e_min = 0;
  uint64_t spread = 0;
  if (!ComputeSpread(e, n, &e_min, &spread)) return kSembInputTooShort;
  if (spread > static_cast<uint64_t>(semb::kAccumulatorSpreadBits)) {
    if (out_spread) *out_spread = static_cast<int64_t>(spread);
    return kSembAccumulatorOverflow;
  }
  if (out_spread) *out_spread = static_cast<int64_t>(spread);
  return kSembOk;
}

// TE-166 (U1 slice 4, C34 leg (h)): this TU's own compiled-in
// pooling-scheme label -- D-SLM4727's named PS-1 fallback. Not read by
// `semb_bake_to_path` today (PS-N is the active scheme, ps_n_pool.cpp's
// own copy is what leg (h) uses); present because this epilogue stays
// compiled into the library and sec 3.4's label-check table names "the
// pooling epilogue's own TU" without restricting it to whichever scheme
// is currently active.
namespace {
constexpr char SEMB_PS_VERSION_LABEL[] = "PS-1";
}  // namespace

extern "C" const char* semb_ps1_version_label() { return SEMB_PS_VERSION_LABEL; }
