# SuperEmbedder — top-level CMakeLists.txt.
#
# TE-61 (Claude/Plans/SuperEmbedder_Plan.md's U0, slice A) built the
# scaffold, the engine pin, the two configure guards, the build-flag
# assertion, and CI. TE-63 (U0 slice B) adds the arithmetic: the interim
# engine-adapter TU (sec 4.3), PS-1's pooling epilogue and W-A's primitive
# driver (sec 3.2, sec 4.4), the seam serialisation (sec 4.4b), and the
# witness executable (sec 4.4's structural law).
cmake_minimum_required(VERSION 3.21)
project(superembedder VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(GNUInstallDirs)

# U2-3 item 13: compileFlags is computed from the build's own
# compile_commands.json, so the export is on before the first target is
# declared -- every product target's lines are recorded, semb and
# semb_witness included.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# U3 (te266 sec 4.2): the GPU encode path. Default OFF; Windows only, against
# an engine installed by the pin tool with --with-gpu. Refused by name off
# Windows before the engine is located, and after it when the engine install
# carries no superslm::superslm_gpu (SEMB_ENGINE_GPU_MISSING, below).
option(SEMB_BUILD_GPU "Build the GPU encode path (Windows only; needs a --with-gpu engine install)" OFF)
if(SEMB_BUILD_GPU AND NOT WIN32)
	message(FATAL_ERROR "SEMB_BUILD_GPU=ON requires Windows: the GPU path is D3D12 and the engine builds "
		"superslm_gpu on Windows only. Configure without -DSEMB_BUILD_GPU=ON on this platform.")
endif()

include(EngineGuards)   # find_package(superslm), the two version guards, provenance (C33)

if(SEMB_BUILD_GPU AND NOT TARGET superslm::superslm_gpu)
	message(FATAL_ERROR "SEMB_ENGINE_GPU_MISSING: SEMB_BUILD_GPU=ON, but the SuperSLM install at "
		"${superslm_DIR} carries no superslm::superslm_gpu target. Re-run "
		"tools/pin-superslm/pin_superslm.py with --with-gpu, or configure without SEMB_BUILD_GPU.")
endif()
include(BuildFlags)     # the precise-FP flag (directory-wide) + semb_assert_precise_fp() (C14)
include(SourceProvenance)  # semb_source_sha (C34)
include(CertifiedBuilds)  # the certified-builds list and the certification identity, compiled in (U2-3)

# C38's pin-bump-only construction needs to place one compile definition on
# exactly one downstream target. The pinned SuperSLM tag is never modified:
# tools/engine_side_wa_perturbation.py injects the matching branch into a
# throwaway engine clone, then configures scratch SuperEmbedder builds with
# this selector. Empty is the only production value.
set(SEMB_C38_DEFINE_TARGET "" CACHE STRING
	"C38 scratch construction only: empty, library, or witness")
set_property(CACHE SEMB_C38_DEFINE_TARGET PROPERTY STRINGS "" library witness)
if(NOT SEMB_C38_DEFINE_TARGET STREQUAL "" AND
		NOT SEMB_C38_DEFINE_TARGET STREQUAL "library" AND
		NOT SEMB_C38_DEFINE_TARGET STREQUAL "witness")
	message(FATAL_ERROR
		"SEMB_C38_DEFINE_TARGET must be empty, library, or witness; got '${SEMB_C38_DEFINE_TARGET}'")
endif()
if(NOT SEMB_C38_DEFINE_TARGET STREQUAL "")
	set(_semb_c38_header "${SEMB_ENGINE_PREFIX}/include/superslm/layer_marshal.h")
	if(NOT EXISTS "${_semb_c38_header}")
		message(FATAL_ERROR "C38 scratch header not found: ${_semb_c38_header}")
	endif()
	file(READ "${_semb_c38_header}" _semb_c38_header_text)
	string(FIND "${_semb_c38_header_text}" "SUPERSLM_C38_INLINE_PERTURBATION" _semb_c38_marker)
	if(_semb_c38_marker EQUAL -1)
		message(FATAL_ERROR
			"SEMB_C38_DEFINE_TARGET was set, but the installed scratch engine header carries no "
			"SUPERSLM_C38_INLINE_PERTURBATION branch")
	endif()
endif()

# engine_path_sha (sec 4.3: "the build selects one adapter translation unit
# in one branch of one variable... its bytes are engine_path_sha, manifest
# provenance since two adapters computing the same vectors share an
# epoch") — a content digest of the one selected adapter TU's own bytes,
# computed at configure (a byte digest of a produced/committed file, never
# a path or a CMake property — StandardsDocument.md sec 3.3's own
# producer-identity rule, applied here to the one variable this project
# has today: SEMB_ENGINE_ADAPTER_TU always resolves to the interim TU
# until U3 adds a second branch). Computed BEFORE the configure_file()
# call below (TE-79 F-11, TE-76 review) so it reaches the generated pin
# header like the other three pin fields, rather than only a
# message(STATUS) line no build artifact carries.
set(SEMB_ENGINE_ADAPTER_TU "${CMAKE_SOURCE_DIR}/src/superembedder/engine_adapter_interim.cpp")
file(SHA256 "${SEMB_ENGINE_ADAPTER_TU}" SEMB_ENGINE_PATH_SHA)
message(STATUS "engine_path_sha (${SEMB_ENGINE_ADAPTER_TU}): ${SEMB_ENGINE_PATH_SHA}")

# The pinned engine's tag/commit/tree, and engine_path_sha, baked in for
# the CLI to report and for any future unit that wants a compile-time
# handle on the pin.
configure_file(
	"${CMAKE_SOURCE_DIR}/cmake/templates/semb_engine_pin.h.in"
	"${CMAKE_BINARY_DIR}/generated/semb_engine_pin.h"
	@ONLY
)

# sec 8 C31(ii) (SEMB_WITNESS_DETACHED_LINK, sec 4.4d item 2): a per-configure
# 128-bit-WIDE hex token, baked into a generated header the same way the
# four pin fields above are -- compiled into both the library and the
# witness executable's own TU, so the two copies are identical if and only
# if both were configured together. string(RANDOM ...) with no RANDOM_SEED
# given draws each character from CMake's own rand()-based generator, seeded
# once per CMake PROCESS -- executed (TE-89 build log, M-4): five consecutive
# `cmake -P` runs of this exact call produced five distinct tokens, so the
# per-configure-distinct property this clause needs is verified by execution,
# not merely asserted. TE-89 M-4: the token is 128 bits WIDE and its ENTROPY
# is bounded at ~2^32 -- each character is drawn from the SAME 32-bit seed,
# not independently, so the collision probability between two specific
# configures is ~2^-32, not ~2^-128 -- and CMake's own documentation for
# string(RANDOM) describes RANDOM_SEED only; it does not document
# per-invocation reseeding from the clock, so that behavior above is stated
# as executed, not as CMake's documented contract.
string(RANDOM LENGTH 32 ALPHABET "0123456789abcdef" SEMB_WITNESS_BUILD_GENERATION_TOKEN)
configure_file(
	"${CMAKE_SOURCE_DIR}/cmake/templates/semb_witness_build_generation.h.in"
	"${CMAKE_BINARY_DIR}/generated/semb_witness_build_generation.h"
	@ONLY
)

# ---------------------------------------------------------------------------
# The superembedder library. TE-63 adds the arithmetic: PS-1's pooling
# epilogue + W-A's primitive driver (ps1_pool.cpp), the seam serialisation
# (seam_serialize.cpp), and the ONE interim-adapter TU that names the
# engine's forward entry points (engine_adapter_interim.cpp — C39's
# singleton scan enforces there is only one).
# ---------------------------------------------------------------------------
set(SEMB_LIBRARY_SOURCES
	src/version.cpp
	src/superembedder/encoder.cpp
	src/superembedder/query.cpp
	src/superembedder/ps1_pool.cpp
	src/superembedder/ps_n_pool.cpp
	src/superembedder/seam_serialize.cpp
	src/superembedder/wa_digest.cpp
	src/superembedder/wb_corpus.cpp
	src/superembedder/witness_build_generation.cpp
	src/superembedder/engine_adapter_interim.cpp
	src/superembedder/json_lite.cpp
	src/superembedder/bank.cpp
	src/superembedder/certification.cpp
	src/superembedder/engine_link_check.cpp
)
# U3 (te266 sec 4.2): the CPU-only or GPU choice is which of two adapter TUs
# links; nothing else SEMB_BUILD_GPU introduces reaches another translation
# unit (no compile definition, include directory or option on the library).
if(SEMB_BUILD_GPU)
	list(APPEND SEMB_LIBRARY_SOURCES src/superembedder/engine_adapter_gpu.cpp)
else()
	list(APPEND SEMB_LIBRARY_SOURCES src/superembedder/engine_adapter_gpu_off.cpp)
endif()

# U2-3 items 1 and 13: the step every build runs before any library
# translation unit compiles (tools/semb_build_step.py): it refuses
# SEMB_BUILD_AMBIENT_COMPILER_INPUT and SEMB_ENGINE_CHANGED_SINCE_CONFIGURE,
# and writes the product's compile-flag record and the header that compiles
# its digest in as the descriptor's compileFlags. The header exists before
# the first build so configure's source-closure scan resolves it; its
# content is build-directory state, outside semb_source_sha.
set(_semb_compile_flags_header "${CMAKE_BINARY_DIR}/generated/semb_compile_flags.h")
if(NOT EXISTS "${_semb_compile_flags_header}")
	file(WRITE "${_semb_compile_flags_header}"
		"// Placeholder written by configure; tools/semb_build_step.py rewrites it before any compile.
"
		"#pragma once
#define SEMB_COMPILE_FLAGS_SHA \"unrecorded\"\n")
endif()
add_custom_target(semb_build_step
	COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/semb_build_step.py"
		--build-dir "${CMAKE_BINARY_DIR}"
		--source-dir "${CMAKE_SOURCE_DIR}"
		--engine-prefix "${SEMB_ENGINE_PREFIX}"
		--provenance-sha256 "${SEMB_ENGINE_PROVENANCE_SHA256}"
		--library "$<TARGET_FILE:superslm::superslm>"
		--library-sha256 "${SEMB_ENGINE_LIBRARY_SHA256}"
		--compiler-id "${CMAKE_CXX_COMPILER_ID}"
		--frontend "${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}"
		--target superembedder --target semb --target semb_witness
		--header "${_semb_compile_flags_header}"
	BYPRODUCTS "${_semb_compile_flags_header}" "${CMAKE_BINARY_DIR}/generated/compile-flags.json"
	COMMENT "compile-flag record, ambient compiler inputs, engine unchanged since configure"
	VERBATIM)

# W-Q's committed population is data, not a second set of literals in the
# query TU.  Configure verifies the fixture bytes against their manifest and
# compiles those exact bytes into the library; an edited fixture therefore
# stops configuration until its manifest hash is intentionally re-pinned.
set(_semb_wq_fixture_dir "${CMAKE_SOURCE_DIR}/tests/fixtures/wq")
set(_semb_wq_manifest_path "${_semb_wq_fixture_dir}/wq-queries.manifest.json")
# The generated W-Q population is compiled into query.cpp. Track each source
# fixture at configure time so a re-pin cannot leave stale generated bytes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	"${_semb_wq_manifest_path}"
	"${_semb_wq_fixture_dir}/wq-queries.jsonl"
	"${_semb_wq_fixture_dir}/wq-truncation-leg.jsonl"
)
file(READ "${_semb_wq_manifest_path}" _semb_wq_manifest)
string(JSON _semb_wq_truncation_cap GET "${_semb_wq_manifest}" truncation_content_cap)
if(NOT _semb_wq_truncation_cap MATCHES "^[1-9][0-9]*$")
	message(FATAL_ERROR "W-Q manifest truncation_content_cap must be a positive integer")
endif()
foreach(_semb_wq_fixture IN ITEMS wq-queries.jsonl wq-truncation-leg.jsonl)
	string(JSON _semb_wq_expected_sha GET "${_semb_wq_manifest}" files "${_semb_wq_fixture}" sha256)
	file(SHA256 "${_semb_wq_fixture_dir}/${_semb_wq_fixture}" _semb_wq_actual_sha)
	if(NOT _semb_wq_actual_sha STREQUAL _semb_wq_expected_sha)
		message(FATAL_ERROR
			"W-Q fixture '${_semb_wq_fixture}' SHA-256 mismatch: expected ${_semb_wq_expected_sha}, got ${_semb_wq_actual_sha}")
	endif()
	file(READ "${_semb_wq_fixture_dir}/${_semb_wq_fixture}" _semb_wq_${_semb_wq_fixture})
	string(FIND "${_semb_wq_${_semb_wq_fixture}}" ")SEMB_WQ\"" _semb_wq_raw_delimiter)
	if(NOT _semb_wq_raw_delimiter EQUAL -1)
		message(FATAL_ERROR "W-Q fixture '${_semb_wq_fixture}' contains the generated-source raw-string delimiter")
	endif()
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/generated/semb_wq_population.h"
"#pragma once\n\n#include <cstddef>\n#include <string_view>\n\nnamespace semb {\n\n"
"inline constexpr std::string_view kWqJoinJsonl = R\"SEMB_WQ(${_semb_wq_wq-queries.jsonl})SEMB_WQ\";\n"
"inline constexpr std::string_view kWqTruncationJsonl = R\"SEMB_WQ(${_semb_wq_wq-truncation-leg.jsonl})SEMB_WQ\";\n"
"inline constexpr size_t kWqTruncationContentCap = ${_semb_wq_truncation_cap};\n\n}  // namespace semb\n")

