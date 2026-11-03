// src/superembedder/engine_identity/build_identity.cpp -- the engine's build-identity verb (plan U2-3
// item 14). This text is compiled into the ENGINE, never into the product library.
//
// The pin tool compiles it with the engine kernel translation unit's own compile command (src/matmul.cpp's),
// in the environment the engine was just built in, and appends the object to the installed library when the
// engine does not define the verb itself. Every predefined macro read here is therefore one the kernels were
// compiled under. The function returns the address of a constant object and calls nothing.
#if defined(__has_include)
#if __has_include(<version>)
#include <version>
#else
#include <cstddef>
#endif
#else
#include <cstddef>
#endif

#include "superslm_build_identity.h"

// The build record: supplied by the build system that compiled this object (the pin tool writes
// superslm_build_record.inc into its own work directory and puts that directory on the include path).
// Absent -> the empty record, which a consumer refuses as unrecorded.
#if defined(__has_include)
#if __has_include("superslm_build_record.inc")
#include "superslm_build_record.inc"
#endif
#endif
#ifndef SUPERSLM_BUILD_RECORD
#define SUPERSLM_BUILD_RECORD ""
#endif

#if defined(_MSC_FULL_VER)
#define SSLM_BI_MSC ((uint32_t)_MSC_FULL_VER)
#else
#define SSLM_BI_MSC 0u
#endif
#if defined(__clang__)
#define SSLM_BI_CLANG ((uint32_t)(__clang_major__ * 10000 + __clang_minor__ * 100 + __clang_patchlevel__))
#else
#define SSLM_BI_CLANG 0u
#endif
#if defined(__GNUC__) && !defined(__clang__)
#define SSLM_BI_GNUC ((uint32_t)(__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__))
#else
#define SSLM_BI_GNUC 0u
#endif
#if defined(_MSVC_STL_UPDATE)
#define SSLM_BI_STL ((uint32_t)_MSVC_STL_UPDATE)
#else
#define SSLM_BI_STL 0u
#endif
#if defined(__GLIBCXX__)
#define SSLM_BI_GLIBCXX ((uint32_t)__GLIBCXX__)
#else
#define SSLM_BI_GLIBCXX 0u
#endif
#if defined(_LIBCPP_VERSION)
#define SSLM_BI_LIBCPP ((uint32_t)_LIBCPP_VERSION)
#else
#define SSLM_BI_LIBCPP 0u
#endif

// The forced tier, by the same four macros src/matmul.cpp dispatches on.
#if defined(SUPERSLM_FORCE_SCALAR_MATMUL)
#define SSLM_BI_FORCED SSLM_FORCED_TIER_SCALAR
#define SSLM_BI_FORCED_ISA 0ull
#elif defined(SUPERSLM_FORCE_SSE2_MATMUL)
#define SSLM_BI_FORCED SSLM_FORCED_TIER_SSE2
#define SSLM_BI_FORCED_ISA 0ull
#elif defined(SUPERSLM_FORCE_AVX2_MATMUL)
#define SSLM_BI_FORCED SSLM_FORCED_TIER_AVX2
#define SSLM_BI_FORCED_ISA (SSLM_ISA_AVX | SSLM_ISA_AVX2)
#elif defined(SUPERSLM_FORCE_AVX512_MATMUL)
#define SSLM_BI_FORCED SSLM_FORCED_TIER_AVX512
#define SSLM_BI_FORCED_ISA (SSLM_ISA_AVX512F | SSLM_ISA_AVX512BW)
#else
#define SSLM_BI_FORCED SSLM_FORCED_TIER_NONE
#define SSLM_BI_FORCED_ISA 0ull
#endif

