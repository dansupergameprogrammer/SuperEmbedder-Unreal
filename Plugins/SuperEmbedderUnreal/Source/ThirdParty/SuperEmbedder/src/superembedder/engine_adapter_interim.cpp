// src/superembedder/engine_adapter_interim.cpp -- THE interim adapter, and
// the only one (`Claude/Plans/SuperEmbedder_Plan.md` sec 4.3: "the skeleton
// composes the forward from SuperSLM's installed C++ headers behind
// SuperEmbedder's own engine-adapter seam, calling EmbedEntry +
// RunLayerLoopChunkBatched"). This is the ONE translation unit in this
// library that names EmbedEntry, RunLayerLoopChunkBatched, RunLayerLoop,
// MarshalLayer or SslmModel::Load -- `tests/c39_singleton_check.py` is the
// structural check that enforces this (sec 8, C39/C31 clause 3: W-A's
// drivers and this adapter are compiled INTO the library; the engine's
// `inline` primitives must be reached through the library's own
// translation unit, never re-compiled inside a sibling TU).
//
// Ported from `Claude/Vitruvius/t2217-fold4-probe/cost/semb_encoder_interim.cpp`
// (TE-63, U0 slice B) under product discipline; see that round's build log
// for the port's own dispositions. U1 slice 1 (C12b first) adds, in this
// same TU:
//
//   * the interim handle surface (include/superembedder/encoder_interim_abi.h): the artifact
//     read ONCE at open (the engine's loader copies it into the model view's
//     own backing, and this adapter's copy is then released), every layer AND
//     the final norm marshaled once, each encode's K/V workspace slot sized to
//     its own document (U2-9), and many documents encoded against that one handle -- the
//     shape a bake over 239 documents needs and the one-shot witness verb
//     below (which re-opens the 633 MB artifact per call) cannot supply;
//   * `q_width` threaded EXPLICITLY to both forward entry points
//     (`num_attention_heads * head_dim`), the way `sslm_prefill` itself does.
//     The U0 port relied on the parameter's default (0 = "derive as
//     hidden_size"), which is bit-identical for every square incumbent and
//     WRONG for the ratified candidate (hidden_size 1024, heads*head_dim
//     2048, sec 4.2 Track A) -- exactly the "silent-undercount defect" the
//     engine's own header names for this call site, and exactly C12b's
//     "the port is wrong" attribution, found by reading the production
//     caller before the first real-artifact run rather than by a red digest
//     after it. The witness verb inherits the fix; U0's synthetic fixture is
//     square, so its committed anchors are unaffected by construction (and
//     C39 grades that by execution);
//   * PS-N's selection (sec 3.2): the engine's own final norm applied to
//     the ONE selected position, through the same `RmsNormSite` under the
//     same "final_norm" site name and site constant the engine's own
//     generation loop uses;
//   * the per-token leg (the engine's single-token `RunLayerLoop`, one
//     position at a time against the same K/V store), so C12b's third
//     attribution -- "the entry-point substitution is not bit-identical after
//     all" -- is decided by running both entry points on the same document in
//     the same process, per row, never inherited from the header's claim.
#include "superembedder/encoder_interim_abi.h"
#include "superembedder/seam_abi.h"  // semb_seam_serialize_document (TE-159 C-2's seam tap)
#include "encoder_open_internal.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "c_boundary.h"

#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
#include "support/semb_workspace_seam.h"
#include "support/semb_u2_memory_seam.h"
#include <atomic>
#include <system_error>
#include "superslm/forward_sites.h"
#include <chrono>
#include <condition_variable>

namespace semb::test_seam {
// U3 (te266 sec 4.6 row 11, T388-11): calls to the C verb
// semb_interim_ps_n_select, every caller. A CPU open makes none; a GPU open
// makes one per W-B document for the agreement check's CPU triples.
static std::atomic<uint64_t> ps_n_select_calls{0};
static std::mutex state_mutex;
static std::condition_variable state_changed;
static uint32_t park_budget = 0;
static uint32_t parked = 0;
static uint64_t release_generation = 0;
static std::atomic<uint64_t> acquisitions{0};

static void OnAcquired() {
  acquisitions.fetch_add(1);
  std::unique_lock<std::mutex> lock(state_mutex);
  if (park_budget == 0) return;
  --park_budget;
  ++parked;
  const uint64_t generation = release_generation;
  state_changed.notify_all();
  state_changed.wait(lock, [generation] { return release_generation != generation; });
  --parked;
}

// TE-184 (D-SLM6434 item 2, TE-161 N-5, TE-149 sec 7 bare cell 2): a
// SECOND, independent park point at per-DOCUMENT granularity, distinct
// from OnAcquired's once-per-ACQUISITION park above. The batch verb
// (semb_interim_encode_batch_ps_n, below) acquires at most T workspace
// slots for the whole call (TE-350: T <= min(doc_count, max_concurrency, hardware
// concurrency) -- an upper bound, not an equality; contention with another
// in-flight call can leave fewer free (TE-356) -- one worker thread per
// acquired slot) and each worker holds
// its own slot across every document that worker processes; OnAcquired
// fires once per slot, at that WORKER's entry (TE-356: every slot is
// actually acquired earlier, on the calling thread, inside WorkspacePool's
// own constructor, before any worker exists -- OnAcquired's firing point is
// a worker starting to use an already-acquired slot, not the acquisition
// itself), so no existing construction can observe a slot BETWEEN two documents processed by the
// SAME worker on that slot -- only between two separate acquisitions
// (which, at T == 1, is exactly "between two separate calls", the original
// TE-184 property, unchanged). This hook, called once per document inside
// a worker's own per-document loop -- possibly by more than one worker
// thread concurrently when T > 1 -- lets a test park a calling thread
// mid-call, still holding that thread's own slot, so a contender's own
// attempt to acquire a DIFFERENT slot at that finer granularity is
// observable: a defect that released and reacquired mid-row would be
// invisible to OnAcquired's own once-per-acquisition crossing alone.
// Shares state_mutex/state_changed with OnAcquired above (one lock
// protects both counters, safe under concurrent callers); its own
// budget/counter/generation are independent so arming one hook never arms
// the other.
static uint32_t row_park_budget = 0;
static uint32_t row_parked = 0;
static uint64_t row_release_generation = 0;
static std::atomic<uint64_t> row_acquisitions{0};

static void OnRowAcquired() {
  row_acquisitions.fetch_add(1);
  std::unique_lock<std::mutex> lock(state_mutex);
  if (row_park_budget == 0) return;
  --row_park_budget;
  ++row_parked;
  const uint64_t generation = row_release_generation;
  state_changed.notify_all();
  state_changed.wait(lock, [generation] { return row_release_generation != generation; });
  --row_parked;
}

// ---------------------------------------------------------------------------
// U2 cloud build, the seam commit (te267 sec 6.1 step 1): U2-9 Builds item 9's
// readers and arms, and U2-10 Builds item 2c's spawn fail arm (C66). Test-only,
// under this guard, so none of it reaches the shipped library. The C surface is
// tests/support/semb_u2_memory_seam.h; the C++ hooks below are what the builder
// calls from the U2-9/U2-10 build (that header's comment lists each one). On
// today's pool nothing calls the growth, fill, forced-reference or gate hooks.
// ---------------------------------------------------------------------------
static std::atomic<uint64_t> slot_growths{0};
static std::atomic<int32_t> fill_byte{-1};
static std::atomic<int32_t> forced_reference{0};

static std::mutex growth_fail_mutex;
static uint64_t growth_fail_min_c = 0;
static bool growth_fail_fired = false;
static uint64_t growth_fail_capacity_at_fire = 0;

// The builder's growth site calls this inside the new buffer's allocation
// step, before the allocation: `c` is the new C, `current_capacity` the slot
// buffer's capacity at that moment. Throws std::bad_alloc when the fail arm
// is armed and c >= its value (one shot), recording the capacity.
[[maybe_unused]] static void BeforeSlotGrowth(uint64_t c, uint64_t current_capacity) {
  std::lock_guard<std::mutex> lock(growth_fail_mutex);
  if (growth_fail_min_c != 0 && c >= growth_fail_min_c) {
    growth_fail_min_c = 0;
    growth_fail_fired = true;
    growth_fail_capacity_at_fire = current_capacity;
    throw std::bad_alloc();
  }
}

// After a successful growth: counts it, and applies the fill arm.
[[maybe_unused]] static void AfterSlotGrowth(uint8_t* data, size_t bytes) {
  slot_growths.fetch_add(1);
  const int32_t fb = fill_byte.load();
  if (fb >= 0 && data && bytes) std::memset(data, fb, bytes);
}

[[maybe_unused]] static bool ForcedReferenceArmed() { return forced_reference.load() != 0; }

// The long-document gate's hooks (U2-9 item 6).
static uint32_t gate_park_budget = 0;
static uint32_t gate_parked = 0;
static uint64_t gate_release_generation = 0;
static uint32_t gate_holders = 0;
static uint32_t gate_high_water = 0;
static uint32_t gate_waiters = 0;
static std::vector<uint64_t> gate_take_records;
static std::vector<uint64_t> gate_give_records;

[[maybe_unused]] static void OnGateWaitBegin() {
  std::lock_guard<std::mutex> lock(state_mutex);
  ++gate_waiters;
  state_changed.notify_all();
}
[[maybe_unused]] static void OnGateWaitEnd() {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (gate_waiters) --gate_waiters;
  state_changed.notify_all();
}
[[maybe_unused]] static void OnGateTaken(uint64_t capacity) {
  std::lock_guard<std::mutex> lock(state_mutex);
  gate_take_records.push_back(capacity);
  ++gate_holders;
  if (gate_holders > gate_high_water) gate_high_water = gate_holders;
  state_changed.notify_all();
}
// The park point: after the holder's buffer has grown, before its forward.
[[maybe_unused]] static void OnGateHolding() {
  std::unique_lock<std::mutex> lock(state_mutex);
  if (gate_park_budget == 0) return;
  --gate_park_budget;
  ++gate_parked;
  const uint64_t generation = gate_release_generation;
  state_changed.notify_all();
  state_changed.wait(lock, [generation] { return gate_release_generation != generation; });
}
[[maybe_unused]] static void OnGateGiveBack(uint64_t capacity) {
  std::lock_guard<std::mutex> lock(state_mutex);
  gate_give_records.push_back(capacity);
  if (gate_holders) --gate_holders;
  state_changed.notify_all();
}

// The poison arm (C60's must-reject and must-accept).
static std::mutex poison_mutex;
static bool poison_armed = false;
static bool poison_fired = false;
static uint64_t poison_split = 0;
static uint64_t poison_begin = 0;
static uint64_t poison_end = 0;
static uint8_t poison_byte = 0;

// Called by semb::Encode at the start of each chunk-batched call (at = its
// offset) and before each per-token position (at = the position).
// `context_cap` must be the cap the forward itself receives.
[[maybe_unused]] static void PoisonAt(uint64_t at, uint8_t* workspace, uint32_t num_layers,
                                      int64_t context_cap, size_t num_kv_heads, size_t head_dim) {
  std::lock_guard<std::mutex> lock(poison_mutex);
  if (!poison_armed || at != poison_split) return;
  poison_armed = false;
  poison_fired = true;
  const uint64_t end = std::min<uint64_t>(poison_end, static_cast<uint64_t>(context_cap));
  for (uint32_t layer = 0; layer < num_layers; ++layer) {
    for (size_t kvh = 0; kvh < num_kv_heads; ++kvh) {
      for (uint64_t pos = poison_begin; pos < end; ++pos) {
        int8_t* row = superslm::MutableKeyRow(workspace, layer, context_cap, num_kv_heads, head_dim,
                                              kvh, static_cast<int64_t>(pos));
        std::memset(row, poison_byte, head_dim);
      }
    }
  }
}

// C66's spawn fail arm (U2-10 Builds item 2c): called in place of each
// worker thread's construction with a token unique to the batch call
// (NextBatchCallToken(), taken once per batch call).
static std::atomic<uint64_t> batch_call_tokens{0};
[[maybe_unused]] static uint64_t NextBatchCallToken() { return ++batch_call_tokens; }
static std::mutex spawn_mutex;
static uint32_t spawn_fail_k = 0;
static uint32_t spawn_seen = 0;
static bool spawn_fired = false;
static uint64_t spawn_fired_call = 0;
static uint32_t spawn_starts_after = 0;
static uint64_t spawn_current_call = 0;

[[maybe_unused]] static void OnWorkerStart(uint64_t call_token) {
  std::lock_guard<std::mutex> lock(spawn_mutex);
  if (spawn_fired) {
    if (call_token == spawn_fired_call) ++spawn_starts_after;
    return;
  }
  if (spawn_fail_k == 0) return;
  if (call_token != spawn_current_call) {
    spawn_current_call = call_token;
    spawn_seen = 0;
  }
  ++spawn_seen;
  if (spawn_seen == spawn_fail_k) {
    spawn_fail_k = 0;
    spawn_fired = true;
    spawn_fired_call = call_token;
    spawn_starts_after = 0;
    throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
  }
}
}  // namespace semb::test_seam

