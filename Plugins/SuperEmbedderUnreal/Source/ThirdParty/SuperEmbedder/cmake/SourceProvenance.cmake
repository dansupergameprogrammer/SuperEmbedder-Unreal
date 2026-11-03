# cmake/SourceProvenance.cmake — §8 C34: semb_source_sha, a digest over
# every file the compiler reads to build the `superembedder` library
# target — its declared sources plus their transitive include closure, as
# the compiler itself resolves it (§4.4) — baked into a generated header at
# configure time and re-verified against the REAL compiled closure (Ninja's
# own deps db) as a post-build step.
#
# semb_compute_source_provenance(<target>
#   SOURCES <src1> <src2> ...
#   INCLUDE_DIRS <dir1> <dir2> ...
#   OUT_HEADER <path>)
#
# INCLUDE_DIRS must match what target_include_directories() is about to set
# on the real target — the covered set is only as accurate as the include
# path it is resolved under (TE-65 fix round, F-1).
#
# Call once, after the covered set is final and BEFORE add_library()/
# target_sources() so the generated header exists in time to be compiled in.
# Returns the sidecar's path in SEMB_SOURCE_PROVENANCE_SIDECAR_<target> (TE-89,
# TE-88 review S-5: a variable named per target, so two calls for two targets
# cannot alias) -- pass that straight to semb_verify_source_provenance(<target>
# SIDECAR ...) below.

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Leg (a): SEMB_SOURCE_SHA is an OUTPUT of configure, never an input. A cache
# variable by that name means someone tried to override the computed value on
# the command line.
if(DEFINED CACHE{SEMB_SOURCE_SHA})
	message(FATAL_ERROR "SEMB_SOURCE_SHA_NOT_SETTABLE: SEMB_SOURCE_SHA is computed by configure from "
		"the library's own covered source set (§8 C34) — it is never a build input. Remove "
		"-DSEMB_SOURCE_SHA=... from the configure command.")
endif()

