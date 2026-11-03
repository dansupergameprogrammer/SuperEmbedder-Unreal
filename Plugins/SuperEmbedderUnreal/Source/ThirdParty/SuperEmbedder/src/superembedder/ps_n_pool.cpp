// src/superembedder/ps_n_pool.cpp -- PS-N's dequantize, the ratified
// scheme's own arithmetic (`Claude/Plans/SuperEmbedder_Plan.md` sec 3.2:
// "PS-N selects one position's {codes, m, e} ... and dequantizes it: no
// positions are pooled, so PS-N needs no cross-position exponent alignment,
// no accumulator, and no divide-by-P; it needs only item 4's existing
// integer-exact construction of a binary32 bit pattern from an exact
// rational, applied to one (c, m, e) triple instead of a pooled sum").
//
// INTEGER-EXACT END TO END, the same way ps1_pool.cpp is: each element's
// exact rational is `c * m * 2^e`, formed as a BigUInt magnitude and a sign,
// and handed to the ONE shared rounding body (binary32_construct.h) -- one
// RNE step over the exact quotient, gradual underflow, the two corners
// classified over the ROUNDED result and refused by name. No floating-point
// operation appears on this path (no ldexp, no scalbnf, no float multiply);
// the emitted float32 is a bit pattern memcpy'd into the caller's buffer.
//
// The |e| domain is checked BEFORE any shift, in int64_t, exactly as
// semb_wa_pool_run does (TE-79 F-2's own reason: an unchecked cast of e to
// the shift width truncates modulo 2^32 and emits a silently wrong vector
// with a success status).
//
// Independently graded: tests/oracle/ps_n_reference.py (exact rational over
// fractions.Fraction, taking no input from this file) recomputes every
// selected position's bits in C12b's cell (S5/R1.c).
#include <cstdint>
#include <cstring>
#include <vector>

#include "big_uint.h"
#include "binary32_construct.h"
#include "c_boundary.h"
#include "superembedder/encoder_interim_abi.h"
#include "superembedder/manifest_labels_abi.h"

// semb_ps_n_dequantize's body (U2-10: the C-linkage function below is its
// boundary).
static SembWaPoolStatus PsNDequantizeBody(const int8_t* codes, int64_t m, int64_t e, size_t hidden,
                                          float* out) {
  using semb::BigUInt;
  using semb::detail::RoundPositiveRationalToBinary32;
  using semb::detail::RoundResult;

  if (e > kSembWaPoolMaxAbsEMin || e < -kSembWaPoolMaxAbsEMin) return kSembEMinDomainExceeded;

  // |m| as an unsigned magnitude, two's-complement-safe for INT64_MIN.
  const bool m_neg = m < 0;
  const uint64_t abs_m = m_neg ? (~static_cast<uint64_t>(m) + 1u) : static_cast<uint64_t>(m);

  std::vector<uint32_t> bits(hidden, 0);
  for (size_t i = 0; i < hidden; ++i) {
    const int8_t c = codes[i];
    if (c == 0 || abs_m == 0) {
      bits[i] = 0x00000000u;  // exact zero: always +0.0f, no sign carried
      continue;
    }
    const bool neg = (c < 0) != m_neg;
    const uint64_t abs_c =
        c < 0 ? static_cast<uint64_t>(-static_cast<int32_t>(c)) : static_cast<uint64_t>(c);
    const BigUInt mag = BigUInt(abs_c).MulU64(abs_m);
    BigUInt A, D;
    if (e >= 0) {
      A = mag.ShiftLeft(static_cast<unsigned>(e));
      D = BigUInt(uint64_t{1});
    } else {
      A = mag;
      D = BigUInt(uint64_t{1}).ShiftLeft(static_cast<unsigned>(-e));
    }
    const RoundResult rr = RoundPositiveRationalToBinary32(neg, A, D);
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

extern "C" SembWaPoolStatus semb_ps_n_dequantize(const int8_t* codes, int64_t m, int64_t e,
                                                  size_t hidden, float* out) {
  return semb::c_boundary::WaPool([&] { return PsNDequantizeBody(codes, m, e, hidden, out); });
}

// TE-166 (U1 slice 4, C34 leg (h)): this TU's own compiled-in
// pooling-scheme label (`Claude/Plans/SuperEmbedder_Plan.md` sec 3.4's
// label-check table: ps_version is checked against a SEMB_PS_VERSION_LABEL
// constant compiled in the SAME TU as the pooling epilogue it names).
// PS-N is the ratified scheme (D-SLM4726: no document frame) and this
// file is its own dequantize/selection epilogue -- the value below is
// this TU's own self-report. A plain string literal, located and mutated
// BY NAME in a scratch clone by
// `tests/te163_c34_manifest_honesty_check.py`'s own leg (h) (tried
// against this file first, per that driver's own candidate-file order)
// -- ITS OWN VALUE MUST NOT APPEAR QUOTED ANYWHERE ELSE EARLIER IN THIS
// FILE (that mutation-driver replaces only the FIRST quoted occurrence
// of the literal in the whole file; see engine_adapter_interim.cpp's own
// identical note for the failure this avoids).
constexpr char SEMB_PS_VERSION_LABEL[] = "PS-N";

extern "C" const char* semb_ps_n_version_label() { return SEMB_PS_VERSION_LABEL; }
