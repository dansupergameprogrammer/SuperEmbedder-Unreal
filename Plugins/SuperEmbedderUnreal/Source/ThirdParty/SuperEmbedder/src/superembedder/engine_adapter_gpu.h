// src/superembedder/engine_adapter_gpu.h -- U3 (te266 sec 4.2): the GPU
// adapter's internal surface. C++ linkage only: no verb here is part of the
// public C ABI, and the process-wide submission lock the adapter takes is held
// only inside these C++-linkage bodies (U2 plan sec 6.2 J12).
//
// Two translation units define it, and exactly one links:
//   engine_adapter_gpu.cpp      SEMB_BUILD_GPU=ON (Windows, D3D12);
//   engine_adapter_gpu_off.cpp  SEMB_BUILD_GPU=OFF: every entry answers
//                               SEMB_DEVICE_UNSUPPORTED.
// The code that dispatches on semb_config.device (encoder.cpp) includes only
// this header, so it compiles identically in both builds.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "superembedder/semb.h"

namespace superslm {
struct SslmModelView;
}

namespace semb::gpu {

// PS-N's selected triple: the post-final-norm codes of the last position and
// their carried scale.
struct Triple {
  std::vector<int8_t> codes;
  int64_t m = 0;
  int64_t e = 0;
};

// One agreement-check sequence: the composed ids (terminator included) and
// the CPU's triple for them. Indices 0-15 are W-B's documents in corpus
// order, 16-21 W-Q's queries in order (21 is the truncation query).
struct WitnessSequence {
  std::vector<int32_t> ids;
  Triple cpu;
};

// The manifest's gpu_* provenance labels (sec 4.3), read at open. A label
// that cannot be read is "unknown"; a label never refuses an open.
struct AdapterLabels {
  std::string adapter_index;
  std::string vendor_id;
  std::string device_id;
  std::string adapter_name;
  std::string driver_version;
};

struct Encoder;

// True in the SEMB_BUILD_GPU=ON build.
bool Built() noexcept;

// Creates the engine context, maps `view`, reads the adapter labels and runs
// the agreement check over `witness`. On SEMB_OK `*out` is a GPU encoder
// with up to max(1, max_concurrency) lazily created slots; on any other
// status every engine handle the call created has been released.
// `public_handle` is the semb_encoder value the call log attributes calls to.
semb_status Open(const superslm::SslmModelView* view, int64_t context_cap, uint32_t max_concurrency,
                 uint64_t public_handle, const std::vector<WitnessSequence>& witness, Encoder** out,
                 AdapterLabels* labels);

// Encodes `n` composed id sequences (terminator included) one after another on
// one slot, and returns their triples. Nothing is returned unless every
// sequence succeeded. SEMB_CONCURRENCY_EXCEEDED when every slot is busy.
semb_status EncodeSequences(Encoder* encoder, const int32_t* const* ids, const size_t* lens, size_t n,
                            std::vector<Triple>* out);

// Releases every sequence, then the model map, then the context.
void Close(Encoder* encoder) noexcept;

#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
uint64_t SlotRecordCount(const Encoder* encoder) noexcept;
uint64_t SlotBound(const Encoder* encoder) noexcept;
#endif

}  // namespace semb::gpu

#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
namespace semb::test_seam {
// encoder.cpp: the GPU encoder of a public handle, or null.
semb::gpu::Encoder* GpuEncoderOf(const void* enc);
}  // namespace semb::test_seam
#endif
