#!/bin/bash
# check-one-blank-run.sh — the "leading blanks of a text" gate: ONE scanner.
#
# How many spaces and tabs a text starts with has ONE owner in the dialect
# tools: blank_run in tools/texteditor/editor_events.inc (the shared editor
# layer every madcide and vised file includes). On 2026-10-04 the chthonia
# line edits (Indent, Dedent, Toggle comment) needed it and found two
# hand-rolled copies already standing — a layout line's nesting depth
# (madcide_core.inc) and the indent an MCP fragment inherits
# (madcide_mcp.inc) — each a `while` over ' ' and '\t'. Both now call the
# owner. A copy that counts only spaces, or forgets the tab, indents one
# surface differently from the other.
#
# Rule: outside the owner, no tools/ source loops over a text's leading
# blanks by hand: a `while` whose condition tests a byte against ' ' AND
# '\t'.
#
# Negative control: a synthetic copy of each shape must FAIL the scan, and
# a loop over ' ' alone (a different question) must PASS, else the gate
# itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

OWNER=tools/texteditor/editor_events.inc
PATTERN="while *\\(.*== *' '.*== *'\\\\t'"

scan() {
	# $@ = files; prints violations, returns 0 when clean.
	grep -nE "$PATTERN" "$@" /dev/null
	test $? -ne 0
}

# --- negative controls -------------------------------------------------------
tmp=$(mktemp)
printf "    while ( rs[d] == ' ' || rs[d] == '\\\\t' )\n" > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-blank-run: NEGATIVE CONTROL FAILED — the scan did not catch a hand-rolled leading-blank loop" >&2
	exit 2
fi
printf "    while ( i < n && (s[i] == ' ' || s[i] == '\\\\t') )\n" > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-blank-run: NEGATIVE CONTROL FAILED — the scan did not catch a bounded leading-blank loop" >&2
	exit 2
fi
printf "    while ( s[k] == ' ' )\n" > "$tmp"
if ! scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-blank-run: POSITIVE CONTROL FAILED — the scan flagged a loop over spaces alone" >&2
	exit 2
fi
rm -f "$tmp"

# --- the owner keeps the one scanner -----------------------------------------
n=$(grep -cE "^long blank_run\(" "$OWNER")
if [ "$n" != "1" ]; then
	echo "check-one-blank-run: blank_run defined $n times in $OWNER (want 1)" >&2
	exit 1
fi

# --- the tree ---------------------------------------------------------------
files=$(git ls-files 'tools/*.inc' 'tools/*.mad' 'tools/*.madv' | grep -v "^$OWNER\$")
# shellcheck disable=SC2086
if ! out=$(scan $files 2>&1); then
	echo "check-one-blank-run: a text's leading blanks scanned outside the one owner ($OWNER):" >&2
	echo "$out" >&2
	echo "  -> blank_run(const char *s): the count of leading spaces and tabs" >&2
	exit 1
fi
echo "check-one-blank-run: OK (one owner: blank_run in $OWNER)"
