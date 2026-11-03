# cmake/CertifiedBuilds.cmake -- the certified-builds list and the certification identity, compiled in (plan
# U2-3 items 1, 9 and 11; FORMAT.md sec 3 and sec 4).
#
# docs/certification-identity.json is what certification_epoch digests: a certified cell's epoch is the
# SHA-256 of its exact bytes, an uncertified build's the SHA-256 of "SEMB-UNCERTIFIED\0" followed by them.
# docs/certified-builds.json is read by the cell verdict at encoder open and never digested, so a list that
# gains cells moves neither value. Both are read here, refused by name when missing or not parseable as JSON,
# and written byte for byte into the generated header semb_certified_builds.h; the encoder never reads either
# file at run time. An edit to either re-runs configure (CMAKE_CONFIGURE_DEPENDS), so a rebuild compiles the
# files it names, and a binary built before an edit keeps the bytes it was built with (C42 (d)).
#
# The canonical bytes are the files' exact committed bytes: the repository's `* -text` attribute makes them
# identical on every checkout, so no line-ending normalization is applied.
#
# Configure also refuses a list that is not proven (item 11): tools/check_certified_builds.py --structural
# runs C42 (a) and (b) -- every cell paired with every other, every pair's rowDigest the SHA-256 of both
# cells' row-digests.tsv under their evidence paths -- and configure refuses SEMB_CERTIFIED_BUILDS_UNPROVEN on
# the checker's own REFUSED C42(a) or C42(b) lines, never on its exit status alone. Every evidence file the
# check read becomes a configure dependency.

set(SEMB_CERTIFIED_BUILDS_DOCUMENT "${CMAKE_SOURCE_DIR}/docs/certified-builds.json")
set(SEMB_CERTIFICATION_IDENTITY_DOCUMENT "${CMAKE_SOURCE_DIR}/docs/certification-identity.json")

if(NOT EXISTS "${SEMB_CERTIFIED_BUILDS_DOCUMENT}")
	message(FATAL_ERROR
		"SEMB_CERTIFIED_BUILDS_MISSING: ${SEMB_CERTIFIED_BUILDS_DOCUMENT} does not exist. The cell verdict reads "
		"it, compiled into the library, to decide whether this build is a certified cell.")
endif()
if(NOT EXISTS "${SEMB_CERTIFICATION_IDENTITY_DOCUMENT}")
	message(FATAL_ERROR
		"SEMB_CERTIFICATION_IDENTITY_MISSING: ${SEMB_CERTIFICATION_IDENTITY_DOCUMENT} does not exist. "
		"certification_epoch is derived from this file's bytes, compiled into the library; the build cannot "
		"derive the encoder fingerprint without it (tools/write_certification_identity.py writes it).")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	"${SEMB_CERTIFIED_BUILDS_DOCUMENT}" "${SEMB_CERTIFICATION_IDENTITY_DOCUMENT}")

# semb_certified_bytes(<file> <status> <out prefix>): refuses <status> when <file> is not JSON, then sets
# <out prefix>_BYTES (a C array initializer, sixteen bytes a line), <out prefix>_SIZE and <out prefix>_SHA256.
function(semb_certified_bytes _file _status _prefix)
	file(READ "${_file}" _text)
	string(JSON _type ERROR_VARIABLE _error TYPE "${_text}")
	if(NOT _error STREQUAL "NOTFOUND")
		message(FATAL_ERROR "${_status}: ${_file} is not parseable as JSON: ${_error}")
	endif()
	file(READ "${_file}" _hex HEX)
	string(LENGTH "${_hex}" _hex_len)
	math(EXPR _size "${_hex_len} / 2")
	string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
	string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n    " _bytes "${_bytes}")
	file(SHA256 "${_file}" _sha)
	set(${_prefix}_BYTES "${_bytes}" PARENT_SCOPE)
	set(${_prefix}_SIZE "${_size}" PARENT_SCOPE)
	set(${_prefix}_SHA256 "${_sha}" PARENT_SCOPE)
endfunction()

semb_certified_bytes("${SEMB_CERTIFIED_BUILDS_DOCUMENT}" SEMB_CERTIFIED_BUILDS_UNPARSEABLE SEMB_CERTIFIED_BUILDS)
semb_certified_bytes("${SEMB_CERTIFICATION_IDENTITY_DOCUMENT}" SEMB_CERTIFICATION_IDENTITY_UNPARSEABLE
	SEMB_CERTIFICATION_IDENTITY)
message(STATUS "certification identity (docs/certification-identity.json, ${SEMB_CERTIFICATION_IDENTITY_SIZE} "
	"bytes): member epoch ${SEMB_CERTIFICATION_IDENTITY_SHA256}")
message(STATUS "certified-builds list (docs/certified-builds.json, ${SEMB_CERTIFIED_BUILDS_SIZE} bytes): "
	"${SEMB_CERTIFIED_BUILDS_SHA256}")

# Item 11: the list is proven, by the checker's structural mode (one implementation of C42 (a) and (b)).
set(_semb_cb_checker "${CMAKE_SOURCE_DIR}/tools/check_certified_builds.py")
execute_process(
	COMMAND "${Python3_EXECUTABLE}" "${_semb_cb_checker}" --root "${CMAKE_SOURCE_DIR}" --structural
	RESULT_VARIABLE _semb_cb_rc
	OUTPUT_VARIABLE _semb_cb_out
	ERROR_VARIABLE _semb_cb_err)
string(REGEX MATCHALL "REFUSED C42\\([ab]\\)[^\n]*" _semb_cb_refusals "${_semb_cb_out}")
if(_semb_cb_refusals AND NOT _semb_cb_rc EQUAL 0)
	list(JOIN _semb_cb_refusals "\n" _semb_cb_lines)
	message(FATAL_ERROR "SEMB_CERTIFIED_BUILDS_UNPROVEN: ${SEMB_CERTIFIED_BUILDS_DOCUMENT} names a cell no "
		"execution proved against every other cell:\n${_semb_cb_lines}")
elseif(NOT _semb_cb_rc EQUAL 0 OR _semb_cb_refusals)
	message(FATAL_ERROR "SEMB_CERTIFIED_BUILDS_CHECK_FAILED: tools/check_certified_builds.py --structural "
		"exited ${_semb_cb_rc} without deciding the list:\n${_semb_cb_out}${_semb_cb_err}")
endif()
string(REGEX MATCHALL "READ [^\n]+" _semb_cb_read "${_semb_cb_out}")
foreach(_semb_cb_line IN LISTS _semb_cb_read)
	string(SUBSTRING "${_semb_cb_line}" 5 -1 _semb_cb_path)
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_semb_cb_path}")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_semb_cb_checker}")

configure_file(
	"${CMAKE_SOURCE_DIR}/cmake/templates/semb_certified_builds.h.in"
	"${CMAKE_BINARY_DIR}/generated/semb_certified_builds.h"
	@ONLY
)