extern "C" int32_t semb_test_seam_arm_park(uint32_t count) {
  std::lock_guard<std::mutex> lock(semb::test_seam::state_mutex);
  if (semb::test_seam::parked != 0) return 0;
  semb::test_seam::park_budget = count;
  return 1;
}

extern "C" uint32_t semb_test_seam_parked(void) {
  std::lock_guard<std::mutex> lock(semb::test_seam::state_mutex);
  return semb::test_seam::parked;
}

extern "C" int32_t semb_test_seam_wait_parked(uint32_t n, uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(semb::test_seam::state_mutex);
  return semb::test_seam::state_changed.wait_for(
      lock, std::chrono::milliseconds(timeout_ms),
      [n] { return semb::test_seam::parked >= n; }) ? 1 : 0;
}

extern "C" void semb_test_seam_release(void) {
  std::lock_guard<std::mutex> lock(semb::test_seam::state_mutex);
  ++semb::test_seam::release_generation;
  semb::test_seam::park_budget = 0;
  semb::test_seam::state_changed.notify_all();
}

extern "C" uint64_t semb_test_seam_acquisitions(void) {
  return semb::test_seam::acquisitions.load();
}

extern "C" int32_t semb_test_seam_reset(void) {
  // The caller joins every worker/helper before resetting the counter epoch.
  std::lock_guard<std::mutex> lock(semb::test_seam::state_mutex);
  if (semb::test_seam::parked != 0) return 0;
  semb::test_seam::park_budget = 0;
  semb::test_seam::acquisitions.store(0);
  ++semb::test_seam::release_generation;
  return 1;
}

// TE-184 (D-SLM6434 item 2): the per-document park point's own surface, in
// the identical shape as the once-per-call surface above -- arm/parked/
// wait_parked/release/acquisitions/reset -- but naming the "row" hook
// (OnRowAcquired) each function reaches, so a test that wants call
// granularity and one that wants row granularity can arm each
// independently without one silently arming the other.
extern "C" int32_t semb_test_seam_row_arm_park(uint32_t count) {
  std::lock_guard<std::mutex> lock(semb::test_seam::state_mutex);
  if (semb::test_seam::row_parked != 0) return 0;
  semb::test_seam::row_park_budget = count;
  return 1;
}

extern "C" uint32_t semb_test_seam_row_parked(void) {
  std::lock_guard<std::mutex> lock(semb::test_seam::state_mutex);
  return semb::test_seam::row_parked;
}

extern "C" int32_t semb_test_seam_row_wait_parked(uint32_t n, uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(semb::test_seam::state_mutex);
  return semb::test_seam::state_changed.wait_for(
      lock, std::chrono::milliseconds(timeout_ms),
      [n] { return semb::test_seam::row_parked >= n; }) ? 1 : 0;
}

extern "C" void semb_test_seam_row_release(void) {
  std::lock_guard<std::mutex> lock(semb::test_seam::state_mutex);
  ++semb::test_seam::row_release_generation;
  semb::test_seam::row_park_budget = 0;
  semb::test_seam::state_changed.notify_all();
}

extern "C" uint64_t semb_test_seam_row_acquisitions(void) {
  return semb::test_seam::row_acquisitions.load();
}

extern "C" int32_t semb_test_seam_row_reset(void) {
  // The caller joins every worker/helper before resetting the counter epoch.
  std::lock_guard<std::mutex> lock(semb::test_seam::state_mutex);
  if (semb::test_seam::row_parked != 0) return 0;
  semb::test_seam::row_park_budget = 0;
  semb::test_seam::row_acquisitions.store(0);
  ++semb::test_seam::row_release_generation;
  return 1;
}
#endif

#include "superslm/forward_sites.h"
#include "superslm/layer_marshal.h"
#include "superslm/model.h"

#include "superembedder/manifest_labels_abi.h"

using namespace superslm;
using superslm_marshal::LayerBacking;
using superslm_marshal::MarshalLayer;
using superslm_marshal::PreflightScanWscFolds;
using superslm_marshal::ReadCarriedScale;
using superslm_marshal::ReadFile;
using superslm_marshal::WidenGainToInt32;

