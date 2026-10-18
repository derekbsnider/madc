#!/bin/bash
# GATE — the dialect is pay-per-use (docs/plans/2026-10-17-dialect-builtin-
# registration.md, R5): a dialect compile builds the generated table's entries
# for the names it USES, never for the names the table holds. A definition a
# program does not name costs it nothing; the table's size is not a compile's
# cost.
#
# The measure is the compiler's own count (`--show-stats`: "N of M names
# built", Program::dialect_builds). Each dialect parse-cost workload
# (scripts/parse_cost/<name>.mad) records the count it builds in
# scripts/parse_cost/<name>.builds — the measured rows of the profiling gate
# are the programs this gate holds to their names.
#
#   growth past the record — RED: some path builds names the program does not
#     use (a namespace-wide or table-wide build on the dialect path).
#   a drop — reported, re-record the .builds file to keep it.
#   N >= M — RED: the whole table was built.
#
# Detector control: a program naming TWO php:: functions, judged against a
# record of 1, must be red — the comparator sees a build past the record.
set -u
cd "$(dirname "$0")/.."

MADC_BIN="${MADC_BIN:-bin/madc}"
if [ ! -x "$MADC_BIN" ]; then
	echo "check-dialect-pay-per-use: $MADC_BIN not built" >&2
	exit 2
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# built SRC — prints "N M" from the compile's dialect table line, or nothing.
built() {
	( ulimit -t 120; timeout 120 "$MADC_BIN" --std=madc --show-stats -c -o "$tmp/out.o" "$1" ) \
		2>&1 >/dev/null | sed -n 's/.* \([0-9][0-9]*\) of \([0-9][0-9]*\) names built.*/\1 \2/p'
}

# judge LABEL SRC RECORD — 0 green, 1 red; prints the finding.
judge() {
	local label="$1" src="$2" record="$3" nm n m
	nm=$(built "$src")
	if [ -z "$nm" ]; then
		echo "check-dialect-pay-per-use: $label reported no dialect table (did it compile under --std=madc?)"
		return 1
	fi
	n=${nm% *}; m=${nm#* }
	if [ "$n" -ge "$m" ]; then
		echo "check-dialect-pay-per-use: $label built the whole table ($n of $m)"
		return 1
	fi
	if [ "$n" -gt "$record" ]; then
		echo "check-dialect-pay-per-use: $label built $n of $m names (record $record) — a path builds names the program does not use"
		return 1
	fi
	if [ "$n" -lt "$record" ]; then
		echo "check-dialect-pay-per-use: $label built $n of $m names (record $record) — re-record its .builds to keep the drop"
	fi
	return 0
}

fail=0
checked=0
for rec in scripts/parse_cost/*.builds; do
	[ -f "$rec" ] || continue
	src="${rec%.builds}.mad"
	if [ ! -f "$src" ]; then
		echo "check-dialect-pay-per-use: $rec names no workload ($src missing)"
		fail=1
		continue
	fi
	judge "$(basename "$src")" "$src" "$(tr -d ' \t\n' < "$rec")" || fail=1
	checked=$((checked + 1))
done
if [ "$checked" -eq 0 ]; then
	echo "check-dialect-pay-per-use: no scripts/parse_cost/*.builds records — nothing was judged"
	fail=1
fi

printf 'int main() { println("{} {}", php::strtoupper("a"), php::strtolower("B")); return 0; }\n' > "$tmp/two.mad"
if judge "detector control (two php:: names, record 1)" "$tmp/two.mad" 1 > /dev/null; then
	echo "check-dialect-pay-per-use: DETECTOR CONTROL FAILED — a build past the record was not seen:"
	built "$tmp/two.mad"
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	echo "Owner: Program::dialect_demand / run_dialect_entry (src/madc_builtin_registry.cpp) and their callers."
	exit 1
fi
echo "check-dialect-pay-per-use: OK — $checked dialect workload(s) build only the names they use"
