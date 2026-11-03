// SuperEmbedder's frozen public C ABI.
#ifndef SUPEREMBEDDER_SEMB_H
#define SUPEREMBEDDER_SEMB_H

#include <stddef.h>
#include <stdint.h>

/* SEMB_API marks each verb this header declares, and only those. The
 * shared library exports exactly the verbs a line-anchored scan of this header
 * reads (tools/generate_export_list.py); its objects are compiled with hidden
 * visibility, so SEMB_API is also what gives each verb default visibility.
 * On Windows it is dllexport while the library itself is compiled
 * (SEMB_BUILDING_LIBRARY), dllimport for a consumer of the shared library
 * (SEMB_SHARED, set by the installed package's target), and empty for a
 * static link. */
#if defined(_WIN32)
#  if defined(SEMB_BUILDING_LIBRARY)
#    define SEMB_API __declspec(dllexport)
#  elif defined(SEMB_SHARED)
#    define SEMB_API __declspec(dllimport)
#  else
#    define SEMB_API
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define SEMB_API __attribute__((visibility("default")))
#else
#  define SEMB_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct semb_encoder_s* semb_encoder;

// A baked bank: library-owned, opaque, not copyable, released only by
// semb_bank_close; a double close returns SEMB_INVALID_HANDLE. A distinct
// opaque tag from semb_encoder/semb_query -- the three handle kinds are
// never interchangeable.
typedef struct semb_bank_opaque* semb_bank;

// A bank-bound query handle. It is library-owned and distinct from both the
// encoder and bank handles; query encodes can only occur after open compared
// the encoder's fingerprint against the bank's retained manifest
// encoder_fingerprint.
typedef struct semb_query_opaque* semb_query;