function(semb_compute_source_provenance target)
	cmake_parse_arguments(ARG "" "OUT_HEADER;SIDECAR_NAME" "SOURCES;INCLUDE_DIRS" ${ARGN})

	# Leg (b): every declared source must exist on disk before it is hashed —
	# checked here (not only inside the python helper) so the FATAL_ERROR
	# fires with this file's own name rather than a python traceback.
	foreach(_src IN LISTS ARG_SOURCES)
		if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
			message(FATAL_ERROR "SEMB_SOURCE_FILE_MISSING: '${_src}' is declared as a covered source "
				"for '${target}' but does not exist at ${CMAKE_CURRENT_SOURCE_DIR}/${_src}.")
		endif()
	endforeach()

	# Leg (c): SEMB_ENGINE_PATH is recorded on first configure. TE-69 P-9
	# (the comment corrected; the check's own behavior was already right):
	# TE-65 F-8 deliberately made SEMB_ENGINE_PATH NOT an input to
	# semb_source_sha, so the digest is path-invariant like engine_tree_sha
	# is (tools/semb_source_hash.py's own comment already states this in the
	# same tree this file lives in) -- this comment kept saying the opposite
	# of what the code two files over does. This check's own reason to exist
	# is independent of the digest: SEMB_ENGINE_PATH_RECORDED tracks which
	# engine install a build directory was LAST configured against, and a
	# later reconfigure resolving a DIFFERENT engine path in the same build
	# directory is refused because reusing incremental build state across an
	# engine change is not something this project has verified is safe, not
	# because the digest itself would go stale.
	if(DEFINED CACHE{SEMB_ENGINE_PATH_RECORDED} AND NOT "${SEMB_ENGINE_PREFIX}" STREQUAL "${SEMB_ENGINE_PATH_RECORDED}")
		message(FATAL_ERROR "SEMB_ENGINE_PATH_CHANGED: this build directory recorded engine path "
			"'${SEMB_ENGINE_PATH_RECORDED}' on an earlier configure; this configure resolves "
			"'${SEMB_ENGINE_PREFIX}' instead. Reusing this build directory's incremental state across "
			"an engine-path change is unverified — remove the build directory and reconfigure from "
			"clean rather than changing the engine path in place.")
	endif()
	set(SEMB_ENGINE_PATH_RECORDED "${SEMB_ENGINE_PREFIX}" CACHE INTERNAL "engine path this build directory was last configured against")

	# TE-87 (sec 8 C31(i)): the sidecar filename USED TO be a single
	# hardcoded literal, correct only as long as exactly one target ever
	# called this function per build directory. Applying this pair to a
	# SECOND target (semb_witness) with the default name unchanged would
	# have both calls write the SAME file -- the second call's own
	# execute_process() silently overwrites the first target's sidecar,
	# so its POST_BUILD verify step reads the WRONG target's digest and
	# fails a build that is actually clean. SIDECAR_NAME defaults to the
	# ORIGINAL literal ("semb_source_provenance.json") so the existing
	# `superembedder` call site (and tools/ci/run_configure_matrix.py's
	# own hardcoded read of that exact path, C33/C34's own legs) is
	# UNCHANGED; a second target passes its own distinct SIDECAR_NAME.
	if(ARG_SIDECAR_NAME)
		set(_sidecar_name "${ARG_SIDECAR_NAME}")
	else()
		set(_sidecar_name "semb_source_provenance.json")
	endif()
	set(_sidecar "${CMAKE_CURRENT_BINARY_DIR}/generated/${_sidecar_name}")
	get_filename_component(_out_header_dir "${ARG_OUT_HEADER}" DIRECTORY)
	file(MAKE_DIRECTORY "${_out_header_dir}")

	# TE-63: --compiler-id selects tools/semb_source_hash.py's own closure
	# resolver (_msvc_closure's /showIncludes vs _gcc_clang_closure's -M),
	# and CMAKE_CXX_COMPILER_ID alone is the wrong axis for it -- clang-cl
	# reports CMAKE_CXX_COMPILER_ID=Clang while its own command-line syntax
	# is MSVC's (/showIncludes, /std:, no -M), which is what
	# CMAKE_CXX_COMPILER_FRONTEND_VARIANT names. Executed: without this,
	# a clang-cl configure sends -M/-std=c++20 to a driver that does not
	# accept them and SEMB_SOURCE_PREPROCESS_FAILED's every clang-cl build.
	set(_semb_source_hash_compiler_id "${CMAKE_CXX_COMPILER_ID}")
	if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
		set(_semb_source_hash_compiler_id "MSVC")
	endif()

	set(_py_args
		configure
		--root "${CMAKE_CURRENT_SOURCE_DIR}"
		--compiler "${CMAKE_CXX_COMPILER}"
		--compiler-id "${_semb_source_hash_compiler_id}"
		--std "c++${CMAKE_CXX_STANDARD}"
		--engine-path "${SEMB_ENGINE_PREFIX}"
		--build-dir "${CMAKE_BINARY_DIR}"
		--out-header "${ARG_OUT_HEADER}"
		--out-json "${_sidecar}"
	)
	foreach(_src IN LISTS ARG_SOURCES)
		list(APPEND _py_args --source "${_src}")
	endforeach()
	foreach(_inc IN LISTS ARG_INCLUDE_DIRS)
		list(APPEND _py_args --include-dir "${_inc}")
	endforeach()

	# TE-71 Q-6: a genuine CACHE variable, matching SEMB_TEST_SKIP_FP_DIRECTIVE's
	# own pattern in cmake/BuildFlags.cmake -- never set in normal use. Replaces
	# the prior SEMB_TEST_FORCE_EMPTY_CLOSURE environment-variable hook, which
	# was ambient process state: inherited by every child process, announced
	# nowhere, and recorded in no build artifact, so a configure run under it
	# was indistinguishable afterward from one that was not. A `-D` cache
	# variable lands in CMakeCache.txt and stays readable in the build
	# directory forever, and the message(STATUS ...) below announces it in the
	# configure log too, exactly as the sibling hatch does.
	if(SEMB_TEST_FORCE_EMPTY_CLOSURE)
		message(STATUS "SEMB TEST: forcing an empty resolved closure "
			"(-DSEMB_TEST_FORCE_EMPTY_CLOSURE=1) to prove the two-producer subset check "
			"(SEMB_SOURCE_NOT_IN_OWN_CLOSURE) can fail.")
		list(APPEND _py_args --force-empty-closure)
	endif()

	execute_process(
		COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/semb_source_hash.py" ${_py_args}
		OUTPUT_VARIABLE _semb_src_out
		RESULT_VARIABLE _semb_src_rc
	)
	if(NOT _semb_src_rc EQUAL 0)
		string(JSON _semb_src_err ERROR_VARIABLE _ignored GET "${_semb_src_out}" "error")
		if(_semb_src_err STREQUAL "SEMB_SOURCE_FILE_MISSING")
			message(FATAL_ERROR "SEMB_SOURCE_FILE_MISSING: a declared source disappeared between the "
				"CMake-side existence check and the python read. Output: ${_semb_src_out}")
		endif()
		message(FATAL_ERROR "SEMB_SOURCE_HASH_FAILED: tools/semb_source_hash.py configure exited "
			"${_semb_src_rc}. Output: ${_semb_src_out}")
	endif()
	message(STATUS "semb_source_sha computed for '${target}': ${_semb_src_out}")

	# TE-89 (TE-88 review S-5): returning the sidecar path in ONE
	# shared-name variable made correctness depend on call-site ORDER --
	# whichever target's semb_verify_source_provenance() call read the
	# variable next got THIS call's sidecar, silently, if it ran before the
	# variable was consumed. A per-target variable name cannot alias: two
	# calls for two different targets write two different variables, so no
	# statement-order requirement exists to violate.
	set(SEMB_SOURCE_PROVENANCE_SIDECAR_${target} "${_sidecar}" PARENT_SCOPE)
