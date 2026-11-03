// src/main.cpp -- Slice 6's `semb` command-line drivers over the frozen C ABI.
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "certification.h"
#include "engine_identity/superslm_build_identity.h"
#include "engine_link_check.h"
#include "json_lite.h"
#include "semb_engine_pin.h"
#include "superembedder/bake_abi.h"
#include "superembedder/bank_internal_abi.h"
#include "superembedder/semb.h"
#include "superembedder/version.h"

namespace {

void PrintStatus(const char* where, semb_status status) {
  std::cerr << where << ": " << semb_status_name(status) << " ("
            << static_cast<int>(status) << ")\n";
}

int Refuse(const char* where, semb_status status) {
  PrintStatus(where, status);
  return 1;
}

const char* Option(const std::vector<std::string>& args, const char* name) {
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == name) return args[i + 1].c_str();
  }
  return nullptr;
}

bool HasOption(const std::vector<std::string>& args, const char* name) {
  return std::find(args.begin(), args.end(), name) != args.end();
}

// TE-356 (C-2): `--max-concurrency` for `semb bake` / `semb verify`, parsed
// like `semb query`'s own `--top` (std::from_chars, the whole option value
// consumed, zero refused). Absent, `default_value` (kBakeMaxConcurrency)
// applies. This is the CALLER's own ceiling; the caller below caps it to the
// corpus's own document count and this process's hardware concurrency before
// this value ever reaches open. Open reserves nothing in proportion to it
// (U2-9): it bounds how many encodes run at once, and each encode's workspace
// is sized to its document.
bool ParseMaxConcurrencyOption(const std::vector<std::string>& args, uint32_t default_value,
                               uint32_t* out_value) {
  const char* raw = Option(args, "--max-concurrency");
  if (!raw) {
    *out_value = default_value;
    return true;
  }
  uint32_t parsed_value = 0;
  const auto parsed = std::from_chars(raw, raw + std::strlen(raw), parsed_value);
  if (parsed.ec != std::errc{} || *parsed.ptr != '\0' || parsed_value == 0) return false;
  *out_value = parsed_value;
  return true;
}

// U3 (te266 sec 4.1, CLI): `--device cpu|gpu` for `semb bake` and `semb
// query`, default cpu. Any other value is refused SEMB_DEVICE_UNSUPPORTED
// before anything opens. `semb verify` takes no --device.
bool ParseDeviceOption(const char* raw, uint32_t* out_device) {
  if (!raw || std::strcmp(raw, "cpu") == 0) {
    *out_device = SEMB_DEVICE_CPU;
    return true;
  }
  if (std::strcmp(raw, "gpu") == 0) {
    *out_device = SEMB_DEVICE_GPU;
    return true;
  }
  return false;
}

struct QueryArguments {
  const char* model = nullptr;
  const char* tokenizer = nullptr;
  const char* top = nullptr;
  const char* device = nullptr;
  bool help = false;
  bool stdin_mode = false;
  bool hex = false;
  bool raw = false;
  std::vector<std::string> positional;
};

// Query deliberately parses left-to-right.  The `--` boundary ends option
// parsing, so option-shaped query text is data there; before it, each known
// option spelling remains control syntax even when options follow positionals.
bool ParseQueryArguments(const std::vector<std::string>& args, QueryArguments* out,
                         const char** bad_option) {
  bool options = true;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& argument = args[i];
    if (!options) {
      out->positional.push_back(argument);
      continue;
    }
    if (argument == "--") {
      options = false;
      continue;
    }
    if (argument == "--help") {
      out->help = true;
      continue;
    }
    if (argument == "--stdin") {
      out->stdin_mode = true;
      continue;
    }
    if (argument == "--hex") {
      out->hex = true;
      continue;
    }
    if (argument == "--raw") {
      out->raw = true;
      continue;
    }
    const char** value = nullptr;
    if (argument == "--model") value = &out->model;
    if (argument == "--tokenizer") value = &out->tokenizer;
    if (argument == "--top") value = &out->top;
    if (argument == "--device") value = &out->device;
    if (value) {
      if (i + 1 == args.size() || args[i + 1] == "--") {
        *bad_option = argument.c_str();
        return false;
      }
      *value = args[++i].c_str();
      continue;
    }
    out->positional.push_back(argument);
  }
  return true;
}

bool ReadFile(const std::string& path, std::string* out) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) return false;
  const std::streamoff size = stream.tellg();
  if (size < 0) return false;
  out->resize(static_cast<size_t>(size));
  stream.seekg(0);
  return size == 0 || static_cast<bool>(stream.read(out->data(), size));
}

// Verify reuses the shipping bake primitive instead of duplicating either
// corpus- or row-digest construction in the CLI. Its private directory lives
// under the system temp root, never beside the bank being checked. Verify
// checks Cleanup before each return; the destructor retries only as fallback.
class TemporaryBakeDirectory {
 public:
  bool Create() {
    std::error_code error;
    const std::filesystem::path root = std::filesystem::temp_directory_path(error);
    if (error) return false;
    static std::atomic<unsigned long long> counter{0};
    const auto ticks = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (unsigned int attempt = 0; attempt < 128; ++attempt) {
      path_ = root / ("semb-verify-" + std::to_string(ticks) + "-" +
                      std::to_string(counter.fetch_add(1, std::memory_order_relaxed)));
      if (std::filesystem::create_directory(path_, error)) {
        created_ = true;
        return true;
      }
      if (error && error != std::errc::file_exists) return false;
      error.clear();
    }
    return false;
  }

  const std::filesystem::path& path() const { return path_; }

  bool Cleanup() {
    if (!created_) return true;
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    if (error) return false;
    created_ = false;
    path_.clear();
    return true;
  }

