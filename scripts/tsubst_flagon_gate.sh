#!/bin/bash
# tsubst_flagon_gate.sh — fast regression gate for the MADC_XTEST_DEP_PARSE
# (two-tree / tsubst) experimental path.
#
# WHY THIS EXISTS: the suite-wide burndown (scripts/tsubst_burndown.sh) is a
# progress TRACKER, not a --check gate, and it only compiles (never RUNS) tests.
# On 2026-07-01 a tsubst WIP regressed the burndown 66%->57% AND introduced a
# runtime SIGSEGV in JIT'd map/set code — and BOTH slipped through a green
# flag-off `make fulltest`, because that gate never exercises the flag-on path.
# See docs/plans/2026-07-01-tsubst-kind3-wip-verification-FINDINGS.md.
#
# This gate closes that hole cheaply: it COMPILES AND RUNS a small set of
# tsubst-heavy container tests under MADC_XTEST_DEP_PARSE=1 and asserts, per test:
#   - exit 0                (catches crashes / miscompiles on the flag-on path)
#   - hit      >= baseline  (catches fallbacks silently reclassified from hits)
#   - fallback <= baseline  (catches burndown regressions on the hot bodies)
# It is fast (a handful of tests) and flag-ON only — the production (flag-off)
# path is unaffected and is covered byte-identically by fulltest + torture.
#
# Usage:  bash scripts/tsubst_flagon_gate.sh            # report
#         bash scripts/tsubst_flagon_gate.sh --check    # gate (exit 1 on regress)
# Env:    MADC=bin/madc  PER_TEST_TIMEOUT=30

set -u
MADC="${MADC:-bin/madc}"
PER_TEST_TIMEOUT="${PER_TEST_TIMEOUT:-30}"
BASELINE_FILE="docs/parity/tsubst-flagon-baseline.txt"

# tsubst-heavy representative tests (containers exercise the hot bodies:
# _Rb_tree::_M_construct_node, vector::_M_realloc_insert, _M_insert_*, etc.)
TESTS="testset testmap testvector testsubscript testcontainerdtor testmadc_ns testlocalclassraii testfunctortmploperator testmembertmplptrret"

# Load baseline: lines "name hit fallback"
declare -A base_hit base_fb
if [ -f "$BASELINE_FILE" ]; then
	while read -r name h f; do
		case "$name" in ''|\#*) continue ;; esac
		base_hit["$name"]="$h"
		base_fb["$name"]="$f"
	done < "$BASELINE_FILE"
fi

rc=0
progress=0
printf '%-20s %-16s %-8s %s\n' "TEST" "hit/fallback" "run" "vs baseline"
for t in $TESTS; do
	f="tests/$t.mad"
	[ -f "$f" ] || { echo "$t: MISSING"; rc=1; continue; }
	out=$(timeout "$PER_TEST_TIMEOUT" env MADC_XTEST_DEP_PARSE=1 "$MADC" \
		--show-stats "$f" 2>&1)
	# run it (exit-code check catches the crash class)
	timeout "$PER_TEST_TIMEOUT" env MADC_XTEST_DEP_PARSE=1 "$MADC" "$f" \
		>/dev/null 2>&1
	run_rc=$?
	line=$(printf '%s\n' "$out" | grep -oE '[0-9]+ hit / [0-9]+ fallback' | head -1)
	h=$(printf '%s' "$line" | grep -oE '^[0-9]+')
	fb=$(printf '%s' "$line" | grep -oE '[0-9]+ fallback' | grep -oE '^[0-9]+')
	h=${h:-0}; fb=${fb:-0}
	note="ok"
	bh=${base_hit["$t"]:-}
	bf=${base_fb["$t"]:-}
	if [ "$run_rc" -ne 0 ]; then
		note="RUN FAILED (rc=$run_rc)"; rc=1
	elif [ -n "$bh" ]; then
		if [ "$h" -lt "$bh" ]; then note="RED hit $h < $bh"; rc=1;
		elif [ -n "$bf" ] && [ "$fb" -gt "$bf" ]; then note="RED fb $fb > $bf"; rc=1;
		elif [ "$h" -gt "$bh" ] || { [ -n "$bf" ] && [ "$fb" -lt "$bf" ]; }; then
			note="PROGRESS ($bh/$bf -> $h/$fb)"; progress=1;
		fi
	else
		note="(no baseline)"
	fi
	printf '%-20s %-16s %-8s %s\n' "$t" "$h/$fb" "rc=$run_rc" "$note"
done

if [ "${1:-}" = "--check" ]; then
	if [ "$rc" -ne 0 ]; then
		echo "RED — tsubst flag-on regression (crash or hit/fallback worse than $BASELINE_FILE)."
		echo "Do NOT mask it with a bail/guard — fix the root cause (see docs/plans/2026-07-01-tsubst-kind3-wip-verification-FINDINGS.md)."
	elif [ "$progress" -ne 0 ]; then
		echo "PROGRESS — some tests improved; update $BASELINE_FILE to the new numbers in this commit."
	else
		echo "GREEN — tsubst flag-on at or above baseline; no crashes."
	fi
	exit $rc
fi
