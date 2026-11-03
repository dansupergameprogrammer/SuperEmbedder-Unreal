# Host suite for the SuperEmbedderUnreal module's engine-free cores (Source/SuperEmbedderUnreal/Private/Core).
# Builds with g++ (and clang) on Linux; no network, no Unreal. The real-encode tests link SuperEmbedderCore's
# vendored library exactly as the module compiles it: every Private/Vendored wrapper plus the plugin's
# token-count shim, under the GCC rename header. See run.sh.
cmake_minimum_required(VERSION 3.16)
project(superembedder_cores_host CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CORES_SANITIZE "Build with AddressSanitizer (everything) and UndefinedBehaviorSanitizer (cores and tests)" OFF)
option(CORES_TSAN "Build with ThreadSanitizer (everything)" OFF)

set(PLUGIN_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(CORE_DIR ${PLUGIN_ROOT}/Source/SuperEmbedderUnreal/Private/Core)
set(SECORE_DIR ${PLUGIN_ROOT}/Source/SuperEmbedderCore)
set(TP_DIR ${PLUGIN_ROOT}/Source/ThirdParty)

set(ASAN_FLAGS "")
set(UBSAN_FLAGS "")
if(CORES_SANITIZE)
  set(ASAN_FLAGS -fsanitize=address -fno-omit-frame-pointer)
  set(UBSAN_FLAGS -fsanitize=undefined -fno-sanitize-recover=all)
endif()
if(CORES_TSAN)
  set(ASAN_FLAGS -fsanitize=thread)
endif()

# SuperEmbedderCore as UBT compiles it: the vendored wrappers and the shim, the rename header force-included,
# precise FP, exceptions on (both libraries narrow exceptions to statuses at their C boundary).
file(GLOB VENDORED_WRAPPERS ${SECORE_DIR}/Private/Vendored/*.cpp)
add_library(superembedder_core STATIC ${VENDORED_WRAPPERS} ${SECORE_DIR}/Private/SuperEmbedderCoreQueryTokens.cpp)
target_include_directories(superembedder_core
  PUBLIC ${TP_DIR}/SuperEmbedder/include ${SECORE_DIR}/Public
  PRIVATE ${TP_DIR}/SuperSLM/include ${SECORE_DIR}/Private/Generated)
target_compile_definitions(superembedder_core PRIVATE SEMB_BUILD_CONFIGURATION="ubt-host" SEMB_BUILD_SYSTEM="ubt")
target_compile_options(superembedder_core PRIVATE -O2 -ffp-contract=off -w
  -include ${SECORE_DIR}/Private/Generated/SuperEmbedderCoreRename.gcc.h ${ASAN_FLAGS})

# The cores, built as Unreal builds the module: no exceptions, no RTTI, and the warnings MSVC raises under
# Unreal's settings (shadowing, narrowing, sign conversion, undefined macros in #if) made errors here.
set(CORE_SOURCES
  ${CORE_DIR}/SuperEmbedderCoreTypes.cpp
  ${CORE_DIR}/SuperEmbedderSha256.cpp
  ${CORE_DIR}/SuperEmbedderText.cpp
  ${CORE_DIR}/SuperEmbedderEncoder.cpp
  ${CORE_DIR}/SuperEmbedderEncodeCache.cpp
  ${CORE_DIR}/SuperEmbedderEncodeService.cpp
  ${CORE_DIR}/SuperEmbedderVectorSetCore.cpp
  ${CORE_DIR}/SuperEmbedderSimilarity.cpp
  ${CORE_DIR}/SuperEmbedderRowsInput.cpp)
set(CORE_WARN -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion -Wundef -pedantic-errors)
add_library(superembedder_cores STATIC ${CORE_SOURCES})
target_include_directories(superembedder_cores PUBLIC ${CORE_DIR})
target_link_libraries(superembedder_cores PUBLIC superembedder_core)
target_compile_options(superembedder_cores PRIVATE ${CORE_WARN} -fno-exceptions -fno-rtti ${ASAN_FLAGS} ${UBSAN_FLAGS})

# The cores as one translation unit (Unreal's unity build): built, never linked.
add_library(superembedder_cores_unity OBJECT unity_cores.cpp)
target_include_directories(superembedder_cores_unity PRIVATE ${CORE_DIR} ${TP_DIR}/SuperEmbedder/include ${SECORE_DIR}/Public)
target_compile_options(superembedder_cores_unity PRIVATE ${CORE_WARN} -fno-exceptions -fno-rtti)

add_executable(superembedder_cores_host_tests
  harness.cpp
  support.cpp
  test_text.cpp
  test_rows.cpp
  test_cache.cpp
  test_set.cpp
  test_similarity.cpp
  test_service_lifecycle.cpp
  test_service_lanes.cpp
  test_service_real.cpp)
target_link_libraries(superembedder_cores_host_tests PRIVATE superembedder_cores pthread)
target_compile_definitions(superembedder_cores_host_tests PRIVATE
  SEMB_HOST_REFERENCE_FILE="${PLUGIN_ROOT}/ci/host/coexist/u1_pair_cmake_reference.txt")
add_dependencies(superembedder_cores_host_tests superembedder_cores_unity)
target_compile_options(superembedder_cores_host_tests PRIVATE -Wall -Wextra -Werror -Wshadow -fno-exceptions -fno-rtti
  ${ASAN_FLAGS} ${UBSAN_FLAGS})
target_link_options(superembedder_cores_host_tests PRIVATE ${ASAN_FLAGS} ${UBSAN_FLAGS})

enable_testing()
add_test(NAME superembedder_cores_host_tests COMMAND superembedder_cores_host_tests)
