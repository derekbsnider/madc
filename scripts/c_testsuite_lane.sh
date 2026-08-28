#!/bin/bash
# c_testsuite_lane.sh — the c-testsuite conformance lane (owner directive
# 2026-08-28). Runs github.com/c-testsuite/c-testsuite's single-exec set
# against bin/madc: each tests/single-exec/NNNNN.c must compile+run rc=0
# with stdout exactly matching NNNNN.c.expected.
#
# RATCHET shape (the torture-set precedent): known-fails live in
# docs/parity/c-testsuite-baseline.txt (one test filename per line, #
# comments allowed). The lane is RED when any non-baseline test fails.
# A baseline test that PASSES is reported loudly so the baseline only
# ever shrinks — never silently pads.
#
#   MADC_BIN            binary under test (default bin/madc)
#   MADC_CTS_DIR        the checkout (default /workspace/c-testsuite)
#   MADC_CTS_BASELINE   baseline file (default docs/parity/c-testsuite-baseline.txt)
set -u
cd "$(dirname "$0")/.."

BIN="${MADC_BIN:-bin/madc}"
CTS="${MADC_CTS_DIR:-/workspace/c-testsuite}"
BASE="${MADC_CTS_BASELINE:-docs/parity/c-testsuite-baseline.txt}"
DIR="$CTS/tests/single-exec"

if [ ! -d "$DIR" ]; then
	echo "c_testsuite_lane: no checkout at $CTS (clone" \
	     "github.com/c-testsuite/c-testsuite there)" >&2
	exit 1
fi
if [ ! -x "$BIN" ]; then
	echo "c_testsuite_lane: $BIN missing — build first" >&2
	exit 1
fi

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
for src in "$DIR"/*.c; do
	name=$(basename "$src")
	exp="$src.expected"
	out=$( ( ulimit -t 10; timeout 15 "$BIN" "$src" ) 2>/dev/null )
	rc=$?
	ok=0
	if [ "$rc" -eq 0 ]; then
		if [ -f "$exp" ]; then
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

echo "c-testsuite: $pass passed, $fail failed" \
     "($newfail outside baseline, $fixed baseline tests now passing)"
if [ "$fixed" -gt 0 ]; then
	echo "c-testsuite: SHRINK THE BASELINE — now passing:$fixed_names"
fi
if [ "$newfail" -gt 0 ]; then
	echo "c-testsuite: RED — non-baseline failure(s):$newfail_names" >&2
	exit 1
fi
exit 0
