// The frozen public raw-text encoder vertical slice. Every document-side entry
// -- semb_encode (a batch of one), semb_encode_batch and W-B's open-time
// derivation -- reaches the one batch composition, ComposeBatch: the caller's
// span is validated, passed unchanged to SuperSLM's tokenizer, and the emitted
// ids enter the PS-N composition already exercised by the row instrument.
#include "superembedder/semb.h"

#include "superembedder/encoder_interim_abi.h"
#include "superembedder/seam_abi.h"
#include "superembedder/witness_abi.h"
#include "superembedder/wb_digest_abi.h"
#include "superembedder/wq_digest_abi.h"
#include "superembedder/bake_abi.h"
#include "superembedder/manifest_labels_abi.h"
#include "superembedder/version.h"
#include "encoder_open_internal.h"
#include "engine_adapter_gpu.h"
#include "certification.h"
#include "query_internal.h"
#include "wb_corpus.h"
#include "json_lite.h"
#include "c_boundary.h"

// Generated at configure time (cmake/EngineGuards.cmake -> semb_engine_pin.h.in),
// a plain configure_file() template with no dependency on this target's own
// source-closure scan -- safe to include here, unlike generated/semb_provenance.h
// (see semb_bake_to_path's own provenance-block comment for why THAT one is not).
#include "semb_engine_pin.h"
#include "engine_link_check.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "superslm/artifact.h"
#include "superslm/sha256.h"
#include "superslm/tokenizer.h"

struct semb_encoder_s {
  // The tokenizer view borrows from these exact once-read bytes. Model bytes
  // are owned by the interim adapter because its SslmModelView borrows them.
  std::vector<uint8_t> tokenizer_bytes;
  SembInterimEncoder* interim = nullptr;
  superslm::SslmArtifact tokenizer_artifact;
  superslm::TokenizerView tokenizer;
  uint32_t dims = 0;
  uint32_t vocab_size = 0;
  uint32_t context_cap = 0;
  size_t chunk_budget = 0;
  // W-B's own digest for this handle's artifacts, derived once at open (see
  // ComputeWbDigest's own comment) and returned by semb_wb_digest without a
  // second computation -- D-SLM3978's no-cache rule binds the FINGERPRINT
  // derivation (no re-read of a file another field derived from), not how
  // many times a diagnostic accessor is called against an already-open
  // handle (witness_abi.h's own semb_wb_digest comment).
  std::array<uint8_t, 32> wb_digest{};
  std::array<uint8_t, 32> wq_digest{};
  // The five clear-text fingerprint components the manifest records and the
  // encoder fingerprint over them (sec 3.3) -- derived once at open, never
  // re-derived at bake (D-SLM3978's no-cache rule: no re-read of a file
  // another field was derived from). `fingerprint` is the one value
  // semb_encode and semb_fingerprint_of return, every baked manifest's
  // encoder_fingerprint, and what semb_query_open compares. See
  // ComputeManifestFingerprintComponents's own comment.
  std::array<uint8_t, 32> manifest_model_sha{};
  std::array<uint8_t, 32> manifest_tokenizer_sha{};
  std::array<uint8_t, 32> manifest_template_sha256{};
  std::array<uint8_t, 32> manifest_witness_sha{};
  std::array<uint8_t, 32> manifest_certification_epoch{};
  std::array<uint8_t, 32> fingerprint{};
  std::atomic<uint32_t> active_calls{0};
  // A query is a library-owned child handle.  It retains the encoder for
  // its full lifetime, independently of an encode call currently in flight.
  // `semb_encoder_close` refuses while this count is nonzero and frees
  // nothing, so a successful close can never poison a live query.
  uint32_t live_queries = 0;
  // TE-165 (N-1, D-SLM6383; D-SLM6390 item 4): false from construction
  // until the fingerprint and wb_digest above are both written. The
  // registry lookup (FindReadyHandleLocked, below) refuses any handle
  // whose ready flag is not yet true, so a cookie inserted ahead of the
  // W-B derivation -- needed so the derivation can reach this handle
  // through the ordinary composition path, see semb_encoder_open's own
  // comment -- names no live handle to any OTHER caller until this
  // becomes true. Set with release ordering once the fingerprint is
  // written; read with acquire ordering by every lookup, so a caller that
  // observes ready == true also observes every field written before it.
  std::atomic<bool> ready{false};
  // U3 (te266 sec 4.1-4.3): the forward this handle's encodes run on
  // (semb_config.device), and, on a GPU handle, the adapter's labels for the
  // manifest. A CPU handle writes no gpu_* label.
  uint32_t device = SEMB_DEVICE_CPU;
  semb::gpu::AdapterLabels gpu_labels;
  // The id the composition appends to every document and query (the
  // tokenizer's <|endoftext|>), for the GPU path's composed ids.
  int32_t appended_token_id = 0;
  // A GPU handle's encoder, set once its open-time agreement check passed.
  semb::gpu::Encoder* gpu = nullptr;
  // Non-null only while a GPU open's CPU witness runs: W-B and W-Q append
  // each sequence's composed ids and CPU triple here for the agreement check
  // (T388-11). A CPU open never sets it and computes no triple.
  std::vector<semb::gpu::WitnessSequence>* witness_capture = nullptr;

  ~semb_encoder_s() {
    // The GPU handles first (te266 sec 4.2): sequences, map, context.
    if (gpu) semb::gpu::Close(gpu);
    if (interim) semb_interim_encoder_close(interim);
  }
};

namespace {

constexpr char kCanonicalEndToken[] = "<|endoftext|>";

// Manifest provenance labels 1.0 does not compute -- present, never omitted,
// never fabricated: each carries a named `SEMB-...` label in place of a
// value (tests/semb_bake_abi_contract.h). None is a fingerprint input.
// Named per field so each is independently greppable.
//
// object_code_sha / engine_object_code_sha: "the SHA-256 of THIS process's
// own compiled object code" has no runtime-readable source in this tree --
// C34's own semb_source_sha is a BUILD-TIME provenance digest
// (cmake/SourceProvenance.cmake), and reading its own generated header
// (generated/semb_provenance.h) back from a file inside THIS target's own
// covered-source list would make the closure the digest is computed over
// include the digest's own output file, which does not exist at the
// moment the closure is scanned -- a genuine circularity, not a gap left
// idle. semb_source_sha therefore also carries a label here (see below).
// Computing object_code_sha, engine_object_code_sha and the CPU tier is
// 1.x scope (D-SLM7212), as is reporting semb_source_sha in the manifest
// (D-SLM7257).
constexpr char kObjectCodeShaUncomputedLabel[] = "SEMB-OBJECT-CODE-SHA-NOT-COMPUTED-IN-1.0";
constexpr char kEngineObjectCodeShaUncomputedLabel[] =
    "SEMB-ENGINE-OBJECT-CODE-SHA-NOT-COMPUTED-IN-1.0";
constexpr char kSourceShaUncomputedLabel[] = "SEMB-SOURCE-SHA-NOT-COMPUTED-IN-1.0";
// U2-3 item 8 (D-SLM7279, overriding D-SLM7212 for this one field): the
// manifest's machine_tier and provenance.witness.cpu_tier carry the tier
// this process dispatched to (certification::DispatchedTier), not a label.

// C34 legs (e)/(h) (sec 3.4's label-check table): the value THIS primitive
// is about to write into label:engine_path_id / label:ps_version,
// maintained independently of engine_adapter_interim.cpp's own
// SEMB_ADAPTER_SELF_ID / ps_n_pool.cpp's own SEMB_PS_VERSION_LABEL so the
// bake-time comparison is genuinely two compiled sources, not one value
// read twice -- see semb_bake_to_path's own comment and
// tests/te163_c34_manifest_honesty_check.py's own single-file source
// mutation, which edits only the TU-local constant and rebuilds.
constexpr char kEngineAdapterIdLabel[] = "interim";
constexpr char kPsVersionLabel[] = "PS-N";

std::string Hex(const uint8_t* d, size_t n) {
  static const char* kHexDigits = "0123456789abcdef";
  std::string out(n * 2, '0');
  for (size_t i = 0; i < n; ++i) {
    out[2 * i] = kHexDigits[(d[i] >> 4) & 0xF];
    out[2 * i + 1] = kHexDigits[d[i] & 0xF];
  }
  return out;
}

std::mutex& RegistryMutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}

// R20 (U2-1): an ordered container. Its member functions do no floating-point
// arithmetic, so no certified object carries a substitutable floating-point
// instantiation for it; the registry is read only by find, emplace and erase,
// never iterated, so its order changes nothing.
using HandleRegistry = std::map<semb_encoder, std::unique_ptr<semb_encoder_s>>;

HandleRegistry& Registry() {
  static auto* registry = new HandleRegistry();
  return *registry;
}

// The ABI type is an opaque pointer, but the published value is a monotonic
// cookie, never an allocation address. Registry entries own the actual handle
// objects and close erases them normally. A stale cookie therefore cannot
// become an alias through allocator address reuse, and closed handles retain
// no per-handle tombstone storage.
bool AllocateHandleCookie(semb_encoder* out) {
  static uintptr_t next = 1;
  if (next == 0) return false;  // The pointer-width cookie space was exhausted.
  *out = reinterpret_cast<semb_encoder>(next++);
  return true;
}

// TE-165 (N-1, D-SLM6383; D-SLM6390 item 4): the ONE lookup every public
// accessor of a cookie goes through, and the one place "ready" is checked.
// A cookie whose handle exists in the registry but has not yet finished
// opening (ready == false) is treated exactly as if the cookie named no
// handle at all -- SEMB_INVALID_HANDLE to a caller, never a half-built
// handle with a zero fingerprint. Must be called with RegistryMutex()
// held; the returned iterator is Registry().end() on any refusal.
HandleRegistry::iterator FindReadyHandleLocked(semb_encoder enc) {
  const auto found = Registry().find(enc);
  if (found == Registry().end()) return Registry().end();
  if (!found->second->ready.load(std::memory_order_acquire)) return Registry().end();
  return found;
}

class HandleLease {
 public:
  explicit HandleLease(semb_encoder enc) {
    if (!enc) return;
    std::lock_guard<std::mutex> lock(RegistryMutex());
    const auto found = FindReadyHandleLocked(enc);
    if (found == Registry().end()) return;
    found->second->active_calls.fetch_add(1, std::memory_order_acq_rel);
    handle_ = found->second.get();
  }

  ~HandleLease() {
    if (handle_) handle_->active_calls.fetch_sub(1, std::memory_order_acq_rel);
  }

  HandleLease(const HandleLease&) = delete;
  HandleLease& operator=(const HandleLease&) = delete;
  semb_encoder_s* get() const { return handle_; }

 private:
  semb_encoder_s* handle_ = nullptr;
};

// TE-165 (N-2, D-SLM6384; D-SLM6390 item 4): a guard object, not a second
// rollback line -- erases the just-inserted, not-yet-ready registry entry
// on ANY exit from semb_encoder_open between insertion and Disarm(),
// including an exception unwinding through that function's own catch
// blocks (a std::bad_alloc from inside the W-B derivation's own
// allocations, for instance). A `return` in between runs this
// destructor exactly as unwinding does, so the same guard covers both
// exit shapes with one piece of code -- no second, easily-missed erase
// call at each new failure branch.
class HandleInsertGuard {
 public:
  explicit HandleInsertGuard(semb_encoder cookie) : cookie_(cookie) {}

  ~HandleInsertGuard() {
    if (!cookie_) return;
    std::lock_guard<std::mutex> lock(RegistryMutex());
    Registry().erase(cookie_);
  }

  void Disarm() { cookie_ = nullptr; }

  HandleInsertGuard(const HandleInsertGuard&) = delete;
  HandleInsertGuard& operator=(const HandleInsertGuard&) = delete;

 private:
  semb_encoder cookie_;
};

bool ReadFileOnce(std::string_view path_span, std::vector<uint8_t>* out) {
  if (path_span.empty() || path_span.find('\0') != std::string_view::npos) return false;
  const std::string path(path_span);
  try {
    std::ifstream stream;
    // Propagate allocation exceptions from stream operations to the ABI's
    // allocation-failure boundary. Ordinary I/O exceptions mean unreadable.
    stream.exceptions(std::ios::badbit);
    stream.open(path, std::ios::binary | std::ios::ate);
    if (!stream) return false;
    const std::streamoff end = stream.tellg();
    if (end < 0 || static_cast<uintmax_t>(end) > std::numeric_limits<size_t>::max() ||
        static_cast<uintmax_t>(end) >
            static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())) return false;
    out->resize(static_cast<size_t>(end));
    stream.seekg(0, std::ios::beg);
    if (!stream) return false;
    // A successfully read empty file is delivered to the artifact boundary.
    if (end == 0) return true;
    return static_cast<bool>(stream.read(reinterpret_cast<char*>(out->data()),
                                         static_cast<std::streamsize>(end)));
  } catch (const std::ios_base::failure&) {
    return false;
  }
}

void Hash(const void* data, size_t size, uint8_t out[32]) {
  superslm::Sha256Hash(static_cast<const uint8_t*>(data), size, out);
}