namespace semb {
namespace {

// The engine's own status NAME for the most recent refusal on this thread,
// returned through every verb's `out_engine_status_name` (sec 8 C4: the
// name crosses the seam, never the enum). A marshal refusal carries the
// marshal's own diagnostic string instead, since MarshalLayer reports
// through a std::string rather than a status.
thread_local std::string t_last_engine_status;

const char* SetEngineStatus(const std::string& s) {
  t_last_engine_status = s;
  return t_last_engine_status.c_str();
}

// One K/V workspace slot record (U2-9 items 2-5). Records are created on
// acquisition, up to the handle's `max_concurrency`, and never removed; their
// addresses are stable (each is its own allocation). The buffer is sized to
// the document an encode runs: default-initialized storage, never filled, so
// only the rows a forward writes become resident. `capacity` is the buffer's
// size in bytes; it and `buffer` change only under the handle's
// workspace_mutex, and only the slot's holder (or, with no holder, the
// end-of-open release) changes them.
struct WorkspaceSlot {
  std::unique_ptr<uint8_t[]> buffer;
  size_t capacity = 0;
  bool in_use = false;
};

// U2-9 item 6: at most one encode of a document longer than
// kLongDocumentPositions runs on a handle at a time. A holder count of 0 or 1.
struct LongDocumentGate {
  std::mutex mutex;
  std::condition_variable released;
  uint32_t holders = 0;
};

// The slot sizing rule (U2-9 item 3): C = min(max(64, the next power of two
// >= n), context_cap). A slot whose C exceeds kLongDocumentPositions takes the
// long-document gate and is released after its encode (items 5 and 6).
constexpr size_t kMinSlotPositions = 64;
constexpr size_t kLongDocumentPositions = 4096;

struct EncoderModel {
  // The artifact's bytes, held only until SslmModel::Load returns: the
  // engine's loader copies them into the view's own backing, so
  // FinishEncoderOpen releases this copy before any marshal step (U2-9
  // item 1).
  std::vector<uint8_t> model_bytes;
  SslmModelView view;
  std::vector<LayerBacking> backings;
  std::vector<LayerWeights> layers;
  const int8_t* embed_weights = nullptr;
  CarriedScale embed_site_constant{};
  std::vector<int32_t> final_norm_gain;
  CarriedScale final_norm_site_constant{};
  std::mutex workspace_mutex;
  std::vector<std::unique_ptr<WorkspaceSlot>> workspaces;
  // The bound on slot records (and so on encodes running at once).
  uint32_t max_concurrency = 1;
  // P: num_hidden_layers * num_key_value_heads * head_dim * 2, the K/V bytes
  // one position needs; a slot of C positions needs P * C bytes.
  size_t kv_bytes_per_position = 0;
  LongDocumentGate long_document_gate;
  size_t hidden_size = 0;
  uint32_t num_layers = 0;
  size_t q_width = 0;
  int64_t context_cap = 0;
  bool option_g_fused = false;
  int32_t appended_token_id = -1;
};

// Finishes an open after the artifact's exactly-once read has transferred its
// backing into `m`. The engine's loader copies those bytes into the model
// view's own backing, so `m`'s copy is released as soon as the load succeeds.
EncoderModel* FinishEncoderOpen(std::unique_ptr<EncoderModel> m, uint32_t max_concurrency,
                                const char** engine_status_name,
                                uint32_t tokenizer_vocab_size = 0,
                                bool* vocab_mismatch = nullptr) {
  std::string model_err;
  const SslmModelStatus ms =
      SslmModel::Load(m->model_bytes.data(), m->model_bytes.size(), m->view, &model_err);
  if (ms != SslmModelStatus::Ok) {
    if (engine_status_name) {
      *engine_status_name =
          SetEngineStatus(std::string(SslmModelStatusName(ms)) + ": " + model_err);
    }
    return nullptr;
  }
  // U2-9 item 1: the marshal borrows weights from the engine's copy, never
  // from this one.
  std::vector<uint8_t>().swap(m->model_bytes);
  const uint32_t num_heads = m->view.config.num_attention_heads;
  const uint32_t num_kv_heads = m->view.config.num_key_value_heads;
  m->num_layers = m->view.config.num_hidden_layers;
  m->hidden_size = m->view.config.hidden_size;
  m->q_width = static_cast<size_t>(num_heads) * m->view.config.head_dim;
  m->context_cap = static_cast<int64_t>(m->view.config.context_cap);
  m->option_g_fused = m->view.option_g_fused_k_landing;

  PreflightScanWscFolds(m->view);
  m->backings.resize(m->num_layers);
  m->layers.resize(m->num_layers);
  for (uint32_t l = 0; l < m->num_layers; ++l) {
    std::string merr;
    if (!MarshalLayer(m->view, l, num_heads, num_kv_heads, m->backings[l], m->layers[l], &merr)) {
      if (engine_status_name) *engine_status_name = SetEngineStatus("MarshalLayer: " + merr);
      return nullptr;
    }
  }
  const SslmTensorView* embed_w = m->view.weights.Tensor("embed");
  const SslmTensorView* final_gain_w = m->view.weights.Tensor("final_norm.gain");
  if (!embed_w || !final_gain_w) {
    if (engine_status_name) *engine_status_name = SetEngineStatus("MissingEmbedOrFinalNormTensor");
    return nullptr;
  }
  m->embed_weights = reinterpret_cast<const int8_t*>(embed_w->data);
  m->final_norm_gain = WidenGainToInt32(*final_gain_w);
  if (m->final_norm_gain.size() != m->hidden_size) {
    if (engine_status_name) *engine_status_name = SetEngineStatus("FinalNormGainWidthMismatch");
    return nullptr;
  }
  bool ok = true;
  m->embed_site_constant = ReadCarriedScale(m->view.composition_constants, "embed", &ok);
  if (!ok) {
    if (engine_status_name) *engine_status_name = SetEngineStatus("MissingEmbedSiteConstant");
    return nullptr;
  }
  m->final_norm_site_constant = ReadCarriedScale(m->view.composition_constants, "final_norm", &ok);
  if (!ok) {
    if (engine_status_name) *engine_status_name = SetEngineStatus("MissingFinalNormSiteConstant");
    return nullptr;
  }
  // The raw-text join follows all engine validation. Interim token-id
  // callers have no tokenizer vocabulary to join.
  if (tokenizer_vocab_size > m->view.config.vocab_size) {
    if (vocab_mismatch) *vocab_mismatch = true;
    return nullptr;
  }
  // P, the forward's own workspace requirement per position (forward_sites.h),
  // identical for both entry points: num_hidden_layers * num_kv_heads *
  // head_dim * 2. P * context_cap is checked here without a wrapping
  // intermediate, so P * C cannot wrap for any C <= context_cap (U2-9 item 3).
  // Open reserves nothing per unit of max_concurrency (U2-9 item 2): slot
  // records are created on acquisition, and each buffer on its first encode.
  size_t per_position = static_cast<size_t>(m->num_layers);
  const size_t factors[] = {static_cast<size_t>(num_kv_heads),
                            static_cast<size_t>(m->view.config.head_dim), size_t{2}};
  for (const size_t factor : factors) {
    if (factor != 0 && per_position > std::numeric_limits<size_t>::max() / factor) {
      if (engine_status_name) *engine_status_name = SetEngineStatus("WorkspaceSizeOverflow");
      return nullptr;
    }
    per_position *= factor;
  }
  const size_t cap = static_cast<size_t>(m->context_cap);
  if (cap != 0 && per_position > std::numeric_limits<size_t>::max() / cap) {
    if (engine_status_name) *engine_status_name = SetEngineStatus("WorkspaceSizeOverflow");
    return nullptr;
  }
  m->kv_bytes_per_position = per_position;
  m->max_concurrency = max_concurrency;
  return m.release();
}

// Opens the artifact and marshals every layer plus the final norm. Returns
// nullptr on any failure with the engine's own status name (or the marshal's
// diagnostic) in `*engine_status_name`.
EncoderModel* EncoderOpenFromMemory(const uint8_t* model_bytes, size_t model_size,
                                    uint32_t max_concurrency,
                                    const char** engine_status_name) {
  if (!model_bytes || model_size == 0 || max_concurrency == 0) {
    if (engine_status_name) *engine_status_name = SetEngineStatus("InvalidOpenInput");
    return nullptr;
  }
  auto m = std::make_unique<EncoderModel>();
  m->model_bytes.assign(model_bytes, model_bytes + model_size);
  return FinishEncoderOpen(std::move(m), max_concurrency, engine_status_name);
}

EncoderModel* EncoderOpenOwned(std::vector<uint8_t>&& model_bytes,
                               uint32_t max_concurrency,
                               const char** engine_status_name,
                               uint32_t tokenizer_vocab_size,
                               bool* vocab_mismatch) {
  if (model_bytes.empty() || max_concurrency == 0) {
    if (engine_status_name) *engine_status_name = SetEngineStatus("InvalidOpenInput");
    return nullptr;
  }
  auto m = std::make_unique<EncoderModel>();
  m->model_bytes = std::move(model_bytes);
  return FinishEncoderOpen(std::move(m), max_concurrency, engine_status_name,
                           tokenizer_vocab_size, vocab_mismatch);
}

EncoderModel* EncoderOpen(const std::string& model_path, const char** engine_status_name) {
  auto m = std::make_unique<EncoderModel>();
  if (!ReadFile(model_path.c_str(), m->model_bytes)) {
    if (engine_status_name) *engine_status_name = SetEngineStatus("ReadFileFailed");
    return nullptr;
  }
  return FinishEncoderOpen(std::move(m), 1, engine_status_name);
}

void EncoderClose(EncoderModel* m) { delete m; }

// Releases a slot's buffer to capacity 0 (U2-9 items 2, 3 and 5). The old
// storage is freed after the mutex is dropped.
void ReleaseSlotBuffer(EncoderModel* m, WorkspaceSlot* slot) {
  std::unique_ptr<uint8_t[]> old;
  std::lock_guard<std::mutex> lock(m->workspace_mutex);
  old.swap(slot->buffer);
  slot->capacity = 0;
}

// Grows a slot to `bytes` for a document of `c` positions (U2-9 item 3). The
// old buffer is released FIRST, then the new one is allocated, with no copy
// and no fill, so a growth never holds two buffers. A failed allocation
// (std::bad_alloc) leaves the slot empty and propagates (U2-9 item 7).
void GrowSlot(EncoderModel* m, WorkspaceSlot* slot, size_t c, size_t bytes) {
  ReleaseSlotBuffer(m, slot);
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
  test_seam::BeforeSlotGrowth(c, slot->capacity);
#else
  (void)c;
#endif
  std::unique_ptr<uint8_t[]> fresh(new uint8_t[bytes]);  // default-initialized
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
  test_seam::AfterSlotGrowth(fresh.get(), bytes);
#endif
  std::lock_guard<std::mutex> lock(m->workspace_mutex);
  slot->buffer.swap(fresh);
  slot->capacity = bytes;
}

// U2-9 items 5 and 6, by RAII: for a document whose C exceeds
// kLongDocumentPositions, takes the handle's long-document gate before the
// slot grows, and on every exit releases the slot's buffer to capacity 0 and
// then gives the gate back. Never held across documents.
class LongDocumentScope {
 public:
  LongDocumentScope(EncoderModel* m, WorkspaceSlot* slot, bool long_document)
      : model_(m), slot_(slot), held_(long_document) {
    if (!held_) return;
    LongDocumentGate& gate = model_->long_document_gate;
    std::unique_lock<std::mutex> lock(gate.mutex);
    if (gate.holders != 0) {
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
      test_seam::OnGateWaitBegin();
#endif
      gate.released.wait(lock, [&gate] { return gate.holders == 0; });
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
      test_seam::OnGateWaitEnd();
#endif
    }
    gate.holders = 1;
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
    test_seam::OnGateTaken(slot_->capacity);
#endif
  }
  ~LongDocumentScope() {
    if (!held_) return;
    ReleaseSlotBuffer(model_, slot_);
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
    test_seam::OnGateGiveBack(slot_->capacity);
#endif
    LongDocumentGate& gate = model_->long_document_gate;
    {
      std::lock_guard<std::mutex> lock(gate.mutex);
      gate.holders = 0;
    }
    gate.released.notify_one();
  }
  LongDocumentScope(const LongDocumentScope&) = delete;
  LongDocumentScope& operator=(const LongDocumentScope&) = delete;
  bool held() const { return held_; }