  ~TemporaryBakeDirectory() {
    (void)Cleanup();
  }

 private:
  std::filesystem::path path_;
  bool created_ = false;
};

bool ManifestField(const std::string& manifest, const char* field, std::string* out) {
  try {
    const semb::json::Value parsed = semb::json::Parse(manifest);
    const semb::json::Value* value = parsed.Find(field);
    if (!value || !value->IsString()) return false;
    *out = value->as_string;
    return true;
  } catch (...) {
    return false;
  }
}

bool LoadCorpus(const std::string& path, std::vector<std::string>* ids,
                std::vector<std::string>* texts, std::vector<semb_document>* docs) {
  // Binary mode on every platform (U2-7 item 3, F19): a line is the bytes up
  // to each LF, so a CR is document text everywhere, and a final line without
  // an LF is still a line.
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return false;
  // Format is selected by the path, never by a line's contents: a
  // case-insensitive `.jsonl` extension selects JSONL; every other extension
  // is one UTF-8 text document per physical line, with the zero-based line
  // number as its id.
  std::string extension = std::filesystem::path(path).extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  const bool jsonl = extension == ".jsonl";
  std::string line;
  size_t line_number = 0;
  try {
    while (std::getline(stream, line)) {
      if (jsonl) {
        if (line.empty()) continue;
        const semb::json::Value row = semb::json::Parse(line);
        const semb::json::Value* id = row.Find("id");
        const semb::json::Value* text = row.Find("text");
        if (!row.IsObject() || !id || !text || !id->IsString() || !text->IsString()) return false;
        ids->push_back(id->as_string);
        texts->push_back(text->as_string);
      } else {
        ids->push_back(std::to_string(line_number));
        texts->push_back(line);
      }
      ++line_number;
    }
  } catch (...) {
    return false;
  }
  docs->clear();
  docs->reserve(ids->size());
  for (size_t i = 0; i < ids->size(); ++i) {
    docs->push_back(semb_document{sizeof(semb_document), ids->at(i).data(), ids->at(i).size(),
                                  texts->at(i).data(), texts->at(i).size()});
  }
  return true;
}

struct Handles {
  semb_encoder encoder = nullptr;
  semb_bank bank = nullptr;
  semb_query query = nullptr;
  ~Handles() {
    if (query) (void)semb_query_close(query);
    if (bank) (void)semb_bank_close(bank);
    if (encoder) (void)semb_encoder_close(encoder);
  }
};

// TE-350: the workspace pool `semb bake` (and `semb verify`'s own re-encode,
// which is a bake in every way that matters here) requests, sized to the
// machine rather than to 1 -- `semb_interim_encode_batch_ps_n`'s own T =
// min(documents, max_concurrency, hardware concurrency) means a pool of 1 is what
// pinned every bake to a single worker thread regardless of how many logical
// CPUs the box has.
//
// TE-356 (C-2): `kBakeMaxConcurrency` was a fixed 8, sized from ONE
// development box's own 32 logical CPUs and ~64 GiB of RAM and compiled
// into every user's copy of a published 1.0 CLI -- on the real
// Qwen3-Embedding-0.6B artifact (num_layers=28, context_cap=32768,
// num_kv_heads=8, head_dim=128) one workspace slot is 1,879,048,192 bytes
// (~1.75 GiB, fixed by context_cap alone, independent of corpus or document
// length), so a pool of 8 committed ~14 GiB at open UNCONDITIONALLY, even
// for a one-document bake on a machine with 4 GiB free. That was the rule
// before U2-9. It is now a CEILING, not a request: OpenBakeConcurrency
// (below) sizes the actual request from min(this ceiling, the corpus's own
// document count, this process's own hardware concurrency). Since U2-9 open
// reserves no workspace at all, and each encode's slot is sized to its own
// document (engine_adapter_interim.cpp's semb::Encode), so the ceiling
// bounds concurrency, not memory committed at open. `--max-concurrency`
// (ParseMaxConcurrencyOption, above) lets a caller lower or raise the
// ceiling itself; it composes with, and never bypasses, either adaptive term.
constexpr uint32_t kBakeMaxConcurrency = 8;

// TE-356 (C-2): the document-count and hardware-concurrency halves of the
// adaptive policy above. `doc_count` is the caller's OWN count of
// what it is actually about to bake/re-encode (post-empty-document-filter),
// never the raw corpus size. Hardware concurrency is applied HERE, not
// inside the shared library open path, because it means a specific thing
// for bake/verify -- one worker thread per slot, inside ONE batch call --
// that does not hold for every caller of `semb_encoder_open` (a caller
// wanting one slot per independent CONCURRENT caller, never more than one
// worker per call, has no reason to have its own request second-guessed by
// this machine's own core count).
uint32_t OpenBakeConcurrency(uint32_t requested_ceiling, size_t doc_count) {
  const unsigned hc = std::thread::hardware_concurrency();
  const size_t hw_cap = hc == 0 ? size_t{1} : static_cast<size_t>(hc);
  const size_t doc_cap = std::max<size_t>(doc_count, size_t{1});
  return static_cast<uint32_t>(std::min<size_t>({static_cast<size_t>(requested_ceiling), doc_cap, hw_cap}));
}

semb_status OpenEncoder(const char* model, const char* tokenizer, Handles* handles,
                        uint32_t max_concurrency, uint32_t device = SEMB_DEVICE_CPU) {
  if (!model || !tokenizer) return SEMB_NULL_ARGUMENT;
  const semb_config config{sizeof(config), model, std::strlen(model), tokenizer,
                           std::strlen(tokenizer), max_concurrency, device};
  return semb_encoder_open(&config, &handles->encoder);
}

