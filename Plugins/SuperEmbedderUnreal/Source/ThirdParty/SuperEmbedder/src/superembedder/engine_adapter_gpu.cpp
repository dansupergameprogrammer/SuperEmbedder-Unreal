// src/superembedder/engine_adapter_gpu.cpp -- U3 (te266 sec 4.2): the
// SEMB_BUILD_GPU=ON build's GPU adapter. engine_adapter_gpu_off.cpp is the OFF
// build's twin; the build links exactly one of the two, and nothing else
// SEMB_BUILD_GPU introduces reaches another translation unit.
//
// What this file does, and what it never does:
//   - It maps the model the CPU open already loaded (the interim encoder's
//     view), so each artifact is read once. It names none of the engine's CPU
//     forward entry points; the CPU adapter stays the only one (C39).
//   - Every call it makes into the GPU engine goes through Call(), one
//     internal function table. Under SEMB_ENABLE_WORKSPACE_TEST_SEAM, which
//     only the superembedder_testseam target defines, that table carries the
//     test seam of tests/support/semb_u3_gpu_seam.h; the shipped library
//     carries none of it.
//   - One process-wide lock serializes every engine call from every handle
//     (sec 4.2, "Slots"), held as one critical section per encode and per
//     open, and taken only inside these C++-linkage bodies (U2 plan J12).
//   - A GPU open refuses unless the GPU reproduces the CPU's selected triple
//     {codes, m, e}, byte for byte, on all 22 witness sequences. It never
//     falls back to the CPU.
#include "engine_adapter_gpu.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

#include "superslm/gpu_1p0.h"

#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
#include "support/semb_u3_gpu_seam.h"
#endif

namespace semb::gpu {

namespace {

// ---------------------------------------------------------------------------
// Engine statuses by name. The engine exposes no name function; this table
// follows SslmGpuStatus's declaration order in superslm/gpu_1p0.h at the pin.
// ---------------------------------------------------------------------------
constexpr const char* kStatusNames[] = {
    "SSLM_OK",
    "SSLM_DISPATCH_BUDGET_TOO_SMALL",
    "SSLM_BUSY",
    "SSLM_CONTEXT_HAS_LIVE_HANDLES",
    "SSLM_MODEL_HAS_LIVE_SEQUENCES",
    "SSLM_ADAPTER_MODEL_MISMATCH",
    "SSLM_ADAPTER_BASE_HASH_MISMATCH",
    "SSLM_SEQUENCE_KV_BUFFER_MISMATCH",
    "SSLM_DEVICE_LOST",
    "SSLM_BATCH_BUDGET_EXHAUSTED",
    "SSLM_TOKEN_ID_OUT_OF_RANGE",
    "SSLM_SEQUENCE_REJECTED",
    "SSLM_RESTORE_MODEL_MISMATCH",
    "SSLM_MODEL_HAS_LIVE_ADAPTERS",
    "SSLM_ADAPTER_HAS_BOUND_SEQUENCES",
    "SSLM_GPU_SHADER_BINARY_STALE",
    "SSLM_GPU_ALLOCATION_FAILED",
    "SSLM_OUTPUT_BUFFER_TOO_SMALL",
    "SSLM_PREFILL_HIDDEN_UNAVAILABLE",
    "SSLM_GPU_SHADER_DIR_INVALID",
    "SSLM_GPU_SHADER_DIR_CONFLICT",
    "SSLM_GPU_PARALLEL_FOR_INVALID",
    "SSLM_GPU_PARALLEL_FOR_INCOMPLETE",
    "SSLM_GPU_RESIDENCY_FLAGS_INVALID",
    "SSLM_GPU_KV_BUDGET_EXCEEDED",
    "SSLM_GPU_KV_POOL_EXHAUSTED",
    "SSLM_GPU_KV_POOL_TOO_LARGE",
    "SSLM_GPU_HOLDER_MODE_MISMATCH",
    "SSLM_GPU_PREFIX_REJECTED",
    "SSLM_GPU_PREFIX_SCHEMA_MISMATCH",
    "SSLM_RESTORE_PRODUCER_MISMATCH",
    "SSLM_GPU_FORCED_TABLE_ABSENT",
    "SSLM_GPU_CONTEXT_CAP_EXCEEDED",
    "SSLM_GPU_ROWS_CHUNK_OPEN",
};
constexpr size_t kStatusCount = sizeof(kStatusNames) / sizeof(kStatusNames[0]);
static_assert(static_cast<uint32_t>(SslmGpuStatus::SSLM_DEVICE_LOST) == 8 &&
                  static_cast<uint32_t>(SslmGpuStatus::SSLM_SEQUENCE_REJECTED) == 11 &&
                  static_cast<uint32_t>(SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED) == 16 &&
                  static_cast<uint32_t>(SslmGpuStatus::SSLM_GPU_RESIDENCY_FLAGS_INVALID) == 23 &&
                  static_cast<uint32_t>(SslmGpuStatus::SSLM_GPU_KV_BUDGET_EXCEEDED) == 24 &&
                  static_cast<uint32_t>(SslmGpuStatus::SSLM_RESTORE_PRODUCER_MISMATCH) == 30 &&
                  static_cast<uint32_t>(SslmGpuStatus::SSLM_GPU_FORCED_TABLE_ABSENT) == 31 &&
                  static_cast<uint32_t>(SslmGpuStatus::SSLM_GPU_CONTEXT_CAP_EXCEEDED) == 32 &&
                  static_cast<uint32_t>(SslmGpuStatus::SSLM_GPU_ROWS_CHUNK_OPEN) == kStatusCount - 1,
              "the status name table follows the pinned engine's SslmGpuStatus order");

[[maybe_unused]] const char* StatusName(SslmGpuStatus s) noexcept {
  const auto v = static_cast<uint32_t>(s);
  return v < kStatusCount ? kStatusNames[v] : "SSLM_UNKNOWN_STATUS";
}

// ---------------------------------------------------------------------------
// The function table's verbs, named as the engine names them.
// ---------------------------------------------------------------------------
enum class Verb : uint32_t {
  kContextCreate,
  kModelMap,
  kSeqCreate,
  kSeqRelease,
  kSeqReset,
  kPrefill,
  kRead,
  kHiddenSize,
  kModelUnmap,
  kContextDestroy,
  kCount
};
constexpr const char* kVerbNames[] = {
    "sslm_gpu_context_create",
    "sslm_gpu_model_map",
    "sslm_gpu_seq_create",
    "sslm_gpu_seq_release",
    "sslm_gpu_seq_reset",
    "SslmGpuSeqPrefillPromptForG5Bridge",
    "sslm_gpu_seq_read_prefill_final_hidden",
    "sslm_gpu_model_hidden_size",
    "sslm_gpu_model_unmap",
    "sslm_gpu_context_destroy",
};
constexpr size_t kVerbCount = static_cast<size_t>(Verb::kCount);
static_assert(sizeof(kVerbNames) / sizeof(kVerbNames[0]) == kVerbCount, "one name per verb");

// Any nonzero budget (E6): the prompt prefill validates it nonzero and submits
// every admitted token as one chunk regardless of its value.
constexpr uint32_t kDispatchBudget = 0xFFFFFFFFu;

// A slot releases its sequence after any encode whose cap exceeds this.
constexpr int64_t kLongSequenceCap = 4096;

// ---------------------------------------------------------------------------
// The process-wide submission lock (sec 4.2, "Slots"). The holder flag is the
// lock-held assertion's input and makes the try-lock safe to call from a
// thread that already holds the lock.
// ---------------------------------------------------------------------------
std::mutex& SubmissionMutex() {
  static std::mutex m;
  return m;
}
thread_local bool t_holds_submission_lock = false;

class SubmissionLock {
 public:
  SubmissionLock() : lock_(SubmissionMutex()) { t_holds_submission_lock = true; }
  ~SubmissionLock() { t_holds_submission_lock = false; }
  SubmissionLock(const SubmissionLock&) = delete;
  SubmissionLock& operator=(const SubmissionLock&) = delete;

