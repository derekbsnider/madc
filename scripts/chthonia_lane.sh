#!/bin/bash
# chthonia_lane.sh — Chthonia built and tested as its own repository builds
# it (docs/plans/2026-10-04-chthonia-own-repository.md §6 step 2): its build
# script and its tests (tools/chthonia/scripts/), against libmadcide and
# madcide's public headers, never the base's sources.
#
#   scripts/chthonia_lane.sh
#       the madc under test (MADC_BIN, default bin/madc): libmadcide built by
#       it into its own lib directory, where `-lmadcide` and chthonia's
#       "libs" find it, and the tree's public headers (tools/madcide/include)
#   scripts/chthonia_lane.sh --installed TARBALL
#       an INSTALLED madc: the relocatable tarball package_release.sh writes,
#       unpacked, with its own libmadcide and share/madcide/include — what
#       Chthonia's CI installs
#
# Steps: build chthonia (scripts/build.sh), run it (--help prints its usage),
# run the console tests, then the window tests under xvfb-run when the host
# has it (the tree form builds libmadcwebview first). One summary line:
#   chthonia: build ok | tests: N passed, 0 failed | gui: M passed, 0 failed
# Record it with scripts/lane_ledger.sh record chthonia "<line>".
set -u
cd "$(dirname "$0")/.." || exit 2
mode=tree
if [ "${1:-}" = "--installed" ]; then
	mode=installed
	tarball=${2:-}
	if [ ! -f "$tarball" ]; then
		echo "usage: $0 [--installed TARBALL]" >&2
		exit 2
	fi
fi
work=$PWD/tmp/chthonia-lane
rm -rf "$work"
mkdir -p "$work" tmp/logs
log=$PWD/tmp/logs/chthonia-lane-$(date +%Y%m%d-%H%M%S).log
case "$(uname -s)" in
Darwin) so=dylib; libpath=DYLD_LIBRARY_PATH ;;
*) so=so; libpath=LD_LIBRARY_PATH ;;
esac
fail() {
	echo "chthonia: FAILED — $1 (log $log)"
	tail -20 "$log" >&2
	exit 1
}

if [ $mode = tree ]; then
	bin=${MADC_BIN:-bin/madc}
	[ -x "$bin" ] || fail "$bin missing — build first"
	madc=$(cd "$(dirname "$bin")" && pwd)/$(basename "$bin")
	inc=$PWD/tools/madcide/include
	libdir=$(cd "$(dirname "$madc")/../lib" && pwd)
	echo "== libmadcide (by $bin) ==" >> "$log"
	# An absolute source path: the base finds its data through __FILE__,
	# which spells the path as given (gcc's rule), and the tests run from
	# tools/chthonia.
	( ulimit -t 900; timeout 900 "$madc" -shared -o "$libdir/libmadcide.$so" \
		"$PWD/tools/madcide/madcide_base.mad" ) >> "$log" 2>&1 || fail "libmadcide did not build"
else
	tar -xf "$tarball" -C "$work" || fail "cannot unpack $tarball"
	madc=$(ls -d "$work"/*/bin/madc 2>/dev/null | head -1)
	[ -x "$madc" ] || fail "no bin/madc in $tarball"
	inc=$(cd "$(dirname "$madc")/.." && pwd)/share/madcide/include
	libdir=$(cd "$(dirname "$madc")/../lib" && pwd)
fi

echo "== build ($madc) ==" >> "$log"
MADC="$madc" MADCIDE_INCLUDE="$inc" bash tools/chthonia/scripts/build.sh -o "$work/chthonia" \
	>> "$log" 2>&1 || fail "chthonia did not build"
help=$(env "$libpath=$libdir" timeout 60 "$work/chthonia" --help 2>&1)
case "$help" in
*"usage: chthonia"*) ;;
*) echo "$help" >> "$log"; fail "chthonia --help printed no usage line" ;;
esac

echo "== tests ==" >> "$log"
MADC="$madc" MADCIDE_INCLUDE="$inc" bash tools/chthonia/scripts/run_tests.sh >> "$log" 2>&1
trc=$?
tests=$(grep -E '^[0-9]+ passed, [0-9]+ failed$' "$log" | tail -1)

gui="not run (no xvfb-run)"
grc=0
if command -v xvfb-run > /dev/null 2>&1; then
	if [ $mode = tree ]; then
		make -C src libmadcwebview >> "$log" 2>&1 || fail "libmadcwebview did not build"
	fi
	echo "== gui ==" >> "$log"
	MADC="$madc" MADCIDE_INCLUDE="$inc" timeout -k 3 600 xvfb-run -a \
		bash tools/chthonia/scripts/run_tests.sh --gui >> "$log" 2>&1
	grc=$?
	gui=$(grep -E '^[0-9]+ passed, [0-9]+ failed$' "$log" | tail -1)
fi
echo "chthonia: build ok | tests: ${tests:-(no summary)} | gui: ${gui:-(no summary)}"
[ $trc -eq 0 ] && [ $grc -eq 0 ] || { tail -40 "$log" >&2; exit 1; }