semb_status OpenCheckedQuery(const std::string& stem, const char* model, const char* tokenizer,
                             Handles* handles, uint32_t max_concurrency, uint32_t device) {
  semb_status status = OpenEncoder(model, tokenizer, handles, max_concurrency, device);
  if (status != SEMB_OK) return status;
  status = semb_bank_open(stem.data(), stem.size(), &handles->bank);
  if (status != SEMB_OK) return status;
  return semb_query_open(handles->encoder, handles->bank, &handles->query);
}

semb_status QueryVector(semb_query query, std::string_view text, std::vector<float>* vector) {
  size_t required = 0;
  semb_status status = semb_query_encode(query, text.data(), text.size(), nullptr, 0, &required);
  if (status != SEMB_BUFFER_TOO_SMALL) return status;
  vector->assign(required / sizeof(float), 0.0f);
  return semb_query_encode(query, text.data(), text.size(), vector->data(), required, &required);
}

std::string Hex(const std::vector<float>& vector) {
  static constexpr char kHex[] = "0123456789abcdef";
  const auto* bytes = reinterpret_cast<const unsigned char*>(vector.data());
  std::string out;
  out.reserve(vector.size() * sizeof(float) * 2);
  for (size_t i = 0; i < vector.size() * sizeof(float); ++i) {
    out.push_back(kHex[bytes[i] >> 4]);
    out.push_back(kHex[bytes[i] & 0x0f]);
  }
  return out;
}

std::string TopHitsJson(semb_bank bank, const std::vector<float>& query, size_t top) {
  semb_bank_ranked_view snapshot{};
  if (semb_bank_ranked_state(bank, &snapshot) != SEMB_OK || snapshot.count < 0 || snapshot.dims <= 0 ||
      query.size() != static_cast<size_t>(snapshot.dims)) {
    return {};
  }
  struct Hit { size_t index; double score; };
  std::vector<Hit> hits;
  hits.reserve(static_cast<size_t>(snapshot.count));
  for (size_t row = 0; row < static_cast<size_t>(snapshot.count); ++row) {
    double dot = 0.0, query_norm = 0.0, row_norm = 0.0;
    for (size_t dim = 0; dim < static_cast<size_t>(snapshot.dims); ++dim) {
      const double q = query[dim];
      const double r = snapshot.rows[row * static_cast<size_t>(snapshot.dims) + dim];
      dot += q * r;
      query_norm += q * q;
      row_norm += r * r;
    }
    const double denominator = std::sqrt(query_norm * row_norm);
    hits.push_back(Hit{row, denominator == 0.0 ? -1.0 : dot / denominator});
  }
  std::sort(hits.begin(), hits.end(), [](const Hit& left, const Hit& right) {
    return left.score != right.score ? left.score > right.score : left.index < right.index;
  });
  semb::json::Value output = semb::json::Value::MakeObject();
  semb::json::Value array = semb::json::Value::MakeArray();
  for (size_t i = 0; i < std::min(top, hits.size()); ++i) {
    semb::json::Value hit = semb::json::Value::MakeObject();
    hit.Set("id", semb::json::Value::MakeString(
        std::string(snapshot.ids[hits[i].index], snapshot.id_lengths[hits[i].index])));
    array.as_array.push_back(std::move(hit));
  }
  output.Set("hits", std::move(array));
  return semb::json::Write(output);
}

// TE-317 (D-SLM7405), fix round TE-320: the library's batch validation
// (encoder.cpp step 9, ComposeBatch) refuses a bake all-or-nothing at the
// FIRST document that fails ANY per-document check, in index order -- an
// empty document (utf8_len == 0) is exactly one such check and the one
// SEMB_INPUT_EMPTY names. A tool user commonly did not author the corpus
// (K1-Run-2 died on BEIR fiqa document 447488, `{"id": "447488", "text":
// ""}`, after a 3-hour bake), so the CLI -- never the library -- skips
// exactly that one failure and bakes the rest, reporting what it skipped
// so the record survives the terminal.
//
// The skip test is `utf8_len == 0` alone, deliberately not "does this
// tokenize to zero content tokens": ComposeBatch's own comment
// (encoder.cpp, content_ids[i].empty() check) states that a zero-content-id
// sequence is unreachable for byte-level BPE over a non-empty span, so
// tokenizing first to ask the broader question would pay for a tokenize
// pass to answer a question the tokenizer can never answer differently.
// `utf8_len == 0` is not an approximation of the library's empty-input
// rule -- for this tokenizer, it IS the rule.
//
// A document is skipped only when empty text is its ONLY defect. Step 9's
// own order checks the id (id_len > 0, no NUL byte, valid UTF-8) BEFORE the
// utf8_len == 0 test, so IdWouldPassStep9 mirrors exactly that ordering: a
// document with an empty, NUL-containing, or invalid-UTF-8 id still reaches
// the library and is refused by name for that defect (SEMB_DOCUMENT_ID_INVALID),
// never silently skipped as "empty". `ValidUtf8` below is a local, byte-for-byte
// mirror of `encoder.cpp`'s own `ValidUtf8`: the library does not export that
// check to any header main.cpp includes, so this is "a faithful local check"
// rather than a reused one (TE-320 S-2).
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

// Step 9's id-only checks, in step 9's own order (encoder.cpp: id_len > 0,
// no NUL byte via memchr, valid UTF-8) -- everything step 9 checks about an
// id EXCEPT the utf8_len == 0 test that follows it. A document for which
// this returns true has no id defect at all, so skipping it for empty text
// is skipping exactly the one thing wrong with it.
bool IdWouldPassStep9(const semb_document& doc) {
  if (doc.id_len == 0) return false;
  if (std::memchr(doc.id, '\0', doc.id_len) != nullptr) return false;
  if (!ValidUtf8(doc.id, doc.id_len)) return false;
  return true;
}