semb_compute_source_provenance(superembedder
	SOURCES ${SEMB_LIBRARY_SOURCES}
	# "${CMAKE_BINARY_DIR}/generated" added for
	# src/superembedder/witness_build_generation.cpp's own
	# #include "semb_witness_build_generation.h" (sec 8 C31(ii)) -- the
	# real compiler invocation this macro drives needs the identical
	# include path the actual target build uses to resolve it.
	INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/include" "${SEMB_ENGINE_PREFIX}/include" "${CMAKE_BINARY_DIR}/generated"
	OUT_HEADER "${CMAKE_BINARY_DIR}/generated/semb_provenance.h"
)

add_library(superembedder STATIC ${SEMB_LIBRARY_SOURCES})
target_include_directories(superembedder
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
	PRIVATE
		"${CMAKE_BINARY_DIR}/generated"
)
target_link_libraries(superembedder PRIVATE superslm::superslm)
if(SEMB_BUILD_GPU)
	target_link_libraries(superembedder PRIVATE superslm::superslm_gpu d3d12 dxgi)
endif()
add_dependencies(superembedder semb_build_step)
# U2-3 item 1: the descriptor's configuration and buildSystem. A build that
# defines neither (a build of the vendored sources by another build system)
# matches no member.
target_compile_definitions(superembedder PRIVATE
	"SEMB_BUILD_CONFIGURATION=\"$<CONFIG>\""
	"SEMB_BUILD_SYSTEM=\"cmake\"")