 private:
  EncoderModel* model_;
  WorkspaceSlot* slot_;
  bool held_;
};

// The slot's C for a document of `n` positions (U2-9 item 3).
size_t SlotPositions(size_t n, size_t context_cap) {
  size_t c = kMinSlotPositions;
  while (c < n && c <= std::numeric_limits<size_t>::max() / 2) c <<= 1;
  if (c < n) c = n;
  return std::min(c, context_cap);
}

// The seam: embed every token, then run the forward down `leg`. On the
// chunk-batched leg `chunk_budget` (0 or >= n resolves to one whole-document
// call) bounds each call's span; on the per-token leg every position runs
// through the engine's single-token loop in order against the same K/V
// store, exactly as the engine's own decode path drives it. The slot is
// sized to this document (U2-9 item 3): the forward receives
// context_cap = C and workspace_size = P * C exactly, whatever larger buffer
// the slot retains, and no byte of it is filled first (U2-9 item 4): the
// forward reads only rows 0..position, each written earlier by this document.
SslmForwardStatus Encode(EncoderModel* m, WorkspaceSlot* slot,
                         const int32_t* tokens, size_t n, size_t chunk_budget,
                         SembForwardLeg leg, int8_t* codes_out, CarriedScale* scales_out) {
  if (n == 0) return SslmForwardStatus::InvalidLayerBudget;
  if (chunk_budget == 0 || chunk_budget > n) chunk_budget = n;
  const size_t hidden = m->hidden_size;
  const int32_t vocab = static_cast<int32_t>(m->view.config.vocab_size);

  for (size_t p = 0; p < n; ++p) {
    CarriedScale es{};
    const SslmForwardStatus st = EmbedEntry(tokens[p], vocab, m->embed_weights, hidden,
                                             m->embed_site_constant, codes_out + p * hidden, &es);
    if (st != SslmForwardStatus::Ok) return st;
    scales_out[p] = es;
  }

  const size_t cap = static_cast<size_t>(m->context_cap);
  size_t c = SlotPositions(n, cap);
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
  // The forced-reference arm: the rule before U2-9, C = context_cap with a
  // zeroed buffer, as the byte-identity reference.
  const bool forced_reference = test_seam::ForcedReferenceArmed();
  if (forced_reference) c = cap;
#endif
  const size_t workspace_bytes = m->kv_bytes_per_position * c;
  const int64_t context_cap = static_cast<int64_t>(c);
  LongDocumentScope long_document(m, slot, c > kLongDocumentPositions);
  if (slot->capacity < workspace_bytes) GrowSlot(m, slot, c, workspace_bytes);
  uint8_t* const workspace = slot->buffer.get();
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
  if (forced_reference && workspace_bytes != 0) std::memset(workspace, 0, workspace_bytes);
  if (long_document.held()) test_seam::OnGateHolding();
#endif

  uint64_t kv_sat = 0;
  if (leg == kSembForwardLegChunkBatched) {
    for (size_t off = 0; off < n; off += chunk_budget) {
      const size_t span = (off + chunk_budget <= n) ? chunk_budget : (n - off);
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
      test_seam::PoisonAt(off, workspace, m->num_layers, context_cap,
                          m->view.config.num_key_value_heads, m->view.config.head_dim);
#endif
      const SslmForwardStatus st = RunLayerLoopChunkBatched(
          codes_out + off * hidden, scales_out + off, span, m->layers.data(), m->num_layers, hidden,
          m->view.config.head_dim, m->view.config.num_key_value_heads,
          m->view.config.intermediate_size, context_cap, static_cast<int64_t>(off),
          m->view.rope_tables, workspace, workspace_bytes, m->option_g_fused, &kv_sat,
          /*site_prefix=*/{}, /*trace_hook_state=*/nullptr, /*q_width=*/m->q_width);
      if (st != SslmForwardStatus::Ok) return st;
    }
    return SslmForwardStatus::Ok;
  }

  // The per-token leg. The K/V store carries state between positions; each
  // position's residual starts from its own embedding, exactly as the
  // chunk-batched path's own contract states.
  SequenceLayerState seq;
  seq.kv_saturation_count = 0;
  seq.context_length = 0;
  for (size_t p = 0; p < n; ++p) {
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
    test_seam::PoisonAt(p, workspace, m->num_layers, context_cap,
                        m->view.config.num_key_value_heads, m->view.config.head_dim);
#endif
    seq.hidden_codes = codes_out + p * hidden;
    seq.hidden_scale = scales_out[p];
    seq.layer_index = 0;
    seq.context_length = static_cast<int64_t>(p);
    SslmForwardStatus st;
    if (m->option_g_fused) {
      st = RunLayerLoop(seq, m->layers.data(), m->num_layers, m->num_layers, hidden,
                        m->view.config.head_dim, m->view.config.num_key_value_heads,
                        m->view.config.intermediate_size, context_cap, m->view.rope_tables,
                        workspace, workspace_bytes, OptionGKLandingMode::kFused,
                        /*site_prefix=*/{}, /*token_index=*/0, /*trace_hook_state=*/nullptr,
                        /*q_width=*/m->q_width);
    } else {
      st = RunLayerLoop(seq, m->layers.data(), m->num_layers, m->num_layers, hidden,
                        m->view.config.head_dim, m->view.config.num_key_value_heads,
                        m->view.config.intermediate_size, context_cap, m->view.rope_tables,
                        workspace, workspace_bytes, /*site_prefix=*/{},
                        /*token_index=*/0, /*trace_hook_state=*/nullptr, /*q_width=*/m->q_width);
    }
    if (st != SslmForwardStatus::Ok) return st;
    scales_out[p] = seq.hidden_scale;
  }
  return SslmForwardStatus::Ok;
}

// U2-9 item 2: hands the caller the LOWEST-indexed free slot record, or
// appends a new one while fewer than max_concurrency exist; nullptr when
// max_concurrency records are all in use. The caller receives the record
// itself, never an index into a list that may grow.
WorkspaceSlot* TryAcquireWorkspace(EncoderModel* m) {
  std::lock_guard<std::mutex> lock(m->workspace_mutex);
  for (const std::unique_ptr<WorkspaceSlot>& record : m->workspaces) {
    if (!record->in_use) {
      record->in_use = true;
      return record.get();
    }
  }
  if (m->workspaces.size() >= static_cast<size_t>(m->max_concurrency)) return nullptr;
  auto record = std::make_unique<WorkspaceSlot>();
  record->in_use = true;
  m->workspaces.push_back(std::move(record));
  return m->workspaces.back().get();
}

void ReleaseWorkspace(EncoderModel* m, WorkspaceSlot* slot) {
  std::lock_guard<std::mutex> lock(m->workspace_mutex);
  slot->in_use = false;
}

struct WorkspaceLease {
  EncoderModel* model;
  WorkspaceSlot* slot;
  ~WorkspaceLease() { ReleaseWorkspace(model, slot); }
};

// TE-350: acquires at most `want` workspace slots up front (fewer if the
// pool has less than `want` free at the moment of the call -- contention
// with another in-flight call on the same handle is not an error, it is
// simply fewer workers for THIS call), and releases every slot it holds
// on destruction, on every path including an exception unwind. Never
// copied or moved, so there is exactly one owner of each acquired index
// for the pool's whole lifetime -- the property that makes "every
// workspace acquired is released before return" checkable by inspection
// rather than by trusting a per-thread cleanup lambda to run on every
// path.
class WorkspacePool {
 public:
  WorkspacePool(EncoderModel* model, size_t want) : model_(model) {
    slots_.reserve(want);
    for (size_t i = 0; i < want; ++i) {
      WorkspaceSlot* slot = TryAcquireWorkspace(model_);
      if (!slot) break;
      slots_.push_back(slot);
    }
  }
  WorkspacePool(const WorkspacePool&) = delete;
  WorkspacePool& operator=(const WorkspacePool&) = delete;
  ~WorkspacePool() {
    for (WorkspaceSlot* slot : slots_) ReleaseWorkspace(model_, slot);
  }
  size_t count() const { return slots_.size(); }
  WorkspaceSlot* slot(size_t worker) const { return slots_[worker]; }

 private:
  EncoderModel* model_;
  std::vector<WorkspaceSlot*> slots_;
};

// TE-350: T = min(documents, max_concurrency, hardware concurrency).
// Bounding by hardware concurrency avoids oversubscribing the machine when
// max_concurrency is larger than the box's own thread count; bounding by
// max_concurrency is the workspace contract itself; bounding by document count
// means a small batch never spins up idle workers with nothing to do.
// std::thread::hardware_concurrency() is permitted to return 0 when it
// cannot determine an answer, in which case this falls back to 1 rather
// than propagating a 0 that would make every batch call refuse for want of
// a worker.
size_t HardwareConcurrencyOrOne() {
  const unsigned hc = std::thread::hardware_concurrency();
  return hc == 0 ? size_t{1} : static_cast<size_t>(hc);
}

}  // namespace
}  // namespace semb

// ---------------------------------------------------------------------------
// The U0 witness verb, unchanged in shape (one-shot: opens, encodes, closes).
// The body is C++-linkage and the C-linkage definition is its boundary
// (U2-10): the model is held by a guard, so every exit, an exception
// included, closes it (U2-10 Builds item 2b).
// ---------------------------------------------------------------------------
namespace semb {
namespace {

SembEncodeStatus EncodeDocumentForWitnessBody(const char* model_path, const int32_t* token_ids,
                                              size_t n, size_t chunk_budget, int8_t* codes_out,
                                              int64_t* m_out, int64_t* e_out, size_t* hidden_out) {
  std::unique_ptr<EncoderModel, void (*)(EncoderModel*)> model(EncoderOpen(model_path, nullptr),
                                                               EncoderClose);
  if (!model) return kSembEncodeEngineRefused;

  const size_t hidden = model->hidden_size;
  // TE-79 F-7 (Significant, TE-76 review): the real hidden_size is only
  // known now, after the model has opened -- refused here, before
  // *hidden_out, codes_out, m_out or e_out are touched, rather than
  // writing n*hidden bytes into a codes_out buffer no caller could have
  // sized correctly (this verb takes no capacity parameter; see
  // seam_abi.h's own kSembWitnessMaxHidden comment for why).
  if (hidden > kSembWitnessMaxHidden) return kSembEncodeHiddenTooLarge;
  if (hidden_out) *hidden_out = hidden;

  std::vector<superslm::CarriedScale> scales(n);
  WorkspaceSlot* slot = TryAcquireWorkspace(model.get());
  if (!slot) return kSembEncodeEngineRefused;
  superslm::SslmForwardStatus st;
  {
    WorkspaceLease lease{model.get(), slot};
    st = Encode(model.get(), slot, token_ids, n, chunk_budget, kSembForwardLegChunkBatched,
                codes_out, scales.data());
  }
  if (st != superslm::SslmForwardStatus::Ok) return kSembEncodeEngineRefused;
  for (size_t p = 0; p < n; ++p) {
    m_out[p] = scales[p].m;
    e_out[p] = scales[p].e;
  }
  return kSembEncodeOk;
}

}  // namespace
}  // namespace semb

extern "C" SembEncodeStatus semb_encode_document_for_witness(const char* model_path,
                                                               const int32_t* token_ids, size_t n,
                                                               size_t chunk_budget,
                                                               int8_t* codes_out, int64_t* m_out,
                                                               int64_t* e_out,
                                                               size_t* hidden_out) {
  return semb::c_boundary::Encode([&] {
    return semb::EncodeDocumentForWitnessBody(model_path, token_ids, n, chunk_budget, codes_out,
                                              m_out, e_out, hidden_out);
  });
}

// ---------------------------------------------------------------------------
// The handle surface (encoder_interim_abi.h).
// ---------------------------------------------------------------------------
struct SembInterimEncoder {
  semb::EncoderModel* model = nullptr;
};

namespace {

const char* OkName() { return "Ok"; }

}  // namespace

static SembInterimStatus EncoderOpenBody(const char* model_path,
                                         int32_t appended_token_id,
                                         SembInterimEncoder** out_encoder,
                                         const char** out_engine_status_name) {
  if (out_engine_status_name) *out_engine_status_name = OkName();
  if (!model_path || !out_encoder) return kSembInterimNullArgument;
  *out_encoder = nullptr;
  const char* name = nullptr;
  semb::EncoderModel* model = semb::EncoderOpen(model_path, &name);
  if (!model) {
    if (out_engine_status_name) *out_engine_status_name = name ? name : "Unknown";
    return kSembInterimEngineRefused;
  }
  std::unique_ptr<semb::EncoderModel, void (*)(semb::EncoderModel*)> model_guard(
      model, semb::EncoderClose);
  model->appended_token_id = appended_token_id;
  auto enc = std::make_unique<SembInterimEncoder>();
  enc->model = model_guard.release();
  *out_encoder = enc.release();
  return kSembInterimOk;
}

extern "C" SembInterimStatus semb_interim_encoder_open(const char* model_path,
                                                        int32_t appended_token_id,
                                                        SembInterimEncoder** out_encoder,
                                                        const char** out_engine_status_name) {
  return semb::c_boundary::Interim(out_engine_status_name, [&] {
    return EncoderOpenBody(model_path, appended_token_id, out_encoder, out_engine_status_name);
  });
}