void UpdateU64(superslm::Sha256* hash, uint64_t value) {
  uint8_t bytes[8];
  for (size_t i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(value >> (8 * i));
  hash->Update(bytes, sizeof(bytes));
}

void UpdateField(superslm::Sha256* hash, const uint8_t field[32]) {
  UpdateU64(hash, 32);
  hash->Update(field, 32);
}

// TE-165: relocated ahead of ComposeBatch (below), which calls it -- was
// previously defined after every other function in this file's own
// anonymous namespace, when only semb_encode/semb_encode_batch (further
// below, outside the namespace) needed it.
semb_status MapInterimStatus(SembInterimStatus status, SembWaPoolStatus dequantize) {
  switch (status) {
    case kSembInterimOk: return SEMB_OK;
    case kSembInterimNullArgument: return SEMB_NULL_ARGUMENT;
    case kSembInterimInputEmpty: return SEMB_INPUT_EMPTY;
    case kSembInterimBufferTooSmall: return SEMB_BUFFER_TOO_SMALL;
    case kSembInterimConcurrencyExceeded: return SEMB_CONCURRENCY_EXCEEDED;
    // U2-10 Builds item 3: the interim family's allocation status, and a
    // dequantize refusal carrying the pool family's, are this verb's own
    // allocation status; the pool's internal failure is never a corner status.
    case kSembInterimAllocationFailed: return SEMB_ALLOCATION_FAILED;
    case kSembInterimDequantizeRefused:
      if (dequantize == kSembPooledSubnormal) return SEMB_POOLED_SUBNORMAL;
      if (dequantize == kSembPooledOverflow) return SEMB_POOLED_OVERFLOW;
      if (dequantize == kSembWaPoolAllocationFailed) return SEMB_ALLOCATION_FAILED;
      return SEMB_ENGINE_REFUSED;
    case kSembInterimEngineRefused:
    case kSembInterimInvalidLeg:
    case kSembInterimPositionOutOfRange:
    case kSembInterimStructSizeMismatch: return SEMB_ENGINE_REFUSED;
  }
  return SEMB_ENGINE_REFUSED;
}

// TE-184 (N-8, D-SLM6406): the document frame -- the empty byte string
// under PS-N (D-SLM4726), the concatenation point a future PS-1 frame
// lands at without changing ComposeBatch's own shape (its comment, below)
// -- named here as ONE constant and concatenated through ONE function, so
// every caller that needs to know what ComposeBatch will tokenize for a
// document (ComposeBatch itself, and CutWbDocumentText's own budget below)
// reads the SAME frame rather than a second, independently-stated one.
constexpr std::string_view kDocumentFrame = "";

void UpdateLengthPrefixed(superslm::Sha256* hash, std::string_view bytes) {
  UpdateU64(hash, bytes.size());
  hash->Update(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

// template_sha256 (plan sec 3.3, D-8): SHA-256 over the three template
// literals the composition applies, each as LE64(length) || bytes, in this
// order -- the document frame (empty under PS-N), the query instruction
// prefix, and the appended end-token literal. Each is read from its one
// definition (kDocumentFrame, semb::kQueryPrefix, kCanonicalEndToken), so a
// change to any literal the product composes with moves this digest, and no
// length is stated a second time here.
std::array<uint8_t, 32> ComputeTemplateDigest() {
  superslm::Sha256 hash;
  UpdateLengthPrefixed(&hash, kDocumentFrame);
  UpdateLengthPrefixed(&hash, std::string_view(semb::kQueryPrefix));
  UpdateLengthPrefixed(&hash, std::string_view(kCanonicalEndToken));
  std::array<uint8_t, 32> out{};
  hash.Final(out.data());
  return out;
}

// certification_epoch (plan U2-3 items 1 and 9): the cell verdict's epoch --
// SHA-256 of the certification identity file's exact bytes as this build
// compiled them in when this build, at the tier this process dispatched to,
// is a proven cell of the compiled-in list; otherwise SHA-256 of
// "SEMB-UNCERTIFIED\0" followed by those bytes. Decided once per process at
// the first open, never read from a file at run time.
std::array<uint8_t, 32> ComputeCertificationEpoch() {
  return semb::certification::ProcessVerdict().epoch;
}

// The exact byte sequence ComposeBatch tokenizes for one document: the
// document frame, then the document's own text -- no second concatenation
// point exists anywhere else in this file (D-SLM6390 item 1's "no
// tokenize-or-cut code exists on the W-B path outside this function",
// extended to the frame's own length: a budget computed without calling
// this function is a second, independent statement of the composition,
// which is exactly N-8's own defect).
std::string ComposeDocumentInput(std::string_view text) {
  std::string out;
  out.reserve(kDocumentFrame.size() + text.size());
  out.append(kDocumentFrame);
  out.append(text);
  return out;
}

// U3 (te266 sec 4.2, "Encode"): a GPU triple goes into the existing PS-N
// dequantize (P5), mapped exactly as the CPU path maps the same refusal.
semb_status DequantizeTriple(const semb::gpu::Triple& t, size_t hidden, float* out) {
  if (t.codes.size() != hidden) return SEMB_ENGINE_REFUSED;
  const SembWaPoolStatus dq = semb_ps_n_dequantize(t.codes.data(), t.m, t.e, hidden, out);
  if (dq == kSembOk) return SEMB_OK;
  if (dq == kSembWaPoolAllocationFailed) return SEMB_ALLOCATION_FAILED;
  if (dq == kSembWaPoolInternalFailure) return SEMB_ENGINE_REFUSED;
  return MapInterimStatus(kSembInterimDequantizeRefused, dq);
}

// Runs `content_ids` (each without its terminator) on the handle's GPU
// encoder, one after another on one slot, and writes each row. The callers
// stage `out_vectors`, so a refusal at any document reaches no caller buffer.
semb_status ComposeBatchGpu(semb_encoder_s* h, const std::vector<std::vector<int32_t>>& content_ids,
                            float* out_vectors, size_t out_capacity_bytes,
                            size_t* out_required_bytes) {
  const size_t n = content_ids.size();
  const size_t hidden = h->dims;
  const size_t required = n * hidden * sizeof(float);
  if (out_required_bytes) *out_required_bytes = required;
  if (out_capacity_bytes < required) return SEMB_BUFFER_TOO_SMALL;
  if (!out_vectors) return SEMB_NULL_ARGUMENT;
  std::vector<std::vector<int32_t>> composed(content_ids.begin(), content_ids.end());
  std::vector<const int32_t*> ptrs(n);
  std::vector<size_t> lens(n);
  for (size_t i = 0; i < n; ++i) {
    composed[i].push_back(h->appended_token_id);
    ptrs[i] = composed[i].data();
    lens[i] = composed[i].size();
  }
  std::vector<semb::gpu::Triple> triples;
  const semb_status st = semb::gpu::EncodeSequences(h->gpu, ptrs.data(), lens.data(), n, &triples);
  if (st != SEMB_OK) return st;
  if (triples.size() != n) return SEMB_ENGINE_REFUSED;
  for (size_t i = 0; i < n; ++i) {
    const semb_status dq = DequantizeTriple(triples[i], hidden, out_vectors + i * hidden);
    if (dq != SEMB_OK) return dq;
  }
  return SEMB_OK;
}

// TE-165 (D-SLM6390, the conductor's ruling re-opening D-SLM6354 on
// TE-161's own measurement, N-3/D-SLM6385): ONE internal batch
// composition -- everything semb_encode_batch does after validating the
// caller's own buffers: per-document tokenization, the document frame
// (empty under PS-N, D-SLM4726), the >= context_cap refusal, the
// appended terminator, the workspace acquisition(s) (TE-350: at most one
// per worker thread, all released before return), the
// chunk-batched forward -- called by the public batch verb, by the raw
// entry semb_encode as a batch of one (D-SLM7197), and by ComputeWbDigest,
// the latter with the seam tapped instead of the row output. No
// tokenize-or-cut code exists on the W-B path outside this
// function: TE-159's own remedy (C-2, D-SLM6354) shared only the INTERIM
// function (semb_interim_encode_batch_ps_n) and kept a second,
// independent tokenize-and-cut inside ComputeWbDigest, which TE-161's
// review proved (a document-frame perturbation at THIS composition's own
// tokenize step left wb_digest and the emitted fingerprint
// byte-identical) never reached the batch verb's own composition at all
// -- N-3. `texts[i]` is each document's OWN input text; W-B's own sixteen
// documents are prepared as text before the call (CutWbDocumentText,
// below) rather than tokenized and cut here a second time.
semb_status ComposeBatch(semb_encoder_s* h, const std::string_view* texts, size_t n,
                         float* out_vectors, size_t out_capacity_bytes,
                         size_t* out_required_bytes, uint8_t* out_seam_stream,
                         size_t out_seam_stream_capacity, size_t* out_seam_stream_len,
                         std::vector<std::vector<int32_t>>* out_composed_ids = nullptr) {
  // Per-document tokenization, with the document frame applied through
  // ComposeDocumentInput (TE-184, N-8, D-SLM6406 -- the empty byte string
  // under PS-N, D-SLM4726, concatenated at the ONE point named above, so a
  // future PS-1 frame lands here without changing this function's own
  // shape) and the >= context_cap refusal (TE-96 sec 4.2 step 11), stopping
  // at the first document that fails: identical to semb_encode_batch's own
  // prior step 11, now the ONE place either caller reaches it. This check
  // is also the guard M-3 named removed: D-SLM6390 item 2 makes it
  // reachable in principle (a bug in CutWbDocumentText's own prefix search
  // could hand this function text that still tokenizes over budget), and
  // this check -- shared with the public verb -- refuses it here rather
  // than overrunning the K/V workspace sec 3.1 sizes from context_cap.
  std::vector<std::vector<int32_t>> content_ids(n);
  for (size_t i = 0; i < n; ++i) {
    content_ids[i] = h->tokenizer.Encode(ComposeDocumentInput(texts[i]));
    // Defensive, matching the raw path's own equivalent check: zero
    // content ids is unreachable for byte-level BPE over a non-empty
    // span. W-B's own texts are never empty (CutWbDocumentText never
    // returns an empty prefix against a real corpus document).
    if (content_ids[i].empty()) return SEMB_INPUT_EMPTY;
    if (content_ids[i].size() >= h->context_cap) return SEMB_CONTEXT_CAP_EXCEEDED;
  }

  // U3: a GPU handle's documents run on the GPU (sec 4.2, "Encode"). The
  // seam tap (W-B at open) is CPU-only by construction: the GPU encoder has
  // no per-position seam.
  if (h->gpu && !out_seam_stream) {
    return ComposeBatchGpu(h, content_ids, out_vectors, out_capacity_bytes, out_required_bytes);
  }

  std::vector<const int32_t*> doc_ptrs(n);
  std::vector<size_t> doc_lens(n);
  for (size_t i = 0; i < n; ++i) {
    doc_ptrs[i] = content_ids[i].data();
    doc_lens[i] = content_ids[i].size();
  }
  // U3 (T388-11): a GPU open's W-B sequences, as the GPU will be given them.
  if (out_composed_ids) {
    out_composed_ids->assign(content_ids.begin(), content_ids.end());
    for (auto& ids : *out_composed_ids) ids.push_back(h->appended_token_id);
  }

  // The appended terminator, the workspace acquisition(s) (TE-350: at most
  // one per worker thread) and the chunk-batched forward all live in the
  // interim layer's own composed path, reached identically by the public
  // verb and by W-B's seam tap.
  SembWaPoolStatus dequantize = kSembOk;
  const char* engine_status = nullptr;
  size_t failed_index = 0;
  const SembInterimStatus status = semb_interim_encode_batch_ps_n(
      h->interim, doc_ptrs.data(), doc_lens.data(), n, h->chunk_budget, out_vectors,
      out_capacity_bytes, out_required_bytes, &failed_index, &dequantize, &engine_status,
      out_seam_stream, out_seam_stream_capacity, out_seam_stream_len);
  (void)engine_status;
  (void)failed_index;  // D-SLM4653: the enum name is the whole diagnosis (TE-96 sec 4.2's own note)
  return MapInterimStatus(status, dequantize);
}

// The one query-side forward composition. Query text is composed before this
// function is called; it tokenizes that whole composition, then applies the
// caller-selected content ceiling, leaving the interim layer to append the
// canonical special token. W-Q and semb_query_encode both reach this path.
// The query side's one tokenization of a composed query: the encode below and
// the body-token count (semb_interim_query_token_count) both call it.
std::vector<int32_t> TokenizeComposedQuery(const semb_encoder_s* h, std::string_view composed) {
  return h->tokenizer.Encode(composed);
}

semb_status EncodeQueryComposedImpl(semb_encoder_s* h, std::string_view composed,
                                    size_t max_content_ids,
                                    std::vector<float>* out_vector) {
  if (!h || !out_vector) return SEMB_NULL_ARGUMENT;
  std::vector<int32_t> ids = TokenizeComposedQuery(h, composed);
  if (ids.empty()) return SEMB_INPUT_EMPTY;
  const size_t context_limit = h->context_cap > 0 ? h->context_cap - 1 : 0;
  const size_t permitted = std::min(context_limit, max_content_ids);
  if (permitted == 0) return SEMB_CONTEXT_CAP_EXCEEDED;
  if (ids.size() > permitted) ids.resize(permitted);

  const size_t required = static_cast<size_t>(h->dims) * sizeof(float);
  std::vector<float> staged(h->dims);
  // U3: a GPU handle's queries run on the GPU, composed exactly as on the CPU.
  if (h->gpu && !h->witness_capture) {
    std::vector<std::vector<int32_t>> one(1, std::move(ids));
    const semb_status st = ComposeBatchGpu(h, one, staged.data(), required, nullptr);
    if (st != SEMB_OK) return st;
    *out_vector = std::move(staged);
    return SEMB_OK;
  }
  SembWaPoolStatus dequantize = kSembOk;
  const char* engine_status = nullptr;
  size_t interim_required = 0;
  SembInterimStatus status;
  semb::InterimTriple triple;
  if (h->witness_capture) {
    // A GPU open's W-Q sequence (T388-11): the witness's own query forward,
    // which also hands back the triple it selected. No second forward runs.
    status = semb::EncodePsNWithTriple(h->interim, ids.data(), ids.size(), h->chunk_budget,
                                       staged.data(), required, &interim_required, &dequantize,
                                       &engine_status, &triple);
  } else {
    status = semb_interim_encode_ps_n(h->interim, ids.data(), ids.size(), h->chunk_budget,
                                      staged.data(), required, &interim_required, &dequantize,
                                      &engine_status);
  }
  (void)engine_status;
  const semb_status mapped = MapInterimStatus(status, dequantize);
  if (mapped != SEMB_OK) return mapped;
  if (h->witness_capture) {
    semb::gpu::WitnessSequence w;
    w.ids = ids;
    w.ids.push_back(h->appended_token_id);
    w.cpu.codes = std::move(triple.codes);
    w.cpu.m = triple.m;
    w.cpu.e = triple.e;
    h->witness_capture->push_back(std::move(w));
  }
  *out_vector = std::move(staged);
  return SEMB_OK;
}

// TE-165 (D-SLM6390 item 2), corrected TE-184 (N-8, D-SLM6406): prepares
// one W-B document as TEXT -- the longest UTF-8-boundary byte prefix such
// that ComposeDocumentInput(prefix)'s tokenized length under `enc`'s OWN
// tokenizer is at most context_cap - 1 -- so ComposeBatch, above, never
// has to cut internally on the W-B path. D-SLM6341's original rule ("cut
// at context_cap - 1 at open") is kept, restated as an INPUT preparation
// rather than a second tokenize-and-cut inside the composition: exactly
// what a real caller with an oversized document would have to do for
// itself before calling semb_encode_batch, since the composition refuses
// rather than cuts. The budget below is checked against the SAME composed
// sequence -- frame then text, via ComposeDocumentInput, the identical
// function ComposeBatch calls -- so a future PS-1 frame's own tokenized
// length is included automatically, with no second, independent
// tokenize-or-cut path stating the frame's length differently (N-8's own
// defect: the budget was checked against the bare text while ComposeBatch
// tokenized frame+text, so a non-empty frame could push the composed
// length over context_cap after this function had already returned the
// whole document as "under budget"). Against the real candidate this
// never cuts (every committed document is short prose, well under any
// realistic context_cap, and the frame is empty); against the mechanism
// pair every document is cut, deterministically, to the same leading byte
// prefix on every call (tokenization is deterministic and the search
// below is a pure function of context_cap, the frame and the document's
// own bytes).
std::string_view CutWbDocumentText(const superslm::TokenizerView& tokenizer,
                                   std::string_view text, uint32_t context_cap) {
  // D-SLM6341: the largest content-id count that leaves room for the
  // appended end token without exceeding context_cap. context_cap == 0
  // (not reachable against a real candidate; guarded rather than assumed)
  // leaves room for no content at all.
  const size_t max_content_ids =
      context_cap > 0 ? static_cast<size_t>(context_cap) - 1 : 0;
  if (tokenizer.Encode(ComposeDocumentInput(text)).size() <= max_content_ids) return text;

  // Walk UTF-8 character boundaries forward, tokenizing each candidate
  // prefix's OWN composed input (frame + prefix, ComposeDocumentInput)
  // through the SAME tokenizer the composition will use, stopping at the
  // first boundary whose own composed tokenized length exceeds the
  // budget. Exact by construction (every candidate is checked by a real
  // Encode call over the actual composed bytes, never inferred from byte
  // length or from the frame's length added separately), and cheap: only
  // exercised against the mechanism pair's own tiny context_cap, over
  // sixteen short committed documents, once per open.
  size_t best = 0;  // the empty prefix always tokenizes to 0 ids.
  size_t i = 0;
  while (i < text.size()) {
    const uint8_t lead = static_cast<uint8_t>(text[i]);
    size_t width = 1;
    if ((lead & 0xE0) == 0xC0) width = 2;
    else if ((lead & 0xF0) == 0xE0) width = 3;
    else if ((lead & 0xF8) == 0xF0) width = 4;
    const size_t next = (i + width < text.size()) ? i + width : text.size();
    if (tokenizer.Encode(ComposeDocumentInput(text.substr(0, next))).size() > max_content_ids) break;
    best = next;
    i = next;
  }
  return text.substr(0, best);
}

// ComputeWbDigest is called once, from semb_encoder_open, once the interim
// encoder and the tokenizer's resolved end-token id are both available
// (sec 3.1: "derives all five fingerprint fields ... runs the witness");
// its own caller states where model_sha/tokenizer_sha are computed.
//
// TE-165 (D-SLM6390 items 1-3, re-opening TE-159's C-2/D-SLM6354): W-B's
// own real digest is derived by calling ComposeBatch -- the SAME
// composition semb_encode_batch itself is built on (above) -- with the
// seam tapped, never a bespoke tokenize-and-forward of its own. The
// sixteen compiled-in documents are prepared as TEXT (CutWbDocumentText,
// above) and handed to ComposeBatch exactly as any other caller's text
// would be; the byte sequence hashed (u64(document_count), then each
// document's own semb_seam_serialize_document stream in committed order)
// is unchanged from every prior round's own reading, because the
// composition's OWN tokenize call and D-SLM6341's cut, now applied to the
// prepared text, produce the identical token ids the old bespoke path
// produced -- proven by execution: this round's own build record quotes
// the unperturbed digest and fingerprint byte-identical to
// TE-147/TE-155/TE-159's own prior readings. What changed is that a
// change to the batch verb's OWN composition -- the document frame, the
// tokenizer call the batch verb makes, the truncation rule -- now moves
// this digest too, because W-B executes that composition rather than a
// second copy of it (TE-161's own frame-perturbation construction,
// re-run and quoted in this round's own build record).
//
// The seam stream's own required capacity is queried through ComposeBatch
// itself, not computed by a second, independent tokenize pass: a first
// call with a non-null, zero-capacity seam pointer is refused before any
// forward runs (the same shape-before-work discipline the interim layer
// already applies) and reports the exact required length; the second
// call, sized from that report, is the one that actually acquires a
// workspace and forwards. Two calls into the ONE composition function,
// never a second tokenizer.
//
// Called once, from semb_encoder_open, AFTER the handle already carries a
// live cookie in the registry (so the composition can reach it through
// `h` directly -- see semb_encoder_open's own comment for why the cookie
// is inserted ahead of this call, and TE-165 N-1/D-SLM6383 for why that
// cookie is not yet OBSERVABLE to any other caller: `h->ready` is still
// false) but BEFORE `h->ready` is set and `*out` is published -- "no
// caller can observe a handle without its fingerprint." A refusal here is
// unwound by HandleInsertGuard at the call site, not by a rollback line
// in this function.
//
// Returns SEMB_OK, or SEMB_ALLOCATION_FAILED when the composition could not
// allocate (U2-10 Builds item 3: a body that receives a callee's allocation
// status returns its own), or SEMB_ENGINE_REFUSED on any other refusal from
// the composition (including the composition's own >= context_cap refusal,
// restoring M-3's removed guard -- see ComposeBatch's own comment), the same
// status every other mid-open engine failure already uses.
// U3 (te266 sec 4.2, T388-11), a GPU open only: each W-B document's CPU
// triple is the last position of its slice of the seam the witness already
// computed, selected through the C verb semb_interim_ps_n_select. The seam
// is `u64 n, u64 hidden, n x (codes[hidden], u64 m, u64 e)` per document
// (seam_serialize.cpp). A selection that refuses on a witness document
// refuses the GPU open with SEMB_ENGINE_REFUSED: that document cannot be
// graded.
semb_status CaptureWbTriples(semb_encoder_s* h, const uint8_t* seam, size_t seam_len,
                             const std::vector<std::vector<int32_t>>& composed_ids) {
  auto get_u64 = [](const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
  };
  size_t off = 0;
  for (size_t doc = 0; doc < composed_ids.size(); ++doc) {
    if (seam_len - off < 16) return SEMB_ENGINE_REFUSED;
    const uint64_t n = get_u64(seam + off);
    const uint64_t hidden = get_u64(seam + off + 8);
    off += 16;
    const uint64_t stride = hidden + 16;
    if (n == 0 || hidden != h->dims || n != composed_ids[doc].size() ||
        (seam_len - off) / stride < n) {
      return SEMB_ENGINE_REFUSED;
    }
    const uint8_t* last = seam + off + (n - 1) * stride;
    const int64_t m = static_cast<int64_t>(get_u64(last + hidden));
    const int64_t e = static_cast<int64_t>(get_u64(last + hidden + 8));
    semb::gpu::WitnessSequence w;
    w.ids = composed_ids[doc];
    w.cpu.codes.resize(hidden);
    const char* engine_status = nullptr;
    const SembInterimStatus st = semb_interim_ps_n_select(
        h->interim, reinterpret_cast<const int8_t*>(last), &m, &e, 1, 0, w.cpu.codes.data(), &w.cpu.m,
        &w.cpu.e, &engine_status);
    (void)engine_status;
    if (st == kSembInterimAllocationFailed) return SEMB_ALLOCATION_FAILED;
    if (st != kSembInterimOk) return SEMB_ENGINE_REFUSED;
    h->witness_capture->push_back(std::move(w));
    off += n * stride;
  }
  return SEMB_OK;
}

semb_status ComputeWbDigest(semb_encoder_s* h, std::array<uint8_t, 32>* out_digest) {
  std::vector<std::string_view> texts(semb::kWbCorpusDocumentCount);
  for (size_t doc_i = 0; doc_i < semb::kWbCorpusDocumentCount; ++doc_i) {
    const semb::WbCorpusDocument& doc = semb::kWbCorpusDocuments[doc_i];
    texts[doc_i] = CutWbDocumentText(h->tokenizer, std::string_view(doc.text, doc.text_len),
                                     h->context_cap);
  }

  // Size the seam stream by querying ComposeBatch itself (a non-null,
  // zero-capacity seam pointer is the same capacity-query shape
  // semb_encode_batch's own step 7 uses for out_rows -- refused before
  // any forward runs, with the exact required length reported).
  uint8_t probe = 0;
  size_t rows_required_ignored = 0;
  size_t seam_required = 0;
  const semb_status sizing_status =
      ComposeBatch(h, texts.data(), texts.size(), /*out_vectors=*/nullptr,
                  /*out_capacity_bytes=*/0, &rows_required_ignored, &probe,
                  /*out_seam_stream_capacity=*/0, &seam_required);
  if (sizing_status == SEMB_ALLOCATION_FAILED) return SEMB_ALLOCATION_FAILED;
  if (sizing_status != SEMB_BUFFER_TOO_SMALL) return SEMB_ENGINE_REFUSED;

  std::vector<uint8_t> seam_stream(seam_required);
  size_t seam_len = 0;
  std::vector<std::vector<int32_t>> composed_ids;
  const semb_status status =
      ComposeBatch(h, texts.data(), texts.size(), /*out_vectors=*/nullptr,
                  /*out_capacity_bytes=*/0, &rows_required_ignored, seam_stream.data(),
                  seam_stream.size(), &seam_len, h->witness_capture ? &composed_ids : nullptr);
  if (status == SEMB_ALLOCATION_FAILED) return SEMB_ALLOCATION_FAILED;
  if (status != SEMB_OK) return SEMB_ENGINE_REFUSED;
  if (h->witness_capture) {
    const semb_status captured = CaptureWbTriples(h, seam_stream.data(), seam_len, composed_ids);
    if (captured != SEMB_OK) return captured;
  }

  std::vector<uint8_t> stream;
  stream.reserve(8 + seam_len);
  auto put_u64 = [](std::vector<uint8_t>* buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) buf->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
  };
  put_u64(&stream, static_cast<uint64_t>(semb::kWbCorpusDocumentCount));
  stream.insert(stream.end(), seam_stream.begin(), seam_stream.begin() + seam_len);

  superslm::Sha256 hash;
  hash.Update(stream.data(), stream.size());
  hash.Final(out_digest->data());
  return SEMB_OK;
}

// The encoder fingerprint's one derivation (plan sec 3.3): the five
// clear-text components the manifest records, and `encoder_fingerprint` --
// SHA-256 over the five, each length-prefixed, in this order: model_sha,
// tokenizer_sha, template_sha256, witness_sha, certification_epoch. The same
// 32 bytes are returned by semb_encode and semb_fingerprint_of, written to
// every manifest this encoder bakes, and compared by semb_query_open against
// a bank's manifest.
//
// model_sha and tokenizer_sha are computed by the CALLER, from the exact
// owned buffers semb_encoder_open reads once (sec 3.3's one-read rule) --
// BEFORE model_bytes is moved into the interim encoder's own open, which
// must happen before W-B's own forward can run. wb_digest and wq_digest are
// the witness legs' outputs, likewise computed by the caller once the
// interim encoder and the tokenizer's resolved end-token id are both
// available (sec 3.1: "derives all five fingerprint fields ... runs the
// witness"). witness_sha is SHA-256(wa_digest || wb_digest || wq_digest).
// `tests/te163_bank_pair_test.cpp` cell 3 recomputes encoder_fingerprint
// independently from a manifest's five clear-text fields
// (RecomputeFingerprintFromManifest).
struct ManifestFingerprintComponents {
  std::array<uint8_t, 32> model_sha;
  std::array<uint8_t, 32> tokenizer_sha;
  std::array<uint8_t, 32> template_sha256;
  std::array<uint8_t, 32> witness_sha;
  std::array<uint8_t, 32> certification_epoch;
  std::array<uint8_t, 32> fingerprint;
};

// Returns W-A's status (U2-10: semb_wa_digest reports) with `*out` unwritten
// on any status but SEMB_OK.
semb_status ComputeManifestFingerprintComponents(
    const uint8_t model_sha[32], const uint8_t tokenizer_sha[32],
    const std::array<uint8_t, 32>& wb_digest, const std::array<uint8_t, 32>& wq_digest,
    ManifestFingerprintComponents* out) {
  ManifestFingerprintComponents c{};
  std::memcpy(c.model_sha.data(), model_sha, 32);
  std::memcpy(c.tokenizer_sha.data(), tokenizer_sha, 32);
  c.template_sha256 = ComputeTemplateDigest();

  uint8_t wa_sha[32], wq_sha[32];
  const semb_status wa_status = semb_wa_digest(wa_sha);
  if (wa_status != SEMB_OK) return wa_status;
  std::memcpy(wq_sha, wq_digest.data(), 32);
  {
    superslm::Sha256 hash;
    hash.Update(wa_sha, 32);
    hash.Update(wb_digest.data(), 32);
    hash.Update(wq_sha, 32);
    hash.Final(c.witness_sha.data());
  }
  c.certification_epoch = ComputeCertificationEpoch();

  superslm::Sha256 fp_hash;
  UpdateField(&fp_hash, c.model_sha.data());
  UpdateField(&fp_hash, c.tokenizer_sha.data());
  UpdateField(&fp_hash, c.template_sha256.data());
  UpdateField(&fp_hash, c.witness_sha.data());
  UpdateField(&fp_hash, c.certification_epoch.data());
  fp_hash.Final(c.fingerprint.data());
  *out = c;
  return SEMB_OK;
}

bool ValidUtf8(const char* data, size_t size) {
  size_t i = 0;
  while (i < size) {
    const uint8_t lead = static_cast<uint8_t>(data[i]);
    if (lead <= 0x7F) {
      ++i;
      continue;
    }
    size_t count = 0;
    uint32_t codepoint = 0;
    uint32_t minimum = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
      count = 2;
      codepoint = lead & 0x1F;
      minimum = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      count = 3;
      codepoint = lead & 0x0F;
      minimum = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      count = 4;
      codepoint = lead & 0x07;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (count > size - i) return false;
    for (size_t j = 1; j < count; ++j) {
      const uint8_t continuation = static_cast<uint8_t>(data[i + j]);
      if ((continuation & 0xC0) != 0x80) return false;
      codepoint = (codepoint << 6) | (continuation & 0x3F);
    }
    if (codepoint < minimum || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
      return false;
    }
    i += count;
  }
  return true;
}

}  // namespace

extern "C" const char* semb_status_name(semb_status status) {
  switch (status) {
#define SEMB_STATUS_NAME_(name, value) case name: return #name;
    SEMB_STATUS_ENUM_LIST(SEMB_STATUS_NAME_)
#undef SEMB_STATUS_NAME_
    case SEMB_STATUS_NEXT_FREE: break;
  }
  return "SEMB_UNKNOWN";
}

#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
// U2 cloud build, C52 (d)'s park point (tests/support/semb_u2_7_memory_open_seam.h).
// Test-only; absent from the shipped library.
namespace semb::test_seam {
namespace {
std::mutex memory_open_mutex;
std::condition_variable memory_open_changed;
bool memory_open_armed = false;
bool memory_open_parked = false;
uint64_t memory_open_release_generation = 0;
}  // namespace

// Called by the memory route of OpenEncoder after both owned copies are
// digested and before the interim open loads them. Armed, it parks there
// until released; one shot.
void MemoryOpenParkPoint() {
  std::unique_lock<std::mutex> lock(memory_open_mutex);
  if (!memory_open_armed) return;
  memory_open_armed = false;
  memory_open_parked = true;
  const uint64_t generation = memory_open_release_generation;
  memory_open_changed.notify_all();
  memory_open_changed.wait(lock, [generation] { return memory_open_release_generation != generation; });
  memory_open_parked = false;
}
}  // namespace semb::test_seam

extern "C" int32_t semb_test_seam_memory_open_arm_park(void) {
  std::lock_guard<std::mutex> lock(semb::test_seam::memory_open_mutex);
  if (semb::test_seam::memory_open_parked) return 0;
  semb::test_seam::memory_open_armed = true;
  return 1;
}

extern "C" int32_t semb_test_seam_memory_open_wait_parked(uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(semb::test_seam::memory_open_mutex);
  return semb::test_seam::memory_open_changed.wait_for(
             lock, std::chrono::milliseconds(timeout_ms),
             [] { return semb::test_seam::memory_open_parked; })
             ? 1
             : 0;
}

extern "C" void semb_test_seam_memory_open_release(void) {
  std::lock_guard<std::mutex> lock(semb::test_seam::memory_open_mutex);
  semb::test_seam::memory_open_armed = false;
  ++semb::test_seam::memory_open_release_generation;
  semb::test_seam::memory_open_changed.notify_all();
}
#endif

namespace {

// How an open receives its two artifacts. Each function fills a buffer the
// encoder owns and returns SEMB_OK or the refusal; every later step of the
// open reads only those buffers. The path verb reads each file once; the
// memory verb copies each caller span (U2-7 item 1).
enum class OpenRoute { kPath, kMemory };
struct ArtifactSource {
  OpenRoute route;
  std::function<semb_status(std::vector<uint8_t>*)> tokenizer;
  std::function<semb_status(std::vector<uint8_t>*)> model;
};

semb_status CopySpan(const uint8_t* bytes, size_t len, std::vector<uint8_t>* out) {
  if (len > 0) out->assign(bytes, bytes + len);
  return SEMB_OK;
}

}  // namespace

// The one internal open both public open verbs call. The two differ only in
// `source`, the way the bytes arrive; everything checked and derived here
// runs on the encoder's own copies.
static semb_status OpenEncoder(const ArtifactSource& source, uint32_t requested_max_concurrency,
                               uint32_t device, semb_encoder* out) {
  // U2-3 item 14: the linked engine's identity, before `source` reads or copies any artifact byte and before
  // any engine code runs. Both public open verbs reach it here, by construction.
  const semb::engine_link::Verdict link = semb::engine_link::CheckLinkedEngine();
  if (link.status != SEMB_OK) return link.status;

  try {
    std::vector<uint8_t> model_bytes;
    std::vector<uint8_t> tokenizer_bytes;
    const semb_status tokenizer_acquired = source.tokenizer(&tokenizer_bytes);
    if (tokenizer_acquired != SEMB_OK) return tokenizer_acquired;

    auto handle = std::make_unique<semb_encoder_s>();
    handle->tokenizer_bytes = std::move(tokenizer_bytes);
    superslm::SslmArtifact tokenizer_artifact;
    superslm::SslmError artifact_error;
    if (superslm::SslmArtifact::OpenFromMemory(handle->tokenizer_bytes.data(),
                                                handle->tokenizer_bytes.size(),
                                                tokenizer_artifact, &artifact_error) !=
        superslm::SslmStatus::Ok) {
      return SEMB_ENGINE_REFUSED;
    }
    superslm::TokenizerView tokenizer;
    std::string tokenizer_error;
    if (!superslm::TokenizerView::Open(tokenizer_artifact, tokenizer, &tokenizer_error)) {
      return SEMB_ENGINE_REFUSED;
    }
    if (tokenizer.VocabSize() <= 0) return SEMB_ENGINE_REFUSED;

    // Resolve the compiled-in literal from the same tokenizer bytes the
    // handle owns and fingerprints, before reading the model artifact.
    const std::vector<int32_t> canonical_end_ids =
        tokenizer.Encode(kCanonicalEndToken);
    if (canonical_end_ids.size() != 1 || canonical_end_ids[0] < 0) {
      return SEMB_TEMPLATE_TOKEN_ABSENT;
    }
    const int32_t appended_token_id = canonical_end_ids[0];

    const semb_status model_acquired = source.model(&model_bytes);
    if (model_acquired != SEMB_OK) return model_acquired;
    // The interim owned-open entry requires nonempty backing. Zero bytes
    // were successfully read but cannot form an engine artifact.
    if (model_bytes.empty()) return SEMB_ENGINE_REFUSED;

    // model_sha/tokenizer_sha: computed from the exact owned buffers NOW,
    // before model_bytes is moved into the interim open below (sec 3.3's
    // one-read rule -- each artifact digested from the same buffer that
    // backs the handle, never a second read). The REST of the fingerprint
    // (witness_sha, which needs W-B's own real forward) is derived further
    // down, once the interim encoder has opened (TE-147: "the open
    // sequence moves the derivation after step 8 of sec 3.1's open table
    // without changing the first refusal a caller sees").
    uint8_t model_sha[32], tokenizer_sha[32];
    Hash(model_bytes.data(), model_bytes.size(), model_sha);
    Hash(handle->tokenizer_bytes.data(), handle->tokenizer_bytes.size(), tokenizer_sha);
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
    if (source.route == OpenRoute::kMemory) semb::test_seam::MemoryOpenParkPoint();
#endif

    const uint32_t max_concurrency = requested_max_concurrency == 0 ? 1 : requested_max_concurrency;
    SembInterimEncoder* interim = nullptr;
    const char* engine_status = nullptr;
    bool vocab_mismatch = false;
    // U3 (te266 sec 4.2 Open step 1): a GPU handle's CPU pool has one slot;
    // its encodes never use a CPU workspace, and its CPU work (the witness and
    // the agreement check's CPU side) runs on one worker.
    const uint32_t cpu_slots = device == SEMB_DEVICE_GPU ? 1u : max_concurrency;
    const SembInterimStatus open_status = semb::OpenEncoderOwnedWithVocabJoin(
        std::move(model_bytes), appended_token_id, cpu_slots,
        static_cast<uint32_t>(tokenizer.VocabSize()), &interim,
        &engine_status, vocab_mismatch);
    (void)engine_status;
    if (vocab_mismatch) return SEMB_TOKENIZER_MODEL_MISMATCH;
    if (open_status != kSembInterimOk) return MapInterimStatus(open_status, kSembOk);

    std::unique_ptr<SembInterimEncoder, void (*)(SembInterimEncoder*)> interim_guard(
        interim, semb_interim_encoder_close);

    SembInterimGeometry geometry{};
    geometry.struct_size = sizeof(geometry);
    if (semb_interim_encoder_geometry(interim, &geometry) != kSembInterimOk) {
      return SEMB_ENGINE_REFUSED;
    }

    // The handle is otherwise complete (everything but its fingerprint):
    // fill it in and register its cookie NOW, before W-B runs (TE-159,
    // C-2, D-SLM6354 originally; TE-165, D-SLM6390 item 4 keeps the same
    // shape and closes what it left open). W-B's own digest must derive
    // through the shared composition (ComposeBatch, above), reached via
    // `h` directly rather than a semb_encoder cookie. The handle's cookie
    // is registered ahead of the derivation regardless, so the registry
    // entry that will eventually be the public one already exists to be
    // completed in place. An inserted handle is NOT yet a LIVE one from
    // any OTHER caller's perspective, though: `h->ready` is false from
    // construction, and FindReadyHandleLocked (used by every accessor,
    // including close) refuses any handle whose ready flag is not yet
    // true -- TE-165 N-1,
    // D-SLM6383. A guessed or probed cookie during this window therefore
    // names no live handle to anyone but this call: `semb_encoder_dims`,
    // `semb_wb_digest`, `semb_encode`, `semb_encode_batch` and
    // `semb_encoder_close` alike return SEMB_INVALID_HANDLE, never a
    // half-built handle with a zero digest and never a use-after-free on
    // a concurrent close. `*out` is additionally not written until
    // `h->ready` is set -- "no caller can observe a handle without its
    // fingerprint" -- belt and suspenders: even a caller that somehow
    // bypassed the ready check would still have no cookie value to use.
    handle->interim = interim_guard.release();
    handle->tokenizer_artifact = std::move(tokenizer_artifact);
    handle->tokenizer = std::move(tokenizer);
    handle->dims = geometry.hidden_size;
    handle->vocab_size = geometry.vocab_size;
    handle->context_cap = geometry.context_cap;
    handle->chunk_budget = 0;
    handle->device = device;
    handle->appended_token_id = appended_token_id;
    semb_encoder cookie = nullptr;
    semb_encoder_s* const h = handle.get();
    {
      std::lock_guard<std::mutex> lock(RegistryMutex());
      if (!AllocateHandleCookie(&cookie)) return SEMB_ALLOCATION_FAILED;
      const auto inserted = Registry().emplace(cookie, std::move(handle));
      if (!inserted.second) return SEMB_ENGINE_REFUSED;
    }
    // TE-165 (N-2, D-SLM6384; D-SLM6390 item 4): a guard object, not a
    // second rollback line -- erases the just-inserted, not-yet-ready
    // entry on ANY exit between here and Disarm() below, including an
    // exception thrown from inside ComputeWbDigest's own allocations
    // (std::bad_alloc, std::length_error) unwinding through this
    // function's own catch blocks. Before this guard, only the `false`
    // return from ComputeWbDigest rolled the insertion back; an exception
    // unwound straight past the rollback and left a live handle in the
    // registry for the process's lifetime.
    HandleInsertGuard insert_guard(cookie);

    // W-B: the real forward over the sixteen compiled-in documents, now
    // that the interim encoder is open and the tokenizer's own resolved
    // end-token id is known, run through the batch verb's own shared
    // composition (see ComputeWbDigest's own comment). Folded into
    // witness_sha below. A refusal here -- including an exception -- is
    // the one new refusal point TE-147 added to open's own sequence; it
    // stays functionally in the same place from a caller's perspective --
    // after every pre-existing failure mode, before the caller can ever
    // observe a cookie -- because insert_guard erases the not-yet-ready
    // entry on every exit until Disarm() is reached below.
    // U3 (T388-11): on a GPU open only, the witness below also records each
    // of its 22 sequences' composed ids and CPU triple for the agreement check.
    std::vector<semb::gpu::WitnessSequence> witness;
    if (device == SEMB_DEVICE_GPU) h->witness_capture = &witness;
    std::array<uint8_t, 32> wb_digest{};
    const semb_status wb_status = ComputeWbDigest(h, &wb_digest);
    if (wb_status != SEMB_OK) return wb_status;

    std::array<uint8_t, 32> wq_digest{};
    const semb_status wq_status = semb_compute_wq_digest(h, &wq_digest);
    h->witness_capture = nullptr;
    if (wq_status != SEMB_OK) return wq_status;
    // U2-9 Builds item 2 (D-SLM7773): the witness encodes above grew slot
    // buffers; open returns holding none, so it reserves nothing in
    // proportion to max_concurrency. No encode can be running: no caller
    // holds this handle yet. The slot records stay.
    const SembInterimStatus release_status = semb_interim_release_workspaces(h->interim);
    if (release_status != kSembInterimOk) return MapInterimStatus(release_status, kSembOk);
    h->wb_digest = wb_digest;
    h->wq_digest = wq_digest;
    // The five components and the fingerprint, derived from the SAME
    // one-read-owned buffers (model_sha/tokenizer_sha) and the SAME witness
    // digests just computed above -- never a second read, never a second
    // forward.
    {
      ManifestFingerprintComponents mc{};
      const semb_status fingerprint_status =
          ComputeManifestFingerprintComponents(model_sha, tokenizer_sha, wb_digest, wq_digest, &mc);
      if (fingerprint_status != SEMB_OK) return fingerprint_status;
      h->manifest_model_sha = mc.model_sha;
      h->manifest_tokenizer_sha = mc.tokenizer_sha;
      h->manifest_template_sha256 = mc.template_sha256;
      h->manifest_witness_sha = mc.witness_sha;
      h->manifest_certification_epoch = mc.certification_epoch;
      h->fingerprint = mc.fingerprint;
    }
    // U3 (te266 sec 4.2 Open steps 2-5): from the same model view, the GPU
    // context, the model map, the adapter labels and the agreement check over
    // the witness's 22 sequences. A refused GPU open releases every engine
    // handle it created; it never falls back to the CPU.
    if (device == SEMB_DEVICE_GPU) {
      semb::gpu::Encoder* gpu = nullptr;
      semb::gpu::AdapterLabels labels;
      const semb_status gpu_status = semb::gpu::Open(
          semb::InterimModelView(h->interim), static_cast<int64_t>(h->context_cap), max_concurrency,
          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cookie)), witness, &gpu, &labels);
      if (gpu_status != SEMB_OK) return gpu_status;
      h->gpu = gpu;
      h->gpu_labels = std::move(labels);
    }
    // The handle is now complete. Publish it: ready first (release order,
    // so every field written above -- including fingerprint and wb_digest
    // -- is visible to any thread that observes ready == true via
    // FindReadyHandleLocked's own acquire load), then disarm the insert
    // guard (nothing left to roll back), then hand the cookie to the
    // caller. No caller can observe this handle -- via the cookie value
    // OR via a guess -- before this point.
    h->ready.store(true, std::memory_order_release);
    insert_guard.Disarm();

    *out = cookie;
    return SEMB_OK;
  } catch (const std::bad_alloc&) {
    return SEMB_ALLOCATION_FAILED;
  } catch (const std::length_error&) {
    return SEMB_ALLOCATION_FAILED;
  } catch (...) {
    return SEMB_ENGINE_REFUSED;
  }
}

