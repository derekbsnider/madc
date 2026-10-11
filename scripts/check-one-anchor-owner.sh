#!/bin/bash
# check-one-anchor-owner.sh — the anchor registry gate: ONE splice owner.
#
# Every byte-offset ANCHOR into a text (the themed highlight spans, the
# selection markers mark / bend, and — with presence — other clients'
# carets) is reanchored across an edit by ONE rule: the engine's anchor rule
# (madc::hub::shift_position in include/madcdis/document.h — the rule a
# document's own anchors follow), reached from the dialect as
# ui::anchor_shift. shift_anchors in tools/texteditor/editor_events.inc
# applies it to every registered anchor in one pass per splice, run by the
# ONE edit follower ed_text_changed (every change to a text reaches it as
# the session's edit notification — madcide's verbs, madc's ex core, a tool;
# split arc 1D). Until the client-server arc V3a, block_copy carried a SECOND
# copy of the splice (a `mark + n` under a `caret <= mark` guard), and until
# split arc 1D the dialect carried its own copy of the rule (shift_offset) —
# a hand-rolled shifter is exactly where two copies of the rule drift apart
# (the delimiter-tracker / key-owner precedent).
#
# Rule: a stored selection anchor (mark / bend) is never SET to an
# ARITHMETIC shift of itself outside the owner — a reanchor goes through
# ui::anchor_shift. The rule is the engine's: no dialect file defines a
# splice primitive (shift_offset), document::shift reads shift_position,
# shift_anchors is defined once in editor_events.inc, and the edit follower
# calls it once per splice (the shift is never silently dropped).
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

# --- the rule is the engine's ------------------------------------------------
if ! grep -q '_anchors\[i\].off = shift_position(' include/madcdis/document.h; then
	echo "check-one-anchor-owner: document::shift no longer applies shift_position" >&2
	fail=1
fi
if ! grep -q 'madc::hub::shift_position(' src/ns_ui.cpp; then
	echo "check-one-anchor-owner: ui::anchor_shift no longer answers by shift_position" >&2
	fail=1
fi
dialect=$(grep -rlE "^long shift_offset\(" tools/ examples/ 2>/dev/null || true)
if [ -n "$dialect" ]; then
	echo "check-one-anchor-owner: a dialect copy of the shift rule (use ui::anchor_shift):" >&2
	echo "$dialect" >&2
	fail=1
fi
# --- the owner keeps the one pass --------------------------------------------
n=$(grep -cE "^void shift_anchors\(" "$OWNER")
if [ "$n" != "1" ]; then
	echo "check-one-anchor-owner: shift_anchors defined $n times in $OWNER (want 1)" >&2
	fail=1
fi
others=$(grep -rlE "^void shift_anchors\(" tools/ | grep -v "^$OWNER$" || true)
if [ -n "$others" ]; then
	echo "check-one-anchor-owner: the anchor pass is redefined outside the owner:" >&2
	echo "$others" >&2
	fail=1
fi
# the edit follower wires the registry for a splice (its erase and its
# insert together), and it is subscribed
if ! grep -q 'shift_anchors(w, es, doc, off, del, n, multi)' "$OWNER"; then
	echo "check-one-anchor-owner: ed_text_changed no longer shifts across a splice" >&2
	fail=1
fi
if ! grep -q 'ui::doc_subscribe(w, ed_text_changed)' "$OWNER"; then
	echo "check-one-anchor-owner: the edit follower is no longer subscribed" >&2
	fail=1
fi
# the shift runs ONLY in the follower: a second caller is a second pass
calls=$(grep -c 'shift_anchors(w, es, doc' "$OWNER")
if [ "$calls" != "1" ]; then
	echo "check-one-anchor-owner: shift_anchors called $calls times in $OWNER (want 1: the follower's splice)" >&2
	fail=1
fi

# --- the scan over the tools tree (outside the owner) -----------------------
viol=$(grep -rnE "$SHIFT_PATTERN" tools/ | grep -v "^$OWNER:" || true)
if [ -n "$viol" ]; then
	echo "check-one-anchor-owner: a stored selection anchor is shifted OUTSIDE the owner (route it through ui::anchor_shift):" >&2
	echo "$viol" >&2
	fail=1
fi

if [ "$fail" != "0" ]; then
	exit 1
fi
echo "check-one-anchor-owner: OK (one shift rule, the engine's shift_position via ui::anchor_shift; shift_anchors the one pass; no hand-rolled marker shift; negative controls bite)"
exit 0