void FilterEmptyDocuments(const std::vector<std::string>& ids,
                          const std::vector<semb_document>& docs,
                          std::vector<semb_document>* kept,
                          std::vector<std::string>* skipped_ids) {
  kept->clear();
  skipped_ids->clear();
  kept->reserve(docs.size());
  for (size_t i = 0; i < docs.size(); ++i) {
    if (docs[i].utf8_len == 0 && IdWouldPassStep9(docs[i])) {
      skipped_ids->push_back(ids[i]);
      continue;
    }
    kept->push_back(docs[i]);
  }
}

// TE-320 M-2: filtering can put one id in both the kept batch and the
// skipped list (a duplicate where one twin is empty), or two skipped ids
// can collide with each other -- neither collision ever reaches the
// library's own step-10 duplicate check, because step 9 will have already
// refused the batch (SEMB_INPUT_EMPTY, on the empty twin) before step 10
// runs. Left unchecked, the sidecar would claim an id both shipped and
// skipped, which defeats the reconciliation the sidecar exists for. Scoped
// to collisions a skipped id takes part in; two kept ids colliding with
// each other (no skip involved) is unchanged and still caught by the
// library's own batch call.
bool SkippedIdCollides(const std::vector<semb_document>& kept,
                       const std::vector<std::string>& skipped_ids) {
  if (skipped_ids.empty()) return false;
  std::unordered_set<std::string_view> seen;
  seen.reserve(kept.size() + skipped_ids.size());
  for (const semb_document& doc : kept) {
    seen.emplace(doc.id, doc.id_len);
  }
  for (const std::string& id : skipped_ids) {
    if (!seen.emplace(id).second) return true;
  }
  return false;
}

// TE-317 round 3 (N-4), corrected round 4 (TE-326 M-4): SkippedIdCollides
// above is a CLI-only concept the library cannot see -- the empty twin
// never reaches it, so the library's own step-10 duplicate check never
// fires on this collision. Refusing SEMB_DOCUMENT_ID_DUPLICATE from the
// collision alone, unconditionally, preempts whatever the library would
// have refused FIRST, in its own per-document order over `kept`, if some
// other kept document also has a defect (bad id, or invalid UTF-8 text) --
// at ANY position, not only an earlier one: step 9 (encoder.cpp) validates
// every document before step 10's uniqueness check ever runs, so a kept
// defect positioned AFTER the colliding twin still outranks the duplicate.
// A collision is only safe to refuse preemptively when nothing else in
// kept would fail the library's own checks, checked over the whole of
// kept; otherwise the real call must run so the library reports its own
// first failure, in its own order.
bool AnyKeptDocumentHasALibraryDefect(const std::vector<semb_document>& kept) {
  for (const semb_document& doc : kept) {
    if (!IdWouldPassStep9(doc)) return true;
    if (doc.utf8_len > 0 && !ValidUtf8(doc.utf8, doc.utf8_len)) return true;
  }
  return false;
}

// TE-320 S-4/M-1: the sidecar, `<stem>.skipped.json`, matches the plan's own
// format (`Claude/Plans/te267-u2-release-plan.md` U2-8) -- a JSON array,
// one object per skipped id, `{"id": ..., "reason": "empty_text"}`. JSON's
// own string escaping (json_lite.cpp WriteEscapedString) closes M-1 by
// construction: a tab or newline in an id round-trips through \t/\n rather
// than corrupting a delimited row. Every id that reaches this array has
// already passed IdWouldPassStep9, so none of them can carry a NUL byte or
// invalid UTF-8 in the first place.
std::string SkippedSidecarJson(const std::vector<std::string>& skipped_ids) {
  semb::json::Value array = semb::json::Value::MakeArray();
  for (const std::string& id : skipped_ids) {
    semb::json::Value entry = semb::json::Value::MakeObject();
    entry.Set("id", semb::json::Value::MakeString(id));
    entry.Set("reason", semb::json::Value::MakeString("empty_text"));
    array.as_array.push_back(std::move(entry));
  }
  return semb::json::Write(array);
}

// TE-320 M-3, hardened round 3 (N-2): written to a temp file beside the
// sidecar's own final path, then renamed into place -- the pattern the
// library's own bank-triple writer uses (AtomicTripleWriter::Commit,
// encoder.cpp) -- so a write failure partway through never leaves a
// truncated `.skipped.json` in place of a prior one. The stream is CLOSED
// explicitly and checked AFTER close, before the rename: a sidecar is
// small enough that every byte sits in the stream's own buffer until the
// close-time flush, so a disk-full (or similar) failure at that flush is
// invisible to any check made right after `<<` -- confirmed by execution
// (WSL GCC, a `.tmp` path symlinked to `/dev/full`): checking only after
// `<<` reports success and renames the failed write over a prior good
// sidecar; checking after an explicit `close()` reports failure and never
// renames. TE-320 S-1: called on EVERY successful bake, not only when
// something was skipped, so a clean re-bake at a stem that previously had
// skips overwrites the old sidecar with `[]` instead of leaving it stale.
// TE-326 M-5: on either failure branch below, also remove any PRIOR
// sidecar sitting at the final `path` (best effort, error ignored) --
// UNLESS that path is a directory, which stays exactly as TE-320 F4 left
// it (a caller-created obstruction, not a stale record). Before this, a
// re-bake whose write failed left the PREVIOUS bake's sidecar in place
// beside the NEW bank, silently naming the wrong bake's skips; a missing
// record is honest, a stale one is not. `WriteSkippedSidecar` is only ever
// called after a successful bake (`Bake`, below), so "the bank is already
// valid" always holds when this runs.
void RemoveStaleSidecarBestEffort(const std::string& path) {
  std::error_code error;
  if (!std::filesystem::is_directory(path, error)) {
    std::filesystem::remove(path, error);
  }
}

