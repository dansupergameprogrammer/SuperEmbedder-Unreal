// include/superembedder/pool_abi.h -- TE-63 (U0 slice B): the C ABI verb
// for W-A's pooling driver, PS-1's epilogue (`Claude/Plans/
// SuperEmbedder_Plan.md` sec 3.2 items 1, 2, 4; sec 4.4).
//
// This header's symbol names, enum values and function signatures satisfy
// `tests/semb_wa_pool_abi_contract.h` (Curie's own concrete pin of the
// entry-point shape the plan commits to in prose) exactly -- the two are
// authored independently (the test contract is outside this seat's
// writable scope) and are kept byte-identical in their public shape by
// hand. TE-79 F-15's own correction: `extern "C"` linkage encodes no
// parameter types in the symbol name, so a divergence in argument order,
// width, or count between the two headers links CLEANLY and is caught
// only by whatever behavioural comparison a caller happens to run against
// it (this suite's own status/bit comparisons, for the enum values -- not
// a general guarantee, and not a link-time one). Two hand-synced copies
// of one ABI, with nothing that structurally compares them.
#ifndef SEMB_POOL_ABI_H
#define SEMB_POOL_ABI_H

#include <cstddef>
#include <cstdint>

enum SembWaPoolStatus : int32_t {
  kSembOk = 0,
  kSembInputTooShort = 1,
  kSembAccumulatorOverflow = 2,
  kSembPooledSubnormal = 3,
  kSembPooledOverflow = 4,
  // TE-79 F-2 (Critical): the pooled positions' minimum carried exponent
  // (e_min) domain, refused BEFORE any alignment/scaling shift is taken,
  // in a width that cannot overflow -- see kSembWaPoolMaxAbsEMin below and
  // semb_wa_pool_run's own doc comment.
  kSembEMinDomainExceeded = 5,
  // U2-10 Builds item 3: the C-linkage boundary's two statuses for this
  // family -- memory could not be allocated, and any other internal failure
  // (an exception of any other type). Neither is a corner refusal.
  kSembWaPoolAllocationFailed = 6,
  kSembWaPoolInternalFailure = 7,
};

// The largest |e_min| (the minimum carried exponent over the POOLED
// positions, 1..n-1) semb_wa_pool_run accepts before refusing
// kSembEMinDomainExceeded, checked in int64_t (a width e_min already
// lives in, so the check itself cannot overflow) BEFORE the 2^e_min
// scaling shift that follows it. 1,000,000 is chosen generously against
// two independent bounds, both far smaller: (1) the measured carried
// range this project has ever produced is [-160, 154] (sec 3.2's own
// executed margins plus W-A's synthetic edge set); (2) IEEE 754 binary32
// resolves any accumulator this library can construct (bounded to <= 128
// bits per sec 3.2 item 4's own accumulator-width proof) to exactly zero,
// a subnormal, or infinity once |e_min| exceeds a few hundred -- so no
// legitimate input is ever close to this bound, and a BigUInt shift by up
// to kSembWaPoolMaxAbsEMin bits is a bounded, sane allocation (~125 KB),
// never the multi-billion-bit allocation an unchecked cast to `unsigned`
// invites at |e_min| near 2^32 (TE-76 review F-2, executed: e_min = 2^32
// returned kSembOk with a silently wrong vector `5*2^30` instead of the
// true value `5*2^(2^32)`, which this domain check now refuses by name
// before either ShiftLeft call is reached).
constexpr int64_t kSembWaPoolMaxAbsEMin = 1000000;

extern "C" {

// PS-1's pooling epilogue. `codes` is n*hidden int8, row-major
// (position-major, dimension minor); `m`/`e` are n carried-scale pairs,
// one per position; `out` receives `hidden` float32 values ONLY when the
// return is kSembOk. Position 0 (index 0) is excluded from pooling by this
// entry point's own wiring.
//
// DOMAIN (TE-79 F-2): the minimum carried exponent over the pooled
// positions (e_min = min(e[1..n-1])) must satisfy
// |e_min| <= kSembWaPoolMaxAbsEMin, checked before any shift; a caller
// that violates it gets kSembEMinDomainExceeded, never a silently wrong
// vector. The cross-position spread (e_max - e_min) must additionally not
// exceed 73 bits (kSembAccumulatorOverflow) -- the two are independent
// domain checks, both refused before this verb performs any alignment or
// scaling shift.
SembWaPoolStatus semb_wa_pool_run(const int8_t* codes, const int64_t* m,
                                   const int64_t* e, size_t n, size_t hidden,
                                   float* out);

// The exponent spread (e_max - e_min) the guard computes over the POOLED
// positions (1..n-1), without running a full pool. Writes *out_spread only
// when the return is kSembOk or kSembAccumulatorOverflow (n >= 2);
// kSembInputTooShort for n < 2 with *out_spread untouched.
SembWaPoolStatus semb_wa_pooled_spread(const int64_t* e, size_t n,
                                        int64_t* out_spread);

}  // extern "C"

#endif  // SEMB_POOL_ABI_H