 private:
  std::lock_guard<std::mutex> lock_;
};

#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
// ---------------------------------------------------------------------------
// The test seam's state (tests/support/semb_u3_gpu_seam.h). Nothing here
// allocates while a thread is inside an engine call: a call's log record is
// reserved before the engine is entered and written after it returns.
// ---------------------------------------------------------------------------
struct Arm {
  Verb verb;
  uint64_t fire_at;  // the verb's call number that fires
  SslmGpuStatus status;
};

struct Seam {
  std::mutex mutex;  // guards log, verb_calls, arms, perturbation and last_read
  std::vector<semb_test_seam_gpu_call> log;
  uint64_t verb_calls[kVerbCount] = {};
  std::vector<Arm> arms;
  std::atomic<uint64_t> call_count{0};
  std::atomic<uint64_t> lock_violations{0};
  std::atomic<uint32_t> occupancy_now{0};
  std::atomic<uint32_t> occupancy_max{0};
  std::atomic<int64_t> context_count{0};
  std::atomic<int32_t> last_diverged{-1};
  // C10 (e)'s perturbation.
  bool perturb_armed = false;
  bool perturb_fired = false;
  int32_t perturb_where = 0;
  int32_t perturb_target = 0;
  int32_t perturb_sequence = 0;
  uint32_t perturb_code_index = 0;
  // The most recent successful read, after any where=1 perturbation.
  bool have_last_read = false;
  std::vector<int8_t> last_read_codes;
  int64_t last_read_m = 0;
  int64_t last_read_e = 0;
};

Seam& S() {
  static Seam* seam = new Seam();  // never destroyed: readers may run at exit
  return *seam;
}

// The seam's storage is created before main, with its first log and last-read
// buffers, so no seam allocation is live-counted after a test's baseline:
// C64-GPU (c)'s close-all check requires the live count to return to its value
// before the handles opened. A later growth replaces a buffer (net zero).
const bool kSeamCreatedBeforeMain = [] {
  Seam& s = S();
  s.log.reserve(256);
  s.last_read_codes.reserve(64);
  return true;
}();

bool VerbByName(const char* name, Verb* out) {
  if (!name) return false;
  for (size_t i = 0; i < kVerbCount; ++i) {
    if (std::strcmp(name, kVerbNames[i]) == 0) {
      *out = static_cast<Verb>(i);
      return true;
    }
  }
  return false;
}

bool StatusByName(const char* name, SslmGpuStatus* out) {
  if (!name) return false;
  for (size_t i = 0; i < kStatusCount; ++i) {
    if (std::strcmp(name, kStatusNames[i]) == 0) {
      *out = static_cast<SslmGpuStatus>(i);
      return true;
    }
  }
  return false;
}

void CopyName(char (&dst)[48], const char* src) {
  std::strncpy(dst, src, sizeof(dst) - 1);
  dst[sizeof(dst) - 1] = '\0';
}

void ApplyPerturbation(int32_t where, int32_t sequence, Triple* t) {
  Seam& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.perturb_armed || s.perturb_where != where) return;
  if (where == 0 && s.perturb_sequence != sequence) return;
  if (s.perturb_target == 0) {
    if (s.perturb_code_index >= t->codes.size()) return;
    int8_t& c = t->codes[s.perturb_code_index];
    c = static_cast<int8_t>(static_cast<uint8_t>(c) + 1u);  // two's-complement wrap
  } else {
    t->m += 1;
  }
  s.perturb_fired = true;
}

void RecordLastRead(const Triple& t) {
  Seam& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.last_read_codes.assign(t.codes.begin(), t.codes.end());
  s.last_read_m = t.m;
  s.last_read_e = t.e;
  s.have_last_read = true;
}
#endif  // SEMB_ENABLE_WORKSPACE_TEST_SEAM

// The one function table: every engine call U3 makes goes through here.
// `arg` is the call's size argument (the token count for the prompt prefill,
// the cap for a sequence create, 0 otherwise) and `handle` the public
// semb_encoder the call is made for. Never throws.
template <typename F>
SslmGpuStatus Call(Verb verb, uint64_t arg, uint64_t handle, F&& engine_call) noexcept {
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
  Seam& s = S();
  if (!t_holds_submission_lock) s.lock_violations.fetch_add(1);
  bool injected = false;
  SslmGpuStatus status = SslmGpuStatus::SSLM_OK;
  bool logged = false;
  uint64_t index = 0;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    // Reserve this call's record now: nothing allocates inside the engine
    // call. A reservation that fails leaves this one call unrecorded.
    try {
      if (s.log.size() == s.log.capacity()) s.log.reserve(std::max<size_t>(256, s.log.capacity() * 2));
      logged = true;
    } catch (...) {
      logged = false;
    }
    const uint64_t nth = ++s.verb_calls[static_cast<size_t>(verb)];
    for (size_t i = 0; i < s.arms.size(); ++i) {
      if (s.arms[i].verb == verb && s.arms[i].fire_at == nth) {
        injected = true;
        status = s.arms[i].status;
        s.arms.erase(s.arms.begin() + static_cast<std::ptrdiff_t>(i));  // shrinking never allocates
        break;
      }
    }
    index = s.call_count.fetch_add(1) + 1;
  }
  if (!injected) {
    const uint32_t now = s.occupancy_now.fetch_add(1) + 1;
    uint32_t seen = s.occupancy_max.load();
    while (now > seen && !s.occupancy_max.compare_exchange_weak(seen, now)) {
    }
    status = engine_call();
    s.occupancy_now.fetch_sub(1);
  }
  if (verb == Verb::kContextCreate && status == SslmGpuStatus::SSLM_OK) s.context_count.fetch_add(1);
  if (verb == Verb::kContextDestroy && status == SslmGpuStatus::SSLM_OK) s.context_count.fetch_sub(1);
  if (logged) {
    std::lock_guard<std::mutex> lock(s.mutex);
    semb_test_seam_gpu_call record{};
    CopyName(record.verb, kVerbNames[static_cast<size_t>(verb)]);
    CopyName(record.status, StatusName(status));
    record.index = index;
    record.arg = arg;
    record.handle = handle;
    record.injected = injected ? 1 : 0;
    if (s.log.size() < s.log.capacity()) s.log.push_back(record);
  }
  return status;
#else
  (void)verb;
  (void)arg;
  (void)handle;
  return engine_call();
#endif
}

