#!/bin/bash
# RULE-DRIFT GATE — every code commit must SHOW it applied the Top 5 rules.
#
# Why an artifact and not more rule text: the rules were already written down
# and were still violated repeatedly on 2026-07-27 — Rule #4 (search first) by
# default, Rule #2 (fix at the deepest layer) with a shim wearing a true-but-not
# -load-bearing principle as cover. Rule text is invisible when skipped. A
# required trailer is not: its ABSENCE is greppable, so a reviewer can spot the
# skip without reading the diff. Same argument as check-one-delim-tracker.sh.
#
# Required on every commit touching a C/C++ source under src/ or include/.
# Build wiring, generated tables, scripts, docs and tests are out of scope.
#
#   Hypothesis: <what you believed was wrong, BEFORE editing>        (Rule #3)
#   Layer:      <the layer chain, and why the one you edited is the
#                deepest — if you cannot write this, you are shimming> (Rules #2, #5)
#   Searched:   <the grep you ran, the CONCEPT, and what came back>    (Rule #4)
#   Oracle:     <what gcc/clang did on a reducer, and what madc did>   (Rule #1)
#
# "n/a — <reason>" is an allowed value. It is NOT an escape hatch: it forces the
# reason into the permanent record where it can be argued with. Silence cannot.
set -u
cd "$(dirname "$0")/.."

# Commits at or before this are pre-gate and not checked. Move it FORWARD only.
EPOCH=a35ee16f

if ! git rev-parse --verify --quiet "$EPOCH" >/dev/null; then
	echo "check-rule-trailers: EPOCH $EPOCH not in this repo — skipping."
	exit 0
fi
if ! git merge-base --is-ancestor "$EPOCH" HEAD 2>/dev/null; then
	echo "check-rule-trailers: EPOCH $EPOCH is not an ancestor of HEAD — skipping."
	exit 0
fi

commits=$(git rev-list "$EPOCH"..HEAD)
bad=0
checked=0

for c in $commits; do
	# Merge commits carry no authored change of their own.
	if [ "$(git rev-list --parents -n 1 "$c" | wc -w)" -gt 2 ]; then
		continue
	fi
	# Actual C/C++ sources only. Build wiring (src/Makefile), generated
	# tables and scripts are out of scope — demanding an Oracle for a
	# Makefile edit is noise, and noise is how a gate gets ignored.
	touched=$(git diff-tree --no-commit-id --name-only -r "$c" \
		| grep -E '^(src|include)/.*\.(c|cc|cpp|h|hpp)$' | head -1)
	[ -z "$touched" ] && continue
	checked=$((checked + 1))
	msg=$(git log -1 --format=%B "$c")
	missing=""
	for field in Hypothesis Layer Searched Oracle; do
		printf '%s' "$msg" | grep -qE "^${field}:[[:space:]]*[^[:space:]]" \
			|| missing="$missing $field"
	done
	if [ -n "$missing" ]; then
		bad=$((bad + 1))
		echo "  $(git log -1 --format='%h %s' "$c")"
		echo "      missing:$missing"
	fi
done

echo "check-rule-trailers: $checked code commit(s) since $EPOCH, $bad missing trailers"
if [ "$bad" -ne 0 ]; then
	echo ""
	echo "A code commit must SHOW that the Top 5 rules were applied."
	echo "Add to the commit message (\"n/a — reason\" is allowed, silence is not):"
	echo "  Hypothesis: what you believed was wrong, before editing   (#3)"
	echo "  Layer:      the layer chain + why yours is the deepest    (#2,#5)"
	echo "  Searched:   the grep, the concept, what came back         (#4)"
	echo "  Oracle:     what gcc/clang did vs what madc did           (#1)"
	echo "See .claude/rules/rule-trailers.md"
	exit 1
fi
echo "GREEN — every code commit since the epoch shows its rule work."
exit 0