static SembInterimStatus EncoderOpenFromMemoryBody(
    const uint8_t* model_bytes, size_t model_size, int32_t appended_token_id,
    uint32_t max_concurrency, SembInterimEncoder** out_encoder,
    const char** out_engine_status_name) {
  if (out_engine_status_name) *out_engine_status_name = OkName();
  if (!model_bytes || model_size == 0 || max_concurrency == 0 || !out_encoder) {
    return kSembInterimNullArgument;
  }
  *out_encoder = nullptr;
  const char* name = nullptr;
  semb::EncoderModel* model =
      semb::EncoderOpenFromMemory(model_bytes, model_size, max_concurrency, &name);
  if (!model) {
    if (out_engine_status_name) *out_engine_status_name = name ? name : "Unknown";
    return kSembInterimEngineRefused;
  }
  std::unique_ptr<semb::EncoderModel, void (*)(semb::EncoderModel*)> model_guard(
      model, semb::EncoderClose);
  model->appended_token_id = appended_token_id;
  auto enc = std::make_unique<SembInterimEncoder>();
  enc->model = model_guard.release();
  *out_encoder = enc.release();
  return kSembInterimOk;
}

extern "C" SembInterimStatus semb_interim_encoder_open_from_memory(
    const uint8_t* model_bytes, size_t model_size, int32_t appended_token_id,
    uint32_t max_concurrency, SembInterimEncoder** out_encoder,
    const char** out_engine_status_name) {
  return semb::c_boundary::Interim(out_engine_status_name, [&] {
    return EncoderOpenFromMemoryBody(model_bytes, model_size, appended_token_id, max_concurrency,
                                     out_encoder, out_engine_status_name);
  });
}

SembInterimStatus semb_interim_encoder_open_owned(
    std::vector<uint8_t>&& model_bytes, int32_t appended_token_id,
    uint32_t max_concurrency, SembInterimEncoder** out_encoder,
    const char** out_engine_status_name) {
  bool vocab_mismatch = false;
  return semb::OpenEncoderOwnedWithVocabJoin(
      std::move(model_bytes), appended_token_id, max_concurrency, 0,
      out_encoder, out_engine_status_name, vocab_mismatch);
}

SembInterimStatus semb::OpenEncoderOwnedWithVocabJoin(
    std::vector<uint8_t>&& model_bytes, int32_t appended_token_id,
    uint32_t max_concurrency, uint32_t tokenizer_vocab_size,
    SembInterimEncoder** out_encoder, const char** out_engine_status_name,
    bool& vocab_mismatch) {
  vocab_mismatch = false;
  if (out_engine_status_name) *out_engine_status_name = OkName();
  if (model_bytes.empty() || max_concurrency == 0 || !out_encoder) {
    return kSembInterimNullArgument;
  }
  *out_encoder = nullptr;
  const char* name = nullptr;
  semb::EncoderModel* model =
      semb::EncoderOpenOwned(std::move(model_bytes), max_concurrency, &name,
                             tokenizer_vocab_size, &vocab_mismatch);
  if (!model) {
    if (out_engine_status_name) *out_engine_status_name = name ? name : "Unknown";
    return kSembInterimEngineRefused;
  }
  std::unique_ptr<semb::EncoderModel, void (*)(semb::EncoderModel*)> model_guard(
      model, semb::EncoderClose);
  model->appended_token_id = appended_token_id;
  auto enc = std::make_unique<SembInterimEncoder>();
  enc->model = model_guard.release();
  *out_encoder = enc.release();
  return kSembInterimOk;
}

extern "C" void semb_interim_encoder_close(SembInterimEncoder* encoder) {
  if (!encoder) return;
  semb::EncoderClose(encoder->model);
  delete encoder;
}

static SembInterimStatus EncoderGeometryBody(const SembInterimEncoder* encoder,
                                             SembInterimGeometry* out_geometry) {
  if (!encoder || !out_geometry) return kSembInterimNullArgument;
  if (out_geometry->struct_size != sizeof(SembInterimGeometry)) return kSembInterimStructSizeMismatch;
  const semb::EncoderModel* m = encoder->model;
  out_geometry->hidden_size = m->view.config.hidden_size;
  out_geometry->num_hidden_layers = m->view.config.num_hidden_layers;
  out_geometry->num_attention_heads = m->view.config.num_attention_heads;
  out_geometry->num_key_value_heads = m->view.config.num_key_value_heads;
  out_geometry->head_dim = m->view.config.head_dim;
  out_geometry->vocab_size = m->view.config.vocab_size;
  out_geometry->context_cap = m->view.config.context_cap;
  out_geometry->appended_token_id = m->appended_token_id;
  return kSembInterimOk;
}

extern "C" SembInterimStatus semb_interim_encoder_geometry(const SembInterimEncoder* encoder,
                                                            SembInterimGeometry* out_geometry) {
  return semb::c_boundary::Interim(nullptr, [&] {
    return EncoderGeometryBody(encoder, out_geometry);
  });
}

static SembInterimStatus EncodeSeamBody(
    SembInterimEncoder* encoder, const int32_t* content_ids, size_t n_content, size_t chunk_budget,
    SembForwardLeg leg, int8_t* codes_out, size_t codes_capacity, int64_t* m_out, int64_t* e_out,
    size_t scales_capacity, size_t* out_positions, const char** out_engine_status_name) {
  if (out_engine_status_name) *out_engine_status_name = OkName();
  if (!encoder || !out_positions) return kSembInterimNullArgument;
  if (n_content > 0 && !content_ids) return kSembInterimNullArgument;
  if (n_content == 0) {
    *out_positions = 0;
    return kSembInterimInputEmpty;
  }
  if (leg != kSembForwardLegChunkBatched && leg != kSembForwardLegPerToken) {
    *out_positions = n_content + 1;
    return kSembInterimInvalidLeg;
  }
  semb::EncoderModel* m = encoder->model;
  const size_t n = n_content + 1;  // the appended end token
  *out_positions = n;
  const size_t hidden = m->hidden_size;
  if (codes_capacity < n * hidden || scales_capacity < n) return kSembInterimBufferTooSmall;
  if (!codes_out || !m_out || !e_out) return kSembInterimNullArgument;

  // SuperEmbedder's own code appends the single fixed end token id (sec 3.2)
  // to the token-id sequence, never composed as text and re-tokenized.
  std::vector<int32_t> tokens(n);
  std::memcpy(tokens.data(), content_ids, n_content * sizeof(int32_t));
  tokens[n_content] = m->appended_token_id;

  std::vector<superslm::CarriedScale> scales(n);
  semb::WorkspaceSlot* const slot = semb::TryAcquireWorkspace(m);
  if (!slot) return kSembInterimConcurrencyExceeded;
  semb::WorkspaceLease lease{m, slot};
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
  semb::test_seam::OnAcquired();
#endif
  const superslm::SslmForwardStatus st = semb::Encode(m, slot, tokens.data(), n, chunk_budget, leg,
                                                      codes_out, scales.data());
  if (st != superslm::SslmForwardStatus::Ok) {
    if (out_engine_status_name) {
      *out_engine_status_name = semb::SetEngineStatus(superslm::SslmForwardStatusName(st));
    }
    return kSembInterimEngineRefused;
  }
  for (size_t p = 0; p < n; ++p) {
    m_out[p] = scales[p].m;
    e_out[p] = scales[p].e;
  }
  return kSembInterimOk;
}

extern "C" SembInterimStatus semb_interim_encode_seam(
    SembInterimEncoder* encoder, const int32_t* content_ids, size_t n_content, size_t chunk_budget,
    SembForwardLeg leg, int8_t* codes_out, size_t codes_capacity, int64_t* m_out, int64_t* e_out,
    size_t scales_capacity, size_t* out_positions, const char** out_engine_status_name) {
  return semb::c_boundary::Interim(out_engine_status_name, [&] {
    return EncodeSeamBody(encoder, content_ids, n_content, chunk_budget, leg, codes_out,
                          codes_capacity, m_out, e_out, scales_capacity, out_positions,
                          out_engine_status_name);
  });
}

static SembInterimStatus PsNSelectBody(const SembInterimEncoder* encoder,
                                       const int8_t* codes, const int64_t* m,
                                       const int64_t* e, size_t n, size_t position,
                                       int8_t* out_codes, int64_t* out_m,
                                       int64_t* out_e,
                                       const char** out_engine_status_name) {
  if (out_engine_status_name) *out_engine_status_name = OkName();
  if (!encoder || !codes || !m || !e || !out_codes || !out_m || !out_e) {
    return kSembInterimNullArgument;
  }
  if (position >= n) return kSembInterimPositionOutOfRange;
  const semb::EncoderModel* model = encoder->model;
  const size_t hidden = model->hidden_size;
  const superslm::CarriedScale incoming{m[position], e[position]};
  superslm::CarriedScale out_scale{};
  // The engine's own final norm, under the same site name and site constant
  // its generation loop applies before the head -- PS-N's post-final-norm
  // source state (sec 3.2). The incoming scale is annihilated by the site's
  // own scale-killing form (forward_sites.h's RmsNormSite contract).
  const superslm::SslmForwardStatus st = superslm::RmsNormSite(
      codes + position * hidden, model->final_norm_gain.data(), hidden, incoming,
      model->final_norm_site_constant, out_codes, &out_scale, "final_norm");
  if (st != superslm::SslmForwardStatus::Ok) {
    if (out_engine_status_name) {
      *out_engine_status_name = semb::SetEngineStatus(superslm::SslmForwardStatusName(st));
    }
    return kSembInterimEngineRefused;
  }
  *out_m = out_scale.m;
  *out_e = out_scale.e;
  return kSembInterimOk;
}

extern "C" SembInterimStatus semb_interim_ps_n_select(const SembInterimEncoder* encoder,
                                                       const int8_t* codes, const int64_t* m,
                                                       const int64_t* e, size_t n, size_t position,
                                                       int8_t* out_codes, int64_t* out_m,
                                                       int64_t* out_e,
                                                       const char** out_engine_status_name) {
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
  semb::test_seam::ps_n_select_calls.fetch_add(1);
#endif
  return semb::c_boundary::Interim(out_engine_status_name, [&] {
    return PsNSelectBody(encoder, codes, m, e, n, position, out_codes, out_m, out_e,
                         out_engine_status_name);
  });
}

