#include "superembedder/semb.h"

#include "superembedder/encoder_interim_abi.h"
#include "query_internal.h"
#include "semb_wq_population.h"
#include "c_boundary.h"

#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <map>
#include <vector>

#include "superslm/sha256.h"

// These are deliberately TU-private declarations: the frozen public surface
// remains semb.h's fourteen verbs. The query implementation alone bridges the
// bank registry and encoder's opening-time W-Q composition.
extern "C" semb_status semb_bank_acquire_query(semb_bank bank, const uint8_t fingerprint[32]);
extern "C" semb_status semb_bank_release_query(semb_bank bank);
namespace semb {
semb_status EncoderFingerprint(semb_encoder enc, uint8_t out[32]);
semb_status EncoderAcquireQuery(semb_encoder enc);
semb_status EncoderReleaseQuery(semb_encoder enc);
semb_status EncoderDims(semb_encoder enc, uint32_t* out_dims);
bool EncoderValidUtf8(const char* data, size_t size);
semb_status EncodeQueryComposedForWitness(semb_encoder_s* encoder,
                                          std::string_view composed,
                                          size_t max_content_ids,
                                          std::vector<float>* out_vector);
semb_status EncodeQueryComposed(semb_encoder enc, std::string_view composed,
                                size_t max_content_ids,
                                std::vector<float>* out_vector);
}  // namespace semb

const char semb::kQueryPrefix[] =
    "Instruct: Given a web search query, retrieve relevant passages that answer the query\nQuery:";

struct semb_query_opaque {
  semb_encoder encoder = nullptr;
  semb_bank bank = nullptr;
  std::atomic<uint32_t> active_calls{0};
};

namespace {

using semb::kQueryPrefix;

// These values are generated from the configure-verified committed fixture.
// The truncation cap travels beside that population in its manifest.
constexpr size_t kWitnessQueryContentCap = semb::kWqTruncationContentCap;

std::string ComposeQuery(std::string_view text) {
  std::string composed;
  composed.reserve(sizeof(kQueryPrefix) - 1 + text.size());
  composed.append(kQueryPrefix);
  composed.append(text);
  return composed;
}

bool ParseCommittedTexts(std::string_view jsonl, std::vector<std::string_view>* out) {
  if (!out) return false;
  out->clear();
  size_t line_start = 0;
  while (line_start < jsonl.size()) {
    const size_t line_end = jsonl.find('\n', line_start);
    const std::string_view line = jsonl.substr(
        line_start, (line_end == std::string_view::npos ? jsonl.size() : line_end) - line_start);
    if (!line.empty()) {
      constexpr std::string_view kTextKey = "\"text\": \"";
      const size_t text_start = line.find(kTextKey);
      if (text_start == std::string_view::npos || line.size() < 2 ||
          line.substr(line.size() - 2) != "\"}") return false;
      const size_t value_start = text_start + kTextKey.size();
      if (value_start > line.size() - 2) return false;
      out->push_back(line.substr(value_start, line.size() - 2 - value_start));
    }
    if (line_end == std::string_view::npos) break;
    line_start = line_end + 1;
  }
  return true;
}

std::string BuildTruncationQuery() {
  std::vector<std::string_view> queries;
  if (!ParseCommittedTexts(semb::kWqTruncationJsonl, &queries) || queries.size() != 1) return {};
  return std::string(queries.front());
}

void UpdateU64(superslm::Sha256* hash, uint64_t value) {
  uint8_t bytes[8];
  for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = static_cast<uint8_t>(value >> (8 * i));
  hash->Update(bytes, sizeof(bytes));
}

semb_status HashWitnessPosition(semb_encoder_s* encoder, std::string_view raw_query,
                                size_t max_content_ids, superslm::Sha256* hash) {
  const std::string composed = ComposeQuery(raw_query);
  std::vector<float> vector;
  const semb_status encoded =
      semb::EncodeQueryComposedForWitness(encoder, composed, max_content_ids, &vector);
  if (encoded == SEMB_ALLOCATION_FAILED) return SEMB_ALLOCATION_FAILED;
  if (encoded != SEMB_OK) return SEMB_ENGINE_REFUSED;
  const size_t byte_count = vector.size() * sizeof(float);
  UpdateU64(hash, static_cast<uint64_t>(byte_count));
  hash->Update(reinterpret_cast<const uint8_t*>(vector.data()), byte_count);
  return SEMB_OK;
}

std::mutex& RegistryMutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}
// R20 (U2-1): an ordered container. Its member functions do no floating-point
// arithmetic, so no certified object carries a substitutable floating-point
// instantiation for it; the registry is read only by find, emplace and erase,
// never iterated, so its order changes nothing.
using RegistryType = std::map<semb_query, std::unique_ptr<semb_query_opaque>>;
RegistryType& Registry() {
  static auto* registry = new RegistryType();
  return *registry;
}
bool AllocateCookie(semb_query* out) {
  static uintptr_t next = 1;
  if (next == 0) return false;
  *out = reinterpret_cast<semb_query>(next++);
  return true;
}