if(SEMB_C38_DEFINE_TARGET STREQUAL "library")
	target_compile_definitions(superembedder PRIVATE SUPERSLM_C38_INLINE_PERTURBATION=1)
endif()
semb_verify_source_provenance(superembedder SIDECAR "${SEMB_SOURCE_PROVENANCE_SIDECAR_superembedder}"
	STATUS_NAME "SEMB_SOURCE_SET_DRIFT")

# ---------------------------------------------------------------------------
# U2-1 (F4, F38, F40, F41): the installed library is SHARED, linked from the
# static target's own objects, with the engine linked in privately. The
# static target stays the build's internal library (semb, semb_witness and
# the tests use its internal interfaces) and is not installed.
#
# The objects are position-independent (a shared object needs it on Linux,
# F40) and compiled with hidden visibility, inline functions included, so
# only what semb.h marks SEMB_API has default visibility. The export list is
# generated from semb.h at configure time by one scan rule
# (tools/generate_export_list.py) -- a .def on Windows, a version script on
# Linux, an exported-symbols list on macOS -- and after the link a check
# refuses SEMB_EXPORT_LIST_MISMATCH unless the library's dynamic symbols
# equal that scan exactly. Neither mechanism alone carries the claim.
# ---------------------------------------------------------------------------
set_target_properties(superembedder PROPERTIES
	POSITION_INDEPENDENT_CODE ON
	CXX_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN ON)