static semb_status EncoderOpenBody(const semb_config* cfg, semb_encoder* out) {
  if (out) *out = nullptr;
  if (!cfg || !out) return SEMB_NULL_ARGUMENT;
  if (cfg->struct_size != sizeof(semb_config)) return SEMB_STRUCT_SIZE_MISMATCH;
  if ((!cfg->model_path && cfg->model_path_len > 0) ||
      (!cfg->tokenizer_path && cfg->tokenizer_path_len > 0)) return SEMB_NULL_ARGUMENT;
  // U3 (te266 P:4024): an unknown `device` refuses before any artifact byte
  // is read, and so does SEMB_DEVICE_GPU in a library built without the GPU
  // path.
  if (cfg->device != SEMB_DEVICE_CPU && cfg->device != SEMB_DEVICE_GPU) return SEMB_DEVICE_UNSUPPORTED;
  if (cfg->device == SEMB_DEVICE_GPU && !semb::gpu::Built()) return SEMB_DEVICE_UNSUPPORTED;
  const auto read_file = [](const char* path, size_t path_len, std::vector<uint8_t>* bytes) {
    return ReadFileOnce(std::string_view(path ? path : "", path_len), bytes) ? SEMB_OK
                                                                            : SEMB_ARTIFACT_UNREADABLE;
  };
  ArtifactSource source{
      OpenRoute::kPath,
      [&](std::vector<uint8_t>* bytes) {
        return read_file(cfg->tokenizer_path, cfg->tokenizer_path_len, bytes);
      },
      [&](std::vector<uint8_t>* bytes) { return read_file(cfg->model_path, cfg->model_path_len, bytes); }};
  return OpenEncoder(source, cfg->max_concurrency, cfg->device, out);
}

