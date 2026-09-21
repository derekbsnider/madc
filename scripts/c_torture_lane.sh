#!/bin/bash
# c_torture_lane.sh — the gcc.c-torture execute lane, a RATCHET.
#
# Drives gcc's own gcc.c-torture/execute set through bin/madc and compares the
# failing set against docs/parity/c-torture-baseline.txt:
#   RED   when a test OUTSIDE the baseline fails (a regression)
#   LOUD  when a test IN the baseline passes (shrink the baseline)
# so the baseline only ever shrinks and never silently pads.
#
# WHY THIS EXISTS (2026-09-20): torture had no lane script and no ledger row.
# Nothing re-ran it between 2026-08-12 and 2026-09-20, and it drifted from 1614
# passing to 1587 while every other lane stayed green — three standard-C
# regressions, one a SIGSEGV in the parser, carried silently for five weeks.
# The run takes about four minutes. It belongs in the FAST tier that runs after
# every commit (scripts/fast_lanes.sh), not in the multi-hour merge gate.
#
# The test loop lives in scripts/run_gcc_testsuite.py (--suite c-torture) —
# this lane runs that ONE runner and ratchets its output. Never reimplement
# the loop here.
#
#   MADC_BIN               binary under test (default bin/madc)
#   MADC_TORTURE_ROOT      testsuite root (default /workspace/gcc_testsuite; the
#                          repo's gcc_testsuite symlink DANGLES on the container)
#   MADC_TORTURE_BASELINE  baseline file
#   MADC_TORTURE_STD       --std= handed to madc (default c17: torture is C-era
#                          code and K&R / implicit-int recovery only exists
#                          under --std=c78..c17)
#   MADC_TORTURE_OUT       raw runner output (kept for triage)
#   MADC_TORTURE_REUSE=1   ratchet an EXISTING MADC_TORTURE_OUT instead of
#                          running the suite again (triage, and the ratchet's
#                          own controls)
set -u
cd "$(dirname "$0")/.."

BIN="${MADC_BIN:-bin/madc}"
ROOT="${MADC_TORTURE_ROOT:-/workspace/gcc_testsuite}"
BASE="${MADC_TORTURE_BASELINE:-docs/parity/c-torture-baseline.txt}"
STD="${MADC_TORTURE_STD:-c17}"
OUT="${MADC_TORTURE_OUT:-tmp/torture/lane.txt}"

if [ ! -d "$ROOT/gcc.c-torture/execute" ]; then
	echo "c_torture_lane: no gcc.c-torture/execute under $ROOT" >&2
	exit 1
fi
if [ ! -x "$BIN" ]; then
	echo "c_torture_lane: $BIN missing — build first" >&2
	exit 1
fi

mkdir -p "$(dirname "$OUT")" tmp/torture
if [ "${MADC_TORTURE_REUSE:-0}" = "1" ]; then
	if [ ! -f "$OUT" ]; then
		echo "c_torture_lane: MADC_TORTURE_REUSE=1 but $OUT does not exist" >&2
		exit 1
	fi
	echo "c-torture: REUSING $OUT (no suite run)"
else
	python3 scripts/run_gcc_testsuite.py --root "$ROOT" --suite c-torture \
		--madc "$BIN" --std "$STD" > "$OUT" 2>&1
fi
# The runner exits nonzero whenever anything failed, which is the normal state
# while the baseline is non-empty. The ratchet below is the verdict, not that
# status — and a run that DIED mid-way prints counts that look like a
# measurement, so the summary line is checked first.
summary=$(grep -c 'passed,' "$OUT")
if [ "$summary" -eq 0 ]; then
	echo "c_torture_lane: runner produced no summary line — it died mid-run; see $OUT" >&2
	exit 1
fi

strip="s|^[^:]*: ||; s| (.*||; s|.*/||"
grep -E '^(FAIL|TIMEOUT)' "$OUT" | sed "$strip" | sort > tmp/torture/.fail.$$
grep -v '^[[:space:]]*#' "$BASE" 2>/dev/null | grep -v '^[[:space:]]*$' | sort > tmp/torture/.base.$$

comm -13 tmp/torture/.base.$$ tmp/torture/.fail.$$ > tmp/torture/.newfail.$$
comm -23 tmp/torture/.base.$$ tmp/torture/.fail.$$ > tmp/torture/.fixed.$$

nfail=$(wc -l < tmp/torture/.fail.$$ 2>/dev/null)
nnew=$(wc -l < tmp/torture/.newfail.$$ 2>/dev/null)
nfixed=$(wc -l < tmp/torture/.fixed.$$ 2>/dev/null)
# An EMPTY count means the comm/sed pipeline above did not produce its files —
# a broken ratchet, not a clean lane. Without this the `-gt` tests below error
# into "false" and the lane exits 0: a dead gate that reads GREEN, which is the
# exact failure this lane was created to stop.
case "$nfail$nnew$nfixed" in
	*[!0-9]*|"")
		echo "c_torture_lane: ratchet produced no counts (fail=[$nfail]" \
		     "new=[$nnew] fixed=[$nfixed]) — the compare pipeline broke" >&2
		exit 1;;
esac

echo "c-torture: $(grep 'passed,' "$OUT" | tail -1) (--std=$STD)"
echo "c-torture: $nfail failing, $nnew outside baseline, $nfixed baseline tests now passing"
if [ "$nfixed" -gt 0 ]; then
	echo "c-torture: SHRINK THE BASELINE — now passing:"
	sed 's|^|  |' tmp/torture/.fixed.$$
fi
if [ "$nnew" -gt 0 ]; then
	echo "c-torture: RED — non-baseline failure(s):" >&2
	sed 's|^|  |' tmp/torture/.newfail.$$ >&2
	rm -f tmp/torture/.fail.$$ tmp/torture/.base.$$ tmp/torture/.newfail.$$ tmp/torture/.fixed.$$
	exit 1
fi
rm -f tmp/torture/.fail.$$ tmp/torture/.base.$$ tmp/torture/.newfail.$$ tmp/torture/.fixed.$$
exit 0
