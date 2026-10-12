#!/bin/bash
# check-one-anchor-owner.sh — the anchor registry gate, madc's half: ONE
# shift rule.
#
# Every byte-offset ANCHOR into a text (a document's own anchors, and an
# editor's highlight spans, selection markers and other clients' carets) is
# reanchored across an edit by ONE rule: madc::hub::shift_position in
# include/madcdis/document.h. A document's anchors follow it in
# document::shift, and the dialect reaches it as ui::anchor_shift
# (src/ns_ui.cpp). Until split arc 1D madcide carried its own copy of the
# rule (shift_offset) — a hand-rolled shifter is exactly where two copies of
# the rule drift apart (the delimiter-tracker / key-owner precedent).
# madcide's own half of this gate (its scripts/check-one-anchor-owner.sh)
# keeps its editor on ui::anchor_shift with one pass per splice.
#
# Rule: shift_position is defined once, document::shift applies it, and
# ui::anchor_shift answers by it; no example defines a dialect copy
# (shift_offset).
#
# Negative control: a synthetic second definition must FAIL the scan, else
# the gate itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

OWNER=include/madcdis/document.h
DEF_PATTERN='^inline size_t shift_position\('

# --- negative control --------------------------------------------------------
tmp=$(mktemp)
printf 'inline size_t shift_position(size_t p, bool right, size_t off, size_t o, size_t n)\n' > "$tmp"
if ! grep -qE "$DEF_PATTERN" "$tmp"; then
	rm -f "$tmp"
	echo "check-one-anchor-owner: NEGATIVE CONTROL FAILED — the scan did not see a definition of the rule" >&2
	exit 2
fi
rm -f "$tmp"

fail=0
defs=$(git ls-files --cached --others --exclude-standard 'src/*' 'include/*' | xargs grep -lE "$DEF_PATTERN" /dev/null)
if [ "$defs" != "$OWNER" ]; then
	echo "check-one-anchor-owner: shift_position must be defined once, in $OWNER (found in: $(echo $defs))" >&2
	fail=1
fi
if ! grep -q '_anchors\[i\].off = shift_position(' "$OWNER"; then
	echo "check-one-anchor-owner: document::shift no longer applies shift_position" >&2
	fail=1
fi
if ! grep -q 'madc::hub::shift_position(' src/ns_ui.cpp; then
	echo "check-one-anchor-owner: ui::anchor_shift no longer answers by shift_position" >&2
	fail=1
fi
dialect=$(grep -rlE "^long shift_offset\(" examples/ 2>/dev/null || true)
if [ -n "$dialect" ]; then
	echo "check-one-anchor-owner: a dialect copy of the shift rule (use ui::anchor_shift):" >&2
	echo "$dialect" >&2
	fail=1
fi

if [ "$fail" != "0" ]; then
	exit 1
fi
echo "check-one-anchor-owner: OK (one shift rule, shift_position in $OWNER; document::shift and ui::anchor_shift answer by it; negative control bites)"
exit 0
