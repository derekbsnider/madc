#!/bin/bash
# c2mir_tests_lane.sh — the c2mir test corpora lane (owner directive
# 2026-09-21). Runs the tests MIR ships beside our own backend against
# bin/madc: each .c must compile+run with the exit code in <test>.expectrc
# (default 0) and stdout exactly matching <test>.expect when present.
#
# WHY THESE THREE DIRS, AND NOT THE OTHER TWO. third_party/mir/c-tests holds
# 1119 .c files in five corpora, but two of them are corpora we ALREADY run:
#   gcc/               693 tests, 691 of whose names are in the c-torture
#                      corpus the c_torture_lane.sh ratchet already covers
#   andrewchambers_c/   67 tests — a subset of the c-testsuite project the
#                      c_testsuite_lane.sh ratchet already runs at 220/220
# Adding either would pad the denominator without adding coverage, which is
# exactly the self-flattery docs/conformance-coverage.md warns about. The
# three taken here are genuinely new ground:
#   new/     132 — MIR's OWN regression tests, written by the author of the
#                  backend madc ships, against the constructs c2mir cares
#                  about. The corpus most likely to find real defects.
#   lacc/    215 — an INDEPENDENT C compiler's suite: different authorship,
#                  so different blind spots than gcc's.
#   havoc/    12 — small, free.
#
# The corpus needs no fetch step: it rides in the third_party/mir subtree.
#
# C MODE (--std=gnu11), the c_testsuite_lane.sh ruling: these are C language
# tests measured in the mode their oracles use, and MIR leans on GNU
# extensions.
#
# RATCHET shape (the torture-set precedent): known-fails live in
# docs/parity/c2mir-tests-baseline.txt (one `dir/name.c` per line, #
# comments allowed). The lane is RED when any non-baseline test fails. A
# baseline test that PASSES is reported loudly so the baseline only ever
# shrinks — never silently pads.
#
#   MADC_BIN             binary under test (default bin/madc)
#   MADC_C2MIR_TESTS     corpus root (default third_party/mir/c-tests)
#   MADC_C2MIR_BASELINE  baseline file (default docs/parity/c2mir-tests-baseline.txt)
set -u
cd "$(dirname "$0")/.."

BIN="${MADC_BIN:-bin/madc}"
ROOT="${MADC_C2MIR_TESTS:-third_party/mir/c-tests}"
BASE="${MADC_C2MIR_BASELINE:-docs/parity/c2mir-tests-baseline.txt}"
DIRS="new lacc havoc"

if [ ! -x "$BIN" ]; then
	echo "c2mir_tests_lane: $BIN missing — build first" >&2
	exit 1
fi
for d in $DIRS; do
	if [ ! -d "$ROOT/$d" ]; then
		echo "c2mir_tests_lane: no corpus at $ROOT/$d (it rides in the" \
		     "third_party/mir subtree — check the checkout)" >&2
		exit 1
	fi
done

declare -A baseline
if [ -f "$BASE" ]; then
	while IFS= read -r line; do
		case "$line" in ''|'#'*) continue;; esac
		baseline["$line"]=1
	done < "$BASE"
fi

pass=0; fail=0; newfail=0; fixed=0
newfail_names=""
fixed_names=""
mkdir -p tmp
for d in $DIRS; do
	for src in "$ROOT/$d"/*.c; do
		[ -e "$src" ] || continue
		name="$d/$(basename "$src")"
		# MIR's own fixture convention (c-tests/runtests.sh): the suffix
		# hangs off the FULL filename — havoc1.c.expectrc, not
		# havoc1.expectrc. runtests.sh also accepts the .c-stripped form
		# for .expect, so both are checked, exactly as it does. Reading
		# only the stripped form defaulted every expectation to rc 0 and
		# scored all 12 havoc tests (deliberately corrupt source, want
		# rc=1) as failures.
		stem="${src%.c}"
		want_rc=0
		[ -f "$src.expectrc" ] && want_rc=$(cat "$src.expectrc")
		exp=""
		[ -f "$stem.expect" ] && exp="$stem.expect"
		[ -f "$src.expect" ] && exp="$src.expect"
		opts=""
		[ -f "$src.opt" ] && opts=$(cat "$src.opt")
		[ -f "$stem.opt" ] && opts=$(cat "$stem.opt")
		out=$( ( ulimit -t 10; timeout 15 "$BIN" --std=gnu11 $opts "$src" ) 2>/dev/null )
		rc=$?
		ok=0
		if [ "$rc" -eq "$want_rc" ]; then
			if [ -n "$exp" ]; then
				[ "$out" = "$(cat "$exp")" ] && ok=1
			else
				ok=1
			fi
		fi
		if [ "$ok" -eq 1 ]; then
			pass=$((pass + 1))
			if [ -n "${baseline[$name]:-}" ]; then
				fixed=$((fixed + 1))
				fixed_names="$fixed_names $name"
			fi
		else
			fail=$((fail + 1))
			if [ -z "${baseline[$name]:-}" ]; then
				newfail=$((newfail + 1))
				newfail_names="$newfail_names $name"
			fi
		fi
	done
done

# A lane that produced no counts is BROKEN, not green (the c_torture_lane.sh
# guard: an empty run once read as a pass).
if [ $((pass + fail)) -eq 0 ]; then
	echo "c2mir_tests_lane: ran ZERO tests — corpus or glob is wrong" >&2
	exit 1
fi

echo "c2mir-tests: $pass passed, $fail failed" \
     "($newfail outside baseline, $fixed baseline tests now passing)"
if [ "$fixed" -gt 0 ]; then
	echo "c2mir-tests: SHRINK THE BASELINE — now passing:$fixed_names"
fi
if [ "$newfail" -gt 0 ]; then
	echo "c2mir-tests: RED — non-baseline failure(s):$newfail_names" >&2
	exit 1
fi
exit 0