class QueryLease {
 public:
  explicit QueryLease(semb_query q) {
    if (!q) return;
    std::lock_guard<std::mutex> lock(RegistryMutex());
    const auto found = Registry().find(q);
    if (found == Registry().end()) return;
    found_ = true;
    uint32_t expected = 0;
    if (!found->second->active_calls.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return;
    query_ = found->second.get();
  }
  ~QueryLease() {
    if (query_) query_->active_calls.store(0, std::memory_order_release);
  }
  semb_query_opaque* get() const { return query_; }
  bool found() const { return found_; }
 private:
  semb_query_opaque* query_ = nullptr;
  bool found_ = false;
};

}  // namespace

semb_status semb_compute_wq_digest(semb_encoder_s* encoder, std::array<uint8_t, 32>* out_digest) {
  if (!encoder || !out_digest) return SEMB_ENGINE_REFUSED;
  std::vector<std::string_view> join_queries;
  if (!ParseCommittedTexts(semb::kWqJoinJsonl, &join_queries) || join_queries.size() != 5) {
    return SEMB_ENGINE_REFUSED;
  }
  if (BuildTruncationQuery().empty()) return SEMB_ENGINE_REFUSED;
  superslm::Sha256 hash;
  UpdateU64(&hash, join_queries.size() + 1);
  for (std::string_view query : join_queries) {
    const semb_status st =
        HashWitnessPosition(encoder, query, std::numeric_limits<size_t>::max(), &hash);
    if (st != SEMB_OK) return st;
  }
  const semb_status st =
      HashWitnessPosition(encoder, BuildTruncationQuery(), kWitnessQueryContentCap, &hash);
  if (st != SEMB_OK) return st;
  hash.Final(out_digest->data());
  return SEMB_OK;
}

namespace semb {

semb_status EncodeQuery(semb_encoder enc, std::string_view text, std::vector<float>* out_vector) {
  const std::string composed = ComposeQuery(text);
  return EncodeQueryComposed(enc, composed, std::numeric_limits<size_t>::max(), out_vector);
}

}  // namespace semb

static semb_status QueryOpenBody(semb_encoder enc, semb_bank bank, semb_query* out) {
  if (out) *out = nullptr;
  if (!enc || !bank || !out) return SEMB_NULL_ARGUMENT;
  uint8_t fingerprint[32];
  const semb_status fingerprint_status = semb::EncoderFingerprint(enc, fingerprint);
  if (fingerprint_status != SEMB_OK) return fingerprint_status;
  const semb_status encoder_status = semb::EncoderAcquireQuery(enc);
  if (encoder_status != SEMB_OK) return encoder_status;
  const semb_status bank_status = semb_bank_acquire_query(bank, fingerprint);
  if (bank_status != SEMB_OK) {
    (void)semb::EncoderReleaseQuery(enc);
    return bank_status;
  }
  // U2-10 Builds item 2b: both counts are raised, so this guard owns them
  // and releases the bank's and the encoder's on every exit, an exception
  // included, until the query is in the registry.
  struct CountsGuard {
    semb_encoder enc;
    semb_bank bank;
    bool armed = true;
    ~CountsGuard() {
      if (!armed) return;
      (void)semb_bank_release_query(bank);
      (void)semb::EncoderReleaseQuery(enc);
    }
  } counts_guard{enc, bank};
  try {
    auto query = std::make_unique<semb_query_opaque>();
    query->encoder = enc;
    query->bank = bank;
    semb_query cookie = nullptr;
    {
      std::lock_guard<std::mutex> lock(RegistryMutex());
      if (!AllocateCookie(&cookie)) return SEMB_ALLOCATION_FAILED;
      Registry().emplace(cookie, std::move(query));
      counts_guard.armed = false;
    }
    *out = cookie;
    return SEMB_OK;
  } catch (const std::bad_alloc&) {
    return SEMB_ALLOCATION_FAILED;
  }
}

extern "C" semb_status semb_query_open(semb_encoder enc, semb_bank bank, semb_query* out) {
  return semb::c_boundary::Status([&] {
    return QueryOpenBody(enc, bank, out);
  });
}