// Closed and append-only: existing names and ordinals never move.
#define SEMB_STATUS_ENUM_LIST(X) \
  X(SEMB_OK,                        0) \
  X(SEMB_NULL_ARGUMENT,             1) \
  X(SEMB_STRUCT_SIZE_MISMATCH,       2) \
  X(SEMB_INVALID_HANDLE,            3) \
  X(SEMB_HANDLE_IN_USE,             4) \
  X(SEMB_CONCURRENCY_EXCEEDED,       5) \
  X(SEMB_BUFFER_TOO_SMALL,          6) \
  X(SEMB_MISALIGNED_BUFFER,         7) \
  X(SEMB_INPUT_EMPTY,               8) \
  X(SEMB_INVALID_UTF8,              9) \
  X(SEMB_CONTEXT_CAP_EXCEEDED,     10) \
  X(SEMB_ARTIFACT_UNREADABLE,      11) \
  X(SEMB_ENGINE_REFUSED,           12) \
  X(SEMB_TOKENIZER_MODEL_MISMATCH, 13) \
  X(SEMB_TEMPLATE_TOKEN_ABSENT,    14) \
  X(SEMB_POOLED_SUBNORMAL,         15) \
  X(SEMB_POOLED_OVERFLOW,          16) \
  X(SEMB_ALLOCATION_FAILED,        17) \
  X(SEMB_CORPUS_EMPTY,             18) \
  X(SEMB_DOCUMENT_ID_INVALID,      19) \
  X(SEMB_DOCUMENT_ID_DUPLICATE,    20) \
  X(SEMB_CORPUS_TOO_LARGE,         21) \
  X(SEMB_FINGERPRINT_MISMATCH,             22) \
  X(SEMB_MANIFEST_SCHEMA_UNSUPPORTED,      23) \
  X(SEMB_BANK_SIZE_MISMATCH,               24) \
  X(SEMB_ARTIFACT_SHA_MISMATCH,            25) \
  X(SEMB_ADAPTER_ID_MISMATCH,              26) \
  X(SEMB_PS_VERSION_LABEL_MISMATCH,        27) \
  X(SEMB_BANK_IDS_COUNT_MISMATCH,          28) \
  X(SEMB_BANK_NONFINITE_ROW,               29) \
  X(SEMB_BANK_ZERO_NORM_ROW,               30) \
  X(SEMB_BANK_GEOMETRY_CEILING_EXCEEDED,   31) \
  /* A bank manifest whose metric or dtype this library does not     */ \
  /* read (semb_bank_open). An unknown schemaVersion is              */ \
  /* SEMB_MANIFEST_SCHEMA_UNSUPPORTED.                                */ \
  X(SEMB_BANK_METRIC_DTYPE_UNSUPPORTED,    32) \
  /* A manifest digest (row_digest recomputed over the payload, or    */ \
  /* corpus_digest disagreeing with its own provenance.witness copy)  */ \
  /* disagreeing with what it is checked against (semb_bank_open,     */ \
  /* bank.cpp). Distinct from SEMB_FINGERPRINT_MISMATCH (the encoder  */ \
  /* fingerprint's own five-component recomputation) and              */ \
  /* SEMB_ARTIFACT_SHA_MISMATCH (provenance.artifact_sha).            */ \
  X(SEMB_BANK_DIGEST_MISMATCH,             33) \
  X(SEMB_BANK_IN_USE,                      34) \
  /* `semb verify`: the bank manifest's corpus_digest disagrees with  */ \
  /* the digest of the corpus the verify re-encoded.                  */ \
  X(SEMB_CORPUS_DIGEST_MISMATCH,           35) \
  /* Encoder and bank open read the LINKED                            */ \
  /* engine's own build identity (sslm_build_identity) before any     */ \
  /* artifact byte is read and before any engine code runs, and       */ \
  /* refuse, in this order of checks: no readable identity; an engine */ \
  /* needing an instruction-set extension this host lacks; a compiler */ \
  /* or standard library other than this library's; no build record;  */ \
  /* a build record other than the one configure verified.            */ \
  X(SEMB_ENGINE_BUILD_UNRECORDED,          36) \
  X(SEMB_ENGINE_TOOLCHAIN_MISMATCH,        37) \
  X(SEMB_ENGINE_LINK_MISMATCH,             38) \
  X(SEMB_CPU_TIER_UNSUPPORTED,             39) \
  /* The device statuses. semb_encoder_open refuses                   */ \
  /* SEMB_DEVICE_UNSUPPORTED for a `device` value it does not know, or */ \
  /* SEMB_DEVICE_GPU in a library built without the GPU path;          */ \
  /* SEMB_DEVICE_UNAVAILABLE when the GPU cannot be used (no adapter,  */ \
  /* a lost device, shaders missing or stale); SEMB_DEVICE_DIVERGED    */ \
  /* when the GPU's open-time witness disagrees with the CPU's. A GPU  */ \
  /* handle whose device is lost returns SEMB_DEVICE_UNAVAILABLE from  */ \
  /* every later encode.                                               */ \
  X(SEMB_DEVICE_UNSUPPORTED,               40) \
  X(SEMB_DEVICE_UNAVAILABLE,               41) \
  X(SEMB_DEVICE_DIVERGED,                  42)

typedef enum semb_status {
#define SEMB_STATUS_ENUM_VALUE_(name, value) name = value,
  SEMB_STATUS_ENUM_LIST(SEMB_STATUS_ENUM_VALUE_)
#undef SEMB_STATUS_ENUM_VALUE_
  SEMB_STATUS_NEXT_FREE
} semb_status;

