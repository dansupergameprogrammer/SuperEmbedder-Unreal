// src/superembedder/bank.cpp -- TE-166 (U1 slice 4): `semb_bank_open` /
// `semb_bank_close` (`Claude/Plans/SuperEmbedder_Plan.md` sec 3.1, sec 3.4,
// sec 8 C9/C18/C34; `tests/semb_bank_abi_contract.h` is the suite's own
// concrete pin). Opens and validates a `.wvbank.json`/`.wvbank.bin`/
// `.semb.json` triple per FORMAT.md sec 1's own importer obligations plus
// the manifest's own internal self-consistency (cell 3, reading (a),
// D-SLM6391: no encoder argument, no comparison to any encoder -- the
// handle's own fingerprint validity is checked entirely from the
// manifest's own clear-text fields).
#include "superembedder/semb.h"

#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <map>
#include <set>
#include <vector>

#include "json_lite.h"
#include "c_boundary.h"
#include "engine_link_check.h"
#include "superslm/sha256.h"

#include "superembedder/bank_internal_abi.h"
#include "superembedder/manifest_labels_abi.h"

#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
#include "support/semb_bank_open_seam.h"
#include <chrono>
#include <condition_variable>
#endif

// Completes semb.h's own forward declaration (`typedef struct
// semb_bank_opaque* semb_bank;`, global scope) -- defined here at GLOBAL
// scope too, not inside the anonymous namespace below, so this is the
// SAME type the public header names rather than a second, unrelated type
// that happens to share the name (MSVC C2872 "ambiguous symbol" is the
// build's own proof the two would otherwise collide).
//
// TE-191 F4: previously empty, so an open handle owned nothing and
// Slice 5's frozen checked path (`semb_query_open` reads the expected
// fingerprint from the already-open bank, sec 3.1/D-SLM4145) had no field
// to read without re-opening the manifest itself -- exactly what the
// checked-path design forbids (open validates once; nothing is re-read
// from disk afterward). The handle now retains the expected fingerprint
// semb_bank_open already validated (the same 32 bytes `encoder_fingerprint`
// decodes to) and the geometry (`count`, `dims`) the header already
// validated -- both already computed by the checks above, copied once,
// never re-read from disk after open. `bank_internal_abi.h`'s
// `semb_bank_debug_state` and `semb_bank_ranked_state` are the read sides,
// kept internal/non-frozen rather than added to the twelve verbs (sec 3.1,
// D-SLM4646).
struct semb_bank_opaque {
  std::array<uint8_t, 32> fingerprint{};
  int64_t count = 0;
  int64_t dims = 0;
  std::vector<std::string> ids;
  std::vector<const char*> id_data;
  std::vector<size_t> id_lengths;
  std::vector<float> rows;
  uint32_t live_queries = 0;
};

#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
namespace semb::test_seam {

static std::mutex snapshot_state_mutex;
static std::condition_variable snapshot_state_changed;
static uint32_t snapshot_park_budget = 0;
static uint32_t snapshot_parked = 0;
static uint64_t snapshot_release_generation = 0;

void OnBankSnapshotMaterialize() {
  std::unique_lock<std::mutex> lock(snapshot_state_mutex);
  if (snapshot_park_budget == 0) return;
  --snapshot_park_budget;
  ++snapshot_parked;
  const uint64_t generation = snapshot_release_generation;
  snapshot_state_changed.notify_all();
  snapshot_state_changed.wait(lock, [generation] { return snapshot_release_generation != generation; });
  --snapshot_parked;
}

}  // namespace semb::test_seam

extern "C" int32_t semb_bank_test_seam_arm_snapshot_park(uint32_t count) {
  std::lock_guard<std::mutex> lock(semb::test_seam::snapshot_state_mutex);
  if (semb::test_seam::snapshot_parked != 0) return 0;
  semb::test_seam::snapshot_park_budget = count;
  return 1;
}

extern "C" uint32_t semb_bank_test_seam_snapshot_parked(void) {
  std::lock_guard<std::mutex> lock(semb::test_seam::snapshot_state_mutex);
  return semb::test_seam::snapshot_parked;
}

extern "C" int32_t semb_bank_test_seam_snapshot_wait_parked(uint32_t n, uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(semb::test_seam::snapshot_state_mutex);
  return semb::test_seam::snapshot_state_changed.wait_for(
      lock, std::chrono::milliseconds(timeout_ms),
      [n] { return semb::test_seam::snapshot_parked >= n; }) ? 1 : 0;
}

extern "C" void semb_bank_test_seam_snapshot_release(void) {
  std::lock_guard<std::mutex> lock(semb::test_seam::snapshot_state_mutex);
  ++semb::test_seam::snapshot_release_generation;
  semb::test_seam::snapshot_park_budget = 0;
  semb::test_seam::snapshot_state_changed.notify_all();
}