extern "C" semb_status semb_encoder_open(const semb_config* cfg, semb_encoder* out) {
  return semb::c_boundary::Status([&] {
    return EncoderOpenBody(cfg, out);
  });
}

// U2-7 item 1: the same open, with each span copied into an encoder-owned
// buffer where the path verb reads a file. The copy is what the fingerprint
// is digested from and what the engine loads, so a caller rewriting its span
// during the call cannot make the fingerprint describe bytes the engine did
// not load; and the caller may release its spans once the call returns.
static semb_status EncoderOpenMemoryBody(const semb_memory_config* cfg, semb_encoder* out) {
  if (out) *out = nullptr;
  if (!cfg || !out) return SEMB_NULL_ARGUMENT;
  if (cfg->struct_size != sizeof(semb_memory_config)) return SEMB_STRUCT_SIZE_MISMATCH;
  if ((!cfg->model_bytes && cfg->model_len > 0) ||
      (!cfg->tokenizer_bytes && cfg->tokenizer_len > 0)) return SEMB_NULL_ARGUMENT;
  ArtifactSource source{
      OpenRoute::kMemory,
      [&](std::vector<uint8_t>* bytes) { return CopySpan(cfg->tokenizer_bytes, cfg->tokenizer_len, bytes); },
      [&](std::vector<uint8_t>* bytes) { return CopySpan(cfg->model_bytes, cfg->model_len, bytes); }};
  // U3 ruling 5: semb_memory_config has no `device`; a memory open always
  // runs on the CPU.
  return OpenEncoder(source, cfg->max_concurrency, SEMB_DEVICE_CPU, out);
}