// C(n) = max(64, the next power of two at or above n), n counting the
// terminator (sec 4.2, "Sequence sizing"), and never above the model's own
// context: a model whose context is not a power of two sizes its longest
// documents at the context itself. n never exceeds the context (composition
// refuses first).
int64_t SequenceCap(size_t n, int64_t context_cap) {
  int64_t c = 64;
  while (static_cast<uint64_t>(c) < n) c <<= 1;
  if (context_cap > 0 && c > context_cap) c = context_cap;
  return c;
}

std::string EnvOrEmpty(const char* name) {
#if defined(_WIN32)
  // getenv is not guaranteed to see a value set through the Win32 API; the
  // engine reads the variable with GetEnvironmentVariableA.
  char buf[256];
  const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  if (n == 0 || n >= sizeof(buf)) return std::string();
  return std::string(buf, n);
#else
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
#endif
}

#if defined(_WIN32)
std::string Utf8FromWide(const wchar_t* w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return std::string();
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
  out.resize(static_cast<size_t>(n - 1));
  return out;
}

std::string Hex4(UINT v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%04x", static_cast<unsigned>(v));
  return buf;
}

// The adapter the engine's own selection rule picks (E16): an explicit
// SSLM_GPU_ADAPTER_INDEX enumerates with EnumAdapters1; none takes the first
// hardware adapter in EnumAdapterByGpuPreference(HIGH_PERFORMANCE) order that
// creates a feature-level 12_0 device.
IDXGIAdapter1* SelectAdapter(IDXGIFactory6* factory, const std::string& index) {
  IDXGIAdapter1* adapter = nullptr;
  if (!index.empty()) {
    char* end = nullptr;
    const unsigned long i = std::strtoul(index.c_str(), &end, 10);
    if (end == index.c_str() || *end != '\0' || i > 0xFFFFu) return nullptr;
    if (FAILED(factory->EnumAdapters1(static_cast<UINT>(i), &adapter))) return nullptr;
    return adapter;
  }
  for (UINT i = 0;; ++i) {
    IDXGIAdapter1* candidate = nullptr;
    if (FAILED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                   IID_PPV_ARGS(&candidate)))) {
      return nullptr;
    }
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(candidate->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
        SUCCEEDED(D3D12CreateDevice(candidate, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr))) {
      return candidate;
    }
    candidate->Release();
  }
}
#endif