// `out_triple`, when non-null, also receives the selected triple dequantize
// reads (U3: a GPU open's W-Q agreement-check triples, T388-11). The public
// verb passes none, so a CPU open computes nothing extra.
static SembInterimStatus EncodePsNBody(
    SembInterimEncoder* encoder, const int32_t* content_ids, size_t n_content, size_t chunk_budget,
    float* out_vector, size_t out_capacity_bytes, size_t* out_required_bytes,
    SembWaPoolStatus* out_dequantize_status, const char** out_engine_status_name,
    semb::InterimTriple* out_triple = nullptr) {
  if (out_engine_status_name) *out_engine_status_name = OkName();
  if (out_dequantize_status) *out_dequantize_status = kSembOk;
  if (!encoder || !out_required_bytes) return kSembInterimNullArgument;
  semb::EncoderModel* m = encoder->model;
  const size_t hidden = m->hidden_size;
  *out_required_bytes = hidden * sizeof(float);
  if (n_content == 0) return kSembInterimInputEmpty;
  if (out_capacity_bytes < hidden * sizeof(float)) return kSembInterimBufferTooSmall;
  if (!out_vector || !content_ids) return kSembInterimNullArgument;

  const size_t n = n_content + 1;
  std::vector<int8_t> codes(n * hidden);
  std::vector<int64_t> ms(n), es(n);
  size_t positions = 0;
  // U2-10: the same-file bodies, not their C-linkage boundaries -- a nested
  // boundary would turn an escaping exception into a status here, and the
  // outer boundary is the one that reports it.
  SembInterimStatus st = EncodeSeamBody(encoder, content_ids, n_content, chunk_budget,
                                        kSembForwardLegChunkBatched, codes.data(), codes.size(),
                                        ms.data(), es.data(), n, &positions,
                                        out_engine_status_name);
  if (st != kSembInterimOk) return st;

  // PS-N's position rule: the LAST position -- the appended end token's.
  std::vector<int8_t> sel(hidden);
  int64_t sel_m = 0, sel_e = 0;
  st = PsNSelectBody(encoder, codes.data(), ms.data(), es.data(), n, n - 1, sel.data(), &sel_m,
                     &sel_e, out_engine_status_name);
  if (st != kSembInterimOk) return st;
  if (out_triple) {
    out_triple->codes = sel;
    out_triple->m = sel_m;
    out_triple->e = sel_e;
  }

  const SembWaPoolStatus dq = semb_ps_n_dequantize(sel.data(), sel_m, sel_e, hidden, out_vector);
  if (out_dequantize_status) *out_dequantize_status = dq;
  // U2-10 Builds item 3: a callee's allocation status becomes this verb's own.
  if (dq == kSembWaPoolAllocationFailed) return kSembInterimAllocationFailed;
  // U2-10 Builds item 3: the pool's catch-all is an internal failure, never a
  // dequantize corner -- it reports as this family's catch-all.
  if (dq == kSembWaPoolInternalFailure) {
    if (out_engine_status_name) *out_engine_status_name = "UnhandledException";
    return kSembInterimEngineRefused;
  }
  if (dq != kSembOk) return kSembInterimDequantizeRefused;
  return kSembInterimOk;
}

extern "C" SembInterimStatus semb_interim_encode_ps_n(
    SembInterimEncoder* encoder, const int32_t* content_ids, size_t n_content, size_t chunk_budget,
    float* out_vector, size_t out_capacity_bytes, size_t* out_required_bytes,
    SembWaPoolStatus* out_dequantize_status, const char** out_engine_status_name) {
  return semb::c_boundary::Interim(out_engine_status_name, [&] {
    return EncodePsNBody(encoder, content_ids, n_content, chunk_budget, out_vector,
                         out_capacity_bytes, out_required_bytes, out_dequantize_status,
                         out_engine_status_name);
  });
}

namespace semb {

// U3 (te266 sec 4.2): C++-linkage internals for the GPU open path. Neither is
// a C verb, so neither joins tools/c_boundary_table.json.
SembInterimStatus EncodePsNWithTriple(SembInterimEncoder* encoder, const int32_t* content_ids,
                                      size_t n_content, size_t chunk_budget, float* out_vector,
                                      size_t out_capacity_bytes, size_t* out_required_bytes,
                                      SembWaPoolStatus* out_dequantize_status,
                                      const char** out_engine_status_name, InterimTriple* out_triple) {
  return semb::c_boundary::Interim(out_engine_status_name, [&] {
    return EncodePsNBody(encoder, content_ids, n_content, chunk_budget, out_vector,
                         out_capacity_bytes, out_required_bytes, out_dequantize_status,
                         out_engine_status_name, out_triple);
  });
}

// The interim encoder's own model view: the GPU map reads the model the CPU
// open already loaded (sec 3.3's one-read rule), so the model is read once.
const superslm::SslmModelView* InterimModelView(const SembInterimEncoder* encoder) {
  return encoder && encoder->model ? &encoder->model->view : nullptr;
}

}  // namespace semb