extern "C" semb_status semb_encoder_open_memory(const semb_memory_config* cfg, semb_encoder* out) {
  return semb::c_boundary::Status([&] {
    return EncoderOpenMemoryBody(cfg, out);
  });
}

#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
// U2 cloud build, the seam commit (tests/support/semb_u2_memory_seam.h): the
// public handle's interim encoder, so the slot-record readers apply to it.
// Test-only; absent from the shipped library.
extern "C" SembInterimEncoder* semb_test_seam_interim_of(const void* enc) {
  // The public handle is a registry cookie, never the object's address:
  // resolve it through the same ready-gated lookup every verb reads through.
  if (!enc) return nullptr;
  std::lock_guard<std::mutex> lock(RegistryMutex());
  const auto found = FindReadyHandleLocked(reinterpret_cast<semb_encoder>(const_cast<void*>(enc)));
  return found == Registry().end() ? nullptr : found->second->interim;
}

// U3: the public handle's GPU encoder (null for a CPU handle), for the GPU
// slot-record readers (tests/support/semb_u3_gpu_seam.h). Test-only.
namespace semb::test_seam {
semb::gpu::Encoder* GpuEncoderOf(const void* enc) {
  if (!enc) return nullptr;
  std::lock_guard<std::mutex> lock(RegistryMutex());
  const auto found = FindReadyHandleLocked(reinterpret_cast<semb_encoder>(const_cast<void*>(enc)));
  return found == Registry().end() ? nullptr : found->second->gpu;
}
}  // namespace semb::test_seam
#endif

static semb_status EncoderCloseBody(semb_encoder enc) {
  if (!enc) return SEMB_NULL_ARGUMENT;
  std::unique_ptr<semb_encoder_s> closing;
  {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    // TE-165 (N-1, D-SLM6383; D-SLM6390 item 4): the same ready-gated
    // lookup semb_encoder_open publishes through and HandleLease reads
    // through. A cookie whose handle has not finished opening names no
    // live handle to close -- SEMB_INVALID_HANDLE, never a destroy racing
    // the open-time derivation still running on that same handle object.
    const auto found = FindReadyHandleLocked(enc);
    if (found == Registry().end()) return SEMB_INVALID_HANDLE;
    if (found->second->active_calls.load(std::memory_order_acquire) != 0) {
      return SEMB_HANDLE_IN_USE;
    }
    if (found->second->live_queries != 0) return SEMB_HANDLE_IN_USE;
    closing = std::move(found->second);
    Registry().erase(found);
  }
  closing.reset();
  return SEMB_OK;
}

extern "C" semb_status semb_encoder_close(semb_encoder enc) {
  return semb::c_boundary::Status([&] {
    return EncoderCloseBody(enc);
  });
}

static semb_status EncoderDimsBody(semb_encoder enc, uint32_t* out_dims) {
  if (!enc || !out_dims) return SEMB_NULL_ARGUMENT;
  HandleLease lease(enc);
  if (!lease.get()) return SEMB_INVALID_HANDLE;
  *out_dims = lease.get()->dims;
  return SEMB_OK;
}

extern "C" semb_status semb_encoder_dims(semb_encoder enc, uint32_t* out_dims) {
  return semb::c_boundary::Status([&] {
    return EncoderDimsBody(enc, out_dims);
  });
}

