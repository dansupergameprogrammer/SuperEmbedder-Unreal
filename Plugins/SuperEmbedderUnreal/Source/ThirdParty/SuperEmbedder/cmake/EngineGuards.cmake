# cmake/EngineGuards.cmake — consumes the pinned SuperSLM engine install
# (tools/pin-superslm) and enforces the guards Claude/Plans/SuperEmbedder_Plan.md
# §4.1 specifies: two version guards (floor, exact pin) and the provenance
# checks that make `superslm.lock` an independently-verifiable claim rather
# than a string the build merely trusts.
#
# DESIGN NOTE ON find_package's VERSION ARGUMENT (build log: te61-u0a, read
# before editing this file). §4.1's prose calls for
# `find_package(superslm 1.3 CONFIG REQUIRED)`, relying on
# superslmConfigVersion.cmake's SameMajorVersion mode to accept 1.x >= 1.3 —
# but passing "1.3" to find_package means CMake's OWN version-compatibility
# machinery silently refuses (with CMake's generic "no suitable version
# found" message, not a named SEMB_* error) any installed engine below the
# floor, which would make the floor guard below UNREACHABLE for the exact
# defect it exists to name. This file therefore calls find_package WITHOUT a
# version constraint — accepting whatever superslm_VERSION the found package
# reports, whatever it is — and enforces BOTH the floor and the exact pin
# itself, by name, below. This preserves the intended protection in full (no
# real installed version can slip past either guard) while making both
# independently testable via a real `cmake -S ... -B ...` invocation, which
# is what C33 requires.

# ---------------------------------------------------------------------------
# 1. Parse superslm.lock. `tag=` is the only field a person edits; `commit=`
#    and `tree=` are written by tools/pin-superslm and read back here.
# ---------------------------------------------------------------------------
if(NOT DEFINED SEMB_ENGINE_LOCK_FILE)
	set(SEMB_ENGINE_LOCK_FILE "${CMAKE_SOURCE_DIR}/superslm.lock")
endif()

if(NOT EXISTS "${SEMB_ENGINE_LOCK_FILE}")
	message(FATAL_ERROR "SEMB_ENGINE_LOCK_MISSING: ${SEMB_ENGINE_LOCK_FILE} does not exist. "
		"Run tools/pin-superslm/pin_superslm.py first.")
endif()

file(STRINGS "${SEMB_ENGINE_LOCK_FILE}" _semb_lock_lines)
set(SEMB_LOCK_TAG "")
set(SEMB_LOCK_COMMIT "")
set(SEMB_LOCK_TREE "")
foreach(_line IN LISTS _semb_lock_lines)
	string(STRIP "${_line}" _line)
	if(_line MATCHES "^tag=(.*)$")
		set(SEMB_LOCK_TAG "${CMAKE_MATCH_1}")
	elseif(_line MATCHES "^commit=(.*)$")
		set(SEMB_LOCK_COMMIT "${CMAKE_MATCH_1}")
	elseif(_line MATCHES "^tree=(.*)$")
		set(SEMB_LOCK_TREE "${CMAKE_MATCH_1}")
	endif()
endforeach()

if(SEMB_LOCK_TAG STREQUAL "")
	message(FATAL_ERROR "SEMB_ENGINE_LOCK_MALFORMED: ${SEMB_ENGINE_LOCK_FILE} has no 'tag=' line.")
endif()
if(SEMB_LOCK_COMMIT STREQUAL "" OR SEMB_LOCK_TREE STREQUAL "")
	message(FATAL_ERROR "SEMB_ENGINE_LOCK_MALFORMED: ${SEMB_ENGINE_LOCK_FILE} is missing 'commit=' or "
		"'tree=' — run tools/pin-superslm/pin_superslm.py to populate them before configuring.")
endif()

string(REGEX REPLACE "^v" "" SEMB_LOCK_VERSION "${SEMB_LOCK_TAG}")

