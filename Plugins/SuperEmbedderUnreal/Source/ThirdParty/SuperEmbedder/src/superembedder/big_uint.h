// src/superembedder/big_uint.h -- TE-63 (U0 slice B): an arbitrary-precision
// unsigned integer, used solely to construct PS-1's emitted binary32 bit
// pattern from the exact rational S_i * 2^e_min / P by INTEGER arithmetic
// (Claude/Plans/SuperEmbedder_Plan.md sec 3.2 item 4: "sign, biased exponent
// and significand computed by integer arithmetic over S_i, e_min and P...
// no floating-point operation appears anywhere on this path").
//
// This is not a general-purpose bignum library: it implements exactly the
// operations the PS-1 construction needs (construct from a 64-bit value,
// shift, compare, add, subtract, multiply by a 64-bit scalar, and long
// division by another BigUInt for the final round-to-nearest-ties-to-even
// step) and nothing else. No third-party dependency is available to this
// project's build (matching tests/oracle/ps1_reference.py's own stated
// constraint), and MSVC has no native __int128 (matching the reason the
// withdrawn fold-round-11 probe cell gives for its own I128 type) -- this
// class generalizes that idea to an unbounded width so the construction is
// correct for whatever spread/e_min/P the caller passes, not merely the
// widths one probe population happened to exercise.
#ifndef SEMB_BIG_UINT_H
#define SEMB_BIG_UINT_H

#include <algorithm>
#include <cstdint>
#include <vector>

namespace semb {

// A non-negative arbitrary-precision integer: little-endian base-2^32
// limbs. Canonical form has no trailing (most-significant) zero limbs,
// except that a zero value is represented as a single limb {0} so
// IsZero()/BitLength() have one case to handle, not two.
class BigUInt {
 public:
  BigUInt() : limbs_{0} {}
  explicit BigUInt(uint64_t v) {
    limbs_.push_back(static_cast<uint32_t>(v & 0xFFFFFFFFu));
    limbs_.push_back(static_cast<uint32_t>(v >> 32));
    Trim();
  }

  bool IsZero() const { return limbs_.size() == 1 && limbs_[0] == 0; }

  // Exact bit length: 0 for a zero value, else the position of the highest
  // set bit plus one (so 2^(BitLength()-1) <= *this < 2^BitLength()).
  unsigned BitLength() const {
    if (IsZero()) return 0;
    uint32_t top = limbs_.back();
    unsigned bits_in_top = 0;
    while (top) {
      ++bits_in_top;
      top >>= 1;
    }
    return static_cast<unsigned>((limbs_.size() - 1) * 32) + bits_in_top;
  }

  // The low 64 bits, as an unsigned value -- the caller is responsible for
  // knowing the value fits (every call site in this codebase calls it only
  // after establishing BitLength() <= 64, e.g. a mantissa known < 2^24).
  uint64_t ToU64() const {
    uint64_t v = limbs_.size() > 0 ? limbs_[0] : 0;
    if (limbs_.size() > 1) v |= static_cast<uint64_t>(limbs_[1]) << 32;
    return v;
  }

  bool GetBit(unsigned i) const {
    const size_t limb = i / 32;
    if (limb >= limbs_.size()) return false;
    return ((limbs_[limb] >> (i % 32)) & 1u) != 0;
  }

  // -1 / 0 / 1 as *this is <, ==, > other.
  int Compare(const BigUInt& other) const {
    if (limbs_.size() != other.limbs_.size())
      return limbs_.size() < other.limbs_.size() ? -1 : 1;
    for (size_t i = limbs_.size(); i-- > 0;) {
      if (limbs_[i] != other.limbs_[i]) return limbs_[i] < other.limbs_[i] ? -1 : 1;
    }
    return 0;
  }
  bool operator<(const BigUInt& o) const { return Compare(o) < 0; }
  bool operator>=(const BigUInt& o) const { return Compare(o) >= 0; }
  bool operator==(const BigUInt& o) const { return Compare(o) == 0; }

  BigUInt ShiftLeft(unsigned bits) const {
    if (IsZero() || bits == 0) return *this;
    const unsigned limb_shift = bits / 32;
    const unsigned bit_shift = bits % 32;
    BigUInt out;
    out.limbs_.assign(limbs_.size() + limb_shift + 1, 0);
    for (size_t i = 0; i < limbs_.size(); ++i) {
      const uint64_t v = static_cast<uint64_t>(limbs_[i]) << bit_shift;
      out.limbs_[i + limb_shift] |= static_cast<uint32_t>(v & 0xFFFFFFFFu);
      out.limbs_[i + limb_shift + 1] |= static_cast<uint32_t>(v >> 32);
    }
    out.Trim();
    return out;
  }

