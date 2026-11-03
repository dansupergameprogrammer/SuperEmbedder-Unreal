/* src/superembedder/engine_identity/superslm_build_identity.h -- the engine's build-identity verb
 * (plan U2-3 item 14; FORMAT.md sec 5.1).
 *
 * The engine reports how its own kernel translation unit was compiled: the compiler and
 * standard-library macros, the forced dispatch tier, the instruction-set extensions the
 * compile allows outside the dispatcher, and a build record its build system supplied.
 * Every field is a compile-time constant of the object that holds it, so a caller that
 * links a different engine reads that engine's values.
 *
 * No engine tag defines the verb. The pin tool compiles build_identity.cpp, beside this header, with the
 * engine's own recorded compile command for src/matmul.cpp and appends the object to the installed engine
 * library (tools/pin-superslm/pin_superslm.py), so every value below is the kernel compile's. The product
 * reads the layout from this header (engine_link_check.cpp).
 *
 * Append-only: a later version adds fields after `build_record` and grows struct_size. */
#ifndef SUPERSLM_BUILD_IDENTITY_H
#define SUPERSLM_BUILD_IDENTITY_H

#include <stdint.h>

#ifndef SUPERSLM_API
#define SUPERSLM_API
#endif

/* Instruction-set extensions, one bit each. */
#define SSLM_ISA_SSE3        (1ull << 0)
#define SSLM_ISA_SSSE3       (1ull << 1)
#define SSLM_ISA_SSE4_1      (1ull << 2)
#define SSLM_ISA_SSE4_2      (1ull << 3)
#define SSLM_ISA_POPCNT      (1ull << 4)
#define SSLM_ISA_AVX         (1ull << 5)
#define SSLM_ISA_AVX2        (1ull << 6)
#define SSLM_ISA_FMA         (1ull << 7)
#define SSLM_ISA_F16C        (1ull << 8)
#define SSLM_ISA_BMI1        (1ull << 9)
#define SSLM_ISA_BMI2        (1ull << 10)
#define SSLM_ISA_LZCNT       (1ull << 11)
#define SSLM_ISA_MOVBE       (1ull << 12)
#define SSLM_ISA_AVX512F     (1ull << 13)
#define SSLM_ISA_AVX512BW    (1ull << 14)
#define SSLM_ISA_AVX512DQ    (1ull << 15)
#define SSLM_ISA_AVX512VL    (1ull << 16)
#define SSLM_ISA_AVX512CD    (1ull << 17)
#define SSLM_ISA_AVX512VNNI  (1ull << 18)
#define SSLM_ISA_AVX512VBMI  (1ull << 19)
#define SSLM_ISA_AVX512IFMA  (1ull << 20)
#define SSLM_ISA_AVX512BF16  (1ull << 21)
#define SSLM_ISA_AVX512FP16  (1ull << 22)
#define SSLM_ISA_AVXVNNI     (1ull << 23)
#define SSLM_ISA_AVX10       (1ull << 24)

/* forced_tier values: the SUPERSLM_FORCE_*_MATMUL macro in effect, or none. */
#define SSLM_FORCED_TIER_NONE    0u
#define SSLM_FORCED_TIER_SCALAR  1u
#define SSLM_FORCED_TIER_SSE2    2u
#define SSLM_FORCED_TIER_AVX2    3u
#define SSLM_FORCED_TIER_AVX512  4u

typedef struct SslmBuildIdentity {
  uint32_t struct_size;      /* sizeof(SslmBuildIdentity) in the engine's build */
  uint32_t schema_version;   /* 1 */
  uint32_t msc_full_ver;     /* _MSC_FULL_VER, or 0 */
  uint32_t clang_version;    /* __clang_major__*10000 + minor*100 + patchlevel, or 0 */
  uint32_t gnuc_version;     /* the same for GCC proper (not clang), or 0 */
  uint32_t msvc_stl_update;  /* _MSVC_STL_UPDATE, or 0 */
  uint32_t glibcxx;          /* __GLIBCXX__, or 0 */
  uint32_t libcpp_version;   /* _LIBCPP_VERSION, or 0 */
  uint32_t forced_tier;      /* SSLM_FORCED_TIER_* */
  uint32_t reserved0;        /* 0 */
  uint64_t compiled_isa;     /* SSLM_ISA_* bits the compile's predefined macros allow anywhere */
  uint64_t required_isa;     /* compiled_isa plus the forced tier's kernel extensions */
  const char* build_record;  /* NUL-terminated record the build system supplied; "" if none */
} SslmBuildIdentity;

#ifdef __cplusplus
extern "C" {
#endif
/* Returns the address of a constant-initialized object. Executes no instruction beyond
 * forming that address, so it is safe to call before the host's capability is checked. */
SUPERSLM_API const SslmBuildIdentity* sslm_build_identity(void);
#ifdef __cplusplus
}
#endif

#endif /* SUPERSLM_BUILD_IDENTITY_H */