// The complete fallback set (sec 4.3, P:4212): every label "unknown" except
// the index this open read. Never throws: "unknown" and "default" fit every
// supported standard library's small-string buffer, and an index whose copy
// cannot be allocated is itself "unknown".
void SetFallbackLabels(const std::string& index, AdapterLabels* labels) noexcept {
  labels->vendor_id = "unknown";
  labels->device_id = "unknown";
  labels->adapter_name = "unknown";
  labels->driver_version = "unknown";
  try {
    labels->adapter_index = index.empty() ? "default" : index;
  } catch (...) {
    labels->adapter_index = "unknown";
  }
}

// The manifest's gpu_* labels (sec 4.3), read through DXGI from the adapter
// the engine's rule picks for the SSLM_GPU_ADAPTER_INDEX value this open
// reads, into a local set that starts as the fallback set. A value that cannot
// be read stays "unknown"; a label never refuses. May throw (an allocation):
// the caller commits the result only once this returns.
AdapterLabels ReadAdapterLabels(const std::string& index) {
  AdapterLabels labels;
  SetFallbackLabels(index, &labels);
#if defined(_WIN32)
  IDXGIFactory6* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return labels;
  IDXGIAdapter1* adapter = SelectAdapter(factory, index);
  if (adapter) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      labels.vendor_id = Hex4(desc.VendorId);
      labels.device_id = Hex4(desc.DeviceId);
      const std::string name = Utf8FromWide(desc.Description);
      if (!name.empty()) labels.adapter_name = name;
    }
    LARGE_INTEGER umd{};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", static_cast<unsigned>(HIWORD(umd.HighPart)),
                    static_cast<unsigned>(LOWORD(umd.HighPart)), static_cast<unsigned>(HIWORD(umd.LowPart)),
                    static_cast<unsigned>(LOWORD(umd.LowPart)));
      labels.driver_version = buf;
    }
    adapter->Release();
  }
  factory->Release();
#endif
  return labels;
}

}  // namespace

// ---------------------------------------------------------------------------
// The GPU encoder.
// ---------------------------------------------------------------------------
struct Slot {
  SslmGpuSequenceHandle* seq = nullptr;
  int64_t cap = 0;
  bool busy = false;
  // A default-row status left a sequence the release refused: release it
  // before the slot's next use.
  bool must_release = false;
};

struct Encoder {
  SslmGpuContext* ctx = nullptr;
  SslmGpuModelHandle* model = nullptr;
  uint32_t hidden = 0;
  int64_t context_cap = 0;
  uint64_t public_handle = 0;
  // GPU slot records are created on acquisition, up to `bound`, and never
  // sized at open (TE-412); records have stable addresses.
  uint32_t bound = 1;
  std::mutex slots_mutex;
  std::vector<std::unique_ptr<Slot>> slots;
  // Set by any SSLM_DEVICE_LOST: every later call that needs the GPU returns
  // SEMB_DEVICE_UNAVAILABLE without an engine call. Close still releases.
  std::atomic<bool> poisoned{false};
};

