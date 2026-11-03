#!/bin/bash
# GCC host pre-checks for SuperEmbedderCore's own-copy engine (the proposal's section 3.1; cells R-1, R-1b,
# R-7, and the host pre-check legs of R-8/R-9's link-and-run). Reads the plugin tree and the SuperSLM
# plugin's vendored tree; writes only under the output directory. R-8 and R-9's grown-struct runs (a re-vendor
# that grows sslm_schema_s or sslm_kv_pool_s) are grown/run_grown.py, which runs the same way with GCC or MSVC.
#
#   run.sh --pair-dir DIR [--out DIR] [--opts "-O0 -O2"] [--asan]
#
# --pair-dir: SuperEmbedder's U1 synthetic model + tokenizer pair (u1_pair_model.sslm,
#   u1_pair_tokenizer.sslm), minted by the SuperEmbedder repository's
#   tests/fixtures/regenerate_tokenizer_pair_fixture.py at the pinned engine tag. Also read from
#   SEMB_U1_PAIR_DIR.
#
# At each optimization level it builds, with the same flags:
#   P  the SuperSLM plugin's vendored engine (Plugins/SuperSLMUnreal, or SUPERSLM_PLUGIN_TREE), its core sources, plain
#   R  every SuperEmbedderCore vendored TU (21 engine + 16 SuperEmbedder wrappers) under the GCC rename header
#   N  the same engine wrappers (and the identity verb) plain: the same sources, which catches a missed name
#   S  the SuperEmbedder wrappers plain, for the plain reference binary (N + S, no rename)
# and checks: R-1 (R vs P, R vs N), the body check (R vs P, R vs N), one image linking P and R as static
# archives in both orders (zero multiple definitions), a control link of P and the plain N (which must
# collide), both coexist binaries run, and the semb.h smoke's vector digests equal the plain reference's.
set -u
export PYTHONDONTWRITEBYTECODE=1
HERE=$(cd "$(dirname "$0")" && pwd)
PLUG=$(cd "$HERE/../../.." && pwd)
SLMP=${SUPERSLM_PLUGIN_TREE:-$(cd "$PLUG/../SuperSLMUnreal/Source/ThirdParty/SuperSLM" && pwd)}
PAIR=${SEMB_U1_PAIR_DIR:-}
OUT=""
OPTS="-O0 -O2"
SAN=""
while [ $# -gt 0 ]; do
	case "$1" in
		--pair-dir) PAIR=$2; shift 2;;
		--out) OUT=$2; shift 2;;
		--opts) OPTS=$2; shift 2;;
		--asan) SAN="-fsanitize=address"; shift;;
		*) echo "unknown argument $1"; exit 2;;
	esac
done
[ -n "$PAIR" ] && [ -f "$PAIR/u1_pair_model.sslm" ] && [ -f "$PAIR/u1_pair_tokenizer.sslm" ] || {
	echo "SEMB_UE_HOST_PAIR_MISSING: pass --pair-dir with u1_pair_model.sslm and u1_pair_tokenizer.sslm"; exit 2; }
OUT=${OUT:-$(mktemp -d)}
mkdir -p "$OUT"
CXX=${CXX:-g++}
JOBS=$(nproc 2>/dev/null || echo 4)
TP=$PLUG/Source/ThirdParty
GEN=$PLUG/Source/SuperEmbedderCore/Private/Generated
VEND=$PLUG/Source/SuperEmbedderCore/Private/Vendored
HDR=$GEN/SuperEmbedderCoreRename.gcc.h
NAMES=$PLUG/ci/rename/gcc.names
CONFLICTS=$PLUG/ci/rename/gcc.conflicts
fail=0
step() { echo; echo "== $*"; }
check() { "$@" || { echo "  FAILED: $*"; fail=$((fail + 1)); }; }

step "Generated files and vendored trees are current"
check python3 "$PLUG/ci/check_vendored_tree_matches_manifest.py" --plugin-root "$PLUG"
check python3 "$PLUG/ci/check_vendored_wrappers_complete.py" --plugin-root "$PLUG"
check python3 "$PLUG/ci/generate_semb_config.py" --plugin-root "$PLUG" --check
check python3 "$PLUG/ci/generate_rename_header.py" --plugin-root "$PLUG" --preprocessor gcc --cxx "$CXX" --check
echo "  rename list: $(wc -l < "$NAMES") names; conflict set: $(wc -l < "$CONFLICTS")"

step "R-7: the header changes no text outside the vendored trees"
check python3 "$HERE/check_r7.py" "$PLUG" "$HDR" --cxx "$CXX"

