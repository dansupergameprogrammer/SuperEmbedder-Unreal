// src/superembedder/binary32_construct.h -- the ONE integer construction of a
// binary32 bit pattern from an exact rational (`Claude/Plans/
// SuperEmbedder_Plan.md` sec 3.2 item 4: "That value's IEEE-754 binary32 bit
// pattern is CONSTRUCTED in integer code from the exact rational -- sign,
// biased exponent and significand computed by integer arithmetic ... with
// round-to-nearest-ties-to-even applied once, to the exact quotient. It is
// implemented ONCE, above the adapter seam, and used on every platform").
//
// Moved here verbatim from ps1_pool.cpp (TE-63/TE-79) so PS-1's pooling
// epilogue and PS-N's single-position dequantize (ps_n_pool.cpp) share the
// identical rounding body rather than two copies that could drift -- sec
// 3.2's own "PS-N needs only item 4's existing integer-exact construction ...
// applied to one (c, m, e) triple instead of a pooled sum". No arithmetic in
// this header changed in the move: PS-1's C1/W-A cells grade that by
// execution (tests/c1_ps1_reference_test.cpp, tests/w_a_pool_population_test.cpp).
//
// Internal to the library (src/, never include/): not an ABI surface.
#ifndef SEMB_BINARY32_CONSTRUCT_H
#define SEMB_BINARY32_CONSTRUCT_H

#include <cstdint>

#include "big_uint.h"

namespace semb {
namespace detail {

constexpr int kMantissaBits = 23;
constexpr int kExpBias = 127;
constexpr int64_t kMinNormalExp = -126;
constexpr int64_t kMaxNormalExp = 127;

// Compares A against D * 2^E for a possibly-negative E, without
// materializing 2^E on its own -- shifts whichever side of the comparison
// avoids a fractional (negative) shift.
inline int CompareAToDShiftedByE(const BigUInt& A, const BigUInt& D, int64_t E) {
  if (E >= 0) return A.Compare(D.ShiftLeft(static_cast<unsigned>(E)));
  return A.ShiftLeft(static_cast<unsigned>(-E)).Compare(D);
}

struct RoundResult {
  enum Kind { kZero, kNormal, kSubnormal, kInfinity } kind;
  uint32_t bits;
};

// Rounds the exact positive rational A/D (A > 0, D > 0) to the nearest
// binary32, ties to even, under gradual underflow -- mirrors sec 3.2 item
// 4's own specification: find the binary exponent E with 2^E <= A/D <
// 2^(E+1) by exact comparison (never log/float), scale to a 24-bit (or,
// under the subnormal floor, narrower) integer significand with one
// RNE step over the exact remainder, and classify the ROUNDED result
// (never the exact one) against the subnormal/overflow corners -- an
// exactly-zero A is the caller's responsibility to special-case BEFORE
// calling this (there is no "exact zero" rational with A > 0).
inline RoundResult RoundPositiveRationalToBinary32(bool sign, const BigUInt& A, const BigUInt& D) {
  const unsigned a_len = A.BitLength();
  const unsigned d_len = D.BitLength();
  int64_t E = static_cast<int64_t>(a_len) - static_cast<int64_t>(d_len);
  while (CompareAToDShiftedByE(A, D, E) < 0) --E;
  while (CompareAToDShiftedByE(A, D, E + 1) >= 0) ++E;

  int64_t exp_field;
  int64_t scale;
  if (E >= kMinNormalExp) {
    exp_field = E;
    scale = kMantissaBits - E;
  } else {
    exp_field = kMinNormalExp;
    scale = kMantissaBits - kMinNormalExp;  // 149
  }

  BigUInt num, den;
  if (scale >= 0) {
    num = A.ShiftLeft(static_cast<unsigned>(scale));
    den = D;
  } else {
    num = A;
    den = D.ShiftLeft(static_cast<unsigned>(-scale));
  }
  BigUInt q, rem;
  BigUInt::DivMod(num, den, &q, &rem);

  const BigUInt twice_rem = rem.ShiftLeft(1);
  const int cmp = twice_rem.Compare(den);
  bool round_up = false;
  if (cmp > 0) {
    round_up = true;
  } else if (cmp == 0) {
    round_up = q.GetBit(0);  // ties to even: round up only if q is odd
  }
  if (round_up) q = q.Add(BigUInt(uint64_t{1}));

  const uint32_t sign_bit = sign ? 1u : 0u;
  if (E >= kMinNormalExp) {
    if (q.Compare(BigUInt(uint64_t{1} << (kMantissaBits + 1))) == 0) {
      q = q.ShiftRight(1);
      ++exp_field;
    }
    if (exp_field > kMaxNormalExp) {
      return {RoundResult::kInfinity, (sign_bit << 31) | (0xFFu << 23)};
    }
    const uint32_t biased = static_cast<uint32_t>(exp_field + kExpBias);
    const uint32_t mantissa =
        static_cast<uint32_t>(q.Sub(BigUInt(uint64_t{1} << kMantissaBits)).ToU64());
    return {RoundResult::kNormal, (sign_bit << 31) | (biased << 23) | mantissa};
  }
  if (q.Compare(BigUInt(uint64_t{1} << kMantissaBits)) == 0) {
    // Rounded up across the subnormal/normal boundary: this IS exactly the
    // smallest normal (2^-126).
    return {RoundResult::kNormal, (sign_bit << 31) | (1u << 23)};
  }
  if (q.IsZero()) {
    return {RoundResult::kZero, sign_bit << 31};
  }
  return {RoundResult::kSubnormal, (sign_bit << 31) | static_cast<uint32_t>(q.ToU64())};
}

}  // namespace detail
}  // namespace semb

#endif  // SEMB_BINARY32_CONSTRUCT_H
