#!/bin/bash
# index_c_lane.sh — the kostya/index-c stress lane.
#
# index-c (github.com/kostya/index-c, MIT) is a single self-contained 16k-line
# C program: ~50 real-world tasks — JSON, base64, CSV, a neural net, three
# compressors, maze A*, graph algorithms, sorting, hashing, an interpreter,
# parallel matmul — amalgamated from the LangArena benchmark.
#
# WHY THIS LANE EXISTS, and why it is not a tests/ entry: it VERIFIES ITSELF.
# Every task computes a checksum and compares it against a known-expected
# value, prints OK or ERR[actual=..., expected=...], and the program exit(1)s
# if any task fails. So a wrong lowering surfaces as a WRONG ANSWER rather
# than a crash or a diagnostic — the silent-wrong-answer class the .expect
# suites are weakest at. One program, 50 independent oracles, no golden output
# to maintain: only the timings vary, and this lane ignores them.
#
# It is a LANE and not tests/testindexc.mad because the corpus is third-party
# (run in place, like the gcc testsuites — never vendored) and because the run
# takes minutes, which the per-test suites multiply by every execution lane.
#
# -D_DEFAULT_SOURCE is REQUIRED and is not a workaround: the program calls
# clock_gettime(CLOCK_MONOTONIC), and glibc hides the POSIX clock constants
# under strict -std=c17. `gcc -std=c17` fails on the identical two lines;
# the project's own build line uses the gnu default. When madc gains
# --std=gnu* (claude_status UPDATE 76 NEXT 4) this becomes that flag.
#
# OPTIMIZATION LEVEL: the lane runs -O2, which is both FASTER and STRICTER
# than the default. Measured on this corpus (task time, all 50/50 correct in
# every configuration): gcc -O0 118.0s, madc default 104.0s, madc -O2 85.0s,
# madc -O3 85.2s (identical -- MIR's generator tops out at level 2), gcc -O3
# 51.8s. So madc's default already beats gcc -O0, and -O2 costs the lane less
# wall time while putting the optimizer's own passes under the 50 checksums.
# The unoptimized path is what the 1500-test suite exercises all day; the
# optimizer is the part with less coverage, so this is where it earns its time.
#
#   MADC_BIN           binary under test (default bin/madc)
#   MADC_INDEX_C_DIR   the checkout (default /workspace/index-c)
#   MADC_INDEX_C_ARGS  compile flags (default -O2; set empty for the default JIT level)
set -u
cd "$(dirname "$0")/.."

BIN="${MADC_BIN:-bin/madc}"
DIR="${MADC_INDEX_C_DIR:-/workspace/index-c}"
SRC="$DIR/index.c"

if [ ! -f "$SRC" ]; then
	echo "index_c_lane: no checkout at $DIR (clone" \
	     "github.com/kostya/index-c there)" >&2
	exit 1
fi
if [ ! -x "$BIN" ]; then
	echo "index_c_lane: $BIN missing — build first" >&2
	exit 1
fi

ARGS="${MADC_INDEX_C_ARGS--O2}"
out=$("$BIN" --std=c17 -D_DEFAULT_SOURCE $ARGS "$SRC" 2>&1)
rc=$?

# The summary line is the whole verdict: "Summary: <secs>s, <total>, <ok>, <fails>".
summary=$(printf '%s\n' "$out" | grep '^Summary:' | tail -1)
fails=$(printf '%s\n' "$summary" | sed -n 's/.*, *\([0-9][0-9]*\) *$/\1/p')
total=$(printf '%s\n' "$summary" | sed -n 's/^Summary:[^,]*, *\([0-9][0-9]*\),.*/\1/p')
ok=$(printf '%s\n' "$summary" | sed -n 's/^Summary:[^,]*, *[0-9][0-9]*, *\([0-9][0-9]*\),.*/\1/p')

if [ -z "$summary" ]; then
	echo "index_c_lane: RED — no Summary line (rc=$rc); the program did not finish"
	printf '%s\n' "$out" | grep -iE 'error|ERR\[' | head -20
	exit 1
fi

# A task that computed the wrong answer names itself; show every one.
printf '%s\n' "$out" | grep 'ERR\[' | head -20

echo "index-c: $ok/$total tasks OK, $fails failed (rc=$rc)${ARGS:+ [$ARGS]} — $summary"

if [ "$rc" -ne 0 ] || [ "${fails:-1}" != "0" ] || [ "${ok:-0}" != "${total:-1}" ]; then
	echo "index_c_lane: RED — a task produced the wrong checksum"
	exit 1
fi
echo "index_c_lane: GREEN — all $total tasks verified"