PSRCS=$(sed -n '/set(SUPERSLM_CORE_SOURCES/,/)/p' "$SLMP/CMakeLists.txt" | grep -oE 'src/[^ )]+\.cpp')
SE_INC="-I$TP/SuperSLM/include -I$TP/SuperEmbedder/include -I$GEN"
SE_DEF=(-DSEMB_BUILD_CONFIGURATION=\"ubt-host\" -DSEMB_BUILD_SYSTEM=\"ubt\")

compile_all() { # outdir flags... -- files...
	local out=$1; shift; local flags=(); while [ "$1" != "--" ]; do flags+=("$1"); shift; done; shift
	mkdir -p "$out"
	printf '%s\n' "$@" | xargs -P "$JOBS" -I{} sh -c 'f="$1"; shift; n=$(basename "$f" .cpp); "$@" -c "$f" -o "'"$out"'/$n.o" 2>"'"$out"'/$n.err" || { echo "  COMPILE FAILED $f"; head -5 "'"$out"'/$n.err"; }' _ {} "$CXX" "${flags[@]}"
}

for OPT in $OPTS; do
	O=$OUT/gcc$OPT${SAN:+-asan}
	rm -rf "$O"; mkdir -p "$O"
	FL=(-std=c++20 "$OPT" -fPIC -ffp-contract=off $SAN)
	step "[$OPT${SAN:+ asan}] compile P (the SuperSLM plugin's engine, plain), R (renamed), N and S (plain)"
	compile_all "$O/P" "${FL[@]}" -I"$SLMP/include" -- $(for s in $PSRCS; do echo "$SLMP/$s"; done)
	compile_all "$O/R" "${FL[@]}" -include "$HDR" $SE_INC "${SE_DEF[@]}" -- "$VEND"/*.cpp
	compile_all "$O/N" "${FL[@]}" $SE_INC "${SE_DEF[@]}" -- "$VEND"/SuperEmbedderVendoredEngine_*.cpp "$VEND"/SuperEmbedderVendored_build_identity.cpp
	compile_all "$O/S" "${FL[@]}" $SE_INC "${SE_DEF[@]}" -- $(ls "$VEND"/SuperEmbedderVendored_*.cpp | grep -v _build_identity.cpp)
	echo "  objects: P $(ls "$O"/P/*.o | wc -l), R $(ls "$O"/R/*.o | wc -l), N $(ls "$O"/N/*.o | wc -l), S $(ls "$O"/S/*.o | wc -l)"

	step "[$OPT] R-1: renamed copy vs the SuperSLM plugin's release"
	check python3 "$HERE/check_symbols.py" "$O/R" "$O/P" "$NAMES" "$CONFLICTS"
	step "[$OPT] R-1: renamed copy vs the same sources plain"
	check python3 "$HERE/check_symbols.py" "$O/R" "$O/N" "$NAMES" "$CONFLICTS"
	step "[$OPT] R-1b: body check, renamed copy vs the SuperSLM plugin's release"
	check python3 "$HERE/bodycmp.py" "$O/R" "$O/P"
	step "[$OPT] R-1b: body check, renamed copy vs the same sources plain"
	check python3 "$HERE/bodycmp.py" "$O/R" "$O/N"

	step "[$OPT] one image: both copies as static archives, both link orders"
	"$CXX" "${FL[@]}" -c -DDRV_NAME=drv_plugin -I"$SLMP/include" "$HERE/drv.cpp" -o "$O/drv_p.o"
	"$CXX" "${FL[@]}" -c -DDRV_NAME=drv_embedder -include "$HDR" -I"$TP/SuperSLM/include" "$HERE/drv.cpp" -o "$O/drv_r.o"
	"$CXX" "${FL[@]}" -c -I"$TP/SuperEmbedder/include" "$HERE/main.cpp" -o "$O/main.o"
	"$CXX" "${FL[@]}" -c -I"$TP/SuperEmbedder/include" "$HERE/ref_main.cpp" -o "$O/ref_main.o"
	rm -f "$O"/*.a
	ar rcs "$O/libsslm_plugin.a" "$O"/P/*.o "$O/drv_p.o"
	ar rcs "$O/libsemb_owncopy.a" "$O"/R/*.o "$O/drv_r.o"
	ar rcs "$O/libsemb_plain_engine.a" "$O"/N/*.o
	WA=-Wl,--whole-archive; NWA=-Wl,--no-whole-archive
	"$CXX" $SAN -o "$O/coexist_pr" "$O/main.o" $WA "$O/libsslm_plugin.a" "$O/libsemb_owncopy.a" $NWA -lpthread 2> "$O/link_pr.txt"
	rc_pr=$?; dup_pr=$(grep -c 'multiple definition' "$O/link_pr.txt")
	"$CXX" $SAN -o "$O/coexist_rp" "$O/main.o" $WA "$O/libsemb_owncopy.a" "$O/libsslm_plugin.a" $NWA -lpthread 2> "$O/link_rp.txt"
	rc_rp=$?; dup_rp=$(grep -c 'multiple definition' "$O/link_rp.txt")
	echo "  link, SuperSLM plugin first: exit $rc_pr, multiple definitions $dup_pr"
	echo "  link, SuperEmbedderCore first: exit $rc_rp, multiple definitions $dup_rp"
	[ $rc_pr -eq 0 ] && [ $dup_pr -eq 0 ] && [ $rc_rp -eq 0 ] && [ $dup_rp -eq 0 ] || { echo "  FAILED: link"; fail=$((fail + 1)); }
	"$CXX" $SAN -o "$O/control" "$O/main.o" $WA "$O/libsslm_plugin.a" "$O/libsemb_plain_engine.a" $NWA -lpthread 2> "$O/link_control.txt"
	dup_c=$(grep -c 'multiple definition' "$O/link_control.txt")
	echo "  control, the two engine copies with NO rename: multiple definitions $dup_c (must be > 0)"
	[ "$dup_c" -gt 0 ] || { echo "  FAILED: the control link did not collide, so the link legs prove nothing"; fail=$((fail + 1)); }

	step "[$OPT] run: both coexist binaries, and the plain reference (no rename, alone)"
	"$CXX" $SAN -o "$O/reference" "$O/ref_main.o" "$O"/S/*.o "$O"/N/*.o -lpthread 2> "$O/link_ref.txt" || { echo "  FAILED: reference link"; cat "$O/link_ref.txt" | head; fail=$((fail + 1)); }
	for b in coexist_pr coexist_rp reference; do
		ASAN_OPTIONS=detect_leaks=0 "$O/$b" "$PAIR/u1_pair_model.sslm" "$PAIR/u1_pair_tokenizer.sslm" > "$O/run_$b.txt" 2>&1
		rc=$?; echo "  $b: exit $rc"; grep -vE '^preflight:|^digest' "$O/run_$b.txt" | sed 's/^/    /'
		[ $rc -eq 0 ] || fail=$((fail + 1))
		grep -E '^(digest|fingerprint|dims)' "$O/run_$b.txt" > "$O/digests_$b.txt"
	done
	n=$(grep -c '^digest' "$O/digests_reference.txt")
	if [ "$n" -gt 0 ] && cmp -s "$O/digests_coexist_pr.txt" "$O/digests_reference.txt" && cmp -s "$O/digests_coexist_rp.txt" "$O/digests_reference.txt"; then
		echo "  smoke: $n vector digests and the fingerprint equal the plain reference's, both link orders"
		sed 's/^/    /' "$O/digests_reference.txt"
	else
		echo "  FAILED: smoke digests differ from the plain reference"; diff "$O/digests_coexist_pr.txt" "$O/digests_reference.txt" | head; fail=$((fail + 1))
	fi
	cp "$O/digests_reference.txt" "$OUT/digests$OPT${SAN:+-asan}.txt"
	REFF=$HERE/u1_pair_cmake_reference.txt
	if grep -q "^pair u1_pair_model.sslm $(sha256sum "$PAIR/u1_pair_model.sslm" | cut -d' ' -f1)\$" "$REFF" &&
	   grep -q "^pair u1_pair_tokenizer.sslm $(sha256sum "$PAIR/u1_pair_tokenizer.sslm" | cut -d' ' -f1)\$" "$REFF"; then
		if grep -E '^(digest|fingerprint|dims)' "$REFF" | cmp -s - "$O/digests_coexist_pr.txt"; then
			echo "  smoke: equal to SuperEmbedder's own CMake build (u1_pair_cmake_reference.txt)"
		else
			echo "  FAILED: smoke digests differ from SuperEmbedder's own CMake build"; fail=$((fail + 1))
		fi
	else
		echo "  smoke: this pair is not the one u1_pair_cmake_reference.txt was recorded on; CMake comparison skipped"
	fi
done

echo
if [ $fail -eq 0 ]; then echo "host pre-checks: PASS (output in $OUT)"; else echo "host pre-checks: $fail FAILED (output in $OUT)"; fi
exit $([ $fail -eq 0 ] && echo 0 || echo 1)