  // Floor(*this / 2^bits).
  BigUInt ShiftRight(unsigned bits) const {
    if (IsZero() || bits == 0) return *this;
    const unsigned limb_shift = bits / 32;
    const unsigned bit_shift = bits % 32;
    if (limb_shift >= limbs_.size()) return BigUInt();
    BigUInt out;
    out.limbs_.assign(limbs_.size() - limb_shift, 0);
    for (size_t i = limb_shift; i < limbs_.size(); ++i) {
      uint64_t v = static_cast<uint64_t>(limbs_[i]) >> bit_shift;
      if (bit_shift != 0 && i + 1 < limbs_.size())
        v |= static_cast<uint64_t>(limbs_[i + 1]) << (32 - bit_shift);
      out.limbs_[i - limb_shift] = static_cast<uint32_t>(v & 0xFFFFFFFFu);
    }
    out.Trim();
    return out;
  }

  BigUInt Add(const BigUInt& other) const {
    BigUInt out;
    const size_t n = std::max(limbs_.size(), other.limbs_.size());
    out.limbs_.assign(n + 1, 0);
    uint64_t carry = 0;
    for (size_t i = 0; i < n; ++i) {
      const uint64_t a = i < limbs_.size() ? limbs_[i] : 0;
      const uint64_t b = i < other.limbs_.size() ? other.limbs_[i] : 0;
      const uint64_t s = a + b + carry;
      out.limbs_[i] = static_cast<uint32_t>(s & 0xFFFFFFFFu);
      carry = s >> 32;
    }
    out.limbs_[n] = static_cast<uint32_t>(carry);
    out.Trim();
    return out;
  }

  // Requires *this >= other; result is *this - other.
  BigUInt Sub(const BigUInt& other) const {
    BigUInt out;
    out.limbs_.assign(limbs_.size(), 0);
    int64_t borrow = 0;
    for (size_t i = 0; i < limbs_.size(); ++i) {
      const int64_t a = limbs_[i];
      const int64_t b = i < other.limbs_.size() ? other.limbs_[i] : 0;
      int64_t d = a - b - borrow;
      if (d < 0) {
        d += (int64_t{1} << 32);
        borrow = 1;
      } else {
        borrow = 0;
      }
      out.limbs_[i] = static_cast<uint32_t>(d);
    }
    out.Trim();
    return out;
  }

  // *this * v, v an arbitrary 64-bit scalar.
  BigUInt MulU64(uint64_t v) const {
    if (v == 0 || IsZero()) return BigUInt();
    const uint32_t v_lo = static_cast<uint32_t>(v & 0xFFFFFFFFu);
    const uint32_t v_hi = static_cast<uint32_t>(v >> 32);
    BigUInt out;
    out.limbs_.assign(limbs_.size() + 3, 0);
    for (size_t i = 0; i < limbs_.size(); ++i) {
      const uint64_t a = limbs_[i];
      // a * v_lo, added at limb offset i.
      {
        const uint64_t p = a * static_cast<uint64_t>(v_lo);
        uint64_t carry = p;
        size_t j = i;
        while (carry) {
          const uint64_t s = static_cast<uint64_t>(out.limbs_[j]) + (carry & 0xFFFFFFFFu);
          out.limbs_[j] = static_cast<uint32_t>(s & 0xFFFFFFFFu);
          carry = (carry >> 32) + (s >> 32);
          ++j;
        }
      }
      if (v_hi) {
        // a * v_hi, added at limb offset i+1.
        const uint64_t p = a * static_cast<uint64_t>(v_hi);
        uint64_t carry = p;
        size_t j = i + 1;
        while (carry) {
          const uint64_t s = static_cast<uint64_t>(out.limbs_[j]) + (carry & 0xFFFFFFFFu);
          out.limbs_[j] = static_cast<uint32_t>(s & 0xFFFFFFFFu);
          carry = (carry >> 32) + (s >> 32);
          ++j;
        }
      }
    }
    out.Trim();
    return out;
  }

  // Long division: returns {quotient, remainder} of *this / divisor.
  // divisor must be non-zero. O(BitLength() * divisor.limbs_.size()) --
  // adequate for this file's population sizes (a few hundred bits, driven a
  // few hundred thousand times across the W-A population; sub-second).
  static void DivMod(const BigUInt& a, const BigUInt& divisor, BigUInt* out_q, BigUInt* out_r) {
    BigUInt quotient;
    BigUInt remainder;
    const unsigned bits = a.BitLength();
    quotient.limbs_.assign((bits / 32) + 1, 0);
    for (unsigned i = bits; i-- > 0;) {
      remainder = remainder.ShiftLeft(1);
      if (a.GetBit(i)) remainder.limbs_[0] |= 1u;
      remainder.Trim();
      if (remainder.Compare(divisor) >= 0) {
        remainder = remainder.Sub(divisor);
        const size_t limb = i / 32;
        if (limb >= quotient.limbs_.size()) quotient.limbs_.resize(limb + 1, 0);
        quotient.limbs_[limb] |= (1u << (i % 32));
      }
    }
    quotient.Trim();
    remainder.Trim();
    if (out_q) *out_q = quotient;
    if (out_r) *out_r = remainder;
  }

 private:
  void Trim() {
    while (limbs_.size() > 1 && limbs_.back() == 0) limbs_.pop_back();
  }

  std::vector<uint32_t> limbs_;
};

}  // namespace semb

#endif  // SEMB_BIG_UINT_H