static semb_status EncodeBody(semb_encoder enc, const char* utf8, size_t utf8_len,
                              float* out_vector, size_t out_capacity_bytes,
                              size_t* out_required_bytes,
                              semb_fingerprint* out_fingerprint) {
  if (!enc || !out_required_bytes || !out_fingerprint) return SEMB_NULL_ARGUMENT;
  try {
    HandleLease lease(enc);
    if (!lease.get()) return SEMB_INVALID_HANDLE;
    if (out_fingerprint->struct_size != sizeof(semb_fingerprint)) {
      return SEMB_STRUCT_SIZE_MISMATCH;
    }

    const size_t required = static_cast<size_t>(lease.get()->dims) * sizeof(float);
    if (out_vector && (reinterpret_cast<uintptr_t>(out_vector) & 15u) != 0) {
      return SEMB_MISALIGNED_BUFFER;
    }
    if (out_capacity_bytes < required) {
      *out_required_bytes = required;
      return SEMB_BUFFER_TOO_SMALL;
    }
    if (!out_vector) return SEMB_NULL_ARGUMENT;
    if (utf8_len == 0) return SEMB_INPUT_EMPTY;
    if (!utf8) return SEMB_NULL_ARGUMENT;
    if (!ValidUtf8(utf8, utf8_len)) return SEMB_INVALID_UTF8;

    // The raw entry is the batch composition over one document (D-SLM7197,
    // D-SLM7205): ComposeBatch tokenizes the document frame and the text,
    // refuses an empty or >= context_cap id sequence, appends the terminator
    // and forwards, exactly as semb_encode_batch does for each of its rows.
    // W-B executes that same composition at open, so the witness covers this
    // entry structurally. Only argument validation (above) and the output
    // copy (below) are raw-specific. Caller bytes stay private until the
    // composition has fully succeeded, including every allocation it makes.
    const std::string_view text(utf8, utf8_len);
    std::vector<float> staged_vector(lease.get()->dims);
    size_t composed_required = 0;
    const semb_status composed =
        ComposeBatch(lease.get(), &text, 1, staged_vector.data(), required, &composed_required,
                     /*out_seam_stream=*/nullptr, /*out_seam_stream_capacity=*/0,
                     /*out_seam_stream_len=*/nullptr);
    if (composed != SEMB_OK) return composed;
    std::memcpy(out_vector, staged_vector.data(), required);
    *out_required_bytes = composed_required;
    std::memcpy(out_fingerprint->digest, lease.get()->fingerprint.data(),
                lease.get()->fingerprint.size());
    return SEMB_OK;
  } catch (const std::bad_alloc&) {
    return SEMB_ALLOCATION_FAILED;
  } catch (const std::length_error&) {
    return SEMB_ALLOCATION_FAILED;
  } catch (...) {
    return SEMB_ENGINE_REFUSED;
  }
}

extern "C" semb_status semb_encode(semb_encoder enc, const char* utf8, size_t utf8_len,
                                    float* out_vector, size_t out_capacity_bytes,
                                    size_t* out_required_bytes,
                                    semb_fingerprint* out_fingerprint) {
  return semb::c_boundary::Status([&] {
    return EncodeBody(enc, utf8, utf8_len, out_vector, out_capacity_bytes, out_required_bytes,
                      out_fingerprint);
  });
}

// U2-7 item 2: the raw path's query form. semb_encode's validation order and
// output-buffer convention; the vector comes from semb::EncodeQuery, the one
// composition semb_query_encode also calls, and the fingerprint is the
// encoder's, as semb_encode returns it. The lease held for the call is
// semb_encode's per-encoder concurrency.
static semb_status EncodeQueryBody(semb_encoder enc, const char* utf8, size_t utf8_len,
                                   float* out_vector, size_t out_capacity_bytes,
                                   size_t* out_required_bytes,
                                   semb_fingerprint* out_fingerprint) {
  if (!enc || !out_required_bytes || !out_fingerprint) return SEMB_NULL_ARGUMENT;
  try {
    HandleLease lease(enc);
    if (!lease.get()) return SEMB_INVALID_HANDLE;
    if (out_fingerprint->struct_size != sizeof(semb_fingerprint)) {
      return SEMB_STRUCT_SIZE_MISMATCH;
    }
    const size_t required = static_cast<size_t>(lease.get()->dims) * sizeof(float);
    if (out_vector && (reinterpret_cast<uintptr_t>(out_vector) & 15u) != 0) {
      return SEMB_MISALIGNED_BUFFER;
    }
    if (out_capacity_bytes < required) {
      *out_required_bytes = required;
      return SEMB_BUFFER_TOO_SMALL;
    }
    if (!out_vector) return SEMB_NULL_ARGUMENT;
    if (utf8_len == 0) return SEMB_INPUT_EMPTY;
    if (!utf8) return SEMB_NULL_ARGUMENT;
    if (!ValidUtf8(utf8, utf8_len)) return SEMB_INVALID_UTF8;

    std::vector<float> staged_vector;
    const semb_status encoded =
        semb::EncodeQuery(enc, std::string_view(utf8, utf8_len), &staged_vector);
    if (encoded != SEMB_OK) return encoded;
    std::memcpy(out_vector, staged_vector.data(), required);
    *out_required_bytes = required;
    std::memcpy(out_fingerprint->digest, lease.get()->fingerprint.data(),
                lease.get()->fingerprint.size());
    return SEMB_OK;
  } catch (const std::bad_alloc&) {
    return SEMB_ALLOCATION_FAILED;
  } catch (const std::length_error&) {
    return SEMB_ALLOCATION_FAILED;
  } catch (...) {
    return SEMB_ENGINE_REFUSED;
  }
}

extern "C" semb_status semb_encode_query(semb_encoder enc, const char* utf8, size_t utf8_len,
                                          float* out_vector, size_t out_capacity_bytes,
                                          size_t* out_required_bytes,
                                          semb_fingerprint* out_fingerprint) {
  return semb::c_boundary::Status([&] {
    return EncodeQueryBody(enc, utf8, utf8_len, out_vector, out_capacity_bytes, out_required_bytes,
                           out_fingerprint);
  });
}

// TE-147 (U1 slice 2 build): W-B's own diagnostic accessor
// (`include/superembedder/witness_abi.h`; the contract of record is
// `tests/semb_wb_witness_abi_contract.h`, AMENDED TE-151 to this
// handle-scoped shape). The value is derived once, at semb_encoder_open,
// and simply returned here -- D-SLM3978's no-cache rule binds the
// FINGERPRINT derivation (no re-read of a file another field was derived
// from), not how many times this accessor is called against an
// already-open handle.
static semb_status WbDigestBody(semb_encoder enc, uint8_t out_digest[32]) {
  if (!enc) return SEMB_NULL_ARGUMENT;
  HandleLease lease(enc);
  if (!lease.get()) return SEMB_INVALID_HANDLE;
  if (!out_digest) return SEMB_NULL_ARGUMENT;
  std::memcpy(out_digest, lease.get()->wb_digest.data(), lease.get()->wb_digest.size());
  return SEMB_OK;
}

extern "C" semb_status semb_wb_digest(semb_encoder enc, uint8_t out_digest[32]) {
  return semb::c_boundary::Status([&] {
    return WbDigestBody(enc, out_digest);
  });
}

static semb_status WqDigestBody(semb_encoder enc, uint8_t out_digest[32]) {
  if (!enc) return SEMB_NULL_ARGUMENT;
  HandleLease lease(enc);
  if (!lease.get()) return SEMB_INVALID_HANDLE;
  if (!out_digest) return SEMB_NULL_ARGUMENT;
  std::memcpy(out_digest, lease.get()->wq_digest.data(), lease.get()->wq_digest.size());
  return SEMB_OK;
}

extern "C" semb_status semb_wq_digest(semb_encoder enc, uint8_t out_digest[32]) {
  return semb::c_boundary::Status([&] {
    return WqDigestBody(enc, out_digest);
  });
}

namespace semb {

semb_status EncoderAcquireQuery(semb_encoder enc) {
  if (!enc) return SEMB_NULL_ARGUMENT;
  std::lock_guard<std::mutex> lock(RegistryMutex());
  const auto found = FindReadyHandleLocked(enc);
  if (found == Registry().end()) return SEMB_INVALID_HANDLE;
  ++found->second->live_queries;
  return SEMB_OK;
}

semb_status EncoderReleaseQuery(semb_encoder enc) {
  if (!enc) return SEMB_NULL_ARGUMENT;
  std::lock_guard<std::mutex> lock(RegistryMutex());
  const auto found = FindReadyHandleLocked(enc);
  if (found == Registry().end()) return SEMB_INVALID_HANDLE;
  if (found->second->live_queries == 0) return SEMB_INVALID_HANDLE;
  --found->second->live_queries;
  return SEMB_OK;
}

semb_status EncoderFingerprint(semb_encoder enc, uint8_t out[32]) {
  if (!enc || !out) return SEMB_NULL_ARGUMENT;
  HandleLease lease(enc);
  if (!lease.get()) return SEMB_INVALID_HANDLE;
  std::memcpy(out, lease.get()->fingerprint.data(), 32);
  return SEMB_OK;
}

semb_status EncoderDims(semb_encoder enc, uint32_t* out_dims) {
  if (!enc || !out_dims) return SEMB_NULL_ARGUMENT;
  HandleLease lease(enc);
  if (!lease.get()) return SEMB_INVALID_HANDLE;
  *out_dims = lease.get()->dims;
  return SEMB_OK;
}

bool EncoderValidUtf8(const char* data, size_t size) {
  return data && ValidUtf8(data, size);
}

semb_status EncodeQueryComposedForWitness(semb_encoder_s* encoder,
                                          std::string_view composed,
                                          size_t max_content_ids,
                                          std::vector<float>* out_vector) {
  return EncodeQueryComposedImpl(encoder, composed, max_content_ids, out_vector);
}

semb_status EncodeQueryComposed(semb_encoder enc, std::string_view composed,
                                size_t max_content_ids,
                                std::vector<float>* out_vector) {
  if (!enc || !out_vector) return SEMB_NULL_ARGUMENT;
  HandleLease lease(enc);
  if (!lease.get()) return SEMB_INVALID_HANDLE;
  return EncodeQueryComposedImpl(lease.get(), composed, max_content_ids, out_vector);
}

semb_status ComposedQueryTokenCount(semb_encoder enc, std::string_view composed, size_t* out_count) {
  if (!enc || !out_count) return SEMB_NULL_ARGUMENT;
  HandleLease lease(enc);
  if (!lease.get()) return SEMB_INVALID_HANDLE;
  *out_count = TokenizeComposedQuery(lease.get(), composed).size();
  return SEMB_OK;
}

}  // namespace semb