target_compile_definitions(superembedder PRIVATE SEMB_BUILDING_LIBRARY)

set(_semb_public_header "${CMAKE_SOURCE_DIR}/include/superembedder/semb.h")
set(_semb_export_tool "${CMAKE_SOURCE_DIR}/tools/generate_export_list.py")
if(WIN32)
	set(_semb_export_format def)
	set(_semb_export_list "${CMAKE_BINARY_DIR}/generated/semb_exports.def")
elseif(APPLE)
	set(_semb_export_format exported-symbols)
	set(_semb_export_list "${CMAKE_BINARY_DIR}/generated/semb_exports.txt")
else()
	set(_semb_export_format version-script)
	set(_semb_export_list "${CMAKE_BINARY_DIR}/generated/semb_exports.map")
endif()
execute_process(
	COMMAND "${Python3_EXECUTABLE}" "${_semb_export_tool}" generate
		--header "${_semb_public_header}" --format ${_semb_export_format} --out "${_semb_export_list}"
	RESULT_VARIABLE _semb_export_result
	OUTPUT_VARIABLE _semb_export_output
	ERROR_VARIABLE _semb_export_output)
if(NOT _semb_export_result EQUAL 0)
	message(FATAL_ERROR "The export list could not be generated from semb.h:\n${_semb_export_output}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_semb_public_header}" "${_semb_export_tool}")

