// src/superembedder/semb_build_descriptor.h -- the build descriptor's compiler-derived fields (plan U2-3
// items 1 and 13; FORMAT.md sec 3.2).
//
// The fields a cell matches on that describe the compiler -- `os`, `isa`, `toolchain` and `stdlib` -- are
// string literals formed here from the compiler's own predefined macros and nothing else, so no build system
// can supply a different compiler identity. The rule is written once, as string-literal macros: the product
// compiles this header, and the engine pin tool preprocesses a probe that includes it with the engine's own
// recorded compile command for its kernel translation unit, so both sides evaluate one text and the strings
// compare by equality.
//
// This header is also the pin tool's probe input, so it includes only <version> and defines only macros.
#ifndef SEMB_BUILD_DESCRIPTOR_H_
#define SEMB_BUILD_DESCRIPTOR_H_

#include <version>  // defines the standard library's own version macros

#define SEMB_DESCRIPTOR_STR_(x) #x
#define SEMB_DESCRIPTOR_STR(x) SEMB_DESCRIPTOR_STR_(x)

// os
#if defined(_WIN32)
#define SEMB_DESCRIPTOR_OS "windows"
#elif defined(__APPLE__)
#define SEMB_DESCRIPTOR_OS "macos"
#elif defined(__linux__)
#define SEMB_DESCRIPTOR_OS "linux"
#else
#define SEMB_DESCRIPTOR_OS "unknown"
#endif

// isa
#if defined(_M_X64) || defined(__x86_64__)
#define SEMB_DESCRIPTOR_ISA "x86-64"
#elif defined(_M_ARM64) || defined(__aarch64__)
#define SEMB_DESCRIPTOR_ISA "arm64"
#else
#define SEMB_DESCRIPTOR_ISA "unknown"
#endif

// toolchain: clang with _MSC_VER is the MSVC frontend, and carries the _MSC_FULL_VER it reports.
#if defined(__clang__) && defined(_MSC_VER)
#define SEMB_DESCRIPTOR_TOOLCHAIN                                                              \
  "Clang " SEMB_DESCRIPTOR_STR(__clang_major__) "." SEMB_DESCRIPTOR_STR(__clang_minor__) "." \
      SEMB_DESCRIPTOR_STR(__clang_patchlevel__) " (MSVC frontend " SEMB_DESCRIPTOR_STR(_MSC_FULL_VER) ")"
#elif defined(__clang__) && defined(__apple_build_version__)
#define SEMB_DESCRIPTOR_TOOLCHAIN                                                                   \
  "AppleClang " SEMB_DESCRIPTOR_STR(__clang_major__) "." SEMB_DESCRIPTOR_STR(__clang_minor__) "." \
      SEMB_DESCRIPTOR_STR(__clang_patchlevel__) " (" SEMB_DESCRIPTOR_STR(__apple_build_version__) ")"
#elif defined(__clang__)
#define SEMB_DESCRIPTOR_TOOLCHAIN                                                              \
  "Clang " SEMB_DESCRIPTOR_STR(__clang_major__) "." SEMB_DESCRIPTOR_STR(__clang_minor__) "." \
      SEMB_DESCRIPTOR_STR(__clang_patchlevel__)
#elif defined(_MSC_VER)
#define SEMB_DESCRIPTOR_TOOLCHAIN "MSVC " SEMB_DESCRIPTOR_STR(_MSC_FULL_VER) "." SEMB_DESCRIPTOR_STR(_MSC_BUILD)
#elif defined(__GNUC__)
#define SEMB_DESCRIPTOR_TOOLCHAIN                                                         \
  "GNU " SEMB_DESCRIPTOR_STR(__GNUC__) "." SEMB_DESCRIPTOR_STR(__GNUC_MINOR__) "." \
      SEMB_DESCRIPTOR_STR(__GNUC_PATCHLEVEL__)
#else
#define SEMB_DESCRIPTOR_TOOLCHAIN "unknown"
#endif

// stdlib
#if defined(_MSVC_STL_UPDATE)
#define SEMB_DESCRIPTOR_STDLIB "MSVC STL " SEMB_DESCRIPTOR_STR(_MSVC_STL_UPDATE)
#elif defined(_LIBCPP_VERSION)
#define SEMB_DESCRIPTOR_STDLIB "libc++ " SEMB_DESCRIPTOR_STR(_LIBCPP_VERSION)
#elif defined(__GLIBCXX__)
#define SEMB_DESCRIPTOR_STDLIB "libstdc++ " SEMB_DESCRIPTOR_STR(__GLIBCXX__)
#else
#define SEMB_DESCRIPTOR_STDLIB "unknown"
#endif

// dispatch, as the engine's kernel compile sees it: the forced tier its SUPERSLM_FORCE_*_MATMUL macro
// selects, or auto. Read by the pin tool's probe of the engine's kernel compile (item 5); the product's own
// compile never defines these macros.
#if defined(SUPERSLM_FORCE_SCALAR_MATMUL)
#define SEMB_DESCRIPTOR_ENGINE_DISPATCH "scalar"
#elif defined(SUPERSLM_FORCE_SSE2_MATMUL)
#define SEMB_DESCRIPTOR_ENGINE_DISPATCH "sse2"
#elif defined(SUPERSLM_FORCE_AVX2_MATMUL)
#define SEMB_DESCRIPTOR_ENGINE_DISPATCH "avx2"
#elif defined(SUPERSLM_FORCE_AVX512_MATMUL)
#define SEMB_DESCRIPTOR_ENGINE_DISPATCH "avx512"
#else
#define SEMB_DESCRIPTOR_ENGINE_DISPATCH "auto"
#endif

#endif  // SEMB_BUILD_DESCRIPTOR_H_
