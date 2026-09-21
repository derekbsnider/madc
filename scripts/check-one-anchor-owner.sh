#!/bin/bash
# check-one-anchor-owner.sh — the anchor registry gate: ONE splice owner.
#
# Every byte-offset ANCHOR into a document (the themed highlight spans, the
# selection markers mark / bend, and — with presence — other clients'
# carets) is reanchored across an edit by ONE primitive: shift_offset,
# called in one pass by shift_anchors, both in
# tools/texteditor/editor_events.inc (the ONE text-mutation owner
# ed_text_insert / ed_text_erase route through). Until the client-server arc
# V3a, block_copy carried a SECOND copy of the splice (a `mark + n` under a
# `caret <= mark` guard) — a hand-rolled shifter is exactly where two copies
# of the rule drift apart (the delimiter-tracker / key-owner precedent).
#
# Rule: a stored selection anchor (mark / bend) is never SET to an
# ARITHMETIC shift of itself outside the owner — a reanchor goes through
# shift_offset. And the owner keeps the one primitive: shift_offset and
# shift_anchors are each defined once, in editor_events.inc, and both
# mutation wrappers call shift_anchors (the shift is never silently dropped).
#
# Negative controls: a synthetic duplicate shifter must FAIL the scan, and a
# legitimate marker set (to the caret, to -1, restored from a snapshot) must
# PASS, else the gate itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

OWNER=tools/texteditor/editor_events.inc
# A selection anchor SET to an arithmetic shift (an ident / paren followed
# by ` + `): the duplicate-shifter smell. Legit sets are to `caret`, `-1`,
# or a snapshot read (`… .as_integer()`) — none carry a ` + ` in the value.
SHIFT_PATTERN='ui::set\([^;]*"(mark|bend)"[^;]*[A-Za-z0-9_)] *\+ '

scan() {
	# $@ = files; prints violations, returns 0 when clean.
	grep -nE "$SHIFT_PATTERN" "$@" /dev/null
	test $? -ne 0
}

# --- negative controls -------------------------------------------------------
tmp=$(mktemp)
printf '\tui::set(w, es, "mark", mark + n);\n' > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-anchor-owner: NEGATIVE CONTROL FAILED — the scan did not catch a hand-rolled marker shift" >&2
	exit 2
fi
printf '\tui::set(w, es, "mark", caret);\n\tui::set(w, es, "bend", -1);\n\tui::set(w, es, "mark", t["mark"].as_integer());\n' > "$tmp"
if ! scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-anchor-owner: POSITIVE CONTROL FAILED — the scan flagged a legitimate marker set" >&2
	exit 2
fi
rm -f "$tmp"

fail=0

# --- the owner keeps the one primitive --------------------------------------
for fn in shift_offset shift_anchors; do
	n=$(grep -cE "^(long|void) $fn\(" "$OWNER")
	if [ "$n" != "1" ]; then
		echo "check-one-anchor-owner: $fn defined $n times in $OWNER (want 1)" >&2
		fail=1
	fi
done
# the splice primitive is defined nowhere else
others=$(grep -rlE "^(long|void) (shift_offset|shift_anchors)\(" tools/ | grep -v "^$OWNER$" || true)
if [ -n "$others" ]; then
	echo "check-one-anchor-owner: the splice primitive is redefined outside the owner:" >&2
	echo "$others" >&2
	fail=1
fi
# both mutation wrappers wire the registry (the shift is never dropped)
if ! grep -q 'shift_anchors(w, es, doc, off, strlen' "$OWNER"; then
	echo "check-one-anchor-owner: ed_text_insert no longer calls shift_anchors" >&2
	fail=1
fi
if ! grep -q 'shift_anchors(w, es, doc, off, -n)' "$OWNER"; then
	echo "check-one-anchor-owner: ed_text_erase no longer calls shift_anchors" >&2
	fail=1
fi

# --- the scan over the tools tree (outside the owner) -----------------------
viol=$(grep -rnE "$SHIFT_PATTERN" tools/ | grep -v "^$OWNER:" || true)
if [ -n "$viol" ]; then
	echo "check-one-anchor-owner: a stored selection anchor is shifted OUTSIDE the owner (route it through shift_offset):" >&2
	echo "$viol" >&2
	fail=1
fi

if [ "$fail" != "0" ]; then
	exit 1
fi
echo "check-one-anchor-owner: OK (one splice primitive shift_offset; shift_anchors the one pass; no hand-rolled marker shift; negative controls bite)"
exit 0