add_library(superembedder_shared SHARED $<TARGET_OBJECTS:superembedder>)
set_target_properties(superembedder_shared PROPERTIES
	OUTPUT_NAME semb
	EXPORT_NAME superembedder
	LINK_DEPENDS "${_semb_export_list}"
	# Its own directory: on Windows the DLL's import library is semb.lib,
	# which must not meet anything the executables beside it write.
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared"
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared")
target_include_directories(superembedder_shared
	INTERFACE
		$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(superembedder_shared INTERFACE SEMB_SHARED)
target_link_libraries(superembedder_shared PRIVATE superslm::superslm)
if(SEMB_BUILD_GPU)
	target_link_libraries(superembedder_shared PRIVATE superslm::superslm_gpu d3d12 dxgi)
endif()
if(WIN32)
	target_sources(superembedder_shared PRIVATE "${_semb_export_list}")
elseif(APPLE)
	target_link_options(superembedder_shared PRIVATE "LINKER:-exported_symbols_list,${_semb_export_list}")
else()
	target_link_options(superembedder_shared PRIVATE
		"LINKER:--version-script=${_semb_export_list}" "LINKER:--no-undefined")
endif()
if(NOT WIN32)
	add_custom_command(TARGET superembedder_shared POST_BUILD
		COMMAND "${Python3_EXECUTABLE}" "${_semb_export_tool}" check
			--header "${_semb_public_header}" --library "$<TARGET_FILE:superembedder_shared>"
			--format $<IF:$<BOOL:${APPLE}>,macho,elf> --nm "${CMAKE_NM}"
		VERBATIM)
endif()

# R20 (U2-1): no certified object defines a floating-point substitutable
# function. After the archive is built, the census reads every object it
# contains (never a linked image) and fails the build naming each one. The
# reader is the R20 cell's own, per object format: COFF through dumpbin and
# lib.exe, ELF through binutils. Mach-O has no reader yet (REL-2's macOS leg).
set(_semb_r20_reader "${CMAKE_SOURCE_DIR}/tests/te331_r20_object_census_check.py")
set(_semb_r20_stamp "${CMAKE_BINARY_DIR}/superembedder_r20_census.stamp")
if(WIN32)
	get_filename_component(_semb_linker_dir "${CMAKE_LINKER}" DIRECTORY)
	find_program(SEMB_DUMPBIN NAMES dumpbin HINTS "${_semb_linker_dir}")
	find_program(SEMB_LIB_EXE NAMES lib HINTS "${_semb_linker_dir}")
	set(_semb_r20_command msvc --lib "$<TARGET_FILE:superembedder>"
		--dumpbin "${SEMB_DUMPBIN}" --libexe "${SEMB_LIB_EXE}")
elseif(NOT APPLE)
	set(_semb_r20_command gcc --archive "$<TARGET_FILE:superembedder>"
		--ar "${CMAKE_AR}" --readelf "${CMAKE_READELF}" --objdump "${CMAKE_OBJDUMP}")
endif()
if(DEFINED _semb_r20_command)
	add_custom_command(OUTPUT "${_semb_r20_stamp}"
		COMMAND "${Python3_EXECUTABLE}" "${_semb_r20_reader}" ${_semb_r20_command}
		COMMAND "${CMAKE_COMMAND}" -E touch "${_semb_r20_stamp}"
		DEPENDS superembedder "${_semb_r20_reader}"
		COMMENT "R20: floating-point substitutable functions in the certified archive's objects"
		VERBATIM)
	add_custom_target(superembedder_r20_census ALL DEPENDS "${_semb_r20_stamp}")
else()
	message(STATUS "R20: no object reader for this format; the census does not run on this build")
endif()

include(CMakePackageConfigHelpers)
install(TARGETS superembedder_shared EXPORT superembedderTargets
	RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
	LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
	ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES
	"${CMAKE_SOURCE_DIR}/include/superembedder/semb.h"
	"${CMAKE_SOURCE_DIR}/include/superembedder/version.h"
	DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/superembedder")
install(EXPORT superembedderTargets
	NAMESPACE superembedder::
	DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/superembedder")
file(WRITE "${CMAKE_BINARY_DIR}/superembedderConfig.cmake"
	"# superembedder::superembedder, the shared library. The engine is linked into\n"
	"# it privately, so this package depends on no other.\n"
	"include(\"\${CMAKE_CURRENT_LIST_DIR}/superembedderTargets.cmake\")\n")
write_basic_package_version_file("${CMAKE_BINARY_DIR}/superembedderConfigVersion.cmake"
	VERSION "${PROJECT_VERSION}"
	COMPATIBILITY SameMajorVersion)
install(FILES
	"${CMAKE_BINARY_DIR}/superembedderConfig.cmake"
	"${CMAKE_BINARY_DIR}/superembedderConfigVersion.cmake"
	DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/superembedder")


# ---------------------------------------------------------------------------
# The semb CLI exposes Slice 6's bake, query, verify, and inspect verbs over
# the checked SuperEmbedder ABI.
# ---------------------------------------------------------------------------
add_executable(semb src/main.cpp)
target_link_libraries(semb PRIVATE superembedder)
# Command-line queries are UTF-8 on Windows (src/semb.manifest): without it argv arrives in the ANSI
# code page and every non-ASCII query is refused as SEMB_INVALID_UTF8.
if(WIN32)
	target_sources(semb PRIVATE src/semb.manifest)
endif()
target_include_directories(semb PRIVATE "${CMAKE_BINARY_DIR}/generated" "${CMAKE_SOURCE_DIR}/src/superembedder")
# U2-1: `semb --version`'s build line, from what CMake itself recorded.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT)
	set(_semb_frontend "${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}")
else()
	set(_semb_frontend "${CMAKE_CXX_COMPILER_ID}")
endif()
target_compile_definitions(semb PRIVATE
	"SEMB_BUILD_OS=\"${CMAKE_SYSTEM_NAME}\""
	"SEMB_BUILD_ARCH=\"${CMAKE_SYSTEM_PROCESSOR}\""
	"SEMB_BUILD_COMPILER_ID=\"${CMAKE_CXX_COMPILER_ID}\""
	"SEMB_BUILD_COMPILER_VERSION=\"${CMAKE_CXX_COMPILER_VERSION}\""
	"SEMB_BUILD_COMPILER_FRONTEND=\"${_semb_frontend}\""
	"SEMB_BUILD_CONFIGURATION=\"$<CONFIG>\"")
# U2-1: the CLI is installed beside the shared library and the header.
install(TARGETS semb RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# ---------------------------------------------------------------------------
# C12b's instrument (sec 8, C12b; D-SLM4648): per-row byte digests over the
# seam, PS-N's selected position and the emitted vector, through the
# library's own interim handle surface
# (include/superembedder/encoder_interim_abi.h). Like
# the witness, it is a loop, a serialiser and a digest -- no arithmetic of its
# own on the reproducible path, and it links the engine directly only for
# superslm::Sha256/ToHex. Graded by tests/c12b_row_digest_check.py.
# ---------------------------------------------------------------------------
add_executable(semb_c12b_row_digest src/c12b/c12b_row_digest_main.cpp)
target_link_libraries(semb_c12b_row_digest PRIVATE superembedder superslm::superslm)

# ---------------------------------------------------------------------------
# The witness executable (sec 4.4b's structural law, clauses 1/2/4): a
# target of THIS build, linking the real superembedder library (and,
# transitively, the real installed superslm library) — never a prebuilt
# binary and never a second configure. Its own translation unit
# (src/witness/witness_main.cpp) is a loop and a serialiser, no
# arithmetic — clause 3's own reason W-A's driver and the interim adapter
# live in the library, not here.
# ---------------------------------------------------------------------------
add_executable(semb_witness src/witness/witness_main.cpp)
if(SEMB_C38_DEFINE_TARGET STREQUAL "witness")
	target_compile_definitions(semb_witness PRIVATE SUPERSLM_C38_INLINE_PERTURBATION=1)
endif()

# sec 8 C31(i) (SEMB_WITNESS_FOREIGN_BUILD, sec 4.4d item 1): the same
# source-provenance digest/re-verify pair already proven for the
# `superembedder` library target (C34), applied to `semb_witness` as a
# second, independent target -- a rebuild whose compiled closure disagrees
# with the configure-time digest fails the BUILD, by name, before any
# executable exists to misbehave.
semb_compute_source_provenance(semb_witness
	SOURCES src/witness/witness_main.cpp
	# "${CMAKE_BINARY_DIR}/generated" added for witness_main.cpp's own
	# #include "semb_witness_build_generation.h" (sec 8 C31(ii)).
	INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/include" "${SEMB_ENGINE_PREFIX}/include" "${CMAKE_BINARY_DIR}/generated"
	OUT_HEADER "${CMAKE_BINARY_DIR}/generated/semb_witness_provenance.h"
	# A distinct sidecar name -- `superembedder`'s own call above uses the
	# default, unparameterized name; without an explicit override here
	# this call would silently overwrite THAT sidecar (both default to
	# the same file otherwise), corrupting superembedder's own POST_BUILD
	# re-verification (cmake/SourceProvenance.cmake's own SIDECAR_NAME
	# comment).
	SIDECAR_NAME "semb_witness_source_provenance.json"
)

# Links the engine directly too (not only transitively through
# superembedder, which links it PRIVATE) — this TU calls
# superslm::Sha256/ToHex directly for the seam digest, sec 4.4b's own "the
# same routine model_sha uses."
target_link_libraries(semb_witness PRIVATE superembedder superslm::superslm)
# sec 8 C31(ii): witness_main.cpp includes the generated build-generation
# token header directly (semb_witness_build_generation.h) -- not exposed
# through superembedder's own PUBLIC include dirs, since that header lives
# in the same PRIVATE generated dir the library itself uses.
target_include_directories(semb_witness PRIVATE "${CMAKE_BINARY_DIR}/generated")
# TE-89 (TE-88 review S-3, sec 8 C31(i), sec 4.4d item 1): §4.4d says the
# drift "fails the build, by name" -- cmd_verify used to report only
# {"agrees": false, ...}, a JSON field with no named status a consumer could
# grep for the way every other refusal in this project is named. STATUS_NAME
# parameterises the same POST_BUILD verify step this target shares with
# `superembedder` (which keeps its own name, SEMB_SOURCE_SET_DRIFT) with the
# name sec 4.4d actually specifies for THIS target's own drift.
semb_verify_source_provenance(semb_witness SIDECAR "${SEMB_SOURCE_PROVENANCE_SIDECAR_semb_witness}"
	STATUS_NAME "SEMB_WITNESS_FOREIGN_BUILD")

# TE-89 (TE-88 review C-1, sec 8 C31(i)/C31(ii)): tests/witness_structural_matrix.py
# and tools/engine_side_wa_perturbation.py used to hardcode "semb_witness.exe"
# and "superembedder.lib" -- correct on Windows, wrong on ubuntu-latest and
# macos-latest, where a genuinely successful build's own real output is
# named differently and the hardcoded lookup reported an EVALUATED FAIL
# rather than the honest MISSING PREREQUISITE or PASS the platform earned.
# The fix is to ask THIS BUILD for its own real artifact paths rather than
# have a python test branch on the platform string: file(GENERATE) resolves
# $<TARGET_FILE:...> at CMake's own generate step (using its own, always-
# correct per-platform executable-suffix and static-library-prefix/suffix
# rules) into a small JSON manifest every configure produces, single-config
# generator or not. Every scratch tree the matrix and the perturbation
# script configure carries this same CMakeLists.txt (copy_repo()), so every
# scratch configure regenerates its own manifest too -- there is no
# separate maintenance path for the scratch case.
file(GENERATE
	OUTPUT "${CMAKE_BINARY_DIR}/semb_artifact_manifest.json"
	CONTENT "{\n  \"semb_witness\": \"$<TARGET_FILE:semb_witness>\",\n  \"superembedder\": \"$<TARGET_FILE:superembedder>\"\n}\n"
)

# ---------------------------------------------------------------------------
# Tests: the negative-configure matrix (C33's provenance legs, C34's
# source-provenance legs, and C14's build-configuration legs) and the
# positive-configure smoke.
# ---------------------------------------------------------------------------
enable_testing()

# TE-62's red suite (U0 slice B): C1, C3, C14, W-A's population, W-B, C39,
# C40, C31/C36/C38's structural matrix. tests/CMakeLists.txt is
# add_subdirectory-able because the superembedder target and
# superslm::superslm already exist in this scope by this point.
add_subdirectory(tests)

# TE-164 (U1 slice 3 build): the epoch store's own minting tool (sec 8,
# C32). Added here, after add_subdirectory(tests) so the superembedder
# target already exists, and before semb_assert_precise_fp() below so this
# target is swept into C14's directory-wide precise-FP audit like every
# other target this file declares.
add_subdirectory(tools/epoch-store)

# U3 (te266 sec 4.2, "Shaders"): the build stages the installed shader set
# beside semb and beside every GPU test executable, once per output directory
# (cmake/GpuShaders.cmake), and `cmake --install` places it beside the
# installed semb. The U3 GPU test executables are registered by a deferred call
# in tests/ that runs at the end of this directory, so the staging is deferred
# too: it is registered after add_subdirectory(tests), and deferred calls run
# in the order they were registered.
if(SEMB_BUILD_GPU)
	include(GpuShaders)
	cmake_language(DEFER CALL semb_stage_gpu_shaders "${superslm_GPU_SHADER_DIR}"
		semb u3_gpu_cells_test u3_gpu_alloc_test u3_c64_gpu_leg_test)
	install(DIRECTORY "${superslm_GPU_SHADER_DIR}/" DESTINATION "${CMAKE_INSTALL_BINDIR}/shaders"
		FILES_MATCHING PATTERN "*.cso")
endif()

# TE-79 F-4 (Significant, TE-76 review): semb_assert_precise_fp() moved
# HERE, after every add_subdirectory() this file calls -- it used to run
# BEFORE add_subdirectory(tests), so its own recursive BUILDSYSTEM_TARGETS
# walk (cmake/BuildFlags.cmake) saw an EMPTY subdirectory list and checked
# only [superembedder;semb;semb_witness], none of the nine test targets
# (executed, confirmed from the project's own recorded configure output:
# the printed population never named a test TU). Both product targets
# above (and any future target this file declares) inherit the precise-FP
# flag from BuildFlags.cmake's directory-wide add_compile_options()
# automatically — there is no per-target call left to make. Assert the
# whole set now, once, after nothing else can add or remove a target --
# which is only true once every add_subdirectory() call has already run.
semb_assert_precise_fp()

add_test(
	NAME provenance_and_flag_matrix
	COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/ci/run_configure_matrix.py"
		--repo-root "${CMAKE_SOURCE_DIR}"
		--engine-prefix "${SEMB_ENGINE_PREFIX}"
		--cmake-executable "${CMAKE_COMMAND}"
		--generator "${CMAKE_GENERATOR}"
		--ninja-executable "${CMAKE_MAKE_PROGRAM}"
		--python-executable "${Python3_EXECUTABLE}"
)
