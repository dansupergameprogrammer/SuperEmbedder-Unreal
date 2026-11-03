#!/bin/bash
# The engine-free cores' host suite (Source/SuperEmbedderUnreal/Private/Core/): GCC, then AddressSanitizer with
# UndefinedBehaviorSanitizer, then ThreadSanitizer. The real-encode cells open SuperEmbedder's U1 synthetic
# pair through the vendored library compiled under the GCC rename header, and compare their vector digests
# with SuperEmbedder's own CMake build (ci/host/coexist/u1_pair_cmake_reference.txt).
#
#   run.sh --pair-dir DIR [--out DIR] [--only gcc|asan|tsan]
#
# --pair-dir (or SEMB_U1_PAIR_DIR): u1_pair_model.sslm, u1_pair_tokenizer.sslm and
#   u1_pair_tokenizer_no_endoftext.sslm, minted by the SuperEmbedder repository's
#   tests/fixtures/regenerate_tokenizer_pair_fixture.py at the pinned engine tag.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
PAIR=${SEMB_U1_PAIR_DIR:-}
OUT=""
ONLY=""
while [ $# -gt 0 ]; do
	case "$1" in
		--pair-dir) PAIR=$2; shift 2;;
		--out) OUT=$2; shift 2;;
		--only) ONLY=$2; shift 2;;
		*) echo "unknown argument $1"; exit 2;;
	esac
done
[ -n "$PAIR" ] && [ -f "$PAIR/u1_pair_model.sslm" ] && [ -f "$PAIR/u1_pair_tokenizer.sslm" ] || {
	echo "SEMB_UE_HOST_PAIR_MISSING: pass --pair-dir with the U1 pair"; exit 2; }
[ -n "$OUT" ] || OUT=$(mktemp -d)
mkdir -p "$OUT"
JOBS=$(nproc 2>/dev/null || echo 4)
FAILED=0
for FLAVOR in gcc asan tsan; do
	[ -n "$ONLY" ] && [ "$ONLY" != "$FLAVOR" ] && continue
	case "$FLAVOR" in
		gcc) FLAGS="-DCMAKE_BUILD_TYPE=Debug";;
		asan) FLAGS="-DCORES_SANITIZE=ON";;
		tsan) FLAGS="-DCORES_TSAN=ON";;
	esac
	echo "== $FLAVOR"
	cmake -S "$HERE" -B "$OUT/$FLAVOR" $FLAGS >"$OUT/$FLAVOR.configure.log" 2>&1 || { echo "configure failed: $OUT/$FLAVOR.configure.log"; FAILED=1; continue; }
	cmake --build "$OUT/$FLAVOR" -j "$JOBS" >"$OUT/$FLAVOR.build.log" 2>&1 || { echo "build failed: $OUT/$FLAVOR.build.log"; FAILED=1; continue; }
	"$OUT/$FLAVOR/superembedder_cores_host_tests" --pair-dir "$PAIR" >"$OUT/$FLAVOR.run.log" 2>&1
	STATUS=$?
	tail -3 "$OUT/$FLAVOR.run.log"
	[ "$STATUS" = "0" ] || { echo "$FLAVOR: exit $STATUS ($OUT/$FLAVOR.run.log)"; FAILED=1; }
done
exit $FAILED