bool WriteSkippedSidecar(const std::string& stem, const std::vector<std::string>& skipped_ids) {
  const std::string path = stem + ".skipped.json";
  const std::string temp_path = path + ".tmp";
  {
    std::ofstream sidecar(temp_path, std::ios::binary | std::ios::trunc);
    if (!sidecar) return false;
    sidecar << SkippedSidecarJson(skipped_ids);
    sidecar.close();
    if (!sidecar) {
      std::error_code cleanup_error;
      std::filesystem::remove(temp_path, cleanup_error);
      RemoveStaleSidecarBestEffort(path);
      return false;
    }
  }
  std::error_code error;
  std::filesystem::rename(temp_path, path, error);
  if (error) {
    std::error_code cleanup_error;
    std::filesystem::remove(temp_path, cleanup_error);
    RemoveStaleSidecarBestEffort(path);
    return false;
  }
  return true;
}

// TE-326 R-1: beyond JSON's own mandatory escapes, also escapes DEL
// (U+007F), the C1 control range (U+0080-U+009F) and the Unicode line
// separators U+2028/U+2029 as `\uXXXX` -- all legal in an id (valid UTF-8,
// no NUL, so none of them are refused at bake time) and none of them are
// characters JSON itself requires escaping, but each is a de facto line
// boundary or terminal-control byte in at least one real consumer:
// Python's `str.splitlines()` (this project's own test suite) breaks on
// U+0085/U+2028/U+2029, and a C1 control (U+009B, the single-byte CSI, in
// particular) or DEL can act on a terminal. This walk is local to the
// stderr line ONLY -- the sidecar and every manifest keep exactly the
// bytes `json_lite.cpp`'s own `WriteEscapedString` produces, which is why
// this duplicates a codepoint walk here rather than widening that shared
// writer.
std::string EscapeIdForStderr(const std::string& id) {
  std::string out = "\"";
  size_t i = 0;
  while (i < id.size()) {
    const uint8_t lead = static_cast<uint8_t>(id[i]);
    uint32_t codepoint = lead;
    size_t length = 1;
    if ((lead & 0xE0) == 0xC0) {
      codepoint = lead & 0x1F;
      length = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      codepoint = lead & 0x0F;
      length = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      codepoint = lead & 0x07;
      length = 4;
    }
    for (size_t j = 1; j < length && i + j < id.size(); ++j) {
      codepoint = (codepoint << 6) | (static_cast<uint8_t>(id[i + j]) & 0x3F);
    }
    switch (codepoint) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (codepoint < 0x20 || codepoint == 0x7F ||
            (codepoint >= 0x80 && codepoint <= 0x9F) ||
            codepoint == 0x2028 || codepoint == 0x2029) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", codepoint);
          out += buf;
        } else {
          out.append(id, i, length);
        }
    }
    i += length;
  }
  out += "\"";
  return out;
}

// TE-320 S-3/S-4, hardened round 3 (N-3), extended round 4 (TE-326 R-1):
// plan sec U2-8's own stderr shape, one line per skipped id -- the id
// printed via `EscapeIdForStderr` above, not raw and not the sidecar's own
// (narrower) JSON escaping.
void ReportSkipped(const std::vector<std::string>& skipped_ids) {
  for (const std::string& id : skipped_ids) {
    std::cerr << "skipped " << EscapeIdForStderr(id) << ": SEMB_INPUT_EMPTY\n";
  }
}

int Bake(const std::vector<std::string>& args) {
  if (args.empty()) return Refuse("semb bake", SEMB_NULL_ARGUMENT);
  const char* out = Option(args, "--out");
  const char* model = Option(args, "--model");
  const char* tokenizer = Option(args, "--tokenizer");
  if (!out || !model || !tokenizer) return Refuse("semb bake", SEMB_NULL_ARGUMENT);
  uint32_t device = SEMB_DEVICE_CPU;
  if (!ParseDeviceOption(Option(args, "--device"), &device)) {
    return Refuse("semb bake --device", SEMB_DEVICE_UNSUPPORTED);
  }
  uint32_t requested_concurrency = 0;
  if (!ParseMaxConcurrencyOption(args, kBakeMaxConcurrency, &requested_concurrency)) {
    return Refuse("semb bake --max-concurrency", SEMB_NULL_ARGUMENT);
  }
  const std::string corpus = args.front();
  std::vector<std::string> ids, texts;
  std::vector<semb_document> docs;
  if (!LoadCorpus(corpus, &ids, &texts, &docs)) return Refuse("semb bake corpus", SEMB_ARTIFACT_UNREADABLE);
  std::vector<semb_document> kept;
  std::vector<std::string> skipped_ids;
  FilterEmptyDocuments(ids, docs, &kept, &skipped_ids);
  // A non-empty corpus every one of whose documents was skipped has nothing
  // left to bake: refuse exactly as the library would refuse the same
  // all-empty batch (SEMB_INPUT_EMPTY), without paying for the call. An
  // originally-empty corpus (docs.empty()) is unrelated to this rule and
  // falls through to the library unchanged, which refuses it SEMB_CORPUS_EMPTY.
  if (!docs.empty() && kept.empty()) return Refuse("semb bake", SEMB_INPUT_EMPTY);
  if (SkippedIdCollides(kept, skipped_ids) && !AnyKeptDocumentHasALibraryDefect(kept)) {
    return Refuse("semb bake", SEMB_DOCUMENT_ID_DUPLICATE);
  }
  // TE-356 (C-2): never request more workspace slots than there are
  // documents to bake, or than this process's own hardware concurrency --
  // a one-document corpus opens at 1, not kBakeMaxConcurrency's own 8,
  // closing the small-corpus half of the defect at zero cost (the
  // machine-affording half is FinishEncoderOpen's own job, applied
  // uniformly to every caller including this one). `kept` can be empty
  // only when the corpus itself was empty (docs.empty()), the case already
  // excluded from the SEMB_INPUT_EMPTY refusal above; OpenBakeConcurrency's
  // own floor of 1 keeps that case opening exactly as it always has.
  const uint32_t open_concurrency = OpenBakeConcurrency(requested_concurrency, kept.size());
  Handles handles;
  semb_status status = OpenEncoder(model, tokenizer, &handles, open_concurrency, device);
  if (status != SEMB_OK) return Refuse("semb_encoder_open", status);
  status = semb_bake_to_path(handles.encoder, kept.data(), kept.size(), out, std::strlen(out));
  if (status != SEMB_OK) return Refuse("semb_bake_to_path", status);
  // TE-320 S-1: the sidecar is written on every successful bake, not only
  // when something was skipped, so it can never go stale from a prior bake
  // at the same stem. The stderr report stays conditional -- nothing to say
  // when nothing was skipped. Both happen only once the bank triple itself
  // exists, so a later failure never leaves an orphan sidecar beside a bank
  // that was never written.
  if (!skipped_ids.empty()) ReportSkipped(skipped_ids);
  if (!WriteSkippedSidecar(out, skipped_ids)) {
    return Refuse("semb bake skipped-sidecar", SEMB_ARTIFACT_UNREADABLE);
  }
  return 0;
}