// TE-147 (U1 slice 2 build), re-stated TE-350 for cross-document
// parallelism: the batch composed path, AT MOST T workspace acquisitions
// for the whole call (T <= min(doc_count, max_concurrency, hardware concurrency),
// see encoder_interim_abi.h's own doc comment for the full contract). Every
// document's own per-document seam+select+dequantize steps are unchanged
// (select and dequantize touch no workspace, so they are called exactly as
// the singleton path calls them; only the seam step -- semb::Encode -- is
// inlined here against a worker's OWN already-held workspace rather than
// going through semb_interim_encode_seam's own internal acquire). Threading
// is scheduling only: each document's arithmetic depends on nothing but
// that document's own tokens and the model's read-only weights, so which
// worker computes it and when never changes what it computes -- output
// offsets (row stride, seam-stream offset) are sizes known before any
// forward runs, precomputed once, never accumulated as a running total
// inside the per-document work.
static SembInterimStatus EncodeBatchPsNBody(
    SembInterimEncoder* encoder, const int32_t* const* docs_content_ids,
    const size_t* docs_n_content, size_t doc_count, size_t chunk_budget, float* out_vectors,
    size_t out_capacity_bytes, size_t* out_required_bytes, size_t* out_failed_index,
    SembWaPoolStatus* out_dequantize_status, const char** out_engine_status_name,
    uint8_t* out_seam_stream, size_t out_seam_stream_capacity, size_t* out_seam_stream_len) {
  if (out_engine_status_name) *out_engine_status_name = OkName();
  if (out_dequantize_status) *out_dequantize_status = kSembOk;
  if (out_failed_index) *out_failed_index = 0;
  if (!encoder || !out_required_bytes) return kSembInterimNullArgument;
  semb::EncoderModel* m = encoder->model;
  const size_t hidden = m->hidden_size;
  const size_t required = doc_count * hidden * sizeof(float);
  // TE-159 (C-2): row output is now OPTIONAL. out_vectors == nullptr skips
  // the capacity gate and the select+dequantize half of the composition
  // entirely for every document -- a caller that only taps the seam (W-B)
  // never has to supply a row buffer or pay for PS-N's own final-norm
  // selection and dequantize. *out_required_bytes still reports what a row
  // buffer WOULD need, matching every other capacity-convention verb here.
  *out_required_bytes = required;
  if (out_vectors) {
    if (out_capacity_bytes < required) return kSembInterimBufferTooSmall;
  }
  if (doc_count == 0) {
    // TE-184 (D-SLM6434 item 1, TE-161/TE-148 M-2): the header's own
    // contract ("On success *out_seam_stream_len holds the exact length
    // written") applies to this early return too -- zero documents means
    // zero bytes written to out_seam_stream, and a caller that supplied
    // the pointer reads that fact rather than its own caller-supplied
    // sentinel.
    if (out_seam_stream_len) *out_seam_stream_len = 0;
    return kSembInterimOk;  // the caller's own CORPUS_EMPTY guards n==0 earlier
  }
  if (!docs_content_ids || !docs_n_content) return kSembInterimNullArgument;

  // The seam tap's own capacity, checked BEFORE any forward runs (shape-
  // before-work, the same discipline semb_encode_batch's own steps 1-11
  // apply before its first forward). Each document's serialised record is
  // exactly semb_seam_serialize_document's own VERBATIM layout: u64(n) ||
  // u64(hidden) || n * (codes[hidden] || u64(m) || u64(e)) -- a fixed
  // function of that document's own token count alone, so every document's
  // start offset is knowable from docs_n_content up front (TE-350: this is
  // what makes per-document seam writes safe to run out of order, on
  // disjoint threads, without any running accumulator).
  size_t seam_required = 0;
  std::vector<size_t> seam_offsets;  // seam_offsets[doc]..seam_offsets[doc+1] is doc's own slice
  if (out_seam_stream) {
    seam_offsets.resize(doc_count + 1);
    for (size_t doc = 0; doc < doc_count; ++doc) {
      seam_offsets[doc] = seam_required;
      const size_t n = docs_n_content[doc] + 1;
      seam_required += 16 + n * (hidden + 16);
    }
    seam_offsets[doc_count] = seam_required;
    if (out_seam_stream_len) *out_seam_stream_len = seam_required;
    if (out_seam_stream_capacity < seam_required) return kSembInterimBufferTooSmall;
  }

  // T = min(documents, max_concurrency, hardware concurrency). See
  // WorkspacePool's and HardwareConcurrencyOrOne's own comments above.
  const size_t t_target = std::min(
      {doc_count, static_cast<size_t>(m->max_concurrency), semb::HardwareConcurrencyOrOne()});
  semb::WorkspacePool pool(m, t_target);
  const size_t worker_count = pool.count();
  if (worker_count == 0) {
    return kSembInterimConcurrencyExceeded;
  }
  // TE-350's deterministic-failure state: every worker records its own
  // failure, if any, into this ONE mutex-guarded slot, keeping only the
  // LOWEST document index seen so far. Workers never stop early on a
  // failure elsewhere -- every document assigned to every worker still
  // runs to completion -- so by the time every worker has joined, this
  // slot holds the true minimum over every document that failed, regardless
  // of which worker reached which document first or how the workers
  // finished relative to each other. The engine-status message is captured
  // as a copy at the point of failure, on the thread that produced it,
  // because `t_last_engine_status` (SetEngineStatus, above) is thread-local
  // -- a raw pointer into it would dangle or point at the wrong message the
  // instant control returns to the calling thread.
  struct FailureRecord {
    size_t doc_index = 0;
    SembInterimStatus interim_status = kSembInterimOk;
    bool has_engine_status = false;
    std::string engine_status;
    SembWaPoolStatus dequant_status = kSembOk;
  };
  std::mutex failure_mutex;
  bool have_failure = false;
  FailureRecord best_failure{};
  // TE-356 (C-1): an exception from ANY worker (spawned or the calling
  // thread's own inline worker 0) is captured here rather than left to
  // propagate out of that worker's own frame -- an uncaught exception
  // escaping a std::thread's initial function is std::terminate by the
  // standard, and ProcessOneDocument performs five heap allocations per
  // document with no boundary of its own. Guarded by the same mutex
  // RecordFailure already uses; the first exception any worker sees wins
  // (later ones are dropped -- one execution is all the caller ever gets
  // back, and every worker that saw one is a machine already in trouble).
  std::exception_ptr captured_exception;
  auto RecordFailure = [&](size_t doc, SembInterimStatus status, bool has_status,
                           const std::string& status_str, SembWaPoolStatus dq) {
    std::lock_guard<std::mutex> lock(failure_mutex);
    if (!have_failure || doc < best_failure.doc_index) {
      have_failure = true;
      best_failure.doc_index = doc;
      best_failure.interim_status = status;
      best_failure.has_engine_status = has_status;
      best_failure.engine_status = status_str;
      best_failure.dequant_status = dq;
    }
  };

  // One document at a time is claimed from this shared counter -- dynamic
  // scheduling rather than a static split -- so workers balance the load
  // evenly even though document encode cost varies with document length;
  // every claim is a distinct fetch_add result, so every document is
  // claimed by exactly one worker, exactly once.
  std::atomic<size_t> next_doc{0};

  auto ProcessOneDocument = [&](size_t doc, semb::WorkspaceSlot* slot) {
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
    // TE-184 (D-SLM6434 item 2): the per-document park point, fired once
    // per document while that document's OWN worker workspace slot is
    // held. Possibly called concurrently by more than one worker thread
    // when worker_count > 1; state_mutex (shared with OnAcquired) makes
    // that safe. A no-op (one counter increment) when unarmed.
    semb::test_seam::OnRowAcquired();
#endif
    const size_t n_content = docs_n_content[doc];
    const size_t n = n_content + 1;  // the appended end token
    std::vector<int32_t> tokens(n);
    if (n_content > 0) {
      std::memcpy(tokens.data(), docs_content_ids[doc], n_content * sizeof(int32_t));
    }
    tokens[n_content] = m->appended_token_id;

    std::vector<int8_t> codes(n * hidden);
    std::vector<superslm::CarriedScale> scales(n);
    const superslm::SslmForwardStatus st =
        semb::Encode(m, slot, tokens.data(), n, chunk_budget, kSembForwardLegChunkBatched,
                     codes.data(), scales.data());
    if (st != superslm::SslmForwardStatus::Ok) {
      RecordFailure(doc, kSembInterimEngineRefused, /*has_status=*/true,
                    superslm::SslmForwardStatusName(st), kSembOk);
      return;
    }

    std::vector<int64_t> ms(n), es(n);
    for (size_t p = 0; p < n; ++p) {
      ms[p] = scales[p].m;
      es[p] = scales[p].e;
    }

    // The seam tap (TE-159, C-2): this document's FULL per-position seam --
    // every position, pre-final-norm, exactly what select below reads ONE
    // position out of -- serialised into this document's own precomputed
    // slice [seam_offsets[doc], seam_offsets[doc + 1]). Passing the exact
    // slice width, not "capacity to the buffer's end", means a defect that
    // ever wrote more than the documented formula would refuse here rather
    // than silently overwrite a neighbouring document's own region on
    // another thread -- stricter than, and (per this format's own VERBATIM
    // contract above) always equal in practice to, the single-threaded
    // predecessor's "remaining capacity to the end of the buffer" bound.
    if (out_seam_stream) {
      const size_t offset = seam_offsets[doc];
      const size_t slice = seam_offsets[doc + 1] - offset;
      size_t doc_len = 0;
      const SembSerializeStatus sst = semb_seam_serialize_document(
          codes.data(), ms.data(), es.data(), n, hidden, out_seam_stream + offset, slice, &doc_len);
      if (sst != kSembSerializeOk) {
        RecordFailure(doc, kSembInterimBufferTooSmall, /*has_status=*/false, std::string(), kSembOk);
        return;
      }
      if (doc_len != slice) {
        // TE-356 (M-3): doc_len is the serializer's OWN report of what it
        // wrote; this precomputed slice width is the only tie between that
        // write and every OTHER document's own start offset in
        // seam_offsets (above). This function's own `16 + n * (hidden +
        // 16)` and seam_serialize.cpp's identical restatement agree today
        // (verified by reading both, not assumed) -- this refuses rather
        // than silently accepting a write shorter or longer than the slice
        // the offset table promised, which a future drift between the two
        // formulas would otherwise leave uninitialised bytes inside a
        // stream whose reported length is the PRECOMPUTED one, not the
        // one actually written.
        RecordFailure(doc, kSembInterimEngineRefused, /*has_status=*/true, "SeamLengthMismatch",
                      kSembOk);
        return;
      }
    }

    // PS-N's position rule: the LAST position. Neither select nor
    // dequantize touches the workspace, so these are the SAME public
    // primitives the singleton path calls (encoder_interim_abi.h). Skipped
    // entirely when no row output was requested (TE-159 C-2): a seam-only
    // caller does not need PS-N's own final-norm selection or dequantize,
    // and skipping means a dequantize corner (kSembPooledSubnormal /
    // kSembPooledOverflow) on a committed W-B document can never turn into
    // a spurious semb_encoder_open refusal that has nothing to do with the
    // digest actually derived. `out_vectors + doc * hidden` is this
    // document's own fixed-stride row -- disjoint from every other
    // document's row regardless of processing order.
    if (out_vectors) {
      std::vector<int8_t> sel(hidden);
      int64_t sel_m = 0, sel_e = 0;
      const char* local_status = nullptr;
      const SembInterimStatus sel_st =
          PsNSelectBody(encoder, codes.data(), ms.data(), es.data(), n, n - 1, sel.data(), &sel_m,
                        &sel_e, &local_status);
      if (sel_st != kSembInterimOk) {
        RecordFailure(doc, sel_st, /*has_status=*/local_status != nullptr,
                      local_status ? local_status : "", kSembOk);
        return;
      }

      const SembWaPoolStatus dq =
          semb_ps_n_dequantize(sel.data(), sel_m, sel_e, hidden, out_vectors + doc * hidden);
      if (dq != kSembOk) {
        // U2-10 Builds item 3: a callee's allocation status becomes this
        // verb's own.
        RecordFailure(doc,
                      dq == kSembWaPoolAllocationFailed   ? kSembInterimAllocationFailed
                      : dq == kSembWaPoolInternalFailure ? kSembInterimEngineRefused
                                                         : kSembInterimDequantizeRefused,
                      /*has_status=*/false, std::string(), dq);
        return;
      }
    }
  };

  auto Worker = [&](semb::WorkspaceSlot* slot) {
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
    // Fired on THIS worker's own thread, as the first thing it does after
    // being handed its slot -- not in a loop on the calling thread before
    // any worker starts. OnAcquired can itself BLOCK (park) the calling
    // thread; calling it worker_count times in a pre-spawn loop on one
    // thread would serialize what must be simultaneous parks across
    // worker_count independent threads, defeating the exact observation
    // (tests/u1_batch_workspace_test.cpp's C52) this hook exists to make.
    // At worker_count == 1 this is the ONE call the original single-
    // workspace path always made, on the same calling thread it always
    // ran on (Worker 0 below runs inline, no std::thread spawned).
    semb::test_seam::OnAcquired();
#endif
    // TE-356 (C-1): this try/catch is what makes Worker's own frame return
    // normally on every path -- including a std::bad_alloc from any of
    // ProcessOneDocument's five per-document allocations -- so a spawned
    // thread's initial function never exits via an uncaught exception, and
    // the calling thread's own inline call (worker 0, below) never leaves
    // this function with `threads` still holding joinable threads either.
    // The exception is captured, not acted on here; a worker that caught
    // one stops claiming further documents (continuing to allocate on a
    // thread that just failed to allocate is not a recovery), but every
    // OTHER worker still runs every document assigned to it, exactly as
    // RecordFailure's own lowest-index contract already assumes.
    try {
      for (;;) {
        const size_t doc = next_doc.fetch_add(1, std::memory_order_relaxed);
        if (doc >= doc_count) break;
        ProcessOneDocument(doc, slot);
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(failure_mutex);
      if (!captured_exception) captured_exception = std::current_exception();
    }
  };

  // Threads 1..worker_count-1 are spawned FIRST, so every worker (including
  // worker 0 below) reaches its own OnAcquired call at roughly the same
  // time when the test seam arms a park -- required for C52's "kPoolSize
  // simultaneous parks" observation to be reachable at all. Worker 0 runs
  // on the calling thread itself -- no std::thread is ever created when
  // worker_count == 1, so the single-workspace call this function has
  // always supported is not merely a special case of the parallel path, it
  // is the identical call stack it always was. Every spawned thread is
  // joined before this function inspects any failure state or touches any
  // output length.
  //
  // TE-356 (C-1): `joiner`'s destructor joins every thread `threads` holds
  // on EVERY exit from this point on, including std::thread's own
  // constructor throwing std::system_error partway through the spawn loop
  // below (a documented resource-exhaustion path) -- declared after
  // `threads` so it is destroyed FIRST during unwind (reverse declaration
  // order), before `threads` itself destroys its now-non-joinable
  // std::thread objects. Worker's own try/catch above means neither the
  // spawned threads' entry function nor the inline call below can throw at
  // all in the ordinary (allocation-failure) case; this guard exists for
  // the thread-construction failure Worker's own catch cannot reach,
  // because it fires before that thread's entry function ever runs.
  std::vector<std::thread> threads;
  struct ThreadJoiner {
    std::vector<std::thread>& threads;
    ~ThreadJoiner() {
      for (std::thread& t : threads) {
        if (t.joinable()) t.join();
      }
    }
  } joiner{threads};
  threads.reserve(worker_count - 1);
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
  const uint64_t seam_batch_call = semb::test_seam::NextBatchCallToken();
#endif
  // U2-10 Builds item 2c: every worker thread starts through StartWorker. A
  // thread that cannot start because the system is out of a resource
  // (std::errc::resource_unavailable_try_again: on Linux a thread-count
  // limit as well as memory) is not a failure of the call: the spawn loop
  // ends, and the batch runs on the workers already started plus worker 0
  // inline. Its output does not depend on how many workers ran (every row
  // and seam slice is at an offset fixed above, each document is claimed
  // once, and the failure record keeps the lowest index). The slots taken
  // above stay held until return. Every other exception from the start
  // propagates unchanged to this verb's boundary.
  auto StartWorker = [&](semb::WorkspaceSlot* slot) {
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
    // C66's spawn fail arm: fires in place of the thread's construction.
    semb::test_seam::OnWorkerStart(seam_batch_call);
#endif
    threads.emplace_back(Worker, slot);
  };
  for (size_t w = 1; w < worker_count; ++w) {
    try {
      StartWorker(pool.slot(w));
    } catch (const std::system_error& e) {
      if (e.code() != std::errc::resource_unavailable_try_again) throw;
      break;
    }
  }
  Worker(pool.slot(0));
  for (std::thread& t : threads) {
    if (t.joinable()) t.join();
  }

  if (captured_exception) {
    // TE-356 (C-1): rethrown only after every spawned thread has been
    // joined above -- std::rethrow_exception restores the ORIGINAL
    // exception object (a std::bad_alloc stays a std::bad_alloc), so the
    // existing catch(std::bad_alloc) at semb_encode_batch and
    // semb_bake_to_path still produces SEMB_ALLOCATION_FAILED exactly as
    // it always has; no new status was added for this.
    std::rethrow_exception(captured_exception);
  }

  if (have_failure) {
    if (out_failed_index) *out_failed_index = best_failure.doc_index;
    if (best_failure.has_engine_status && out_engine_status_name) {
      *out_engine_status_name = semb::SetEngineStatus(best_failure.engine_status);
    }
    if (out_dequantize_status) *out_dequantize_status = best_failure.dequant_status;
    return best_failure.interim_status;
  }
  if (out_seam_stream && out_seam_stream_len) *out_seam_stream_len = seam_required;
  return kSembInterimOk;
}

extern "C" SembInterimStatus semb_interim_encode_batch_ps_n(
    SembInterimEncoder* encoder, const int32_t* const* docs_content_ids,
    const size_t* docs_n_content, size_t doc_count, size_t chunk_budget, float* out_vectors,
    size_t out_capacity_bytes, size_t* out_required_bytes, size_t* out_failed_index,
    SembWaPoolStatus* out_dequantize_status, const char** out_engine_status_name,
    uint8_t* out_seam_stream, size_t out_seam_stream_capacity, size_t* out_seam_stream_len) {
  return semb::c_boundary::Interim(out_engine_status_name, [&] {
    return EncodeBatchPsNBody(encoder, docs_content_ids, docs_n_content, doc_count, chunk_budget,
                              out_vectors, out_capacity_bytes, out_required_bytes, out_failed_index,
                              out_dequantize_status, out_engine_status_name, out_seam_stream,
                              out_seam_stream_capacity, out_seam_stream_len);
  });
}


// U2-9 Builds item 2: the end-of-open release. No encode runs on the handle
// then (no caller holds it yet), so every record's buffer is released; the
// records themselves stay. A boundary by U2-10 Builds item 6.
static SembInterimStatus ReleaseWorkspacesBody(SembInterimEncoder* encoder) {
  if (!encoder || !encoder->model) return kSembInterimNullArgument;
  semb::EncoderModel* const m = encoder->model;
  std::lock_guard<std::mutex> lock(m->workspace_mutex);
  for (const std::unique_ptr<semb::WorkspaceSlot>& record : m->workspaces) {
    record->buffer.reset();
    record->capacity = 0;
  }
  return kSembInterimOk;
}

extern "C" SembInterimStatus semb_interim_release_workspaces(SembInterimEncoder* encoder) {
  return semb::c_boundary::Interim(nullptr, [&] { return ReleaseWorkspacesBody(encoder); });
}

#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
// ---------------------------------------------------------------------------
// U2 cloud build, the seam commit: the C surface of
// tests/support/semb_u2_memory_seam.h (see the hook block near the top of this
// file). Readers read today's state on today's pool.
// ---------------------------------------------------------------------------
extern "C" uint64_t semb_test_seam_slot_record_count(const SembInterimEncoder* enc) {
  if (!enc || !enc->model) return 0;
  std::lock_guard<std::mutex> lock(enc->model->workspace_mutex);
  return enc->model->workspaces.size();
}

extern "C" int32_t semb_test_seam_slot_capacity(const SembInterimEncoder* enc, uint64_t index,
                                                uint64_t* out_capacity_bytes) {
  if (!enc || !enc->model || !out_capacity_bytes) return 0;
  std::lock_guard<std::mutex> lock(enc->model->workspace_mutex);
  if (index >= enc->model->workspaces.size()) return 0;
  *out_capacity_bytes = enc->model->workspaces[index]->capacity;
  return 1;
}

extern "C" uint64_t semb_test_seam_model_buffer_capacity(const SembInterimEncoder* enc) {
  if (!enc || !enc->model) return 0;
  return enc->model->model_bytes.capacity();
}

extern "C" uint64_t semb_test_seam_slot_growths(void) { return semb::test_seam::slot_growths.load(); }

extern "C" uint64_t semb_test_seam_ps_n_select_count(void) { return semb::test_seam::ps_n_select_calls.load(); }

extern "C" void semb_test_seam_arm_fill(int32_t byte) {
  semb::test_seam::fill_byte.store(byte < 0 ? -1 : (byte & 0xFF));
}

extern "C" void semb_test_seam_arm_forced_reference(int32_t on) {
  semb::test_seam::forced_reference.store(on ? 1 : 0);
}

extern "C" void semb_test_seam_arm_growth_fail(uint64_t min_c) {
  std::lock_guard<std::mutex> lock(semb::test_seam::growth_fail_mutex);
  semb::test_seam::growth_fail_min_c = min_c;
  semb::test_seam::growth_fail_fired = false;
  semb::test_seam::growth_fail_capacity_at_fire = 0;
}

extern "C" int32_t semb_test_seam_growth_fail_fired(uint64_t* out_capacity_at_fire) {
  std::lock_guard<std::mutex> lock(semb::test_seam::growth_fail_mutex);
  if (out_capacity_at_fire) *out_capacity_at_fire = semb::test_seam::growth_fail_capacity_at_fire;
  return semb::test_seam::growth_fail_fired ? 1 : 0;
}

extern "C" void semb_test_seam_arm_poison(uint64_t split, uint64_t pos_begin, uint64_t pos_end,
                                          uint8_t byte) {
  std::lock_guard<std::mutex> lock(semb::test_seam::poison_mutex);
  semb::test_seam::poison_armed = true;
  semb::test_seam::poison_fired = false;
  semb::test_seam::poison_split = split;
  semb::test_seam::poison_begin = pos_begin;
  semb::test_seam::poison_end = pos_end;
  semb::test_seam::poison_byte = byte;
}

extern "C" int32_t semb_test_seam_poison_disarm(void) {
  std::lock_guard<std::mutex> lock(semb::test_seam::poison_mutex);
  semb::test_seam::poison_armed = false;
  const bool fired = semb::test_seam::poison_fired;
  semb::test_seam::poison_fired = false;
  return fired ? 1 : 0;
}

extern "C" int32_t semb_test_seam_gate_arm_park(uint32_t count) {
  std::lock_guard<std::mutex> lock(semb::test_seam::state_mutex);
  if (semb::test_seam::gate_parked != 0) return 0;
  semb::test_seam::gate_park_budget = count;
  return 1;
}

extern "C" int32_t semb_test_seam_gate_wait_parked(uint32_t n, uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(semb::test_seam::state_mutex);
  return semb::test_seam::state_changed.wait_for(
      lock, std::chrono::milliseconds(timeout_ms),
      [n] { return semb::test_seam::gate_parked >= n; }) ? 1 : 0;
}

extern "C" uint32_t semb_test_seam_gate_parked(void) {
  std::lock_guard<std::mutex> lock(semb::test_seam::state_mutex);
  return semb::test_seam::gate_parked;
}

extern "C" void semb_test_seam_gate_release(void) {
  std::lock_guard<std::mutex> lock(semb::test_seam::state_mutex);
  ++semb::test_seam::gate_release_generation;
  semb::test_seam::gate_parked = 0;
  semb::test_seam::state_changed.notify_all();
}

extern "C" uint32_t semb_test_seam_gate_holders(void) {
  std::lock_guard<std::mutex> lock(semb::test_seam::state_mutex);
  return semb::test_seam::gate_holders;
}

extern "C" uint32_t semb_test_seam_gate_high_water(void) {
  std::lock_guard<std::mutex> lock(semb::test_seam::state_mutex);
  return semb::test_seam::gate_high_water;
}

extern "C" uint32_t semb_test_seam_gate_waiters(void) {
  std::lock_guard<std::mutex> lock(semb::test_seam::state_mutex);
  return semb::test_seam::gate_waiters;
}

extern "C" uint64_t semb_test_seam_gate_take_records(uint64_t* out, uint64_t cap) {
  std::lock_guard<std::mutex> lock(semb::test_seam::state_mutex);
  const auto& r = semb::test_seam::gate_take_records;
  for (uint64_t i = 0; out && i < cap && i < r.size(); ++i) out[i] = r[i];
  return r.size();
}

extern "C" uint64_t semb_test_seam_gate_give_records(uint64_t* out, uint64_t cap) {
  std::lock_guard<std::mutex> lock(semb::test_seam::state_mutex);
  const auto& r = semb::test_seam::gate_give_records;
  for (uint64_t i = 0; out && i < cap && i < r.size(); ++i) out[i] = r[i];
  return r.size();
}

extern "C" int32_t semb_test_seam_gate_reset(void) {
  std::lock_guard<std::mutex> lock(semb::test_seam::state_mutex);
  if (semb::test_seam::gate_parked != 0) return 0;
  semb::test_seam::gate_park_budget = 0;
  semb::test_seam::gate_holders = 0;
  semb::test_seam::gate_high_water = 0;
  semb::test_seam::gate_waiters = 0;
  semb::test_seam::gate_take_records.clear();
  semb::test_seam::gate_give_records.clear();
  ++semb::test_seam::gate_release_generation;
  return 1;
}

extern "C" void semb_test_seam_arm_spawn_fail(uint32_t k) {
  std::lock_guard<std::mutex> lock(semb::test_seam::spawn_mutex);
  semb::test_seam::spawn_fail_k = k;
  semb::test_seam::spawn_seen = 0;
  semb::test_seam::spawn_current_call = 0;
  semb::test_seam::spawn_fired = false;
  semb::test_seam::spawn_fired_call = 0;
  semb::test_seam::spawn_starts_after = 0;
}

extern "C" int32_t semb_test_seam_spawn_fail_fired(uint32_t* out_starts_after) {
  std::lock_guard<std::mutex> lock(semb::test_seam::spawn_mutex);
  if (out_starts_after) *out_starts_after = semb::test_seam::spawn_starts_after;
  return semb::test_seam::spawn_fired ? 1 : 0;
}
#endif

// TE-166 (U1 slice 4, C34 leg (e)): this TU's own compiled-in adapter
// identity (`Claude/Plans/SuperEmbedder_Plan.md` sec 3.4's label-check
// table: engine_path_id is checked against the linked adapter TU's own
// compiled-in identity below). This is the ONE translation unit in this
// library that names EmbedEntry/RunLayerLoopChunkBatched/etc. (this
// file's own header comment; C39's singleton scan enforces it) -- the
// value below names the interim adapter itself, distinct from any future
// second adapter U3 might add. A plain string literal, located and
// mutated BY NAME in a scratch clone by
// `tests/te163_c34_manifest_honesty_check.py`'s own leg (e), never by a
// linked symbol lookup -- ITS OWN VALUE MUST NOT APPEAR QUOTED ANYWHERE
// ELSE EARLIER IN THIS FILE (that mutation-driver replaces only the
// FIRST quoted occurrence of the literal in the whole file, so an
// earlier quoted copy in a comment would silently absorb the edit and
// leave this constant, the one this bake actually reads, untouched).
constexpr char SEMB_ADAPTER_SELF_ID[] = "interim";

extern "C" const char* semb_adapter_self_id() { return SEMB_ADAPTER_SELF_ID; }
