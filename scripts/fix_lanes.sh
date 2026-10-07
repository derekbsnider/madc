#!/bin/bash
# fix_lanes.sh — the PER-FIX gate: Tier 1, then Tier 2, and nothing else.
#
#   Tier 1  the fix's tests + the touched subsystem's neighbours, JIT + exe +
#           obj in one run (scripts/run_tests.sh --exe --obj <globs>)
#   Tier 2  scripts/fast_lanes.sh: the seven conformance lanes, under three
#           minutes
#
# About five minutes in all. The BATCH (scripts/batch_lane.sh, the whole tests/
# suite) runs ONCE per batch of fixes, and the packed and platform lanes at the
# seam, never per fix (owner 2026-10-01: "we certainly cannot be running a 30+
# minute barrage of tests for each individual bug fix"). This script makes the
# per-fix run ONE named command: chaining the stages by hand is how the batch
# and the packed suite crept into every fix of a bug run.
#
# Usage: bash scripts/fix_lanes.sh '<glob>' [glob...]
#        basename globs, as scripts/run_tests.sh takes them; at least one.
#   MADC_BIN               binary under test (default bin/madc)
#   MADC_FAST_NO_RECORD=1  passed through to fast_lanes.sh (remote_build.sh's
#                          `fix` stage: a row written on the rsync copy gates
#                          nothing; record the printed tallies on the NAS)
set -u
cd "$(dirname "$0")/.."

if [ "$#" -eq 0 ]; then
	echo "usage: fix_lanes.sh '<glob>' [glob...] — Tier 1 needs the fix's tests" >&2
	exit 2
fi
BIN="${MADC_BIN:-bin/madc}"
if [ ! -x "$BIN" ]; then
	echo "fix_lanes: $BIN missing — build first" >&2
	exit 1
fi

mkdir -p tmp/logs
stamp=$(date +%Y%m%d-%H%M%S)
t1log=tmp/logs/fix-tier1-$stamp.log
t2log=tmp/logs/fix-tier2-$stamp.log
echo "=== fix lanes ($(git rev-parse --short HEAD 2>/dev/null), $(date -u +%FT%TZ)) ==="

t0=$(date +%s)
bash scripts/run_tests.sh --exe --obj "$@" > "$t1log" 2>&1
rc1=$?
t1=$(date +%s)
grep -E '^(FAIL|TIMEOUT|LEAK|NOISY)' "$t1log" >&2
jit=$(grep -E '^[0-9]+ passed, [0-9]+ failed, ' "$t1log" | tail -1)
exe=$(grep -E '^EXE: ' "$t1log" | tail -1)
obj=$(grep -E '^OBJ: ' "$t1log" | tail -1)
[ -z "$jit" ] && jit="(no summary — see $t1log)"
# A Tier 1 that matched no test is a wrong glob, never a green fix.
if echo "$jit" | grep -qE '^0 passed, 0 failed'; then
	echo "fix_lanes: Tier 1 matched no tests — check the globs: $*" >&2
	rc1=1
fi
printf 'tier1        rc=%-2s %4ss  %s | %s | %s\n' "$rc1" "$((t1 - t0))" \
	"$jit" "$exe" "$obj"
# A red Tier 1 means the fix is not done: stop before spending Tier 2 on it.
if [ "$rc1" -ne 0 ]; then
	echo "fix_lanes: RED — Tier 1 rc=$rc1 (log $t1log); Tier 2 not run" >&2
	exit 1
fi

( ulimit -t 3600; timeout 2400 bash scripts/fast_lanes.sh ) > "$t2log" 2>&1
rc2=$?
t2=$(date +%s)
grep -E '^[a-z0-9+-]+ +rc=' "$t2log"
printf 'tier2        rc=%-2s %4ss  (log %s)\n' "$rc2" "$((t2 - t1))" "$t2log"

if [ "$rc2" -ne 0 ]; then
	echo "fix_lanes: RED — Tier 2 rc=$rc2 (log $t2log)" >&2
	exit 1
fi
echo "fix_lanes: GREEN — next: commit; the batch runs once per batch of fixes"
exit 0