namespace {

// Each helper below runs with the submission lock held.

SslmGpuStatus ReleaseSequence(Encoder* enc, Slot* slot) noexcept {
  SslmGpuSequenceHandle* seq = slot->seq;
  const SslmGpuStatus st = Call(Verb::kSeqRelease, 0, enc->public_handle,
                                [&] { return sslm_gpu_seq_release(enc->ctx, seq); });
  if (st == SslmGpuStatus::SSLM_OK) {
    slot->seq = nullptr;
    slot->cap = 0;
    slot->must_release = false;
  } else {
    slot->must_release = true;
  }
  return st;
}

// sec 4.1's encode-time mapping, total over every status.
semb_status MapEncodeStatus(Encoder* enc, Slot* slot, SslmGpuStatus st) noexcept {
  switch (st) {
    case SslmGpuStatus::SSLM_OK:
      return SEMB_OK;
    case SslmGpuStatus::SSLM_DEVICE_LOST:
      enc->poisoned.store(true);
      return SEMB_DEVICE_UNAVAILABLE;
    case SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED:
      // The handle stays usable; every encode resets the slot's sequence first.
      return SEMB_ALLOCATION_FAILED;
    case SslmGpuStatus::SSLM_SEQUENCE_REJECTED:
      // The slot keeps its sequence; the next encode resets it before use.
      return SEMB_ENGINE_REFUSED;
    default:
      // The default row: the slot releases its sequence, so no state the
      // status left behind reaches a later document.
      if (slot->seq && ReleaseSequence(enc, slot) == SslmGpuStatus::SSLM_DEVICE_LOST) {
        enc->poisoned.store(true);
        return SEMB_DEVICE_UNAVAILABLE;
      }
      return SEMB_ENGINE_REFUSED;
  }
}

// A release that is itself the failing call (a resize or a must_release).
semb_status MapReleaseStatus(Encoder* enc, SslmGpuStatus st) noexcept {
  if (st == SslmGpuStatus::SSLM_DEVICE_LOST) {
    enc->poisoned.store(true);
    return SEMB_DEVICE_UNAVAILABLE;
  }
  if (st == SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED) return SEMB_ALLOCATION_FAILED;
  return SEMB_ENGINE_REFUSED;
}

// One document: size the slot's sequence to C(n), reset, prefill, read. One
// critical section, from the create through the release that follows a long
// document. `out->codes` is sized by the caller, before the lock.
semb_status EncodeOne(Encoder* enc, Slot* slot, const int32_t* ids, size_t n, Triple* out) {
  if (n == 0 || n > static_cast<size_t>(INT32_MAX)) return SEMB_ENGINE_REFUSED;
  const int64_t cap = SequenceCap(n, enc->context_cap);
  const uint64_t h = enc->public_handle;
  SubmissionLock lock;
  if (enc->poisoned.load()) return SEMB_DEVICE_UNAVAILABLE;

  semb_status result = SEMB_OK;
  [&] {
    if (slot->seq && (slot->cap != cap || slot->must_release)) {
      const SslmGpuStatus st = ReleaseSequence(enc, slot);
      if (st != SslmGpuStatus::SSLM_OK) {
        result = MapReleaseStatus(enc, st);
        return;
      }
    }
    if (!slot->seq) {
      SslmGpuSequenceHandle* seq = nullptr;
      const SslmGpuStatus st = Call(Verb::kSeqCreate, static_cast<uint64_t>(cap), h,
                                    [&] { return sslm_gpu_seq_create(enc->ctx, enc->model, cap, &seq); });
      if (st != SslmGpuStatus::SSLM_OK) {
        result = MapEncodeStatus(enc, slot, st);
        return;
      }
      slot->seq = seq;
      slot->cap = cap;
    }
    SslmGpuSequenceHandle* seq = slot->seq;
    SslmGpuStatus st = Call(Verb::kSeqReset, 0, h, [&] { return sslm_gpu_seq_reset(enc->ctx, seq); });
    if (st != SslmGpuStatus::SSLM_OK) {
      result = MapEncodeStatus(enc, slot, st);
      return;
    }
    st = Call(Verb::kPrefill, static_cast<uint64_t>(n), h, [&] {
      return SslmGpuSeqPrefillPromptForG5Bridge(enc->ctx, seq, ids, static_cast<int32_t>(n), kDispatchBudget);
    });
    if (st != SslmGpuStatus::SSLM_OK) {
      result = MapEncodeStatus(enc, slot, st);
      return;
    }
    size_t required = 0;
    int64_t m = 0, e = 0;
    st = Call(Verb::kRead, 0, h, [&] {
      return sslm_gpu_seq_read_prefill_final_hidden(enc->ctx, seq, out->codes.data(), out->codes.size(),
                                                    &required, &m, &e);
    });
    if (st != SslmGpuStatus::SSLM_OK) {
      result = MapEncodeStatus(enc, slot, st);
      return;
    }
    if (required != out->codes.size()) {
      result = MapEncodeStatus(enc, slot, SslmGpuStatus::SSLM_OUTPUT_BUFFER_TOO_SMALL);
      return;
    }
    out->m = m;
    out->e = e;
  }();

  // A sequence above 4,096 is released inside the same critical section, so
  // at most one such sequence exists in the process at a time.
  if (slot->seq && slot->cap > kLongSequenceCap && !enc->poisoned.load()) {
    const SslmGpuStatus st = ReleaseSequence(enc, slot);
    if (st == SslmGpuStatus::SSLM_DEVICE_LOST) {
      enc->poisoned.store(true);
      result = SEMB_DEVICE_UNAVAILABLE;
    }
  }
  return result;
}

// An open's engine handles, released on every exit that does not hand them to
// the encoder: each sequence, then the model map, then the context (E9).
// Runs with the submission lock held.
class OpenRollback {
 public:
  explicit OpenRollback(Encoder* enc) : enc_(enc) {}
  ~OpenRollback() {
    if (!armed_) return;
    const uint64_t h = enc_->public_handle;
    if (check_seq) {
      SslmGpuSequenceHandle* seq = check_seq;
      Call(Verb::kSeqRelease, 0, h, [&] { return sslm_gpu_seq_release(enc_->ctx, seq); });
    }
    if (enc_->model) {
      SslmGpuModelHandle* model = enc_->model;
      Call(Verb::kModelUnmap, 0, h, [&] { return sslm_gpu_model_unmap(enc_->ctx, model); });
      enc_->model = nullptr;
    }
    if (enc_->ctx) {
      SslmGpuContext* ctx = enc_->ctx;
      Call(Verb::kContextDestroy, 0, h, [&] { return sslm_gpu_context_destroy(ctx); });
      enc_->ctx = nullptr;
    }
  }
  void Disarm() { armed_ = false; }
  OpenRollback(const OpenRollback&) = delete;
  OpenRollback& operator=(const OpenRollback&) = delete;

