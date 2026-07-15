#!/bin/bash
# Suite-wide class-template parse-once engagement and monotonic ratchet.
#
# Usage: bash scripts/class_parse_burndown.sh
#        bash scripts/class_parse_burndown.sh --check
# Env:   MADC=bin/madc TESTS=tests PER_TEST_TIMEOUT=30 MADC_CPU_LIMIT=30

set -u

MADC="${MADC:-bin/madc}"
TESTS="${TESTS:-tests}"
PER_TEST_TIMEOUT="${PER_TEST_TIMEOUT:-30}"
MADC_CPU_LIMIT="${MADC_CPU_LIMIT:-30}"
BASELINE_FILE="docs/parity/class-parse-baseline.txt"

baseline_parse=""
baseline_pattern=""
baseline_tests=""
baseline_mode_skipped=""
declare -A allowed_reason=()
if [ -f "$BASELINE_FILE" ]; then
	while read -r key value; do
		case "$key" in
		''|\#*) continue ;;
		pattern) baseline_pattern="$value" ;;
		parse) baseline_parse="$value" ;;
		tests) baseline_tests="$value" ;;
		mode-skipped) baseline_mode_skipped="$value" ;;
		reason) allowed_reason["$value"]=1 ;;
		esac
	done < "$BASELINE_FILE"
fi

total_pattern=0
total_parse=0
total_cache=0
total_opaque=0
tests_seen=0
tests_with_class=0
tests_mode_skipped=0
rc=0
declare -A observed_reason=()

for file in "$TESTS"/*.mad; do
	base="${file%.mad}"
	if [ "${file##*/}" = "include_helper.mad" ]; then
		continue
	fi
	if [ -f "$base.mir_skip" ]; then
		continue
	fi
	flags=()
	if [ -f "$base.flags" ]; then
		read -r -a flags < "$base.flags"
	fi
	skip_mode=0
	for flag in "${flags[@]}"; do
		case "$flag" in
		--project|--project=*|*.cc.json) skip_mode=1 ;;
		esac
	done
	if [ "$skip_mode" -eq 1 ]; then
		tests_mode_skipped=$((tests_mode_skipped + 1))
		continue
	fi
	out=$(env MADC_CPU_LIMIT="$MADC_CPU_LIMIT" timeout "$PER_TEST_TIMEOUT" \
		"$MADC" --show-stats --dump-registered "${flags[@]}" "$file" \
		2>&1 >/dev/null)
	test_rc=$?
	if [ "$test_rc" -ne 0 ]; then
		echo "class census failed: $file (rc=$test_rc)" >&2
		rc=1
	fi
	if [[ "$out" =~ class[[:space:]]instantiate[[:space:]]\.[[:space:]]([0-9]+)[[:space:]]pattern[[:space:]]/[[:space:]]([0-9]+)[[:space:]]parse[[:space:]]/[[:space:]]([0-9]+)[[:space:]]cache[[:space:]]/[[:space:]]([0-9]+)[[:space:]]opaque ]]; then
		pattern="${BASH_REMATCH[1]}"
		parse="${BASH_REMATCH[2]}"
		cache="${BASH_REMATCH[3]}"
		opaque="${BASH_REMATCH[4]}"
		total_pattern=$((total_pattern + pattern))
		total_parse=$((total_parse + parse))
		total_cache=$((total_cache + cache))
		total_opaque=$((total_opaque + opaque))
		tests_seen=$((tests_seen + 1))
		if [ $((pattern + parse + cache + opaque)) -gt 0 ]; then
			tests_with_class=$((tests_with_class + 1))
		fi
	else
		echo "missing class stats: $file" >&2
		rc=1
	fi
	in_class_profile=0
	while IFS= read -r line; do
		if [[ "$line" == "[stats]   class parse profile (ranked):" ]]; then
			in_class_profile=1
			continue
		fi
		if [[ "$line" == "[stats]   class parse census ."* ]]; then
			in_class_profile=0
		fi
		if [ "$in_class_profile" -eq 1 ] \
		  && [[ "$line" =~ \[why:[[:space:]]([^]]+)\] ]]; then
			observed_reason["${BASH_REMATCH[1]}"]=1
		fi
	done <<< "$out"
done

echo "=== SUITE-WIDE CLASS PARSE-ONCE BURNDOWN ==="
echo "tests with stats       : $tests_seen"
echo "project-mode skipped   : $tests_mode_skipped"
echo "tests exercising class : $tests_with_class"
echo "total PATTERN          : $total_pattern"
echo "total PARSE            : $total_parse"
echo "total CACHE            : $total_cache"
echo "total OPAQUE           : $total_opaque"
echo "observed reasons       :"
if [ "${#observed_reason[@]}" -eq 0 ]; then
	echo "  (none)"
else
	for reason in "${!observed_reason[@]}"; do
		echo "  $reason"
	done
fi

if [ "${1:-}" = "--check" ]; then
	if [ -z "$baseline_pattern" ] || [ -z "$baseline_parse" ] || [ -z "$baseline_tests" ] || [ -z "$baseline_mode_skipped" ]; then
		echo "RED: incomplete baseline in $BASELINE_FILE" >&2
		rc=1
	elif [ "$tests_seen" -ne "$baseline_tests" ]; then
		echo "RED: stats coverage changed: $tests_seen != $baseline_tests" >&2
		rc=1
	elif [ "$tests_mode_skipped" -ne "$baseline_mode_skipped" ]; then
		echo "RED: project-mode coverage changed: $tests_mode_skipped != $baseline_mode_skipped" >&2
		rc=1
	elif [ "$total_parse" -gt "$baseline_parse" ]; then
		echo "RED: class parse count increased: $total_parse > $baseline_parse" >&2
		rc=1
	elif [ "$total_parse" -lt "$baseline_parse" ]; then
		echo "PROGRESS: class parse count fell: $baseline_parse -> $total_parse"
		echo "Update $BASELINE_FILE in the widening commit."
	fi
	if [ "$total_pattern" -ne "$baseline_pattern" ]; then
		echo "ENGAGEMENT: class pattern count changed: $baseline_pattern -> $total_pattern"
	fi
	for reason in "${!observed_reason[@]}"; do
		if [ -z "${allowed_reason[$reason]:-}" ]; then
			echo "RED: new class parse reason: $reason" >&2
			rc=1
		fi
	done
	if [ "$rc" -eq 0 ]; then
		echo "GREEN: class parse count did not increase; reasons are approved."
	fi
fi

exit "$rc"