// The extensions the compile allows outside the dispatcher: one bit per predefined macro.
#if defined(__SSE3__)
#define SSLM_BI_I0 SSLM_ISA_SSE3
#else
#define SSLM_BI_I0 0ull
#endif
#if defined(__SSSE3__)
#define SSLM_BI_I1 SSLM_ISA_SSSE3
#else
#define SSLM_BI_I1 0ull
#endif
#if defined(__SSE4_1__)
#define SSLM_BI_I2 SSLM_ISA_SSE4_1
#else
#define SSLM_BI_I2 0ull
#endif
#if defined(__SSE4_2__)
#define SSLM_BI_I3 SSLM_ISA_SSE4_2
#else
#define SSLM_BI_I3 0ull
#endif
#if defined(__POPCNT__)
#define SSLM_BI_I4 SSLM_ISA_POPCNT
#else
#define SSLM_BI_I4 0ull
#endif
#if defined(__AVX__)
#define SSLM_BI_I5 SSLM_ISA_AVX
#else
#define SSLM_BI_I5 0ull
#endif
#if defined(__AVX2__)
#define SSLM_BI_I6 SSLM_ISA_AVX2
#else
#define SSLM_BI_I6 0ull
#endif
#if defined(__FMA__)
#define SSLM_BI_I7 SSLM_ISA_FMA
#else
#define SSLM_BI_I7 0ull
#endif
#if defined(__F16C__)
#define SSLM_BI_I8 SSLM_ISA_F16C
#else
#define SSLM_BI_I8 0ull
#endif
#if defined(__BMI__)
#define SSLM_BI_I9 SSLM_ISA_BMI1
#else
#define SSLM_BI_I9 0ull
#endif
#if defined(__BMI2__)
#define SSLM_BI_I10 SSLM_ISA_BMI2
#else
#define SSLM_BI_I10 0ull
#endif
#if defined(__LZCNT__)
#define SSLM_BI_I11 SSLM_ISA_LZCNT
#else
#define SSLM_BI_I11 0ull
#endif
#if defined(__MOVBE__)
#define SSLM_BI_I12 SSLM_ISA_MOVBE
#else
#define SSLM_BI_I12 0ull
#endif
#if defined(__AVX512F__)
#define SSLM_BI_I13 SSLM_ISA_AVX512F
#else
#define SSLM_BI_I13 0ull
#endif
#if defined(__AVX512BW__)
#define SSLM_BI_I14 SSLM_ISA_AVX512BW
#else
#define SSLM_BI_I14 0ull
#endif
#if defined(__AVX512DQ__)
#define SSLM_BI_I15 SSLM_ISA_AVX512DQ
#else
#define SSLM_BI_I15 0ull
#endif
#if defined(__AVX512VL__)
#define SSLM_BI_I16 SSLM_ISA_AVX512VL
#else
#define SSLM_BI_I16 0ull
#endif
#if defined(__AVX512CD__)
#define SSLM_BI_I17 SSLM_ISA_AVX512CD
#else
#define SSLM_BI_I17 0ull
#endif
#if defined(__AVX512VNNI__)
#define SSLM_BI_I18 SSLM_ISA_AVX512VNNI
#else
#define SSLM_BI_I18 0ull
#endif
#if defined(__AVX512VBMI__)
#define SSLM_BI_I19 SSLM_ISA_AVX512VBMI
#else
#define SSLM_BI_I19 0ull
#endif
#if defined(__AVX512IFMA__)
#define SSLM_BI_I20 SSLM_ISA_AVX512IFMA
#else
#define SSLM_BI_I20 0ull
#endif
#if defined(__AVX512BF16__)
#define SSLM_BI_I21 SSLM_ISA_AVX512BF16
#else
#define SSLM_BI_I21 0ull
#endif
#if defined(__AVX512FP16__)
#define SSLM_BI_I22 SSLM_ISA_AVX512FP16
#else
#define SSLM_BI_I22 0ull
#endif
#if defined(__AVXVNNI__)
#define SSLM_BI_I23 SSLM_ISA_AVXVNNI
#else
#define SSLM_BI_I23 0ull
#endif
#if defined(__AVX10_VER__)
#define SSLM_BI_I24 SSLM_ISA_AVX10
#else
#define SSLM_BI_I24 0ull
#endif

// MSVC (not clang-cl) defines no macro for the extensions its /arch levels also let it emit:
// an /arch:AVX2 engine carries BMI1/BMI2 instructions with neither __BMI__ nor __BMI2__
// defined (executed on MSVC 19.33). So under MSVC an /arch level implies its whole x86-64
// microarchitecture level: /arch:AVX2 -> x86-64-v3, /arch:AVX512 -> x86-64-v4. Requiring
// more than the compile uses refuses by name on a host that could have run it; requiring
// less crashes, so the rule errs toward more.
#define SSLM_BI_X86_64_V3                                                                 \
  (SSLM_ISA_AVX | SSLM_ISA_AVX2 | SSLM_ISA_BMI1 | SSLM_ISA_BMI2 | SSLM_ISA_F16C |          \
   SSLM_ISA_FMA | SSLM_ISA_LZCNT | SSLM_ISA_MOVBE)
#if defined(_MSC_VER) && !defined(__clang__) && defined(__AVX512F__)
#define SSLM_BI_MSVC_IMPLIED                                                              \
  (SSLM_BI_X86_64_V3 | SSLM_ISA_AVX512F | SSLM_ISA_AVX512BW | SSLM_ISA_AVX512CD |          \
   SSLM_ISA_AVX512DQ | SSLM_ISA_AVX512VL)
#elif defined(_MSC_VER) && !defined(__clang__) && defined(__AVX2__)
#define SSLM_BI_MSVC_IMPLIED SSLM_BI_X86_64_V3
#elif defined(_MSC_VER) && !defined(__clang__) && defined(__AVX__)
#define SSLM_BI_MSVC_IMPLIED SSLM_ISA_AVX
#else
#define SSLM_BI_MSVC_IMPLIED 0ull
#endif

#define SSLM_BI_COMPILED_ISA                                                              \
  (SSLM_BI_I0 | SSLM_BI_I1 | SSLM_BI_I2 | SSLM_BI_I3 | SSLM_BI_I4 | SSLM_BI_I5 |          \
   SSLM_BI_I6 | SSLM_BI_I7 | SSLM_BI_I8 | SSLM_BI_I9 | SSLM_BI_I10 | SSLM_BI_I11 |        \
   SSLM_BI_I12 | SSLM_BI_I13 | SSLM_BI_I14 | SSLM_BI_I15 | SSLM_BI_I16 | SSLM_BI_I17 |    \
   SSLM_BI_I18 | SSLM_BI_I19 | SSLM_BI_I20 | SSLM_BI_I21 | SSLM_BI_I22 | SSLM_BI_I23 |    \
   SSLM_BI_I24 | SSLM_BI_MSVC_IMPLIED)

namespace {
constexpr SslmBuildIdentity kSslmBuildIdentity = {
    sizeof(SslmBuildIdentity),
    1u,
    SSLM_BI_MSC,
    SSLM_BI_CLANG,
    SSLM_BI_GNUC,
    SSLM_BI_STL,
    SSLM_BI_GLIBCXX,
    SSLM_BI_LIBCPP,
    SSLM_BI_FORCED,
    0u,
    SSLM_BI_COMPILED_ISA,
    SSLM_BI_COMPILED_ISA | SSLM_BI_FORCED_ISA,
    SUPERSLM_BUILD_RECORD,
};
}  // namespace

extern "C" SUPERSLM_API const SslmBuildIdentity* sslm_build_identity(void) {
  return &kSslmBuildIdentity;
}