# ---------------------------------------------------------------------------
# 2. Locate the engine. CMAKE_PREFIX_PATH (or -Dsuperslm_DIR=...) must already
#    point at the prefix tools/pin-superslm installed to; this file does not
#    invoke the pin tool itself (it is a separate, explicit step — §4.1's own
#    "why not FetchContent" reasoning applies here too: configure should not
#    silently reach onto the network).
# ---------------------------------------------------------------------------
find_package(superslm CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# 3. The two version guards.
# ---------------------------------------------------------------------------
if(superslm_VERSION VERSION_LESS "1.3.0")
	message(FATAL_ERROR "SEMB_ENGINE_BELOW_PIN_FLOOR: found SuperSLM ${superslm_VERSION} at "
		"${superslm_DIR}, below the floor 1.3.0. The FP-free open path (§1.3) is a >= requirement: "
		"below the floor, the unmasked-host trap class is live.")
endif()

if(NOT superslm_VERSION VERSION_EQUAL SEMB_LOCK_VERSION)
	message(FATAL_ERROR "SEMB_ENGINE_PIN_MISMATCH: found SuperSLM ${superslm_VERSION} at "
		"${superslm_DIR}, but ${SEMB_ENGINE_LOCK_FILE} pins ${SEMB_LOCK_VERSION}. Bit-identity needs "
		"an exact pin, not a floor: re-run tools/pin-superslm/pin_superslm.py, or edit the lock's "
		"tag= to match what is installed.")
endif()

# ---------------------------------------------------------------------------
# 4. Provenance. superslm_DIR is <prefix>/lib/cmake/superslm; the provenance
#    manifest tools/pin-superslm wrote sits at <prefix>/share/superslm-provenance/.
# ---------------------------------------------------------------------------
# TE-65 F-16: PACKAGE_PREFIX_DIR, not a hardcoded "../../..". The prior
# get_filename_component(... "${superslm_DIR}/../../.." ...) hardcodes the
# lib/cmake/superslm install depth; a multiarch libdir (e.g. Debian's
# lib/x86_64-linux-gnu/cmake/superslm) would resolve to the wrong directory
# and fire SEMB_ENGINE_PROVENANCE_MISSING spuriously. superslmConfig.cmake's
# own @PACKAGE_INIT@ block (CMakePackageConfigHelpers) already computes the
# correct prefix for WHATEVER depth this engine's own install() rules
# actually used, and — verified by execution — that variable remains set in
# this scope after find_package() returns, since find_package() includes
# the config script rather than calling it in a new scope. Using the
# engine's own computation is also more correct than re-deriving it: it is
# automatically right for any install layout the engine's own CMakeLists.txt
# chooses, not only the one this file's author checked.
if(DEFINED PACKAGE_PREFIX_DIR)
	set(SEMB_ENGINE_PREFIX "${PACKAGE_PREFIX_DIR}")
else()
	# Defensive fallback only — no CONFIG package this project has seen omits
	# @PACKAGE_INIT@, but a future or third-party one might.
	get_filename_component(SEMB_ENGINE_PREFIX "${superslm_DIR}/../../.." ABSOLUTE)
endif()
set(SEMB_ENGINE_PROVENANCE_FILE "${SEMB_ENGINE_PREFIX}/share/superslm-provenance/provenance.json")

if(NOT EXISTS "${SEMB_ENGINE_PROVENANCE_FILE}")
	message(FATAL_ERROR "SEMB_ENGINE_PROVENANCE_MISSING: no provenance manifest at "
		"${SEMB_ENGINE_PROVENANCE_FILE}. This install was not produced by "
		"tools/pin-superslm/pin_superslm.py (or its provenance output was deleted); configure cannot "
		"verify what source this engine build came from.")
endif()

file(READ "${SEMB_ENGINE_PROVENANCE_FILE}" _semb_provenance_json)
string(JSON _semb_prov_tag ERROR_VARIABLE _semb_prov_err GET "${_semb_provenance_json}" "tag")
if(_semb_prov_err)
	message(FATAL_ERROR "SEMB_ENGINE_PROVENANCE_MALFORMED: ${SEMB_ENGINE_PROVENANCE_FILE} could not "
		"be parsed as JSON (${_semb_prov_err}).")
endif()
string(JSON _semb_prov_commit GET "${_semb_provenance_json}" "commit")
string(JSON _semb_prov_tree GET "${_semb_provenance_json}" "engine_tree_sha")

# 4a. Lock/provenance label agreement (leg f) — the lock's tag/commit must be
#     what the tool actually resolved when it built THIS prefix.
if(NOT SEMB_LOCK_TAG STREQUAL _semb_prov_tag OR NOT SEMB_LOCK_COMMIT STREQUAL _semb_prov_commit)
	message(FATAL_ERROR "SEMB_PIN_LABEL_MISMATCH: ${SEMB_ENGINE_LOCK_FILE} names tag=${SEMB_LOCK_TAG} "
		"commit=${SEMB_LOCK_COMMIT}, but the installed engine's own provenance "
		"(${SEMB_ENGINE_PROVENANCE_FILE}) names tag=${_semb_prov_tag} commit=${_semb_prov_commit}. "
		"The lock's labels must match what tools/pin-superslm actually resolved for this install.")
endif()

# 4b. Lock/provenance digest agreement (legs a, c) — checked from BOTH sides:
#     a hand-edited lock digest (leg a) and a hand-edited provenance digest
#     (leg c) are both real, independently constructible negative-configure
#     legs that land on this one comparison.
if(NOT SEMB_LOCK_TREE STREQUAL _semb_prov_tree)
	message(FATAL_ERROR "SEMB_ENGINE_PROVENANCE_MISMATCH: ${SEMB_ENGINE_LOCK_FILE} pins "
		"tree=${SEMB_LOCK_TREE}, but the installed engine's own provenance "
		"(${SEMB_ENGINE_PROVENANCE_FILE}) declares engine_tree_sha=${_semb_prov_tree}. One of the two "
		"was hand-edited, or the lock was not re-derived after the pin last changed.")
endif()

# ---------------------------------------------------------------------------
# 5. The deep check: recompute engine_tree_sha fresh from whatever is
#    CURRENTLY on disk under the prefix, via tools/pin-superslm/verify_provenance.py,
#    and require it to still agree with the lock. This is "the lock's digest
#    equals the digest recomputed at configure from the prefix's own bytes"
#    (§4.1) — it catches drift the two label/digest-agreement checks above
#    cannot see, because those only compare two DECLARED strings against each
#    other; this one re-reads real bytes.
#
#    TE-65 F-3: leg (e) (an install built from a clone carrying an
#    uncommitted edit) used to be decided by reading `clean` straight out of
#    provenance.json — a single boolean the SAME producer that wrote
#    engine_tree_sha also wrote, so a construction that hand-edited only
#    `clean` to true (clearing `dirty_paths`) defeated the whole leg without
#    touching anything the digest covers. verify_provenance.py now
#    independently recomputes dirtiness PER COMPILED MEMBER, on every
#    configure, from that member's tag_content_sha256 (computed by
#    pin_superslm.py from git's own tag content, never from the working
#    tree) against the bytes actually on disk right now — `dirty_members`
#    below, not the manifest's own self-reported flag.
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
	COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/pin-superslm/verify_provenance.py" "${SEMB_ENGINE_PREFIX}"
	OUTPUT_VARIABLE _semb_verify_json
	RESULT_VARIABLE _semb_verify_rc
)
if(NOT _semb_verify_rc EQUAL 0)
	message(FATAL_ERROR "SEMB_ENGINE_PROVENANCE_VERIFY_FAILED: verify_provenance.py exited "
		"${_semb_verify_rc}. Output: ${_semb_verify_json}")