// Paths are sized byte spans and need not be NUL-terminated. Open derives
// the appended end token from the tokenizer artifact's <|endoftext|> id.
// `max_concurrency` is the number of encodes that may run on this handle at
// once; zero selects one, and a call beyond it returns
// SEMB_CONCURRENCY_EXCEEDED. Open reserves nothing in proportion to
// `max_concurrency`: it returns holding no workspace buffer, because its own
// witness encodes release theirs before it returns. Each running encode holds
// one workspace slot. The slot's buffer is sized to the document: layers x KV
// heads x head dim x 2 bytes (57,344 for the published model) for each
// position of the next power of two at or above the document's positions, at
// least 64 and at most the model's context. The buffer is kept for that
// slot's next encode, and freed after any document longer than 4,096
// positions. At most one encode of a document longer than 4,096 positions
// runs on a handle at a time; another such encode waits for it. Open returns
// SEMB_ALLOCATION_FAILED when the model, the tokenizer or the open-time
// witness cannot be allocated. An encode returns it when its slot cannot
// grow, and the handle stays usable. (`semb`'s own `bake`/`verify` CLI
// additionally caps its own request by this process's hardware concurrency
// and by the corpus's own document count before it ever reaches open -- a
// caller of this ABI directly chooses its own policy for that half.) A single
// batch call can also acquire every free slot for its own duration
// -- the slots are shared, greedy, and per-handle, not one reserved slot per
// caller.
//
// `device` selects where encodes run: SEMB_DEVICE_CPU (0, the
// zero-initialized default) or SEMB_DEVICE_GPU (1). Every other value refuses
// SEMB_DEVICE_UNSUPPORTED, as does SEMB_DEVICE_GPU in a library built without
// the GPU path. A GPU open first checks the GPU against the CPU on the
// open-time witness and refuses SEMB_DEVICE_DIVERGED on any difference, so a
// GPU handle returns the same bytes a CPU handle does. The GPU path is
// selected through `semb_config.device`; `semb_encoder_open_memory` always
// runs on the CPU.
typedef struct semb_config semb_config;
struct semb_config {
  uint32_t struct_size;
  const char* model_path;
  size_t model_path_len;
  const char* tokenizer_path;
  size_t tokenizer_path_len;
  uint32_t max_concurrency;
  uint32_t device;
};

// semb_config's `device` values.
enum semb_device { SEMB_DEVICE_CPU = 0, SEMB_DEVICE_GPU = 1 };

// semb_encoder_open_memory's argument: the model and tokenizer artifacts as
// sized byte spans in place of paths. A span is required iff its length is
// nonzero. The open copies both spans into buffers the encoder owns before it
// reads them, so the caller may release or rewrite its spans once the call
// returns. `max_concurrency` is semb_config's.
typedef struct semb_memory_config semb_memory_config;
struct semb_memory_config {
  uint32_t struct_size;
  const uint8_t* model_bytes;
  size_t model_len;
  const uint8_t* tokenizer_bytes;
  size_t tokenizer_len;
  uint32_t max_concurrency;
};

typedef struct semb_fingerprint semb_fingerprint;
struct semb_fingerprint {
  uint32_t struct_size;
  uint8_t digest[32];
};

// The per-document argument of semb_encode_batch. The id is
// opaque bytes to the library: validated, never interpreted, never
// tokenized, never digested by any fingerprint field.
typedef struct semb_document semb_document;
struct semb_document {
  uint32_t struct_size;    // sizeof(semb_document), validated exactly, per document
  const char* id;          // the row's id, sized UTF-8 span; required iff id_len > 0
  size_t id_len;            // > 0: every document carries an id
  const char* utf8;         // the document text, sized UTF-8 span; required iff utf8_len > 0
  size_t utf8_len;
};

// The encoder fingerprint: SHA-256 over the five clear components (model_sha,
// tokenizer_sha, template_sha256, witness_sha, certification_epoch), each
// length-prefixed. semb_encode and semb_fingerprint_of return it, every bank
// manifest records it as encoder_fingerprint, and semb_query_open refuses a
// bank whose encoder_fingerprint differs with SEMB_FINGERPRINT_MISMATCH.

// No function declared here lets a C++ exception escape: every call except
// semb_status_name returns a `semb_status`. When a call on a bank, a query, or an encoder opened for the
// CPU fails because memory cannot be allocated, it returns
// SEMB_ALLOCATION_FAILED, and every handle the call used stays usable. On an
// encoder opened for the GPU, memory exhausted, host or GPU, returns
// SEMB_ALLOCATION_FAILED, and the handle stays usable, with one rare
// exception: if the GPU's pending work cannot be confirmed stopped after the
// failure, the call returns SEMB_DEVICE_UNAVAILABLE, and every GPU encoder in
// the process may return it from then on. A lost device returns
// SEMB_DEVICE_UNAVAILABLE, and the handle stops using the GPU; any other GPU
// failure returns SEMB_ENGINE_REFUSED, and the handle stays usable. When the operating system or the C runtime cannot open or read a model,
// tokenizer or bank file, the call returns SEMB_ARTIFACT_UNREADABLE, whatever
// the reason and whichever device an encoder is opened for, a shortage of
// memory included; memory the library allocates to hold a file's bytes falls
// under the clauses above. Any other internal failure returns
// SEMB_ENGINE_REFUSED, or SEMB_ARTIFACT_UNREADABLE from semb_bank_open.