int Query(const std::vector<std::string>& args) {
  if (args.size() == 1 && args[0] == "--help") {
    std::cout << "usage: semb query <name> <text> [--hex | --raw | --top k] [--device cpu|gpu] --model <sslm> --tokenizer <sslm>\n"
                 "       semb query --stdin <name> [--hex | --top k] [--device cpu|gpu] --model <sslm> --tokenizer <sslm>\n"
                 "One-shot query is a documented diagnostic; --stdin reuses one checked query handle.\n"
                 "Output modes: --hex (default), --raw (float bytes), or --top k (JSON hits).\n"
                 "--top k is a diagnostic: an exact scan of every bank row, not an index search.\n";
    return 0;
  }
  QueryArguments parsed;
  const char* bad_option = nullptr;
  if (!ParseQueryArguments(args, &parsed, &bad_option)) return Refuse(bad_option, SEMB_NULL_ARGUMENT);
  if (parsed.help) return Refuse("semb query --help", SEMB_NULL_ARGUMENT);
  if (!parsed.model || !parsed.tokenizer) return Refuse("semb query", SEMB_NULL_ARGUMENT);
  if ((parsed.stdin_mode && parsed.positional.size() != 1) ||
      (!parsed.stdin_mode && parsed.positional.size() != 2)) {
    return Refuse("semb query", SEMB_NULL_ARGUMENT);
  }
  size_t top = 0;
  if (parsed.top) {
    const auto top_parsed = std::from_chars(parsed.top, parsed.top + std::strlen(parsed.top), top);
    if (top_parsed.ec != std::errc{} || *top_parsed.ptr != '\0' || top == 0) {
      return Refuse("semb query --top", SEMB_NULL_ARGUMENT);
    }
  }
  // Reject incompatible presentation modes before opening the encoder: no
  // session, checked bank, or output stream is created for a usage error.
  if (parsed.hex && top != 0) return Refuse("semb query --hex --top", SEMB_NULL_ARGUMENT);
  if (parsed.raw && parsed.hex) return Refuse("semb query --raw --hex", SEMB_NULL_ARGUMENT);
  if (parsed.raw && top != 0) return Refuse("semb query --raw --top", SEMB_NULL_ARGUMENT);
  if (parsed.raw && parsed.stdin_mode) return Refuse("semb query --raw --stdin", SEMB_NULL_ARGUMENT);
  uint32_t device = SEMB_DEVICE_CPU;
  if (!ParseDeviceOption(parsed.device, &device)) return Refuse("semb query --device", SEMB_DEVICE_UNSUPPORTED);
#if defined(_WIN32)
  if (parsed.raw && _setmode(_fileno(stdout), _O_BINARY) == -1) {
    return Refuse("semb query --raw", SEMB_ARTIFACT_UNREADABLE);
  }
  // U2-7 item 3 (F19): stdin is read in binary mode, so a query line is the
  // bytes up to each LF and a CR before it is query text, as on every other
  // platform.
  if (parsed.stdin_mode && _setmode(_fileno(stdin), _O_BINARY) == -1) {
    return Refuse("semb query --stdin", SEMB_ARTIFACT_UNREADABLE);
  }
#endif
  Handles handles;
  const std::string stem = parsed.positional[0];
  // TE-350: query always encodes exactly one text per call, so a workspace
  // pool above 1 buys nothing here -- this stays the single-workspace path
  // the batch verb has always supported (T == 1 by construction).
  semb_status status = OpenCheckedQuery(stem, parsed.model, parsed.tokenizer, &handles, /*max_concurrency=*/1, device);
  if (status != SEMB_OK) return Refuse("semb query", status);
  auto emit = [&](const std::string& text) -> bool {
    std::vector<float> vector;
    status = QueryVector(handles.query, text, &vector);
    if (status != SEMB_OK) {
      (void)Refuse("semb_query_encode", status);
      return false;
    }
    if (parsed.raw) {
      std::cout.write(reinterpret_cast<const char*>(vector.data()),
                      static_cast<std::streamsize>(vector.size() * sizeof(float)));
      std::cout.flush();
    } else if (top) {
      const std::string output = TopHitsJson(handles.bank, vector, top);
      if (output.empty()) {
        (void)Refuse("semb query bank", SEMB_ARTIFACT_UNREADABLE);
        return false;
      }
      std::cout << output << '\n' << std::flush;
    } else {
      std::cout << "VECTOR_HEX=" << Hex(vector) << '\n' << std::flush;
    }
    return true;
  };
  if (parsed.stdin_mode) {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (!emit(line)) return 1;
    }
  } else if (!emit(parsed.positional[1])) {
    return 1;
  }
  if (parsed.stdin_mode) std::cerr << "query_handle_opens=1\n";
  return 0;
}