endfunction()

# semb_verify_source_provenance(<target> SIDECAR <path>) — call AFTER
# add_library(<target> ...). Registers a POST_BUILD step that re-derives the
# REAL compiled closure from Ninja's own dependency database and fails the
# build (SEMB_SOURCE_SET_DRIFT) if it disagrees with the digest baked at
# configure time.
#
# TE-65 fix round, F-2: this used to WARN and skip the whole gate when the
# generator was not Ninja or `ninja` was not resolvable, so a build could be
# green with the C34 depfile leg silently absent — presence of a gate was a
# property of the environment, not of the source. It is now a hard configure-
# time requirement: this project's own CI (.github/workflows/ci.yml) always
# uses Ninja, so requiring it here does not narrow what is actually
# supported, and a configure that cannot provide the gate refuses loudly
# instead of building unguarded.
function(semb_verify_source_provenance target)
	cmake_parse_arguments(ARG "" "SIDECAR;STATUS_NAME" "" ${ARGN})
	# TE-89 (TE-88 review S-3): §4.4d requires the drift to fail the build
	# "by name" -- SEMB_SOURCE_SET_DRIFT for the library (C34, the existing,
	# already-named behavior) and SEMB_WITNESS_FOREIGN_BUILD for the witness
	# (C31(i), sec 4.4d item 1). STATUS_NAME is the one new argument this
	# call site needs to say which; it defaults to the library's own
	# existing name so every caller before this round is unaffected.
	if(NOT ARG_STATUS_NAME)
		set(ARG_STATUS_NAME "SEMB_SOURCE_SET_DRIFT")
	endif()
	if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
		message(FATAL_ERROR "SEMB_SOURCE_DRIFT_CHECK_UNAVAILABLE: the post-build semb_source_sha "
			"re-verification (§8 C34's depfile leg) needs the Ninja generator's own dependency "
			"database; generator is '${CMAKE_GENERATOR}'. Configure with '-G Ninja' — this project "
			"does not configure without this gate.")
	endif()
	find_program(SEMB_NINJA_EXECUTABLE NAMES ninja)
	if(NOT SEMB_NINJA_EXECUTABLE AND CMAKE_MAKE_PROGRAM)
		set(SEMB_NINJA_EXECUTABLE "${CMAKE_MAKE_PROGRAM}")
	endif()
	if(NOT SEMB_NINJA_EXECUTABLE)
		message(FATAL_ERROR "SEMB_SOURCE_DRIFT_CHECK_UNAVAILABLE: no ninja executable found on PATH "
			"or via CMAKE_MAKE_PROGRAM. §8 C34's post-build depfile leg cannot be registered — this "
			"project does not configure without this gate. Pass -DCMAKE_MAKE_PROGRAM=<path to ninja>.")
	endif()
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/semb_source_hash.py" verify
			--root "${CMAKE_CURRENT_SOURCE_DIR}"
			--build "${CMAKE_BINARY_DIR}"
			--ninja "${SEMB_NINJA_EXECUTABLE}"
			--target "${target}"
			--sidecar "${ARG_SIDECAR}"
			--status-name "${ARG_STATUS_NAME}"
		COMMENT "semb_source_sha post-build re-verification (C34 depfile leg) for ${target}"
		VERBATIM
	)
endfunction()