// TE-147 (U1 slice 2 build): the bake-side corpus entry
// (`Claude/Plans/SuperEmbedder_Plan.md` sec 3.1; the ratified contract is
// `Claude/Fable/te96-u1-contract-closure-2026-09-05.md` sec 4.2, quoted by
// step number throughout this function). Validation is shape-before-
// content-before-work, completed before any forward; nothing is written to
// `out_rows` on any non-SEMB_OK return; `*out_required_bytes` is written on
// exactly SEMB_OK and SEMB_BUFFER_TOO_SMALL.
static semb_status EncodeBatchBody(semb_encoder enc, const semb_document* docs, size_t n,
                                   float* out_rows, size_t out_capacity_bytes,
                                   size_t* out_required_bytes) {
  // Step 1: enc==NULL, out_required_bytes==NULL, or docs==NULL && n>0 (P-8's
  // span rule on the array) -> SEMB_NULL_ARGUMENT. Checked before the
  // handle needs to be live, matching the table's own step ordering.
  if (!enc || !out_required_bytes || (!docs && n > 0)) return SEMB_NULL_ARGUMENT;
  try {
    HandleLease lease(enc);
    // Step 2: enc not a live cookie -> SEMB_INVALID_HANDLE.
    if (!lease.get()) return SEMB_INVALID_HANDLE;
    semb_encoder_s* h = lease.get();

    // Step 3: n == 0 (whatever docs is) -> SEMB_CORPUS_EMPTY.
    if (n == 0) return SEMB_CORPUS_EMPTY;
    // Step 4: n > kSembMaxCorpusRows (1u << 28, FORMAT.md's kMaxBankRows),
    // checked BEFORE the multiplication -- so an oversized n with a
    // deliberately non-dereferenceable docs pointer refuses cleanly rather
    // than touching docs[0] or overflowing the size computation below.
    constexpr size_t kMaxCorpusRows = size_t{1} << 28;
    if (n > kMaxCorpusRows) return SEMB_CORPUS_TOO_LARGE;

    // Step 5: required = n * dims * 4, computed using the handle's actual
    // uint32 dimensions. n <= 2^28 (step 4) and dims <= 2^32-1, so
    // n * dims * 4 <= 2^28 * (2^32-1) * 4 < 2^62, which fits uint64
    // unconditionally; the size_t conversion is still checked so a build
    // where size_t is narrower than 62 bits refuses by name rather than
    // silently truncating.
    const uint64_t required64 =
        static_cast<uint64_t>(n) * static_cast<uint64_t>(h->dims) * 4ull;
    if (required64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      return SEMB_CORPUS_TOO_LARGE;
    }
    const size_t required = static_cast<size_t>(required64);

    // Step 6: out_rows != NULL and not 16-byte aligned -> MISALIGNED_BUFFER,
    // checked BEFORE step 7's capacity so a generously-sized-but-misaligned
    // buffer refuses for the right reason.
    if (out_rows && (reinterpret_cast<uintptr_t>(out_rows) & 15u) != 0) {
      return SEMB_MISALIGNED_BUFFER;
    }
    // Step 7: out_capacity_bytes < required -> BUFFER_TOO_SMALL, required
    // stated. The query form (NULL, 0) lands here.
    if (out_capacity_bytes < required) {
      *out_required_bytes = required;
      return SEMB_BUFFER_TOO_SMALL;
    }
    // Step 8: out_rows == NULL with sufficient capacity -> NULL_ARGUMENT,
    // distinct from the query form above (which has capacity 0).
    if (!out_rows) return SEMB_NULL_ARGUMENT;

    // Step 9: per document, in index order, the first failing document's
    // first failing check. Stops at the FIRST document that fails any
    // check; later documents (even invalid ones) are never examined.
    for (size_t i = 0; i < n; ++i) {
      const semb_document& d = docs[i];
      if (d.struct_size != sizeof(semb_document)) return SEMB_STRUCT_SIZE_MISMATCH;
      if (!d.id && d.id_len > 0) return SEMB_NULL_ARGUMENT;
      if (!d.utf8 && d.utf8_len > 0) return SEMB_NULL_ARGUMENT;
      if (d.id_len == 0) return SEMB_DOCUMENT_ID_INVALID;
      if (std::memchr(d.id, '\0', d.id_len) != nullptr) return SEMB_DOCUMENT_ID_INVALID;
      if (!ValidUtf8(d.id, d.id_len)) return SEMB_DOCUMENT_ID_INVALID;
      if (d.utf8_len == 0) return SEMB_INPUT_EMPTY;
      if (!ValidUtf8(d.utf8, d.utf8_len)) return SEMB_INVALID_UTF8;
    }

    // Step 10: uniqueness over ALL n ids, byte identity (sec 4.3: no
    // normalization, no case folding) -> DOCUMENT_ID_DUPLICATE. Reached
    // only once every document has individually passed step 9.
    {
      // R20 (U2-1): an ordered set, for the reason the handle registry is one;
      // it is read only by emplace and never iterated.
      std::set<std::string_view> seen;
      for (size_t i = 0; i < n; ++i) {
        if (!seen.emplace(std::string_view(docs[i].id, docs[i].id_len)).second) {
          return SEMB_DOCUMENT_ID_DUPLICATE;
        }
      }
    }

    // Steps 11-13 (TE-96 sec 4.2): the ONE internal batch composition
    // (TE-165, D-SLM6390 item 1) -- per-document tokenization, the
    // document frame, the >= context_cap refusal, the appended
    // terminator, the workspace acquisition(s) (TE-350: at most one per
    // worker thread), the chunk-batched
    // forward -- shared with ComputeWbDigest's own open-time derivation
    // (the seam tap, above). Forwards into staged storage; any refusal
    // discards the stage entirely (out_rows is only ever written on full
    // success, step 14).
    std::vector<std::string_view> texts(n);
    for (size_t i = 0; i < n; ++i) {
      texts[i] = std::string_view(docs[i].utf8, docs[i].utf8_len);
    }
    std::vector<float> staged(n * static_cast<size_t>(h->dims));
    size_t batch_required = 0;
    const semb_status batch_status =
        ComposeBatch(h, texts.data(), n, staged.data(), staged.size() * sizeof(float),
                    &batch_required, /*out_seam_stream=*/nullptr,
                    /*out_seam_stream_capacity=*/0, /*out_seam_stream_len=*/nullptr);
    // TE-159 item 6 (M-4), carried: batch_required is written by the
    // composed call (it always equals `required`, already computed and
    // checked at steps 5/7 above) and is never read afterward.
    (void)batch_required;
    if (batch_status != SEMB_OK) {
      return batch_status;
    }

    // Step 14: copy all rows to out_rows (contiguous, row-major, stride
    // dims * 4, no padding, input order); *out_required_bytes = required.
    std::memcpy(out_rows, staged.data(), required);
    *out_required_bytes = required;
    return SEMB_OK;
  } catch (const std::bad_alloc&) {
    return SEMB_ALLOCATION_FAILED;
  } catch (const std::length_error&) {
    return SEMB_ALLOCATION_FAILED;
  } catch (...) {
    return SEMB_ENGINE_REFUSED;
  }
}

extern "C" semb_status semb_encode_batch(semb_encoder enc, const semb_document* docs, size_t n,
                                          float* out_rows, size_t out_capacity_bytes,
                                          size_t* out_required_bytes) {
  return semb::c_boundary::Status([&] {
    return EncodeBatchBody(enc, docs, n, out_rows, out_capacity_bytes, out_required_bytes);
  });
}

namespace {

// A 16-byte-aligned float buffer with slack, the same over-allocate-and-
// align technique tests/u1_raw_text_contract_test.cpp's own SentinelBuffer
// uses -- std::vector<float>::data() carries no 16-byte alignment
// guarantee in general.
float* Align16(std::vector<float>& storage) {
  auto p = reinterpret_cast<uintptr_t>(storage.data());
  p = (p + 15) & ~static_cast<uintptr_t>(15);
  return reinterpret_cast<float*>(p);
}

// TE-166 (U1 slice 4): writes the three sibling files at a stem -- each
// written to a temp file in the SAME directory first, then renamed into
// place, manifest LAST. Every validation and self-check in
// semb_bake_to_path runs BEFORE this is constructed; on any exception this
// class's own Rollback() makes the WRITE all-or-nothing against a live,
// uninterrupted I/O failure -- Commit() either finishes all three renames
// or (via Rollback) leaves none of the three sibling files in place.
//
// TE-191 F6: that guarantee covers a live process; it is NOT crash
// atomicity across the three-rename SET. Each `std::filesystem::rename`
// is individually atomic; a process termination or power loss between two
// of them is not something any in-process catch can undo. Renaming the
// manifest LAST is what bounds the damage: a termination before it
// completes leaves either the pre-bake triple entirely untouched, or the
// new header+payload paired with the OLD manifest -- never a new manifest
// paired with an old header or payload, because the manifest is the last
// thing this function ever touches. `semb_bank_open`'s own manifest
// cross-checks (count, dims, the row digest recomputed over the payload)
// are what turn that hybrid state into a named refusal
// (SEMB_BANK_SIZE_MISMATCH / SEMB_BANK_DIGEST_MISMATCH) instead of a
// silently accepted mismatched pair.
class AtomicTripleWriter {
 public:
  AtomicTripleWriter(std::string header_path, std::string bin_path, std::string manifest_path)
      : header_path_(std::move(header_path)),
        bin_path_(std::move(bin_path)),
        manifest_path_(std::move(manifest_path)),
        header_tmp_(header_path_ + ".tmp"),
        bin_tmp_(bin_path_ + ".tmp"),
        manifest_tmp_(manifest_path_ + ".tmp") {}

  void Commit(const std::string& header_text, const std::string& bin_bytes,
              const std::string& manifest_text) {
    try {
      WriteFile(header_tmp_, header_text);
      WriteFile(bin_tmp_, bin_bytes);
      WriteFile(manifest_tmp_, manifest_text);
      std::filesystem::rename(header_tmp_, header_path_);
      header_renamed_ = true;
      std::filesystem::rename(bin_tmp_, bin_path_);
      bin_renamed_ = true;
      std::filesystem::rename(manifest_tmp_, manifest_path_);
      manifest_renamed_ = true;
    } catch (...) {
      Rollback();
      throw;
    }
  }

 private:
  static void WriteFile(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("semb_bake_to_path: cannot open '" + path + "' for write");
    f.exceptions(std::ios::badbit | std::ios::failbit);
    if (!bytes.empty()) f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.close();
  }
  void Rollback() {
    std::error_code ec;
    if (header_renamed_) std::filesystem::remove(header_path_, ec); else std::filesystem::remove(header_tmp_, ec);
    if (bin_renamed_) std::filesystem::remove(bin_path_, ec); else std::filesystem::remove(bin_tmp_, ec);
    if (manifest_renamed_) std::filesystem::remove(manifest_path_, ec); else std::filesystem::remove(manifest_tmp_, ec);
  }

  std::string header_path_, bin_path_, manifest_path_;
  std::string header_tmp_, bin_tmp_, manifest_tmp_;
  bool header_renamed_ = false, bin_renamed_ = false, manifest_renamed_ = false;
};

}  // namespace

