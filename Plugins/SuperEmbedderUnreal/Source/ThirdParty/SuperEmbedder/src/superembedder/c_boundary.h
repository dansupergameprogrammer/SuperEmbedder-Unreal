// src/superembedder/c_boundary.h -- no C++ exception crosses a C-linkage
// frame (U2-10). Every C-linkage function the shipped library defines is a
// boundary or a leaf (tools/c_boundary_table.json lists each one). A
// boundary's definition is a single expression: a call to one of the
// templates below, with a lambda that calls the C++-linkage function holding
// the verb's body. The template
//
//   * catches std::bad_alloc and std::length_error and returns the family's
//     allocation status;
//   * catches every other exception, including types not derived from
//     std::exception, and returns the family's catch-all status;
//   * rethrows nothing and allocates nothing while handling (the interim
//     family's engine-status channel receives a string literal).
//
// Every object with a destructor lives in the body or deeper, never in the
// C-linkage frame, so the destructors an exception passes have already run
// in C++-linkage frames by the time it is caught, whatever the compiler's
// assumption about C-linkage functions.
#ifndef SEMB_C_BOUNDARY_H
#define SEMB_C_BOUNDARY_H

#include <new>
#include <stdexcept>
#include <utility>

#include "superembedder/encoder_interim_abi.h"
#include "superembedder/pool_abi.h"
#include "superembedder/seam_abi.h"
#include "superembedder/semb.h"

namespace semb::c_boundary {

namespace detail {

template <typename Status, Status kAllocation, Status kCatchAll, typename Body>
Status Invoke(Body&& body) noexcept {
  try {
    return std::forward<Body>(body)();
  } catch (const std::bad_alloc&) {
    return kAllocation;
  } catch (const std::length_error&) {
    return kAllocation;
  } catch (...) {
    return kCatchAll;
  }
}

}  // namespace detail

// semb_status: the public verbs and the internal semb_* verbs.
// SEMB_ALLOCATION_FAILED; SEMB_ENGINE_REFUSED, except semb_bank_open, whose
// catch-all is SEMB_ARTIFACT_UNREADABLE (passed as kCatchAll).
template <semb_status kCatchAll = SEMB_ENGINE_REFUSED, typename Body>
semb_status Status(Body&& body) noexcept {
  return detail::Invoke<semb_status, SEMB_ALLOCATION_FAILED, kCatchAll>(std::forward<Body>(body));
}

// SembInterimStatus: kSembInterimAllocationFailed; kSembInterimEngineRefused
// with the engine-status channel set to "UnhandledException".
template <typename Body>
SembInterimStatus Interim(const char** out_engine_status_name, Body&& body) noexcept {
  try {
    return std::forward<Body>(body)();
  } catch (const std::bad_alloc&) {
    return kSembInterimAllocationFailed;
  } catch (const std::length_error&) {
    return kSembInterimAllocationFailed;
  } catch (...) {
    if (out_engine_status_name) *out_engine_status_name = "UnhandledException";
    return kSembInterimEngineRefused;
  }
}

// SembEncodeStatus (the witness verb): kSembEncodeAllocationFailed;
// kSembEncodeEngineRefused.
template <typename Body>
SembEncodeStatus Encode(Body&& body) noexcept {
  return detail::Invoke<SembEncodeStatus, kSembEncodeAllocationFailed, kSembEncodeEngineRefused>(
      std::forward<Body>(body));
}

// SembWaPoolStatus: kSembWaPoolAllocationFailed; kSembWaPoolInternalFailure.
template <typename Body>
SembWaPoolStatus WaPool(Body&& body) noexcept {
  return detail::Invoke<SembWaPoolStatus, kSembWaPoolAllocationFailed, kSembWaPoolInternalFailure>(
      std::forward<Body>(body));
}

}  // namespace semb::c_boundary

#endif  // SEMB_C_BOUNDARY_H