  SslmGpuSequenceHandle* check_seq = nullptr;

 private:
  Encoder* enc_;
  bool armed_ = true;
};

// sec 4.1: context creation and the model map refuse SEMB_DEVICE_UNAVAILABLE,
// except memory, which is SEMB_ALLOCATION_FAILED.
semb_status MapOpenStatus(SslmGpuStatus st) {
  return st == SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED ? SEMB_ALLOCATION_FAILED : SEMB_DEVICE_UNAVAILABLE;
}

// sec 4.2's agreement check: a GPU refusal of a witness sequence the CPU
// encoded is a mismatch; a lost device, or any other failure to run the
// check, refuses unavailable; memory refuses as memory.
semb_status MapCheckStatus(SslmGpuStatus st) {
  switch (st) {
    case SslmGpuStatus::SSLM_SEQUENCE_REJECTED:
      return SEMB_DEVICE_DIVERGED;
    case SslmGpuStatus::SSLM_GPU_ALLOCATION_FAILED:
      return SEMB_ALLOCATION_FAILED;
    default:
      return SEMB_DEVICE_UNAVAILABLE;
  }
}

void SetLastDiverged(int32_t index) {
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
  S().last_diverged.store(index);
#else
  (void)index;
#endif
}

}  // namespace

bool Built() noexcept { return true; }

semb_status Open(const superslm::SslmModelView* view, int64_t context_cap, uint32_t max_concurrency,
                 uint64_t public_handle, const std::vector<WitnessSequence>& witness, Encoder** out,
                 AdapterLabels* labels) {
  if (out) *out = nullptr;
  if (!view || !out || witness.empty()) return SEMB_NULL_ARGUMENT;
  auto enc = std::make_unique<Encoder>();
  enc->context_cap = context_cap;
  enc->public_handle = public_handle;
  enc->bound = max_concurrency == 0 ? 1u : max_concurrency;

  // Everything the open allocates is allocated before the lock is taken.
  const std::string index = EnvOrEmpty("SSLM_GPU_ADAPTER_INDEX");
  const size_t width = witness.front().cpu.codes.size();
  Triple gpu;
  gpu.codes.resize(width);
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
  {
    Seam& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.last_read_codes.reserve(width);
  }
#endif

  semb_status result = SEMB_OK;
  {
    SubmissionLock lock;
    OpenRollback rollback(enc.get());
    const uint64_t h = public_handle;
    SetLastDiverged(-1);
    result = [&]() -> semb_status {
      SslmGpuContext* ctx = nullptr;
      SslmGpuStatus st = Call(Verb::kContextCreate, 0, h,
                              [&] { return sslm_gpu_context_create(GpuContextConfig{}, &ctx); });
      if (st != SslmGpuStatus::SSLM_OK) return MapOpenStatus(st);
      enc->ctx = ctx;
      SslmGpuModelHandle* model = nullptr;
      st = Call(Verb::kModelMap, 0, h,
                [&] { return sslm_gpu_model_map(enc->ctx, view, GpuResidencyConfig{}, &model); });
      if (st != SslmGpuStatus::SSLM_OK) return MapOpenStatus(st);
      enc->model = model;
      uint32_t hidden = 0;
      st = Call(Verb::kHiddenSize, 0, h, [&] { return sslm_gpu_model_hidden_size(enc->model, &hidden); });
      if (st != SslmGpuStatus::SSLM_OK) return MapOpenStatus(st);
      enc->hidden = hidden;
      if (hidden != width) return SEMB_DEVICE_DIVERGED;

      // The agreement check: every witness sequence on the GPU, compared
      // with the CPU's triple for the same composed ids, the whole triple
      // byte for byte. One sequence, sized to each witness sequence as a slot
      // sizes it, released before the open returns.
      int64_t check_cap = 0;
      for (size_t i = 0; i < witness.size(); ++i) {
        const WitnessSequence& w = witness[i];
        const size_t n = w.ids.size();
        if (n == 0 || n > static_cast<size_t>(INT32_MAX) || w.cpu.codes.size() != width) {
          return SEMB_ENGINE_REFUSED;
        }
        const int64_t cap = SequenceCap(n, context_cap);
        if (rollback.check_seq && check_cap != cap) {
          SslmGpuSequenceHandle* seq = rollback.check_seq;
          st = Call(Verb::kSeqRelease, 0, h, [&] { return sslm_gpu_seq_release(enc->ctx, seq); });
          if (st != SslmGpuStatus::SSLM_OK) return MapCheckStatus(st);
          rollback.check_seq = nullptr;
        }
        if (!rollback.check_seq) {
          SslmGpuSequenceHandle* seq = nullptr;
          st = Call(Verb::kSeqCreate, static_cast<uint64_t>(cap), h,
                    [&] { return sslm_gpu_seq_create(enc->ctx, enc->model, cap, &seq); });
          if (st != SslmGpuStatus::SSLM_OK) return MapCheckStatus(st);
          rollback.check_seq = seq;
          check_cap = cap;
        }
        SslmGpuSequenceHandle* seq = rollback.check_seq;
        st = Call(Verb::kSeqReset, 0, h, [&] { return sslm_gpu_seq_reset(enc->ctx, seq); });
        if (st != SslmGpuStatus::SSLM_OK) return MapCheckStatus(st);
        st = Call(Verb::kPrefill, static_cast<uint64_t>(n), h, [&] {
          return SslmGpuSeqPrefillPromptForG5Bridge(enc->ctx, seq, w.ids.data(), static_cast<int32_t>(n),
                                                    kDispatchBudget);
        });
        if (st == SslmGpuStatus::SSLM_SEQUENCE_REJECTED) SetLastDiverged(static_cast<int32_t>(i));
        if (st != SslmGpuStatus::SSLM_OK) return MapCheckStatus(st);
        size_t required = 0;
        st = Call(Verb::kRead, 0, h, [&] {
          return sslm_gpu_seq_read_prefill_final_hidden(enc->ctx, seq, gpu.codes.data(), gpu.codes.size(),
                                                        &required, &gpu.m, &gpu.e);
        });
        if (st == SslmGpuStatus::SSLM_SEQUENCE_REJECTED) SetLastDiverged(static_cast<int32_t>(i));
        if (st != SslmGpuStatus::SSLM_OK) return MapCheckStatus(st);
        if (required != width) {
          SetLastDiverged(static_cast<int32_t>(i));
          return SEMB_DEVICE_DIVERGED;
        }
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
        RecordLastRead(gpu);
        ApplyPerturbation(0, static_cast<int32_t>(i), &gpu);
#endif
        if (gpu.m != w.cpu.m || gpu.e != w.cpu.e ||
            std::memcmp(gpu.codes.data(), w.cpu.codes.data(), width) != 0) {
          SetLastDiverged(static_cast<int32_t>(i));
          return SEMB_DEVICE_DIVERGED;
        }
      }
      if (rollback.check_seq) {
        SslmGpuSequenceHandle* seq = rollback.check_seq;
        st = Call(Verb::kSeqRelease, 0, h, [&] { return sslm_gpu_seq_release(enc->ctx, seq); });
        if (st != SslmGpuStatus::SSLM_OK) return MapCheckStatus(st);
        rollback.check_seq = nullptr;
      }
      return SEMB_OK;
    }();
    if (result == SEMB_OK) rollback.Disarm();
  }
  if (result != SEMB_OK) return result;

  // The labels read DXGI, not the engine, and never refuse the open (P:4212,
  // PD-1). The destination holds the complete fallback set before the probe
  // runs; the probe's set replaces it through a no-throw swap only after the
  // whole read returned, so an exception anywhere in it leaves the fallback
  // set, never a partial one.
  if (labels) {
    SetFallbackLabels(index, labels);
    try {
      AdapterLabels probed = ReadAdapterLabels(index);
      using std::swap;
      swap(labels->adapter_index, probed.adapter_index);
      swap(labels->vendor_id, probed.vendor_id);
      swap(labels->device_id, probed.device_id);
      swap(labels->adapter_name, probed.adapter_name);
      swap(labels->driver_version, probed.driver_version);
    } catch (...) {
    }
  }
  *out = enc.release();
  return SEMB_OK;
}

