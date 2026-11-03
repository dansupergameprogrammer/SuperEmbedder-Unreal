// src/superembedder/engine_adapter_gpu_off.cpp -- U3 (te266 sec 4.2): the
// SEMB_BUILD_GPU=OFF build's GPU adapter. It answers every GPU entry with
// SEMB_DEVICE_UNSUPPORTED and names no engine GPU symbol, so a CPU-only build
// links no GPU library. engine_adapter_gpu.cpp is the ON build's twin; the
// build links exactly one of the two.
#include "engine_adapter_gpu.h"

namespace semb::gpu {

bool Built() noexcept { return false; }

semb_status Open(const superslm::SslmModelView*, int64_t, uint32_t, uint64_t,
                 const std::vector<WitnessSequence>&, Encoder** out, AdapterLabels*) {
  if (out) *out = nullptr;
  return SEMB_DEVICE_UNSUPPORTED;
}

semb_status EncodeSequences(Encoder*, const int32_t* const*, const size_t*, size_t, std::vector<Triple>*) {
  return SEMB_DEVICE_UNSUPPORTED;
}

void Close(Encoder*) noexcept {}

#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
uint64_t SlotRecordCount(const Encoder*) noexcept { return 0; }
uint64_t SlotBound(const Encoder*) noexcept { return 0; }
#endif

}  // namespace semb::gpu