int Verify(const std::vector<std::string>& args) {
  const char* model = Option(args, "--model");
  const char* tokenizer = Option(args, "--tokenizer");
  const char* corpus = Option(args, "--corpus");
  std::vector<std::string> positional;
  for (size_t i = 0; i < args.size(); ++i) {
    if ((args[i] == "--model" || args[i] == "--tokenizer" || args[i] == "--corpus" ||
        args[i] == "--max-concurrency") &&
        i + 1 < args.size()) {
      ++i;
      continue;
    }
    positional.push_back(args[i]);
  }
  if (!model || !tokenizer || !corpus || positional.size() != 1) return Refuse("semb verify", SEMB_NULL_ARGUMENT);
  uint32_t requested_concurrency = 0;
  if (!ParseMaxConcurrencyOption(args, kBakeMaxConcurrency, &requested_concurrency)) {
    return Refuse("semb verify --max-concurrency", SEMB_NULL_ARGUMENT);
  }

  // TE-356 (C-2): the corpus is loaded and filtered BEFORE the encoder
  // opens (reordered from the prior "open, then load" sequence) so the
  // real document count is known in time to cap the workspace request --
  // the same shape Bake() already uses, and the reason verify's own
  // digest re-encode is safe to open at fewer than kBakeMaxConcurrency
  // slots on a small corpus rather than paying for 8 unconditionally.
  std::vector<std::string> ids, texts;
  std::vector<semb_document> docs;
  if (!LoadCorpus(corpus, &ids, &texts, &docs)) return Refuse("semb verify corpus", SEMB_ARTIFACT_UNREADABLE);
  // TE-317: re-encode over exactly the documents `semb bake` kept, so a bank
  // baked with a skipped empty document re-encodes to the identical row set
  // instead of the library refusing this re-encode with SEMB_INPUT_EMPTY.
  std::vector<semb_document> kept;
  std::vector<std::string> skipped_ids;
  FilterEmptyDocuments(ids, docs, &kept, &skipped_ids);
  if (!docs.empty() && kept.empty()) return Refuse("semb verify", SEMB_INPUT_EMPTY);
  if (SkippedIdCollides(kept, skipped_ids) && !AnyKeptDocumentHasALibraryDefect(kept)) {
    return Refuse("semb verify", SEMB_DOCUMENT_ID_DUPLICATE);
  }
  const uint32_t open_concurrency = OpenBakeConcurrency(requested_concurrency, kept.size());
  Handles handles;
  // TE-350: verify re-encodes the whole corpus (semb_bake_to_path below) to
  // compare digests, which is the bake path in every way that matters here
  // -- it gets the same machine-sized, document-count-capped pool `semb
  // bake` itself now opens with (TE-356).
  semb_status status = OpenCheckedQuery(positional[0], model, tokenizer, &handles, open_concurrency, SEMB_DEVICE_CPU);
  if (status != SEMB_OK) return Refuse("semb verify", status);
  // TE-317 round 3 (conductor direction, D-SLM7433, revising TE-320's own
  // S-3 remedy): verify's contract is "re-baking this corpus under the same
  // skip rule reproduces this bank", which the row/corpus digest comparison
  // below already decides -- it does NOT also read the bank's sidecar. The
  // sidecar is informational only, is not one of the files the library
  // writes or checks, and nothing ties a specific sidecar to a specific
  // bank; a bank moved as its three files (no sidecar), or a re-bake whose
  // sidecar write failed and left a stale one, must not change this
  // verdict, and a corpus with different empty documents than the one baked
  // still verifies OK precisely because it re-bakes to the identical kept
  // set (TE-324 N-1). Verify still reports its own skips on stderr, exactly
  // as bake does.
  if (!skipped_ids.empty()) ReportSkipped(skipped_ids);
  TemporaryBakeDirectory scratch;
  if (!scratch.Create()) return Refuse("semb verify scratch", SEMB_ARTIFACT_UNREADABLE);
  auto finish = [&](int result) {
    if (!scratch.Cleanup()) return Refuse("semb verify scratch cleanup", SEMB_ARTIFACT_UNREADABLE);
    return result;
  };
  const std::string rebaked_stem = (scratch.path() / "rebaked").string();
  status = semb_bake_to_path(handles.encoder, kept.data(), kept.size(), rebaked_stem.data(), rebaked_stem.size());
  if (status != SEMB_OK) return finish(Refuse("semb verify re-encode", status));

  std::string bank_manifest, rebaked_manifest;
  if (!ReadFile(positional[0] + ".semb.json", &bank_manifest) ||
      !ReadFile(rebaked_stem + ".semb.json", &rebaked_manifest)) {
    return finish(Refuse("semb verify manifest", SEMB_ARTIFACT_UNREADABLE));
  }
  std::string bank_corpus_digest, rebaked_corpus_digest, bank_row_digest, rebaked_row_digest;
  if (!ManifestField(bank_manifest, "corpus_digest", &bank_corpus_digest) ||
      !ManifestField(rebaked_manifest, "corpus_digest", &rebaked_corpus_digest) ||
      !ManifestField(bank_manifest, "row_digest", &bank_row_digest) ||
      !ManifestField(rebaked_manifest, "row_digest", &rebaked_row_digest)) {
    return finish(Refuse("semb verify manifest", SEMB_ARTIFACT_UNREADABLE));
  }
  if (bank_corpus_digest != rebaked_corpus_digest) {
    return finish(Refuse("semb verify", SEMB_CORPUS_DIGEST_MISMATCH));
  }
  if (bank_row_digest != rebaked_row_digest) return finish(Refuse("semb verify", SEMB_BANK_DIGEST_MISMATCH));
  return finish(0);
}