// TE-166 (U1 slice 4): the internal bake primitive
// (`tests/semb_bake_abi_contract.h`; `Claude/Plans/te140-u1-remainder-sequencing.md`
// sec 4.4). NOT one of the frozen twelve verbs (sec 3.1, D-SLM4646) --
// Slice 6's `semb bake` CLI wraps this. Computes the rows through the
// SAME validated call `semb_encode_batch` itself is (this function's own
// two calls into it below, a size query then the real call -- "the exact
// same call" per the contract header's own grading note): every refusal
// semb_encode_batch can produce propagates unchanged, and nothing is
// written on any non-SEMB_OK return.
static semb_status BakeToPathBody(semb_encoder enc, const semb_document* docs, size_t n,
                                  const char* path_stem, size_t path_stem_len) {
  // sec 3.1 property 7's own span rule, applied to a path exactly as
  // semb_config's own path members and semb_bank_open's own `path`
  // argument already are: (NULL, n>0) is SEMB_NULL_ARGUMENT; an empty
  // span -- (NULL, 0) or a non-NULL pointer with path_stem_len == 0 -- is
  // SEMB_ARTIFACT_UNREADABLE (an empty path names no writable location),
  // never SEMB_NULL_ARGUMENT.
  if (!enc || (!docs && n > 0) || (!path_stem && path_stem_len > 0)) {
    return SEMB_NULL_ARGUMENT;
  }
  if (path_stem_len == 0) return SEMB_ARTIFACT_UNREADABLE;
  try {
    HandleLease lease(enc);
    if (!lease.get()) return SEMB_INVALID_HANDLE;
    semb_encoder_s* h = lease.get();

    // Compute the rows: a size query (the query form semb_encode_batch's
    // own step 7 defines), then the real call. Every refusal
    // semb_encode_batch itself produces -- CORPUS_EMPTY, per-document
    // validation, DUPLICATE, CONTEXT_CAP_EXCEEDED, engine refusals, etc. --
    // propagates unchanged, before any file touch.
    size_t required = 0;
    const semb_status size_status = semb_encode_batch(enc, docs, n, nullptr, 0, &required);
    if (size_status != SEMB_BUFFER_TOO_SMALL) return size_status;

    std::vector<float> rows_storage(required / sizeof(float) + 4, 0.0f);
    float* aligned_rows = Align16(rows_storage);
    size_t written = 0;
    const semb_status encode_status =
        semb_encode_batch(enc, docs, n, aligned_rows, required, &written);
    if (encode_status != SEMB_OK) return encode_status;

    const uint32_t dims = h->dims;

    // FORMAT.md makes a zero-norm row a bake-time error for cosine banks.
    // Check every staged row before constructing any output content or writer:
    // all validation and every refusal this bake can return therefore precede
    // any final or temporary output file. The integer bit test deliberately
    // treats both signed zeros as zero without introducing FP arithmetic.
    for (size_t row = 0; row < n; ++row) {
      bool all_zero = true;
      const float* row_values = aligned_rows + row * static_cast<size_t>(dims);
      for (uint32_t dim = 0; dim < dims; ++dim) {
        uint32_t bits = 0;
        std::memcpy(&bits, row_values + dim, sizeof(bits));
        if ((bits & 0x7FFFFFFFu) != 0) {
          all_zero = false;
          break;
        }
      }
      if (all_zero) return SEMB_BANK_ZERO_NORM_ROW;
    }

    // C34 legs (e)/(h): the value about to be written, checked against the
    // linked TU's own compiled-in self-report BEFORE any file is touched.
    // In ordinary operation these are the same two literals and this
    // cannot fire on any build this suite configures by itself --
    // tests/te163_c34_manifest_honesty_check.py's own single-file source
    // mutation is the only way to move them apart.
    if (std::string(kEngineAdapterIdLabel) != semb_adapter_self_id()) {
      return SEMB_ADAPTER_ID_MISMATCH;
    }
    if (std::string(kPsVersionLabel) != semb_ps_n_version_label()) {
      return SEMB_PS_VERSION_LABEL_MISMATCH;
    }

    // Per-row token counts: recomputed through ComposeDocumentInput -- the
    // ONE composition point semb_encode_batch itself tokenized (D-SLM6390
    // item 1: no second, independent statement of the composition exists
    // anywhere in this file). TE-193 P-2: the previous raw-text tokenize
    // agreed only while kDocumentFrame was empty (D-SLM4726), so a future
    // PS-1 frame would have silently under-reported every row here. A
    // second, read-only tokenize pass for metadata, not a second forward.
    //
    // U2-3 item 12: the same loop accumulates token_id_digest, C23's rule
    // (tests/c23_token_id_determinism_test.cpp) over the ids the bake
    // tokenized before the end token: per row the u64 LE count, then each id
    // as u32 LE. Provenance, never an identity input.
    std::vector<int64_t> per_row_token_counts(n);
    std::array<uint8_t, 32> token_id_digest{};
    {
      superslm::Sha256 token_hash;
      for (size_t i = 0; i < n; ++i) {
        const std::vector<int32_t> ids =
            h->tokenizer.Encode(ComposeDocumentInput(std::string_view(docs[i].utf8, docs[i].utf8_len)));
        per_row_token_counts[i] = static_cast<int64_t>(ids.size());
        UpdateU64(&token_hash, static_cast<uint64_t>(ids.size()));
        for (const int32_t id : ids) {
          const uint32_t v = static_cast<uint32_t>(id);
          const uint8_t le[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                                 static_cast<uint8_t>(v >> 24)};
          token_hash.Update(le, sizeof(le));
        }
      }
      token_hash.Final(token_id_digest.data());
    }

    // Row digest: SHA-256 over the .wvbank.bin payload bytes just written.
    std::array<uint8_t, 32> row_digest{};
    Hash(aligned_rows, required, row_digest.data());

    // Corpus digest: SHA-256 over count-prefixed, per-row
    // length-prefixed(id) || length-prefixed(utf8), row order -- a
    // provenance record of the input documents (TE-96 sec 4.5: "the
    // manifest's corpus digest covers the input file's bytes"), never a
    // fingerprint field (the producer-identity rule has nothing to say
    // about a datum outside the emitted bytes).
    std::array<uint8_t, 32> corpus_digest{};
    {
      superslm::Sha256 hash;
      UpdateU64(&hash, static_cast<uint64_t>(n));
      for (size_t i = 0; i < n; ++i) {
        UpdateU64(&hash, static_cast<uint64_t>(docs[i].id_len));
        hash.Update(reinterpret_cast<const uint8_t*>(docs[i].id), docs[i].id_len);
        UpdateU64(&hash, static_cast<uint64_t>(docs[i].utf8_len));
        hash.Update(reinterpret_cast<const uint8_t*>(docs[i].utf8), docs[i].utf8_len);
      }
      hash.Final(corpus_digest.data());
    }

    // provenance.artifact_sha: SHA-256(len(model_sha)||model_sha||
    // len(tokenizer_sha)||tokenizer_sha) -- an independent, NARROWER
    // digest over only the two ARTIFACT components (sec 3.3: "model_sha",
    // "tokenizer_sha" -- "the model artifact", "the tokenizer artifact"),
    // distinct from encoder_fingerprint's own full five-component
    // recomputation below. This is what lets semb_bank_open attribute a
    // tampered model_sha/tokenizer_sha to SEMB_ARTIFACT_SHA_MISMATCH
    // specifically (sec 8 C18's own name for this class) rather than the
    // broader SEMB_FINGERPRINT_MISMATCH cell 3 already owns -- this
    // seat's own derivation, since neither the plan text nor the contract
    // header names a mechanism for the two statuses to disagree on a
    // single edited field otherwise; filed as a finding (see the build
    // record) rather than silently invented and left undocumented.
    std::array<uint8_t, 32> artifact_sha{};
    {
      superslm::Sha256 hash;
      UpdateField(&hash, h->manifest_model_sha.data());
      UpdateField(&hash, h->manifest_tokenizer_sha.data());
      hash.Final(artifact_sha.data());
    }

    // --- Build the three files' own content. ---
    namespace js = semb::json;

    js::Value header = js::Value::MakeObject();
    header.Set("schemaVersion", js::Value::MakeInt(1));
    header.Set("dims", js::Value::MakeInt(static_cast<int64_t>(dims)));
    header.Set("count", js::Value::MakeInt(static_cast<int64_t>(n)));
    header.Set("metric", js::Value::MakeString("cosine"));
    header.Set("dtype", js::Value::MakeString("float32"));
    js::Value ids = js::Value::MakeArray();
    for (size_t i = 0; i < n; ++i) {
      ids.as_array.push_back(js::Value::MakeString(std::string(docs[i].id, docs[i].id_len)));
    }
    header.Set("ids", std::move(ids));
    const std::string header_text = js::Write(header);

    const std::string bin_bytes(reinterpret_cast<const char*>(aligned_rows), required);

    uint8_t wa_sha[32];
    const semb_status wa_status = semb_wa_digest(wa_sha);
    if (wa_status != SEMB_OK) return wa_status;

    js::Value witness = js::Value::MakeObject();
    // This bake's own act of encoding `docs` is, for this manifest's own
    // purposes, the witness pass its corpus_digest describes -- reusing
    // the top-level corpus_digest rather than inventing a second,
    // differently-scoped one; this seat's own derivation (filed as a
    // finding, see the build record), since sec 3.4's prose does not
    // pin whether this sub-field is the SAME digest or a distinct one
    // scoped to W-A/W-B's own fixed witness corpus.
    witness.Set("corpus_digest", js::Value::MakeString(Hex(corpus_digest.data(), 32)));
    witness.Set("wa_digest", js::Value::MakeString(Hex(wa_sha, 32)));
    witness.Set("wb_digest", js::Value::MakeString(Hex(h->wb_digest.data(), 32)));
    witness.Set("cpu_tier", js::Value::MakeString(semb::certification::DispatchedTier()));

    js::Value provenance = js::Value::MakeObject();
    provenance.Set("object_code_sha", js::Value::MakeString(kObjectCodeShaUncomputedLabel));
    provenance.Set("engine_object_code_sha", js::Value::MakeString(kEngineObjectCodeShaUncomputedLabel));
    provenance.Set("semb_source_sha", js::Value::MakeString(kSourceShaUncomputedLabel));
    provenance.Set("engine_tree_sha", js::Value::MakeString(SEMB_ENGINE_PIN_TREE_SHA));
    provenance.Set("engine_path_sha", js::Value::MakeString(SEMB_ENGINE_PATH_SHA));
    provenance.Set("engine_chunk_budget", js::Value::MakeInt(static_cast<int64_t>(h->chunk_budget)));
    provenance.Set("artifact_sha", js::Value::MakeString(Hex(artifact_sha.data(), 32)));
    provenance.Set("witness", std::move(witness));

    js::Value manifest = js::Value::MakeObject();
    manifest.Set("encoder_fingerprint", js::Value::MakeString(Hex(h->fingerprint.data(), 32)));
    manifest.Set("model_sha", js::Value::MakeString(Hex(h->manifest_model_sha.data(), 32)));
    manifest.Set("tokenizer_sha", js::Value::MakeString(Hex(h->manifest_tokenizer_sha.data(), 32)));
    manifest.Set("template_sha256", js::Value::MakeString(Hex(h->manifest_template_sha256.data(), 32)));
    manifest.Set("witness_sha", js::Value::MakeString(Hex(h->manifest_witness_sha.data(), 32)));
    manifest.Set("certification_epoch", js::Value::MakeString(Hex(h->manifest_certification_epoch.data(), 32)));
    manifest.Set("certification_identity_path", js::Value::MakeString("docs/certification-identity.json"));
    manifest.Set("certified_builds_path", js::Value::MakeString("docs/certified-builds.json"));
    // U2-3 items 1 and 12: which list the build compiled in and which cell it
    // resolved to on this host, and the descriptor the verdict matched --
    // provenance, never fingerprint inputs.
    manifest.Set("certified_builds_sha256",
                 js::Value::MakeString(Hex(semb::certification::CompiledListSha256().data(), 32)));
    manifest.Set("certified_cell", js::Value::MakeString(semb::certification::ProcessVerdict().cell));
    {
      const semb::certification::Descriptor& d = semb::certification::ThisBuild();
      js::Value descriptor = js::Value::MakeObject();
      descriptor.Set("os", js::Value::MakeString(d.os));
      descriptor.Set("isa", js::Value::MakeString(d.isa));
      descriptor.Set("toolchain", js::Value::MakeString(d.toolchain));
      descriptor.Set("stdlib", js::Value::MakeString(d.stdlib));
      descriptor.Set("configuration", js::Value::MakeString(d.configuration));
      descriptor.Set("buildSystem", js::Value::MakeString(d.build_system));
      descriptor.Set("dispatch", js::Value::MakeString(d.dispatch));
      descriptor.Set("compileFlags", js::Value::MakeString(d.compile_flags));
      descriptor.Set("engineFlags", js::Value::MakeString(d.engine_flags));
      manifest.Set("build_descriptor", std::move(descriptor));
    }
    // U2-3 items 13 and 14: the engine named by content -- the build record the linked engine reports, which
    // the open that produced this handle checked against the record configure verified.
    manifest.Set("engine_build_record", js::Value::MakeString(semb::engine_link::LinkedBuildRecord()));
    manifest.Set("provenance", std::move(provenance));
    manifest.Set("label:ps_version", js::Value::MakeString(kPsVersionLabel));
    manifest.Set("label:engine_path_id", js::Value::MakeString(kEngineAdapterIdLabel));
    manifest.Set("label:superslm_tag_commit",
                 js::Value::MakeString(std::string(SEMB_ENGINE_PIN_TAG) + " @ " + SEMB_ENGINE_PIN_COMMIT));
    manifest.Set("label:semb_version", js::Value::MakeString(superembedder::Version()));
    manifest.Set("count", js::Value::MakeInt(static_cast<int64_t>(n)));
    manifest.Set("dims", js::Value::MakeInt(static_cast<int64_t>(dims)));
    // TE-193 P-4: the manifest binds metric and dtype the same way it
    // binds count/dims -- the bake's own header values, written here so
    // semb_bank_open can refuse a header edited after the bake (metric is
    // the one consumer-visible header field an editor could otherwise flip
    // past the manifest binding with every cross-check still passing).
    manifest.Set("metric", js::Value::MakeString("cosine"));
    manifest.Set("dtype", js::Value::MakeString("float32"));
    manifest.Set("row_digest", js::Value::MakeString(Hex(row_digest.data(), 32)));
    manifest.Set("corpus_digest", js::Value::MakeString(Hex(corpus_digest.data(), 32)));
    manifest.Set("machine_tier", js::Value::MakeString(semb::certification::DispatchedTier()));
    // U3 (te266 sec 4.3; docs/FORMAT.md): labels only, never fingerprint or
    // verdict inputs. `device` names the forward that emitted the rows; a GPU
    // bake adds the adapter it ran on, and a CPU bake writes no gpu_* label.
    manifest.Set("device", js::Value::MakeString(h->device == SEMB_DEVICE_GPU ? "gpu" : "cpu"));
    if (h->device == SEMB_DEVICE_GPU) {
      manifest.Set("gpu_adapter_index", js::Value::MakeString(h->gpu_labels.adapter_index));
      manifest.Set("gpu_vendor_id", js::Value::MakeString(h->gpu_labels.vendor_id));
      manifest.Set("gpu_device_id", js::Value::MakeString(h->gpu_labels.device_id));
      manifest.Set("gpu_adapter_name", js::Value::MakeString(h->gpu_labels.adapter_name));
      manifest.Set("gpu_driver_version", js::Value::MakeString(h->gpu_labels.driver_version));
    }
    manifest.Set("token_id_digest", js::Value::MakeString(Hex(token_id_digest.data(), 32)));
    js::Value token_counts = js::Value::MakeArray();
    for (size_t i = 0; i < n; ++i) token_counts.as_array.push_back(js::Value::MakeInt(per_row_token_counts[i]));
    manifest.Set("per_row_token_counts", std::move(token_counts));
    const std::string manifest_text = js::Write(manifest);

    const std::string stem(path_stem, path_stem_len);

    // TE-184 (D-SLM6440): a missing (or, so far as this check can tell,
    // unwritable) output directory is a path/output problem -- checked
    // explicitly, before AtomicTripleWriter is even constructed, so it
    // reports SEMB_ARTIFACT_UNREADABLE (this primitive's own path-problem
    // status, already used above for an empty path_stem) rather than
    // falling into the generic catch below via AtomicTripleWriter's
    // temp-file open failure, which reads as an engine/model failure to a
    // caller. Found against a real clean-room build tree whose `_scratch`
    // output directory did not yet exist (`tools/te166_bake_239_timer.cpp`,
    // D-SLM6436): root-caused to AtomicTripleWriter::WriteFile's
    // std::ofstream failing to open the temp file and throwing
    // std::runtime_error, indistinguishable at the generic catch from any
    // other unexpected failure.
    const std::filesystem::path stem_path(stem);
    const std::filesystem::path out_dir =
        stem_path.has_parent_path() ? stem_path.parent_path() : std::filesystem::path(".");
    std::error_code out_dir_ec;
    if (!std::filesystem::is_directory(out_dir, out_dir_ec)) {
      return SEMB_ARTIFACT_UNREADABLE;
    }

    AtomicTripleWriter writer(stem + ".wvbank.json", stem + ".wvbank.bin", stem + ".semb.json");
    writer.Commit(header_text, bin_bytes, manifest_text);

    return SEMB_OK;
  } catch (const std::bad_alloc&) {
    return SEMB_ALLOCATION_FAILED;
  } catch (const std::length_error&) {
    return SEMB_ALLOCATION_FAILED;
  } catch (...) {
    // The output directory's existence is checked explicitly above and
    // never reaches here; this generic catch is now only an I/O failure
    // mid-write after that check passed (a race, a permission change
    // after the check, disk full) or any other unexpected exception this
    // function's own logic did not anticipate.
    return SEMB_ENGINE_REFUSED;
  }
}

extern "C" semb_status semb_bake_to_path(semb_encoder enc, const semb_document* docs, size_t n,
                                          const char* path_stem, size_t path_stem_len) {
  return semb::c_boundary::Status([&] {
    return BakeToPathBody(enc, docs, n, path_stem, path_stem_len);
  });
}