extern "C" int32_t semb_bank_test_seam_snapshot_reset(void) {
  std::lock_guard<std::mutex> lock(semb::test_seam::snapshot_state_mutex);
  if (semb::test_seam::snapshot_parked != 0) return 0;
  semb::test_seam::snapshot_park_budget = 0;
  ++semb::test_seam::snapshot_release_generation;
  return 1;
}
#endif

namespace {

// FORMAT.md sec 1's own header-geometry ceilings, mirrored as compiled
// constants (the same convention SEMB_MAX_CORPUS_ROWS already uses in
// encoder.cpp for the batch verb's own row ceiling).
constexpr int64_t kMaxCrossDeviceDims = 131072;  // FORMAT.md: dims > 0 and <= 131,072
constexpr int64_t kMaxBankRows = int64_t{1} << 28;  // FORMAT.md: count >= 0 and <= 2^28
constexpr int64_t kMaxU1BankRows = 10'000'000;  // D-SLM6498: U1 reader ceiling

bool IsJsonWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Reads an ordinary top-level integer member without creating a JSON Value
// tree. The caller validates a candidate over-ceiling header with json_lite
// before refusing it, so malformed and duplicate-key forms remain owned by
// the full parser and report its normal malformed-input status.
bool TryReadTopLevelCount(const std::string& text, int64_t* out) {
  size_t i = 0;
  auto skip_ws = [&]() {
    while (i < text.size() && IsJsonWhitespace(text[i])) ++i;
  };
  auto scan_string = [&](size_t* begin, size_t* end, bool* escaped) -> bool {
    if (i >= text.size() || text[i++] != '"') return false;
    *begin = i;
    *escaped = false;
    while (i < text.size()) {
      const char c = text[i++];
      if (c == '\\') {
        *escaped = true;
        if (i >= text.size()) return false;
        ++i;
      } else if (c == '"') {
        *end = i - 1;
        return true;
      }
    }
    return false;
  };
  auto scan_value = [&]() -> bool {
    size_t nesting = 0;
    bool in_string = false;
    bool escaped = false;
    while (i < text.size()) {
      const char c = text[i];
      if (in_string) {
        ++i;
        if (escaped) escaped = false;
        else if (c == '\\') escaped = true;
        else if (c == '"') in_string = false;
        continue;
      }
      if (c == '"') {
        in_string = true;
        ++i;
      } else if (c == '{' || c == '[') {
        ++nesting;
        ++i;
      } else if (c == '}' || c == ']') {
        if (nesting == 0) return c == '}';
        --nesting;
        ++i;
      } else if (nesting == 0 && c == ',') {
        return true;
      } else {
        ++i;
      }
    }
    return false;
  };

  skip_ws();
  if (i >= text.size() || text[i++] != '{') return false;
  bool found = false;
  int64_t count = 0;
  while (true) {
    skip_ws();
    if (i >= text.size()) return false;
    if (text[i] == '}') {
      ++i;
      skip_ws();
      if (i != text.size() || !found) return false;
      *out = count;
      return true;
    }
    size_t key_begin = 0, key_end = 0;
    bool key_escaped = false;
    if (!scan_string(&key_begin, &key_end, &key_escaped)) return false;
    skip_ws();
    if (i >= text.size() || text[i++] != ':') return false;
    skip_ws();
    const bool is_count = !key_escaped && text.compare(key_begin, key_end - key_begin, "count") == 0;
    if (is_count) {
      if (found) return false;
      const char* first = text.data() + i;
      const char* last = text.data() + text.size();
      const auto parsed = std::from_chars(first, last, count);
      if (parsed.ec != std::errc{} || parsed.ptr == first) return false;
      i = static_cast<size_t>(parsed.ptr - text.data());
      if (i < text.size() && !IsJsonWhitespace(text[i]) && text[i] != ',' && text[i] != '}') return false;
      found = true;
    } else if (!scan_value()) {
      return false;
    }
    skip_ws();
    if (i >= text.size()) return false;
    if (text[i] == ',') {
      ++i;
      continue;
    }
    if (text[i] == '}') {
      ++i;
      skip_ws();
      if (i != text.size() || !found) return false;
      *out = count;
      return true;
    }
    return false;
  }
}

std::mutex& RegistryMutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}
// R20 (U2-1): an ordered container. Its member functions do no floating-point
// arithmetic, so no certified object carries a substitutable floating-point
// instantiation for it; the registry is read only by find, emplace and erase,
// never iterated, so its order changes nothing.
using HandleRegistry = std::map<semb_bank, std::unique_ptr<semb_bank_opaque>>;
HandleRegistry& Registry() {
  static auto* registry = new HandleRegistry();
  return *registry;
}
bool AllocateHandleCookie(semb_bank* out) {
  static uintptr_t next = 1;
  if (next == 0) return false;
  *out = reinterpret_cast<semb_bank>(next++);
  return true;
}