static semb_status QueryEncodeBody(semb_query q, const char* utf8, size_t utf8_len,
                                   float* out_vector, size_t out_capacity_bytes,
                                   size_t* out_required_bytes) {
  if (!q || !out_required_bytes) return SEMB_NULL_ARGUMENT;
  QueryLease lease(q);
  if (!lease.get()) return lease.found() ? SEMB_HANDLE_IN_USE : SEMB_INVALID_HANDLE;
  uint32_t dims = 0;
  const semb_status dims_status = semb::EncoderDims(lease.get()->encoder, &dims);
  if (dims_status != SEMB_OK) return dims_status;
  const size_t required = static_cast<size_t>(dims) * sizeof(float);
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
  if (!semb::EncoderValidUtf8(utf8, utf8_len)) return SEMB_INVALID_UTF8;
  try {
    std::vector<float> staged_vector;
    const semb_status encoded =
        semb::EncodeQuery(lease.get()->encoder, std::string_view(utf8, utf8_len), &staged_vector);
    if (encoded != SEMB_OK) return encoded;
    std::memcpy(out_vector, staged_vector.data(), required);
    *out_required_bytes = required;
    return SEMB_OK;
  } catch (const std::bad_alloc&) {
    return SEMB_ALLOCATION_FAILED;
  }
}

extern "C" semb_status semb_query_encode(semb_query q, const char* utf8, size_t utf8_len,
                                            float* out_vector, size_t out_capacity_bytes,
                                            size_t* out_required_bytes) {
  return semb::c_boundary::Status([&] {
    return QueryEncodeBody(q, utf8, utf8_len, out_vector, out_capacity_bytes, out_required_bytes);
  });
}

static semb_status QueryCloseBody(semb_query q) {
  if (!q) return SEMB_NULL_ARGUMENT;
  std::unique_ptr<semb_query_opaque> closing;
  {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    const auto found = Registry().find(q);
    if (found == Registry().end()) return SEMB_INVALID_HANDLE;
    if (found->second->active_calls.load(std::memory_order_acquire) != 0) return SEMB_HANDLE_IN_USE;
    closing = std::move(found->second);
    Registry().erase(found);
  }
  // U2-10 Builds item 2b: both releases run on every exit; the encoder's
  // runs whether the bank's returned or threw.
  semb_status released = SEMB_ENGINE_REFUSED;
  try {
    released = semb_bank_release_query(closing->bank);
  } catch (...) {
    (void)semb::EncoderReleaseQuery(closing->encoder);
    throw;
  }
  const semb_status encoder_released = semb::EncoderReleaseQuery(closing->encoder);
  return released != SEMB_OK ? released : encoder_released;
}

extern "C" semb_status semb_query_close(semb_query q) {
  return semb::c_boundary::Status([&] {
    return QueryCloseBody(q);
  });
}

static semb_status FingerprintOfBody(semb_encoder enc, semb_fingerprint* out) {
  if (!enc || !out) return SEMB_NULL_ARGUMENT;
  if (out->struct_size != sizeof(semb_fingerprint)) return SEMB_STRUCT_SIZE_MISMATCH;
  // The encoder fingerprint: the value semb_encode returns, every manifest
  // this encoder bakes records, and semb_query_open compares (plan sec 3.3).
  return semb::EncoderFingerprint(enc, out->digest);
}

extern "C" semb_status semb_fingerprint_of(semb_encoder enc, semb_fingerprint* out) {
  return semb::c_boundary::Status([&] {
    return FingerprintOfBody(enc, out);
  });
}

// U2-7 item 6 (R17): the body-token count. The composition is ComposeQuery,
// the tokenization is the one EncodeQueryComposed runs, and the forward does
// not run; the prefix's own count is taken the same way on every call.
static SembInterimStatus QueryTokenCountBody(semb_encoder enc, const char* utf8, size_t utf8_len,
                                             int64_t* out_body_tokens) {
  if (!enc || !out_body_tokens || (!utf8 && utf8_len > 0)) return kSembInterimNullArgument;
  const std::string composed = ComposeQuery(std::string_view(utf8 ? utf8 : "", utf8_len));
  size_t composed_tokens = 0;
  size_t prefix_tokens = 0;
  semb_status counted = semb::ComposedQueryTokenCount(enc, composed, &composed_tokens);
  if (counted == SEMB_OK) {
    counted = semb::ComposedQueryTokenCount(enc, std::string_view(kQueryPrefix), &prefix_tokens);
  }
  if (counted == SEMB_NULL_ARGUMENT) return kSembInterimNullArgument;
  if (counted != SEMB_OK) return kSembInterimEngineRefused;
  *out_body_tokens = static_cast<int64_t>(composed_tokens) - static_cast<int64_t>(prefix_tokens);
  return kSembInterimOk;
}

extern "C" SembInterimStatus semb_interim_query_token_count(semb_encoder enc, const char* utf8,
                                                            size_t utf8_len, int64_t* out_body_tokens) {
  return semb::c_boundary::Interim(nullptr, [&] {
    return QueryTokenCountBody(enc, utf8, utf8_len, out_body_tokens);
  });
}