semb_status EncodeSequences(Encoder* enc, const int32_t* const* ids, const size_t* lens, size_t n,
                            std::vector<Triple>* out) {
  if (!enc || !out || (n > 0 && (!ids || !lens))) return SEMB_NULL_ARGUMENT;
  if (enc->poisoned.load()) return SEMB_DEVICE_UNAVAILABLE;

  // A batch runs on one worker, on one slot (sec 4.2, "Slots"): the lowest
  // free record, or a new one while fewer than the bound exist.
  Slot* slot = nullptr;
  {
    std::lock_guard<std::mutex> lock(enc->slots_mutex);
    for (auto& s : enc->slots) {
      if (!s->busy) {
        slot = s.get();
        break;
      }
    }
    if (!slot) {
      if (enc->slots.size() >= enc->bound) return SEMB_CONCURRENCY_EXCEEDED;
      enc->slots.push_back(std::make_unique<Slot>());
      slot = enc->slots.back().get();
    }
    slot->busy = true;
  }
  struct SlotLease {
    Encoder* enc;
    Slot* slot;
    ~SlotLease() {
      std::lock_guard<std::mutex> lock(enc->slots_mutex);
      slot->busy = false;
    }
  } lease{enc, slot};

  std::vector<Triple> triples(n);
  for (auto& t : triples) t.codes.resize(enc->hidden);
  for (size_t i = 0; i < n; ++i) {
    const semb_status st = EncodeOne(enc, slot, ids[i], lens[i], &triples[i]);
    if (st != SEMB_OK) return st;
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
    ApplyPerturbation(1, -1, &triples[i]);
    RecordLastRead(triples[i]);
#endif
  }
  *out = std::move(triples);
  return SEMB_OK;
}

