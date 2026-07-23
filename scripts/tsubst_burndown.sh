#!/bin/bash
# tsubst_burndown.sh — suite-wide progress toward deprecating re-parse.
#
# Runs every integration test with --show-stats,
# then aggregates the tsubst engagement across the whole suite:
#   - total HIT      : template-body instantiations resolved on the parse-once
#                      spine (the g++ way).
#   - total FALLBACK : instantiations that bailed to the re-parse fallback.
#                      THIS IS THE DISTANCE-TO-GOAL. Target: 0.
#   - the distinct [why:] reason-classes = the remaining KIND worklist, ranked.
#
# 0 fallback across the suite -> re-parse is unreachable -> flip the default
# and delete the re-parse path (g++ parse-once parity). See
# .claude/rules/parse-once.md and docs/rules/parse-once.md.
#
# Usage:  bash scripts/tsubst_burndown.sh
# Env:    MADC=bin/madc  TESTS=tests  PER_TEST_TIMEOUT=30

set -u
MADC="${MADC:-bin/madc}"
TESTS="${TESTS:-tests}"
PER_TEST_TIMEOUT="${PER_TEST_TIMEOUT:-30}"
WHY="$(mktemp)"
trap 'rm -f "$WHY"' EXIT

tot_hit=0
tot_fb=0
tests_with=0

for f in "$TESTS"/*.mad; do
	[ "$(basename "$f")" = "include_helper.mad" ] && continue
	out=$(timeout "$PER_TEST_TIMEOUT" "$MADC" --show-stats "$f" 2>&1)
	line=$(printf '%s\n' "$out" | grep -oE '[0-9]+ hit / [0-9]+ fallback' | head -1)
	[ -z "$line" ] && continue
	h=$(echo "$line" | grep -oE '^[0-9]+')
	m=$(echo "$line" | grep -oE '[0-9]+ fallback' | grep -oE '^[0-9]+')
	tot_hit=$((tot_hit + h))
	tot_fb=$((tot_fb + m))
	tests_with=$((tests_with + 1))
	printf '%s\n' "$out" | grep -oE '\[why: [^]]*\]' >> "$WHY"
done

echo "=== SUITE-WIDE TSUBST BURNDOWN (flag-on) ==="
echo "tests exercising tsubst : $tests_with"
echo "total HIT               : $tot_hit"
echo "total FALLBACK (reparse): $tot_fb"
if [ $((tot_hit + tot_fb)) -gt 0 ]; then
	echo "HIT rate                : $((100 * tot_hit / (tot_hit + tot_fb)))%"
fi
echo
echo "=== distinct [why:] reason-classes remaining (the KIND worklist) ==="
if [ -s "$WHY" ]; then
	sort "$WHY" | uniq -c | sort -rn
else
	echo "(none — re-parse is unreachable across the suite; delete-the-path gate is MET)"
fi