endif()

# 5a. Leg (e), by name: any compiled member whose current bytes disagree with
#     git's own tag content is a dirty install, refused here regardless of
#     what provenance.json's own `clean`/`dirty_paths` fields (still written
#     for diagnostics) claim.
#
#     TE-67 N-13: a read ERROR on this key used to be treated as "nothing to
#     check" (the `if` guarding the refusal never entered), while the
#     adjacent recomputed_engine_tree_sha read three lines below already
#     treated its own read error as FATAL. Two checks reading the SAME JSON
#     object handled a missing key oppositely -- a gate whose presence
#     depends on which key happens to be readable is F-2's own class,
#     restored here. A read error now refuses by the same rule an honest
#     zero-length list does not: absence of evidence is not evidence of
#     cleanliness.
string(JSON _semb_dirty_member_count ERROR_VARIABLE _semb_dm_err LENGTH "${_semb_verify_json}" "dirty_members")
if(_semb_dm_err)
	message(FATAL_ERROR "SEMB_ENGINE_INSTALL_DIVERGED: could not read 'dirty_members' from "
		"verify_provenance.py's own output (${_semb_dm_err}). A key this configure depends on to "
		"decide whether the install is clean is missing or malformed; refused rather than treated "
		"as nothing to check. Output: ${_semb_verify_json}")
endif()
if(_semb_dirty_member_count GREATER 0)
	string(JSON _semb_dirty_member_0 GET "${_semb_verify_json}" "dirty_members" 0)
	message(FATAL_ERROR "SEMB_ENGINE_INSTALL_DIVERGED: ${_semb_dirty_member_count} compiled-closure "
		"member(s) currently on disk under ${SEMB_ENGINE_PREFIX} disagree with what tag "
		"${_semb_prov_tag} records for that path (first: ${_semb_dirty_member_0}) — the clone this "
		"install was built from carried an uncommitted edit inside the compile+install closure.")
endif()

string(JSON _semb_recomputed_tree ERROR_VARIABLE _semb_rt_err GET "${_semb_verify_json}" "recomputed_engine_tree_sha")
if(_semb_rt_err OR _semb_recomputed_tree STREQUAL "" OR _semb_recomputed_tree STREQUAL "NOTFOUND")
	message(FATAL_ERROR "SEMB_ENGINE_INSTALL_DIVERGED: could not recompute engine_tree_sha from the "
		"prefix's current bytes (verify_provenance.py output: ${_semb_verify_json}). A covered file is "
		"missing from disk under ${SEMB_ENGINE_PREFIX}.")