void Close(Encoder* enc) noexcept {
  if (!enc) return;
  {
    SubmissionLock lock;
    const uint64_t h = enc->public_handle;
    for (auto& slot : enc->slots) {
      if (!slot->seq) continue;
      SslmGpuSequenceHandle* seq = slot->seq;
      Call(Verb::kSeqRelease, 0, h, [&] { return sslm_gpu_seq_release(enc->ctx, seq); });
      slot->seq = nullptr;
    }
    if (enc->model) {
      SslmGpuModelHandle* model = enc->model;
      Call(Verb::kModelUnmap, 0, h, [&] { return sslm_gpu_model_unmap(enc->ctx, model); });
      enc->model = nullptr;
    }
    if (enc->ctx) {
      SslmGpuContext* ctx = enc->ctx;
      Call(Verb::kContextDestroy, 0, h, [&] { return sslm_gpu_context_destroy(ctx); });
      enc->ctx = nullptr;
    }
  }
  delete enc;
}

#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
uint64_t SlotRecordCount(const Encoder* enc) noexcept {
  if (!enc) return 0;
  std::lock_guard<std::mutex> lock(const_cast<Encoder*>(enc)->slots_mutex);
  return enc->slots.size();
}

uint64_t SlotBound(const Encoder* enc) noexcept { return enc ? enc->bound : 0; }
#endif

}  // namespace semb::gpu

#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
// ---------------------------------------------------------------------------
// tests/support/semb_u3_gpu_seam.h. Test-only; absent from the shipped
// library (C14-U3a, K-18).
// ---------------------------------------------------------------------------
using semb::gpu::S;

extern "C" uint64_t semb_test_seam_gpu_call_count(void) { return S().call_count.load(); }

extern "C" uint64_t semb_test_seam_gpu_call_log(semb_test_seam_gpu_call* out, uint64_t first, uint64_t cap) {
  auto& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  const uint64_t total = s.log.size();
  if (out) {
    for (uint64_t i = 0; i < cap && first + i < total; ++i) out[i] = s.log[first + i];
  }
  return total;
}

extern "C" int32_t semb_test_seam_gpu_call_log_reset(void) {
  auto& s = S();
  if (s.occupancy_now.load() != 0) return 0;
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.occupancy_now.load() != 0) return 0;
  s.log.clear();
  s.call_count.store(0);
  s.lock_violations.store(0);
  return 1;
}

extern "C" int32_t semb_test_seam_gpu_inject(const char* verb, uint64_t nth, const char* status_name) {
  semb::gpu::Verb v;
  SslmGpuStatus st;
  if (nth == 0 || !semb::gpu::VerbByName(verb, &v) || !semb::gpu::StatusByName(status_name, &st)) return 0;
  auto& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.arms.push_back({v, s.verb_calls[static_cast<size_t>(v)] + nth, st});
  return 1;
}

extern "C" uint64_t semb_test_seam_gpu_inject_clear(void) {
  auto& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  const uint64_t pending = s.arms.size();
  s.arms.clear();
  return pending;
}

extern "C" uint64_t semb_test_seam_gpu_lock_violations(void) { return S().lock_violations.load(); }

extern "C" uint32_t semb_test_seam_gpu_occupancy_max(void) { return S().occupancy_max.load(); }

extern "C" void semb_test_seam_gpu_occupancy_reset(void) { S().occupancy_max.store(S().occupancy_now.load()); }

extern "C" uint32_t semb_test_seam_gpu_occupancy_now(void) { return S().occupancy_now.load(); }

extern "C" int64_t semb_test_seam_gpu_context_count(void) { return S().context_count.load(); }

extern "C" uint64_t semb_test_seam_gpu_slot_record_count(const void* enc) {
  return semb::gpu::SlotRecordCount(semb::test_seam::GpuEncoderOf(enc));
}

extern "C" uint64_t semb_test_seam_gpu_slot_bound(const void* enc) {
  return semb::gpu::SlotBound(semb::test_seam::GpuEncoderOf(enc));
}

extern "C" int32_t semb_test_seam_gpu_try_lock(void) {
  if (semb::gpu::t_holds_submission_lock) return 0;
  if (!semb::gpu::SubmissionMutex().try_lock()) return 0;
  semb::gpu::SubmissionMutex().unlock();
  return 1;
}

extern "C" void semb_test_seam_gpu_arm_perturb(int32_t where, int32_t target, int32_t sequence,
                                               uint32_t code_index) {
  auto& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.perturb_armed = true;
  s.perturb_fired = false;
  s.perturb_where = where;
  s.perturb_target = target;
  s.perturb_sequence = sequence;
  s.perturb_code_index = code_index;
}

extern "C" int32_t semb_test_seam_gpu_perturb_disarm(void) {
  auto& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  const int32_t fired = s.perturb_fired ? 1 : 0;
  s.perturb_armed = false;
  s.perturb_fired = false;
  return fired;
}

extern "C" int32_t semb_test_seam_gpu_last_diverged_sequence(void) { return S().last_diverged.load(); }

extern "C" int32_t semb_test_seam_gpu_last_read(int8_t* out_codes, size_t capacity, size_t* out_width,
                                               int64_t* out_m, int64_t* out_e) {
  auto& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.have_last_read) return 0;
  if (out_width) *out_width = s.last_read_codes.size();
  if (out_codes) std::memcpy(out_codes, s.last_read_codes.data(), std::min(capacity, s.last_read_codes.size()));
  if (out_m) *out_m = s.last_read_m;
  if (out_e) *out_e = s.last_read_e;
  return 1;
}
#endif  // SEMB_ENABLE_WORKSPACE_TEST_SEAM
