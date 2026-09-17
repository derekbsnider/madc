#!/bin/bash
# gxx_lane.sh — the C++11 conformance lane (ROADMAP 2.12), a RATCHET.
#
# Drives gcc's own g++.dg compile-clean tests through bin/madc and compares the
# failing set against docs/parity/gxx-c++11-baseline.txt:
#   RED   when a test OUTSIDE the baseline fails (a regression)
#   LOUD  when a test IN the baseline passes (shrink the baseline)
# so the baseline only ever shrinks and never silently pads.
#
# NOT a gate. Owner ruling 2026-09-04: C++11 parity is a ROADMAP GOAL that
# blocks no push and no release. Ledger row gxx-c++11 carries promote=no.
#
# The test loop lives in scripts/run_gcc_testsuite.py (--suite gxx) — this lane
# runs that ONE runner and ratchets its output. Never reimplement the loop here.
#
#   MADC_BIN           binary under test (default bin/madc)
#   MADC_GXX_ROOT      testsuite root (default /workspace/gcc_testsuite; the
#                      repo's gcc_testsuite symlink DANGLES on the container)
#   MADC_GXX_BASELINE  baseline file
#   MADC_GXX_OUT       raw runner output (kept for triage)
#   MADC_GXX_REUSE=1   ratchet an EXISTING MADC_GXX_OUT instead of running
#                      the suite again (triage, and the ratchet's own controls)
set -u
cd "$(dirname "$0")/.."

BIN="${MADC_BIN:-bin/madc}"
ROOT="${MADC_GXX_ROOT:-/workspace/gcc_testsuite}"
BASE="${MADC_GXX_BASELINE:-docs/parity/gxx-c++11-baseline.txt}"
OUT="${MADC_GXX_OUT:-tmp/gxx/lane.txt}"

if [ ! -d "$ROOT/g++.dg" ]; then
	echo "gxx_lane: no g++.dg under $ROOT — rsync it from /workspace/gcc/gcc/testsuite" >&2
	exit 1
fi
if [ ! -x "$BIN" ]; then
	echo "gxx_lane: $BIN missing — build first" >&2
	exit 1
fi

mkdir -p "$(dirname "$OUT")" tmp/gxx
if [ "${MADC_GXX_REUSE:-0}" = "1" ]; then
	if [ ! -f "$OUT" ]; then
		echo "gxx_lane: MADC_GXX_REUSE=1 but $OUT does not exist" >&2
		exit 1
	fi
	echo "gxx-c++11: REUSING $OUT (no suite run)"
else
	python3 scripts/run_gcc_testsuite.py --root "$ROOT" --suite gxx --madc "$BIN" > "$OUT" 2>&1
fi
# The runner exits 1 whenever anything failed, which is the normal state while
# the baseline is non-empty. The ratchet below is the verdict, not that status.

summary=$(grep -c 'passed,' "$OUT")
if [ "$summary" -eq 0 ]; then
	echo "gxx_lane: runner produced no summary line — it died mid-run; see $OUT" >&2
	exit 1
fi

strip="s|^[^:]*: ||; s| (.*||; s|^$ROOT/||"
grep -E '^(FAIL\(|TIMEOUT:)' "$OUT" | sed "$strip" | sort > tmp/gxx/.fail.$$
grep -E '^SKIP:'              "$OUT" | sed "$strip" | sort > tmp/gxx/.skip.$$
grep -v '^[[:space:]]*#' "$BASE" | grep -v '^[[:space:]]*$' | sort > tmp/gxx/.base.$$

# A baseline test that neither failed nor was skipped this run now PASSES.
comm -13 tmp/gxx/.base.$$ tmp/gxx/.fail.$$ > tmp/gxx/.newfail.$$
comm -23 tmp/gxx/.base.$$ tmp/gxx/.fail.$$ | comm -23 - tmp/gxx/.skip.$$ > tmp/gxx/.fixed.$$

nfail=$(wc -l < tmp/gxx/.fail.$$)
nnew=$(wc -l < tmp/gxx/.newfail.$$)
nfixed=$(wc -l < tmp/gxx/.fixed.$$)

echo "gxx-c++11: $(grep 'passed,' "$OUT" | tail -1)"
echo "gxx-c++11: $nfail failing, $nnew outside baseline, $nfixed baseline tests now passing"
if [ "$nfixed" -gt 0 ]; then
	echo "gxx-c++11: SHRINK THE BASELINE — now passing:"
	sed 's|^|  |' tmp/gxx/.fixed.$$
fi
if [ "$nnew" -gt 0 ]; then
	echo "gxx-c++11: RED — non-baseline failure(s):" >&2
	sed 's|^|  |' tmp/gxx/.newfail.$$ >&2
	rm -f tmp/gxx/.fail.$$ tmp/gxx/.skip.$$ tmp/gxx/.base.$$ tmp/gxx/.newfail.$$ tmp/gxx/.fixed.$$
	exit 1
fi
rm -f tmp/gxx/.fail.$$ tmp/gxx/.skip.$$ tmp/gxx/.base.$$ tmp/gxx/.newfail.$$ tmp/gxx/.fixed.$$
exit 0