bool ReadFileBytes(const std::string& path, std::string* out) {
  std::ifstream stream;
  stream.exceptions(std::ios::badbit);
  try {
    stream.open(path, std::ios::binary | std::ios::ate);
  } catch (const std::ios_base::failure&) {
    return false;
  }
  if (!stream) return false;
  const std::streamoff end = stream.tellg();
  if (end < 0) return false;
  out->resize(static_cast<size_t>(end));
  stream.seekg(0, std::ios::beg);
  if (!stream) return false;
  if (end == 0) return true;
  try {
    return static_cast<bool>(stream.read(out->data(), static_cast<std::streamsize>(end)));
  } catch (const std::ios_base::failure&) {
    return false;
  }
}

void Hash(const void* data, size_t size, uint8_t out[32]) {
  superslm::Sha256Hash(static_cast<const uint8_t*>(data), size, out);
}
// TE-191 F7: FORMAT.md sec 1's payload is raw little-endian float32, not a
// live, aligned `float[]` -- `std::string`'s storage promises contiguous
// `char`s only. `memcpy` into a `float` object is the portable decoder;
// direct typed dereference through a reinterpret_cast is undefined
// behavior when the source pointer is not `alignof(float)`-aligned (the
// review's own construction: a build enforcing alignment/object-lifetime
// rules, or an unaligned input buffer). Every host this project builds
// for (Windows/Linux/macOS, x86-64 and ARM64) is little-endian, so the
// memcpy'd bytes are already in the right order; no byte-swap is needed
// on any currently supported host.
float DecodeFloatLE(const char* bytes) {
  static_assert(sizeof(float) == 4, "FORMAT.md payload floats are 4 bytes");
  float v;
  std::memcpy(&v, bytes, sizeof(float));
  return v;
}
void UpdateU64(superslm::Sha256* hash, uint64_t value) {
  uint8_t bytes[8];
  for (size_t i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(value >> (8 * i));
  hash->Update(bytes, sizeof(bytes));
}
void UpdateField(superslm::Sha256* hash, const std::vector<uint8_t>& field) {
  UpdateU64(hash, static_cast<uint64_t>(field.size()));
  hash->Update(field.data(), field.size());
}
std::string Hex(const uint8_t* d, size_t n) {
  static const char* kHexDigits = "0123456789abcdef";
  std::string out(n * 2, '0');
  for (size_t i = 0; i < n; ++i) {
    out[2 * i] = kHexDigits[(d[i] >> 4) & 0xF];
    out[2 * i + 1] = kHexDigits[d[i] & 0xF];
  }
  return out;
}
bool HexToBytes(const std::string& hex, std::vector<uint8_t>* out) {
  if (hex.size() % 2 != 0) return false;
  out->resize(hex.size() / 2);
  for (size_t i = 0; i < out->size(); ++i) {
    auto nibble = [](char c, int* v) -> bool {
      if (c >= '0' && c <= '9') { *v = c - '0'; return true; }
      if (c >= 'a' && c <= 'f') { *v = c - 'a' + 10; return true; }
      if (c >= 'A' && c <= 'F') { *v = c - 'A' + 10; return true; }
      return false;
    };
    int hi = 0, lo = 0;
    if (!nibble(hex[2 * i], &hi) || !nibble(hex[2 * i + 1], &lo)) return false;
    (*out)[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

}  // namespace

static semb_status BankOpenBody(const char* path, size_t path_len, semb_bank* out) {
  if (out) *out = nullptr;
  if ((!path && path_len > 0) || !out) return SEMB_NULL_ARGUMENT;
  if (!path || path_len == 0) return SEMB_ARTIFACT_UNREADABLE;

  // U2-3 item 14: bank open runs engine code (Sha256) with no encoder handle, so it reads the linked engine's
  // identity first too, before any bank file is read.
  const semb::engine_link::Verdict link = semb::engine_link::CheckLinkedEngine();
  if (link.status != SEMB_OK) return link.status;

  try {
    const std::string stem(path, path_len);
    const std::string header_path = stem + ".wvbank.json";
    const std::string bin_path = stem + ".wvbank.bin";
    const std::string manifest_path = stem + ".semb.json";

    std::string header_text;
    if (!ReadFileBytes(header_path, &header_text)) return SEMB_ARTIFACT_UNREADABLE;
    int64_t header_count = 0;
    if (TryReadTopLevelCount(header_text, &header_count) && header_count > kMaxU1BankRows) {
      // The cheap scan establishes only that this text contains an ordinary
      // top-level integer count. Before that count can take the read-boundary
      // refusal, validate the entire header with json_lite's own grammar and
      // budgets without building a Value tree: headers are extensible and can
      // carry wide per-row arrays or unknown members. Thus the scan changes no
      // header status except that a parser-accepted over-ceiling header refuses
      // before payload or manifest reads.
      try {
        semb::json::Validate(header_text);
      } catch (const std::bad_alloc&) {
        throw;  // U2-10 Builds item 2b: an allocation failure reaches the boundary
      } catch (const std::length_error&) {
        throw;
      } catch (const std::exception&) {
        return SEMB_ARTIFACT_UNREADABLE;
      }
      return SEMB_BANK_GEOMETRY_CEILING_EXCEEDED;
    }

    std::string bin_bytes, manifest_text;
    if (!ReadFileBytes(bin_path, &bin_bytes)) return SEMB_ARTIFACT_UNREADABLE;
    if (!ReadFileBytes(manifest_path, &manifest_text)) return SEMB_ARTIFACT_UNREADABLE;

    namespace js = semb::json;
    js::Value header, manifest;
    try {
      header = js::Parse(header_text);
      manifest = js::Parse(manifest_text);
    } catch (const std::bad_alloc&) {
      throw;  // U2-10 Builds item 2b: an allocation failure reaches the boundary
    } catch (const std::length_error&) {
      throw;
    } catch (const std::exception&) {
      return SEMB_ARTIFACT_UNREADABLE;
    }
    if (!header.IsObject() || !manifest.IsObject()) return SEMB_ARTIFACT_UNREADABLE;

    // FORMAT.md sec 1: "schemaVersion ... Unknown versions are rejected."
    const js::Value* schema_version = header.Find("schemaVersion");
    if (!schema_version || schema_version->kind != js::Kind::kInt || schema_version->as_int != 1) {
      return SEMB_MANIFEST_SCHEMA_UNSUPPORTED;
    }

    // TE-193 P-3: FORMAT.md sec 1's own field table makes "channels" a
    // schemaVersion-2-only table ("and required by it"), and the reference
    // importer refuses it by name (wvbank.py: "channels table requires
    // schemaVersion 2"). A v1 header carrying it violates the v1 schema
    // itself -- the same "unknown schemaVersion, metric, or dtype"
    // importer obligation class the check above already refuses
    // schemaVersion with, so it reuses that status. Any schemaVersion
    // other than 1 is already refused above, so every "channels" key
    // reaching here is a v1 header.
    if (header.Find("channels")) return SEMB_MANIFEST_SCHEMA_UNSUPPORTED;

    const js::Value* dims_v = header.Find("dims");
    const js::Value* count_v = header.Find("count");
    if (!dims_v || dims_v->kind != js::Kind::kInt || !count_v || count_v->kind != js::Kind::kInt) {
      return SEMB_ARTIFACT_UNREADABLE;
    }
    const int64_t dims = dims_v->as_int;
    const int64_t count = count_v->as_int;

    // FORMAT.md sec 1: "header geometry over the format ceilings ...
    // rejected on the header fields alone, BEFORE any payload size is
    // computed from them." D-SLM6393 provisionally names the over-ceiling
    // class SEMB_BANK_GEOMETRY_CEILING_EXCEEDED (D-SLM6392). TE-193 P-12:
    // a dims of 0 or a negative count is not "over a ceiling" -- it is a
    // malformed header value, the same class every other malformed header
    // field here refuses with (SEMB_ARTIFACT_UNREADABLE); the plan's own
    // ordinal-31 row names only "dims > 131,072 or count > 2^28".
    if (dims <= 0 || count < 0) return SEMB_ARTIFACT_UNREADABLE;
    if (dims > kMaxCrossDeviceDims || count > kMaxBankRows || count > kMaxU1BankRows) {
      return SEMB_BANK_GEOMETRY_CEILING_EXCEEDED;
    }

    const js::Value* metric_v = header.Find("metric");
    const js::Value* dtype_v = header.Find("dtype");
    if (!metric_v || !metric_v->IsString() || !dtype_v || !dtype_v->IsString()) {
      return SEMB_ARTIFACT_UNREADABLE;
    }

    // TE-191 F2: FORMAT.md sec 1's own admitted sets -- metric is "dot",
    // "cosine", or "l2"; dtype is "float32" (the interchange payload's
    // only defined dtype). Previously only the STRING-ness of these two
    // fields was checked, so "unknown"/"garbage" opened as SEMB_OK. Both
    // are FORMAT.md's own single "unknown schemaVersion, metric, or
    // dtype" importer obligation (schemaVersion already has
    // SEMB_MANIFEST_SCHEMA_UNSUPPORTED above); SEMB_BANK_METRIC_DTYPE_UNSUPPORTED
    // is this round's own provisional name for the other two fields of
    // that same obligation (D-SLM6467; routed to the planner in the build
    // record).
    {
      const bool metric_admitted = metric_v->as_string == "dot" ||
                                    metric_v->as_string == "cosine" ||
                                    metric_v->as_string == "l2";
      const bool dtype_admitted = dtype_v->as_string == "float32";
      if (!metric_admitted || !dtype_admitted) {
        return SEMB_BANK_METRIC_DTYPE_UNSUPPORTED;
      }
    }

    // TE-193 P-4: the manifest binds metric and dtype the same way it
    // binds count/dims below -- the bake writes both into the manifest
    // (encoder.cpp), and open compares header against manifest so a
    // header edited after the bake refuses rather than silently changing
    // how the bank is scored (FORMAT.md sec 2: a cosine bank is normalized
    // at bake and dot-scored; l2 is a squared distance where lower is
    // better) and silently retiring the cosine-only zero-norm scan above.
    // A header/manifest disagreement over either field is the metric/dtype
    // integrity class -- SEMB_BANK_METRIC_DTYPE_UNSUPPORTED (ordinal 32,
    // D-SLM6467), the same status the admitted-set check above already
    // names for this pair of fields. A manifest missing either field is a
    // required-field absence, the same class every other missing manifest
    // field here refuses with.
    {
      const js::Value* manifest_metric_v = manifest.Find("metric");
      const js::Value* manifest_dtype_v = manifest.Find("dtype");
      if (!manifest_metric_v || !manifest_metric_v->IsString() ||
          !manifest_dtype_v || !manifest_dtype_v->IsString()) {
        return SEMB_ARTIFACT_UNREADABLE;
      }
      if (manifest_metric_v->as_string != metric_v->as_string ||
          manifest_dtype_v->as_string != dtype_v->as_string) {
        return SEMB_BANK_METRIC_DTYPE_UNSUPPORTED;
      }
    }

    // Payload size disagreeing with the header (checked AFTER the ceiling
    // check above, per FORMAT.md's own ordering, so the multiplication
    // below is over already-bounded operands).
    const uint64_t expected_bytes =
        static_cast<uint64_t>(count) * static_cast<uint64_t>(dims) * 4ull;
    if (static_cast<uint64_t>(bin_bytes.size()) != expected_bytes) {
      return SEMB_BANK_SIZE_MISMATCH;
    }

    // ids: "duplicate or wrongly-counted ids" rejected (FORMAT.md sec 1);
    // sec 3.4 requires ids on SuperEmbedder's own banks. Missing or a
    // count other than `count` -> the provisionally-named
    // SEMB_BANK_IDS_COUNT_MISMATCH (D-SLM6392); a duplicate -> the
    // already-pinned SEMB_DOCUMENT_ID_DUPLICATE, reused (C18/C4's own
    // identical semantic, per this round's own casebook).
    const js::Value* ids_v = header.Find("ids");
    if (!ids_v || !ids_v->IsArray() || static_cast<int64_t>(ids_v->as_array.size()) != count) {
      return SEMB_BANK_IDS_COUNT_MISMATCH;
    }
    {
      // R20 (U2-1): an ordered set, for the reason the handle registry is one;
      // it is read only by emplace and never iterated.
      std::set<std::string_view> seen;
      for (const auto& id : ids_v->as_array) {
        if (!id.IsString()) return SEMB_BANK_IDS_COUNT_MISMATCH;
        if (!seen.emplace(std::string_view(id.as_string)).second) {
          return SEMB_DOCUMENT_ID_DUPLICATE;
        }
      }
    }

    // A non-finite row value (FORMAT.md sec 1's own importer obligation).
    // D-SLM6392's own proposed name: SEMB_BANK_NONFINITE_ROW.
    {
      const char* base = bin_bytes.data();
      const size_t total = static_cast<size_t>(count) * static_cast<size_t>(dims);
      for (size_t i = 0; i < total; ++i) {
        if (!std::isfinite(DecodeFloatLE(base + i * sizeof(float)))) return SEMB_BANK_NONFINITE_ROW;
      }
    }

    // A zero-norm row under metric "cosine" (FORMAT.md sec 1; sec 2 of
    // FORMAT.md: "zero-norm rows are a bake-time error" under cosine).
    // D-SLM6392's own proposed name: SEMB_BANK_ZERO_NORM_ROW.
    //
    // TE-193 P-7: the scan computes NO floating-point arithmetic. The
    // previous double sum-of-squares is exactly equivalent to the integer
    // bit test below, so the swap changes no accepted or refused input:
    // after the finite check above, every element is finite; a finite
    // float's square is non-negative and is zero iff the element is +-0.0
    // (double cannot underflow a float's square -- the smallest positive
    // float's square is 2^-298, well inside double's subnormal range --
    // and 131,072 squares of FLT_MAX cannot overflow it either), and a
    // sum of non-negative doubles is zero iff every term is; so sumsq ==
    // 0.0 held iff every element was +-0.0, which holds iff every
    // element's bits with the sign bit masked are zero. That integer form
    // is what the C41 part 4 object-code scan demands of a library TU on
    // the reproducible path: zero floating-point instructions.
    if (metric_v->as_string == "cosine") {
      const char* base = bin_bytes.data();
      for (int64_t row = 0; row < count; ++row) {
        bool all_zero = true;
        const char* row_base = base + static_cast<size_t>(row * dims) * sizeof(float);
        for (int64_t d = 0; d < dims; ++d) {
          uint32_t bits = 0;
          std::memcpy(&bits, row_base + static_cast<size_t>(d) * sizeof(float), sizeof(bits));
          if ((bits & 0x7FFFFFFFu) != 0) {
            all_zero = false;
            break;
          }
        }
        if (all_zero) return SEMB_BANK_ZERO_NORM_ROW;
      }
    }

    // The manifest's own artifact SHA: provenance.artifact_sha, an
    // independent recomputation over model_sha||tokenizer_sha alone
    // (sec 3.4/C18's own "artifact SHA not matching the manifest" class,
    // distinct from the broader encoder_fingerprint self-consistency
    // check below -- see semb_bake_to_path's own comment for why this
    // seat split the two).
    const js::Value* provenance_v = manifest.Find("provenance");
    if (!provenance_v || !provenance_v->IsObject()) return SEMB_ARTIFACT_UNREADABLE;
    const js::Value* artifact_sha_v = provenance_v->Find("artifact_sha");
    const js::Value* model_sha_v = manifest.Find("model_sha");
    const js::Value* tokenizer_sha_v = manifest.Find("tokenizer_sha");
    if (!artifact_sha_v || !artifact_sha_v->IsString() || !model_sha_v || !model_sha_v->IsString() ||
        !tokenizer_sha_v || !tokenizer_sha_v->IsString()) {
      return SEMB_ARTIFACT_UNREADABLE;
    }
    {
      std::vector<uint8_t> model_sha, tokenizer_sha;
      if (!HexToBytes(model_sha_v->as_string, &model_sha) ||
          !HexToBytes(tokenizer_sha_v->as_string, &tokenizer_sha)) {
        return SEMB_ARTIFACT_UNREADABLE;
      }
      superslm::Sha256 hash;
      UpdateField(&hash, model_sha);
      UpdateField(&hash, tokenizer_sha);
      uint8_t recomputed[32];
      hash.Final(recomputed);
      if (Hex(recomputed, 32) != artifact_sha_v->as_string) return SEMB_ARTIFACT_SHA_MISMATCH;
    }

    // The manifest's own encoder_fingerprint: the PURE sec 3.3 formula
    // (SHA-256 over the five clear-text components, length-prefixed, no
    // tag) -- reading (a), D-SLM6391: checked entirely against the
    // manifest's OWN fields, no encoder argument, no comparison to
    // anything this handle does not already own.
    const js::Value* fingerprint_v = manifest.Find("encoder_fingerprint");
    if (!fingerprint_v || !fingerprint_v->IsString()) return SEMB_ARTIFACT_UNREADABLE;
    std::array<uint8_t, 32> fingerprint_bytes{};
    {
      static const char* kFields[] = {"model_sha", "tokenizer_sha", "template_sha256", "witness_sha",
                                       "certification_epoch"};
      superslm::Sha256 hash;
      for (const char* key : kFields) {
        const js::Value* field_v = manifest.Find(key);
        if (!field_v || !field_v->IsString()) return SEMB_ARTIFACT_UNREADABLE;
        std::vector<uint8_t> bytes;
        if (!HexToBytes(field_v->as_string, &bytes)) return SEMB_ARTIFACT_UNREADABLE;
        UpdateField(&hash, bytes);
      }
      hash.Final(fingerprint_bytes.data());
      if (Hex(fingerprint_bytes.data(), 32) != fingerprint_v->as_string) return SEMB_FINGERPRINT_MISMATCH;
    }

    // TE-191 F3+F6: the manifest binds the pair -- previously
    // semb_bank_open never read or cross-checked manifest count, dims,
    // row_digest, corpus_digest, the per-row token-count cardinality, or
    // the two labels this build can verify without an encoder argument,
    // so a manifest disagreeing with its own paired header/payload (or
    // with itself) still opened. Each disagreement below is its own named
    // refusal (D-SLM6467's own ruling on F3+F6):
    //
    // count/dims: reuses SEMB_BANK_SIZE_MISMATCH, the existing "size
    // disagrees with the header" class -- a manifest/header geometry
    // disagreement is the same kind of defect the bin-size check above
    // already names.
    const js::Value* manifest_count_v = manifest.Find("count");
    const js::Value* manifest_dims_v = manifest.Find("dims");
    if (!manifest_count_v || manifest_count_v->kind != js::Kind::kInt ||
        !manifest_dims_v || manifest_dims_v->kind != js::Kind::kInt ||
        manifest_count_v->as_int != count || manifest_dims_v->as_int != dims) {
      return SEMB_BANK_SIZE_MISMATCH;
    }

    // per-row token-count cardinality: one entry per row is the same
    // cardinality obligation ids already carries above -- a length
    // disagreement is a size disagreement, reusing the same status.
    const js::Value* token_counts_v = manifest.Find("per_row_token_counts");
    if (!token_counts_v || !token_counts_v->IsArray() ||
        static_cast<int64_t>(token_counts_v->as_array.size()) != count) {
      return SEMB_BANK_SIZE_MISMATCH;
    }

    // row_digest: recomputed over the payload bytes just read and
    // validated above -- SHA-256(bin_bytes), the same construction
    // semb_bake_to_path itself used to produce the value (encoder.cpp).
    const js::Value* row_digest_v = manifest.Find("row_digest");
    if (!row_digest_v || !row_digest_v->IsString()) return SEMB_ARTIFACT_UNREADABLE;
    {
      uint8_t recomputed_row_digest[32];
      Hash(bin_bytes.data(), bin_bytes.size(), recomputed_row_digest);
      if (Hex(recomputed_row_digest, 32) != row_digest_v->as_string) {
        return SEMB_BANK_DIGEST_MISMATCH;
      }
    }

    // corpus_digest: NOT recomputed here -- the original document text it
    // hashes (encoder.cpp's own corpus-digest construction: per-row
    // length-prefixed id || length-prefixed text) is not retained
    // anywhere in the wvbank pair, only ids and the pooled vectors are, so
    // nothing on disk lets semb_bank_open re-derive it from first
    // principles. What open CAN check is the manifest's own internal
    // self-consistency: D-SLM6409(b) makes provenance.witness.corpus_digest
    // and the top-level corpus_digest the SAME value by construction
    // (this bake's own act of encoding is its own witness pass), so a
    // manifest whose two copies disagree is refused even though the
    // corpus itself is unavailable to check against.
    const js::Value* corpus_digest_v = manifest.Find("corpus_digest");
    const js::Value* witness_v = provenance_v->Find("witness");
    if (!corpus_digest_v || !corpus_digest_v->IsString() || !witness_v || !witness_v->IsObject()) {
      return SEMB_ARTIFACT_UNREADABLE;
    }
    const js::Value* witness_corpus_digest_v = witness_v->Find("corpus_digest");
    if (!witness_corpus_digest_v || !witness_corpus_digest_v->IsString()) {
      return SEMB_ARTIFACT_UNREADABLE;
    }
    if (witness_corpus_digest_v->as_string != corpus_digest_v->as_string) {
      return SEMB_BANK_DIGEST_MISMATCH;
    }

    // The two labels open can verify without an encoder argument
    // (D-SLM6391's own "no comparison to any encoder" rule for fingerprint
    // validity applies the same way here: these are free, handle-less
    // accessors reporting THIS BUILD's own compiled-in identity, sec 3.4's
    // label-check table's own mechanism, not a caller-supplied encoder).
    // superslm_tag_commit is checked by tools/pin-superslm at the point it
    // writes the lock (sec 3.4's own table); label:semb_version needs no
    // cell "by construction" (sec 3.4's own table) -- neither is
    // semb_bank_open's concern.
    const js::Value* label_engine_path_id_v = manifest.Find("label:engine_path_id");
    if (!label_engine_path_id_v || !label_engine_path_id_v->IsString() ||
        label_engine_path_id_v->as_string != semb_adapter_self_id()) {
      return SEMB_ADAPTER_ID_MISMATCH;
    }
    // TE-193 P-5: accept exactly what the bake writes. semb_bake_to_path
    // compares against semb_ps_n_version_label() alone (encoder.cpp), so
    // "PS-1" is a label this build never emits -- the either/or accept was
    // a spelling check rather than the honesty check sec 3.4's label table
    // exists for ("a stale label misleads a human during exactly the
    // bisect this plan specifies for itself"). The shape is the sibling
    // engine_path_id check immediately above: pinned to the one value this
    // build's own bake path writes.
    const js::Value* label_ps_version_v = manifest.Find("label:ps_version");
    if (!label_ps_version_v || !label_ps_version_v->IsString() ||
        label_ps_version_v->as_string != semb_ps_n_version_label()) {
      return SEMB_PS_VERSION_LABEL_MISMATCH;
    }

    // Materialize the complete immutable snapshot before taking the
    // process-global registry lock.  This copies and decodes only values
    // validated above; independent bank/query operations never wait for
    // payload-sized work from this open.
    std::unique_ptr<semb_bank_opaque> handle;
    {
#ifdef SEMB_ENABLE_WORKSPACE_TEST_SEAM
      semb::test_seam::OnBankSnapshotMaterialize();
#endif
      handle = std::make_unique<semb_bank_opaque>();
      handle->fingerprint = fingerprint_bytes;
      handle->count = count;
      handle->dims = dims;
      handle->ids.reserve(ids_v->as_array.size());
      for (const auto& id : ids_v->as_array) handle->ids.push_back(id.as_string);
      handle->id_data.reserve(handle->ids.size());
      handle->id_lengths.reserve(handle->ids.size());
      for (const std::string& id : handle->ids) {
        handle->id_data.push_back(id.data());
        handle->id_lengths.push_back(id.size());
      }
      const size_t total = static_cast<size_t>(count) * static_cast<size_t>(dims);
      handle->rows.resize(total);
      for (size_t i = 0; i < total; ++i) {
        handle->rows[i] = DecodeFloatLE(bin_bytes.data() + i * sizeof(float));
      }
    }

    semb_bank cookie = nullptr;
    {
      // RegistryMutex guards only the cookie namespace and handle registry.
      std::lock_guard<std::mutex> lock(RegistryMutex());
      if (!AllocateHandleCookie(&cookie)) return SEMB_ALLOCATION_FAILED;
      Registry().emplace(cookie, std::move(handle));
    }
    *out = cookie;
    return SEMB_OK;
  } catch (const std::bad_alloc&) {
    return SEMB_ALLOCATION_FAILED;
  } catch (const std::length_error&) {
    return SEMB_ALLOCATION_FAILED;
  } catch (...) {
    return SEMB_ARTIFACT_UNREADABLE;
  }
}

extern "C" semb_status semb_bank_open(const char* path, size_t path_len, semb_bank* out) {
  return semb::c_boundary::Status<SEMB_ARTIFACT_UNREADABLE>([&] {
    return BankOpenBody(path, path_len, out);
  });
}

static semb_status BankCloseBody(semb_bank bank) {
  if (!bank) return SEMB_NULL_ARGUMENT;
  std::unique_ptr<semb_bank_opaque> closing;
  {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    const auto found = Registry().find(bank);
    if (found == Registry().end()) return SEMB_INVALID_HANDLE;
    if (found->second->live_queries != 0) return SEMB_BANK_IN_USE;
    closing = std::move(found->second);
    Registry().erase(found);
  }
  closing.reset();
  return SEMB_OK;
}

extern "C" semb_status semb_bank_close(semb_bank bank) {
  return semb::c_boundary::Status([&] {
    return BankCloseBody(bank);
  });
}

static semb_status BankAcquireQueryBody(semb_bank bank, const uint8_t fingerprint[32]) {
  if (!bank || !fingerprint) return SEMB_NULL_ARGUMENT;
  std::lock_guard<std::mutex> lock(RegistryMutex());
  const auto found = Registry().find(bank);
  if (found == Registry().end()) return SEMB_INVALID_HANDLE;
  if (std::memcmp(found->second->fingerprint.data(), fingerprint, 32) != 0) {
    return SEMB_FINGERPRINT_MISMATCH;
  }
  ++found->second->live_queries;
  return SEMB_OK;
}

extern "C" semb_status semb_bank_acquire_query(semb_bank bank, const uint8_t fingerprint[32]) {
  return semb::c_boundary::Status([&] {
    return BankAcquireQueryBody(bank, fingerprint);
  });
}

static semb_status BankReleaseQueryBody(semb_bank bank) {
  if (!bank) return SEMB_NULL_ARGUMENT;
  std::lock_guard<std::mutex> lock(RegistryMutex());
  const auto found = Registry().find(bank);
  if (found == Registry().end()) return SEMB_INVALID_HANDLE;
  if (found->second->live_queries == 0) return SEMB_INVALID_HANDLE;
  --found->second->live_queries;
  return SEMB_OK;
}

extern "C" semb_status semb_bank_release_query(semb_bank bank) {
  return semb::c_boundary::Status([&] {
    return BankReleaseQueryBody(bank);
  });
}

// TE-191 F4 (bank_internal_abi.h): reads exactly what `bank` retained at
// open -- nothing here re-reads the manifest, header, or payload from
// disk.
static semb_status BankDebugStateBody(semb_bank bank, uint8_t out_fingerprint[32],
                                      int64_t* out_count, int64_t* out_dims) {
  if (!bank || !out_fingerprint || !out_count || !out_dims) return SEMB_NULL_ARGUMENT;
  std::lock_guard<std::mutex> lock(RegistryMutex());
  const auto found = Registry().find(bank);
  if (found == Registry().end()) return SEMB_INVALID_HANDLE;
  const semb_bank_opaque& state = *found->second;
  std::memcpy(out_fingerprint, state.fingerprint.data(), state.fingerprint.size());
  *out_count = state.count;
  *out_dims = state.dims;
  return SEMB_OK;
}

extern "C" semb_status semb_bank_debug_state(semb_bank bank, uint8_t out_fingerprint[32],
                                              int64_t* out_count, int64_t* out_dims) {
  return semb::c_boundary::Status([&] {
    return BankDebugStateBody(bank, out_fingerprint, out_count, out_dims);
  });
}

static semb_status BankRankedStateBody(semb_bank bank, semb_bank_ranked_view* out) {
  if (!bank || !out) return SEMB_NULL_ARGUMENT;
  std::lock_guard<std::mutex> lock(RegistryMutex());
  const auto found = Registry().find(bank);
  if (found == Registry().end()) return SEMB_INVALID_HANDLE;
  const semb_bank_opaque& state = *found->second;
  out->ids = state.id_data.data();
  out->id_lengths = state.id_lengths.data();
  out->rows = state.rows.data();
  out->count = state.count;
  out->dims = state.dims;
  return SEMB_OK;
}

extern "C" semb_status semb_bank_ranked_state(semb_bank bank, semb_bank_ranked_view* out) {
  return semb::c_boundary::Status([&] {
    return BankRankedStateBody(bank, out);
  });
}
