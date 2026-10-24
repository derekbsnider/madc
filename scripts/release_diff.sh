#!/bin/bash
# release_diff.sh — the release candidate against the PREVIOUS RELEASE, side by
# side on one machine (owner 2026-10-24: "more comparison to the previous
# release to ensure no regressions"). v0.106.0 shipped two regressions that a
# comparison with v0.105.0 would have shown, and its speed movers were chased
# by hand for days; this is that comparison as one command, run FIRST in a
# release arc, before the long suites.
#
# A = the previous release's archive entry (scripts/release_bins.sh: the
#     newest archived release at or below B's version whose code differs from
#     this tree), B = this tree's madc-release.
#
# Stages, cheapest first (default: all but packages; packages runs when both
# asset directories are given):
#   startup   madc --version, a JIT hello, a hello image — per run
#   exec      the execution benchmarks (benchmark_lane.sh's corpora): outputs
#             compared, best-of-N interleaved; a row slower than 3% is a MOVER
#             and is SHIFT-TESTED — rebuilt at 5 code placements (padding
#             functions ahead of the source) and averaged: still slower is a
#             REAL regression (RED), level is placement (listed)
#   compile   compile wall time: the benchmarks -c and -o, the parse-cost C/C++
#             workloads -c, the dialect workloads JIT
#   memory    peak RSS of the same compiles (images flushed first)
#   cost      callgrind Ir of the parse-cost live workloads on A and on B
#             (parse_cost_gate.sh --map on each binary)
#   tests     the whole tests/ suite, JIT, on A and on B through run_tests.sh
#             (--report=json + MADC_TEST_SAVE_DIR): a test A passes and B fails
#             is RED; a test whose stderr was empty under A and is not under B
#             is RED (v0.106.1's Windows noise); stdout and stderr changes listed
#   packages  every package of A's release assets against B's: a missing
#             package or a missing member is RED; sizes listed
#
#   bash scripts/release_diff.sh [--prev=vX.Y.Z] [--prev-assets=DIR
#        --cand-assets=DIR] [stage...]
#   MADC_BIN          B (default bin/madc-release)
#   RELEASE_DIFF_N    timing rounds per row (default 5)
#   RELEASE_DIFF_ONLY / RELEASE_DIFF_EXEC_TOL   a benchmark subset and the
#                     mover threshold, for checking the harness itself
#                     (RELEASE_DIFF_EXEC_TOL=-0.5 shift-tests every row)
#
# Output: tmp/release-diff/v<B>-vs-v<A>/<stage>.txt and report.txt. Exit 0 when
# every stage is GREEN, 1 when any is RED, 2 on a usage or harness error. A RED
# row that is a stated, intended change is excused for ONE release by a line of
# docs/parity/release-diff-allow.tsv (version, stage, key, reason).
#
# Runs on the build container (QNAP law: the NAS runs nothing); never beside
# another suite in the same tree (run_tests.sh's cwd LEAK check cross-blames).
set -u
cd "$(dirname "$0")/.." || exit 9
ROOT=$(pwd)
ARCH="${MADC_RELEASE_BINS_DIR:-$ROOT/tmp/release-bins}"
B="${MADC_BIN:-bin/madc-release}"
case "$B" in /*) ;; *) B="$ROOT/$B" ;; esac
prev=""
prev_assets=""
cand_assets=""
stages=()
for a in "$@"; do
	case "$a" in
	--prev=*) prev="${a#--prev=}"; prev="${prev#v}" ;;
	--prev-assets=*) prev_assets="${a#--prev-assets=}" ;;
	--cand-assets=*) cand_assets="${a#--cand-assets=}" ;;
	startup|exec|compile|memory|cost|tests|packages) stages+=("$a") ;;
	*) echo "release_diff: unknown argument '$a'" >&2; exit 2 ;;
	esac
done
[ -x "$B" ] || { echo "release_diff: no $B (make -C src release)" >&2; exit 2; }
bv=$("$B" --version 2>/dev/null | head -1 | awk '{print $NF}')
[ -n "$bv" ] || { echo "release_diff: $B --version printed nothing" >&2; exit 2; }

# A: the newest archived release at or below B's version whose code is not
# this tree's. Before /release bumps VERSION the candidate still reports the
# previous version, and A is that release. After the release is archived, the
# candidate's own entry has this tree's code and is skipped: B is never its
# own A.
if [ -z "$prev" ]; then
	for v in $(for d in "$ARCH"/v*; do
			[ -x "$d/bin/madc-release" ] && echo "${d##*/v}"
		done | sort -V -r); do
		[ "$(printf '%s\n%s\n' "$v" "$bv" | sort -V | tail -1)" = "$bv" ] || continue
		c=$(sed -n 's/^commit: //p' "$ARCH/v$v/PROVENANCE" 2>/dev/null)
		if [ -n "$c" ] && git diff --quiet "$c" HEAD -- src include third_party 2>/dev/null; then
			continue
		fi
		prev=$v
		break
	done