endif()
if(NOT _semb_recomputed_tree STREQUAL SEMB_LOCK_TREE)
	message(FATAL_ERROR "SEMB_ENGINE_INSTALL_DIVERGED: recomputing engine_tree_sha from the bytes "
		"currently on disk under ${SEMB_ENGINE_PREFIX} gives ${_semb_recomputed_tree}, which disagrees "
		"with the lock's tree=${SEMB_LOCK_TREE}. The installed prefix was modified after "
		"tools/pin-superslm wrote its provenance.")
endif()

message(STATUS "SuperSLM engine pin verified: ${superslm_VERSION} @ ${_semb_prov_commit} "
	"(engine_tree_sha=${SEMB_LOCK_TREE})")

# ---------------------------------------------------------------------------
# 6. The engine-build record (plan U2-3 items 5 and 13; D-SLM7333). The pin
#    tool writes `engine_build` into provenance.json: the compiler the engine
#    was built with, the toolchain/stdlib/dispatch strings its kernel compile
#    gives for the product's descriptor header, the digest of its compile-flag
#    record (engineFlags), the installed library's SHA-256 and its build
#    record. A prefix without it refuses; a product compiler other than the
#    engine's refuses before anything compiles, so every build that reaches a
#    verdict compiled both sides with one compiler.
# ---------------------------------------------------------------------------
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SEMB_ENGINE_PROVENANCE_FILE}")
string(JSON _semb_engine_build ERROR_VARIABLE _semb_eb_err GET "${_semb_provenance_json}" "engine_build")
if(_semb_eb_err)
	message(FATAL_ERROR "SEMB_ENGINE_BUILD_UNRECORDED: ${SEMB_ENGINE_PROVENANCE_FILE} carries no engine_build "
		"record. It was written by a pin tool that predates the record; pin the engine again with "
		"tools/pin-superslm/pin_superslm.py.")
endif()
foreach(_semb_eb_key toolchain stdlib dispatch flagsSha librarySha256 buildRecord)
	string(JSON _semb_eb_${_semb_eb_key} ERROR_VARIABLE _semb_eb_err GET "${_semb_engine_build}" "${_semb_eb_key}")
	if(_semb_eb_err OR _semb_eb_${_semb_eb_key} STREQUAL "")
		message(FATAL_ERROR "SEMB_ENGINE_BUILD_UNRECORDED: ${SEMB_ENGINE_PROVENANCE_FILE}'s engine_build carries "
			"no ${_semb_eb_key}.")
	endif()
endforeach()
foreach(_semb_eb_key id version frontend)
	string(JSON _semb_eb_compiler_${_semb_eb_key} ERROR_VARIABLE _semb_eb_err
		GET "${_semb_engine_build}" compiler "${_semb_eb_key}")
	if(_semb_eb_err)
		message(FATAL_ERROR "SEMB_ENGINE_BUILD_UNRECORDED: ${SEMB_ENGINE_PROVENANCE_FILE}'s engine_build carries "
			"no compiler ${_semb_eb_key}.")
	endif()
endforeach()
if(NOT _semb_eb_compiler_id STREQUAL CMAKE_CXX_COMPILER_ID
		OR NOT _semb_eb_compiler_version STREQUAL CMAKE_CXX_COMPILER_VERSION
		OR NOT _semb_eb_compiler_frontend STREQUAL "${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}")
	message(FATAL_ERROR "SEMB_ENGINE_TOOLCHAIN_MISMATCH: this product is configured with "
		"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} (frontend '${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}'), "
		"but the engine at ${SEMB_ENGINE_PREFIX} was built with ${_semb_eb_compiler_id} "
		"${_semb_eb_compiler_version} (frontend '${_semb_eb_compiler_frontend}'). Pin the engine with the "
		"product's compiler, in the product's environment.")
endif()
set(SEMB_ENGINE_TOOLCHAIN "${_semb_eb_toolchain}")
set(SEMB_ENGINE_STDLIB "${_semb_eb_stdlib}")
set(SEMB_ENGINE_DISPATCH "${_semb_eb_dispatch}")
set(SEMB_ENGINE_FLAGS_SHA "${_semb_eb_flagsSha}")
set(SEMB_ENGINE_LIBRARY_SHA256 "${_semb_eb_librarySha256}")
set(SEMB_ENGINE_BUILD_RECORD "${_semb_eb_buildRecord}")
file(SHA256 "${SEMB_ENGINE_PROVENANCE_FILE}" SEMB_ENGINE_PROVENANCE_SHA256)
message(STATUS "SuperSLM engine build: ${SEMB_ENGINE_TOOLCHAIN}, ${SEMB_ENGINE_STDLIB}, dispatch "
	"${SEMB_ENGINE_DISPATCH}, engineFlags ${SEMB_ENGINE_FLAGS_SHA}")
