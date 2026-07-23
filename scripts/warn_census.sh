#!/bin/bash
# Census c2mir compile warnings across integration tests and optionally enforce
# a per-test warning baseline. Missing baseline entries mean zero allowed
# warnings, so new tests are covered automatically. This is the warning RATCHET:
# warnings may only go DOWN. Lower the baseline in the same commit that fixes a
# warning class; the end state is an all-zero baseline.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MADC="${MADC:-bin/madc}"
OUT="$ROOT/tmp/warn_census_out"
BASELINE="$ROOT/docs/parity/warning-baseline.txt"
CHECK=0
TIMEOUT_SECONDS=30

usage() {
	echo "usage: scripts/warn_census.sh [--check] [--baseline FILE] [--out-dir DIR] [--timeout SECONDS]" >&2
}

while [ $# -gt 0 ]; do
	case "$1" in
		--check) CHECK=1; shift ;;
		--baseline)
			if [ $# -lt 2 ]; then usage; exit 2; fi
			BASELINE="$2"; shift 2 ;;
		--out-dir)
			if [ $# -lt 2 ]; then usage; exit 2; fi
			OUT="$2"; shift 2 ;;
		--timeout)
			if [ $# -lt 2 ]; then usage; exit 2; fi
			TIMEOUT_SECONDS="$2"; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) usage; exit 2 ;;
	esac
done

cd "$ROOT"
mkdir -p "$OUT"
: > "$OUT/per_test.txt"
: > "$OUT/all_warns.txt"
: > "$OUT/regressions.txt"

declare -A baseline_counts
baseline_total=0
if [ "$CHECK" -eq 1 ]; then
	if [ ! -f "$BASELINE" ]; then
		echo "warning census baseline missing: $BASELINE" >&2
		exit 1
	fi
	while read -r count test extra; do
		case "${count:-}" in
			""|\#*) continue ;;
		esac
		if [ -n "${extra:-}" ] || ! [[ "$count" =~ ^[0-9]+$ ]] || [ -z "${test:-}" ]; then
			echo "malformed warning baseline line: $count ${test:-} ${extra:-}" >&2
			exit 1
		fi
		baseline_counts["$test"]="$count"
		baseline_total=$((baseline_total + count))
	done < "$BASELINE"
fi

total_tests=0
tests_with_warns=0
total_warns=0
tests_improved=0

for f in tests/*.mad; do
	base="$(basename "$f")"
	[ "$base" = "include_helper.mad" ] && continue

	flags=()
	args=()
	flags_file="tests/${base%.mad}.flags"
	input_file="tests/${base%.mad}.input"
	argv_file="tests/${base%.mad}.argv"
	timeout_file="tests/${base%.mad}.timeout"
	[ -f "$flags_file" ] && read -r -a flags < "$flags_file"
	[ -f "$argv_file" ] && read -r -a args < "$argv_file"
	test_timeout="$TIMEOUT_SECONDS"
	[ -f "$timeout_file" ] && read -r test_timeout < "$timeout_file"

	total_tests=$((total_tests + 1))
	stderr_file="$OUT/${base%.mad}.stderr"
	if [ -f "$input_file" ]; then
		{ timeout "$test_timeout" "$MADC" "${flags[@]}" "$f" "${args[@]}" < "$input_file" > /dev/null; } 2> "$stderr_file"
	else
		{ timeout "$test_timeout" "$MADC" "${flags[@]}" "$f" "${args[@]}" > /dev/null; } 2> "$stderr_file"
	fi
	warn_file="$OUT/${base%.mad}.warnings"
	grep -- 'warning --' "$stderr_file" > "$warn_file"
	n=$(grep -c -- 'warning --' "$warn_file")
	if [ "$n" -gt 0 ]; then
		tests_with_warns=$((tests_with_warns + 1))
		total_warns=$((total_warns + n))
		printf '%5d  %s\n' "$n" "$base" >> "$OUT/per_test.txt"
		cat "$warn_file" >> "$OUT/all_warns.txt"
	fi

	if [ "$CHECK" -eq 1 ]; then
		allowed="${baseline_counts[$base]:-0}"
		if [ "$n" -gt "$allowed" ]; then
			printf '%5d > %5d  %s\n' "$n" "$allowed" "$base" >> "$OUT/regressions.txt"
		elif [ "$n" -lt "$allowed" ]; then
			tests_improved=$((tests_improved + 1))
		fi
	fi
done

echo "=== SUMMARY ==="
echo "tests compiled      : $total_tests"
echo "tests WITH warnings : $tests_with_warns"
echo "total warnings      : $total_warns"
if [ "$CHECK" -eq 1 ]; then
	echo "baseline warnings   : $baseline_total"
	echo "tests improved      : $tests_improved"
fi
echo ""
echo "=== top tests by warning count ==="
if [ -s "$OUT/per_test.txt" ]; then sort -rn "$OUT/per_test.txt" | head -20; else echo "none"; fi
echo ""
echo "=== warning categories (message, normalized) ==="
if [ -s "$OUT/all_warns.txt" ]; then
	sed -E 's/^.*warning -- //; s/[0-9]+/N/g' "$OUT/all_warns.txt" | sort | uniq -c | sort -rn | head -30
else
	echo "none"
fi

if [ "$CHECK" -eq 1 ]; then
	echo ""
	echo "=== warning ratchet ==="
	if [ -s "$OUT/regressions.txt" ]; then
		echo "RED — warning count exceeded baseline:"
		cat "$OUT/regressions.txt"
		exit 1
	fi
	echo "GREEN — no test exceeds warning baseline."
fi