int Inspect(const std::vector<std::string>& args) {
  if (args.size() != 1) return Refuse("semb inspect", SEMB_NULL_ARGUMENT);
  std::string manifest;
  if (!ReadFile(args[0], &manifest)) return Refuse("semb inspect", SEMB_ARTIFACT_UNREADABLE);
  try {
    semb::json::Parse(manifest);
  } catch (...) {
    return Refuse("semb inspect", SEMB_ARTIFACT_UNREADABLE);
  }
  std::cout << manifest;
  if (manifest.empty() || manifest.back() != '\n') std::cout << '\n';
  return 0;
}

std::string HexOf(const std::array<uint8_t, 32>& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (uint8_t b : digest) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

// A descriptor value as a JSON string literal: the compiler strings carry spaces.
std::string Quoted(const std::string& value) {
  return semb::json::Write(semb::json::Value::MakeString(value));
}

// U2-1's build line (plan U2-1, the installed-CLI item; U2-3 items 1 and 14). Every value comes from the
// source encoder open reads: the compiled-in descriptor, with `dispatch` from the linked engine's own
// sslm_build_identity; item 14's CheckLinkedEngine; and, only once that check has passed, the dispatched
// tier and the cell verdict open computes (certification::DispatchedTier and ProcessVerdict). When the check
// refuses, open would refuse before the tier probe runs, so no tier is read and no cell is decided here
// either: both lines name the check's status, and the engine line says what the engine needs and the host
// lacks. The build line still prints, so a forced engine above the host reports its forced dispatch.
int PrintVersion() {
  namespace cert = semb::certification;
  namespace link = semb::engine_link;
  const cert::Descriptor& d = cert::ThisBuild();
  const link::Verdict check = link::CheckLinkedEngine();
  const std::string status = semb_status_name(check.status);
  std::cout << "semb " << superembedder::Version() << " (engine pin " << SEMB_ENGINE_PIN_TAG << " @ "
            << SEMB_ENGINE_PIN_COMMIT << ", engine_path_sha=" << SEMB_ENGINE_PATH_SHA << ")\n"
            << "build: os=" << SEMB_BUILD_OS << " arch=" << SEMB_BUILD_ARCH << " compiler=" << SEMB_BUILD_COMPILER_ID
            << "/" << SEMB_BUILD_COMPILER_VERSION << " frontend=" << SEMB_BUILD_COMPILER_FRONTEND
            << " configuration=" << SEMB_BUILD_CONFIGURATION << " buildSystem=cmake\n"
            << "descriptor: os=" << d.os << " isa=" << d.isa << " toolchain=" << Quoted(d.toolchain)
            << " stdlib=" << Quoted(d.stdlib) << " configuration=" << d.configuration
            << " buildSystem=" << d.build_system << " dispatch=" << d.dispatch
            << " compileFlags=" << d.compile_flags << " engineFlags=" << d.engine_flags
            << " engineToolchain=" << Quoted(SEMB_ENGINE_TOOLCHAIN) << " engineStdlib=" << Quoted(SEMB_ENGINE_STDLIB)
            << "\n";
  if (check.status == SEMB_OK) {
    const cert::Verdict& verdict = cert::ProcessVerdict();
    std::cout << "tier: " << cert::DispatchedTier() << "\n"
              << "cell: " << verdict.cell << " epoch=" << HexOf(verdict.epoch) << "\n";
  } else {
    std::cout << "tier: refused " << status << "\n"
              << "cell: refused " << status << "\n";
  }
  std::cout << "certification: line=" << cert::IdentityLine() << " identity_sha256=" << HexOf(cert::IdentitySha256())
            << " certified_builds_sha256=" << HexOf(cert::CompiledListSha256()) << "\n"
            << "engine: build_record=" << link::LinkedBuildRecord() << " expected=" << SEMB_ENGINE_BUILD_RECORD
            << " forced_tier=" << link::LinkedDispatch() << " required_isa="
            << (check.identity ? link::IsaNames(check.identity->required_isa) : std::string("unreadable"))
            << " check=" << status;
  if (check.missing_isa != 0) std::cout << " missing_isa=" << link::IsaNames(check.missing_isa);
  std::cout << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cout << "semb " << superembedder::Version() << " (engine pin " << SEMB_ENGINE_PIN_TAG
              << " @ " << SEMB_ENGINE_PIN_COMMIT << ", engine_path_sha=" << SEMB_ENGINE_PATH_SHA << ")\n";
    return 0;
  }
  const std::string command = argv[1];
  if (command == "--version" && argc == 2) return PrintVersion();
  std::vector<std::string> args;
  for (int i = 2; i < argc; ++i) args.emplace_back(argv[i]);
  if (command == "bake") return Bake(args);
  if (command == "query") return Query(args);
  if (command == "verify") return Verify(args);
  if (command == "inspect") return Inspect(args);
  return Refuse("semb", SEMB_NULL_ARGUMENT);
}