fi
A="$ARCH/v$prev/bin/madc-release"
[ -n "$prev" ] && [ -x "$A" ] || { echo "release_diff: no archived release before $bv in $ARCH (release_bins.sh list)" >&2; exit 2; }

if [ ${#stages[@]} -eq 0 ]; then
	stages=(startup exec compile memory cost tests)
	[ -n "$prev_assets" ] && [ -n "$cand_assets" ] && stages+=(packages)
fi

OUT="$ROOT/tmp/release-diff/v$bv-vs-v$prev"
mkdir -p "$OUT" || exit 2
export RELEASE_DIFF_VERSION="$bv"

# B runs from an entry laid out exactly as release_bins.sh's put_entry lays out
# A (bin/madc-release + the libmadc directory's contents in lib/). Run from the
# build tree instead, B loaded about 1 ms faster per invocation than the same
# release run from its archive entry, and every small compile read 8% faster
# for no code reason (v0.106.1 against itself: 0.917 tree vs 0.996 entry).
bdir=$(dirname "$B")
blib="$bdir/../lib/release"
[ -d "$blib" ] || blib="$bdir/../lib"
rm -rf "$OUT/B-entry"
mkdir -p "$OUT/B-entry/bin" "$OUT/B-entry/lib" || exit 2
cp -p "$B" "$OUT/B-entry/bin/madc-release" || exit 2
cp -a "$blib/." "$OUT/B-entry/lib/" || exit 2
echo "release_diff: B staged from $B and $blib into $OUT/B-entry (the archive layout)"
B="$OUT/B-entry/bin/madc-release"
echo "release_diff: B = v$bv, A = $A (v$prev); output $OUT"

overall=0
: > "$OUT/report.txt.new"
for s in "${stages[@]}"; do
	t0=$SECONDS
	case "$s" in
	startup|exec|compile|memory)
		python3 scripts/release_diff.py "$s" "$A" "$B" "$OUT" ;;
	cost)
		rm -rf "$OUT/cost-A" "$OUT/cost-B"
		MADC_BIN="$A" bash scripts/parse_cost_gate.sh --map="$OUT/cost-A" --label="v$prev" > "$OUT/cost-A.log" 2>&1
		MADC_BIN="$B" bash scripts/parse_cost_gate.sh --map="$OUT/cost-B" --label="v$bv candidate" > "$OUT/cost-B.log" 2>&1
		python3 scripts/release_diff.py cost "$OUT/cost-A" "$OUT/cost-B" "$OUT" ;;
	tests)
		rm -rf "$OUT/tests-A" "$OUT/tests-B"
		mkdir -p "$OUT/tests-A" "$OUT/tests-B"
		# One suite at a time in this tree; run_tests.sh's own exit status is
		# not the verdict (A fails the tests of features added since).
		MADC_BIN="$A" MADC_TEST_SAVE_DIR="$OUT/tests-A" bash scripts/run_tests.sh --report=json > "$OUT/tests-A.jsonl" 2> "$OUT/tests-A.stderr"
		MADC_BIN="$B" MADC_TEST_SAVE_DIR="$OUT/tests-B" bash scripts/run_tests.sh --report=json > "$OUT/tests-B.jsonl" 2> "$OUT/tests-B.stderr"
		python3 scripts/release_diff.py tests "$OUT" ;;
	packages)
		[ -d "$prev_assets" ] && [ -d "$cand_assets" ] || { echo "release_diff: packages needs --prev-assets=DIR and --cand-assets=DIR" >&2; exit 2; }
		python3 scripts/release_diff.py packages "$prev_assets" "$cand_assets" "$OUT" ;;
	esac
	rc=$?
	[ $rc -eq 0 ] && verdict=GREEN || verdict=RED
	[ $rc -ge 2 ] && verdict=ERROR
	[ $rc -ne 0 ] && overall=1
	[ $rc -ge 2 ] && overall=2
	printf '%-8s %-5s %4ss  %s\n' "$s" "$verdict" "$((SECONDS - t0))" "$(head -1 "$OUT/$s.txt" 2>/dev/null)" >> "$OUT/report.txt.new"
done
mv "$OUT/report.txt.new" "$OUT/report.txt"
echo "=== release_diff v$bv vs v$prev"
cat "$OUT/report.txt"
case $overall in
0) echo "release_diff: GREEN — v$bv is level with or better than v$prev on every stage run" ;;
1) echo "release_diff: RED — see $OUT/<stage>.txt" ;;
*) echo "release_diff: ERROR — a stage could not be measured" ;;
esac
exit $overall