SEMB_API
semb_status semb_encoder_open(const semb_config* cfg, semb_encoder* out);
// Opens an encoder from artifacts already in memory. It runs the open
// semb_encoder_open runs, on copies of the two spans: the same checks, the
// same fingerprint, the same refusals. A span of zero bytes is refused as an
// empty file is by semb_encoder_open.
SEMB_API
semb_status semb_encoder_open_memory(const semb_memory_config* cfg, semb_encoder* out);
SEMB_API
semb_status semb_encoder_close(semb_encoder enc);
SEMB_API
semb_status semb_encoder_dims(semb_encoder enc, uint32_t* out_dims);
// Encodes one document into `out_vector`. The checks run in this order:
// `enc`, `out_required_bytes` and `out_fingerprint` non-NULL; a live handle;
// `out_fingerprint->struct_size == sizeof(semb_fingerprint)`; `out_vector`,
// when non-NULL, 16-byte aligned (SEMB_MISALIGNED_BUFFER); then, when
// `out_capacity_bytes` is below dims * sizeof(float), `*out_required_bytes`
// is set and SEMB_BUFFER_TOO_SMALL returned, so a NULL buffer with zero
// capacity asks for the size; then `out_vector` non-NULL, non-empty text
// (SEMB_INPUT_EMPTY), `utf8` non-NULL, and valid UTF-8 (SEMB_INVALID_UTF8).
SEMB_API
semb_status semb_encode(semb_encoder enc, const char* utf8, size_t utf8_len,
                        float* out_vector, size_t out_capacity_bytes,
                        size_t* out_required_bytes,
                        semb_fingerprint* out_fingerprint);
// The query form of semb_encode: applies the query instruction prefix exactly
// as semb_query_encode does, through the same composition, and returns the
// fingerprint the vector was emitted under, as semb_encode does, with
// semb_encode's validation order and output-buffer convention. It checks
// nothing against a bank; what the caller does with the fingerprint is the
// caller's.
SEMB_API
semb_status semb_encode_query(semb_encoder enc, const char* utf8, size_t utf8_len,
                              float* out_vector, size_t out_capacity_bytes,
                              size_t* out_required_bytes,
                              semb_fingerprint* out_fingerprint);
// Encodes a corpus of documents into rows. Validation is
// shape-before-content-before-work; nothing is
// written to `out_rows` on any non-SEMB_OK return; `*out_required_bytes` is
// written on exactly SEMB_OK and SEMB_BUFFER_TOO_SMALL.
SEMB_API
semb_status semb_encode_batch(semb_encoder enc, const semb_document* docs, size_t n,
                              float* out_rows, size_t out_capacity_bytes,
                              size_t* out_required_bytes);
// Opens and validates a `.wvbank`/`.semb.json` triple named by the shared
// stem `path` (sized UTF-8 span, `path_len` bytes, never NUL-terminated);
// owns the bank's own expected fingerprint, read from the manifest. `*out`
// is set to a live handle only on SEMB_OK; every refusal leaves
// `*out == NULL` (when `out` is non-NULL).
SEMB_API
semb_status semb_bank_open(const char* path, size_t path_len, semb_bank* out);
// Releases the handle. A bank with one or more live query handles returns
// SEMB_BANK_IN_USE and remains live; close those query handles and retry.
SEMB_API
semb_status semb_bank_close(semb_bank bank);
SEMB_API
semb_status semb_query_open(semb_encoder enc, semb_bank bank, semb_query* out);
SEMB_API
semb_status semb_query_encode(semb_query q, const char* utf8, size_t utf8_len,
                              float* out_vector, size_t out_capacity_bytes,
                              size_t* out_required_bytes);
SEMB_API
semb_status semb_query_close(semb_query q);
SEMB_API
semb_status semb_fingerprint_of(semb_encoder enc, semb_fingerprint* out);
SEMB_API
const char* semb_status_name(semb_status s);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // SUPEREMBEDDER_SEMB_H
