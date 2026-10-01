#!/bin/bash
# DRIFT-PREVENTION GATE -- the editor's selection and its clipboard each have
# ONE owner (plan §41.11a step 3e), in tools/texteditor/editor_events.inc:
#   - the selection rule, selection_range(): the block runs [mark,
#     bend-else-caret], and under the two-point personality (es "selblock")
#     a mark without an end is no block. It had three copies -- the edit
#     node's highlight, a peer's presence highlight (which skipped the
#     two-point rule, so a peer's half-made JOE block lit for others) and
#     madcide's block_range;
#   - the clipboard's writers, cut_selection() and copy_selection(): vised's
#     ^K, madcide's ^K Y and its Cut were three copies of the cut.
# A reader asks the owner: selection_range(r, w, es, caret), or the cut /
# copy rules.
#
# Two-sided: the negative control proves each pattern still matches a copy.
set -u
cd "$(dirname "$0")/.."

OWNER=tools/texteditor/editor_events.inc
RULE='\b[a-z]*bend >= 0 \?'
CLIP='ui::set\([^,]+, *[^,]+, *"clip"'

ctl=$(printf '\tlong psend = pbend >= 0 ? pbend : pcaret;\n\tui::set(w, es, "clip", perl::substr(t, lo, n));\n' \
	| grep -cE "$RULE|$CLIP")
if [ "$ctl" -ne 2 ]; then
	echo "check-one-selection-rule: NEGATIVE CONTROL FAILED -- the patterns no longer"
	echo "  match a copy of the selection rule or a clipboard write (matched $ctl of 2)"
	exit 1
fi

# A function's line range in the owner file: its header line .. its closing brace.
range_of() {
	awk -v hdr="$1" 'index($0, hdr) == 1 {s=NR} s && /^}/ {print s" "NR; exit}' "$OWNER"
}
rule=$(range_of 'bool selection_range(')
cut=$(range_of 'bool cut_selection(')
copy=$(range_of 'bool copy_selection(')
if [ -z "$rule" ] || [ -z "$cut" ] || [ -z "$copy" ]; then
	echo "check-one-selection-rule: an owner is missing from $OWNER"
	echo "  (selection_range: '${rule}', cut_selection: '${cut}', copy_selection: '${copy}')"
	exit 1
fi

# within LINE RANGE...: is a hit inside one of the owners' bodies?
inside() {
	awk -F: -v r1="$rule" -v r2="$cut" -v r3="$copy" -v f="$OWNER" '
		function in_r(r, n,   a) { split(r, a, " "); return n >= a[1] && n <= a[2] }
		!($1 == f && (in_r(r1, $2) || in_r(r2, $2) || in_r(r3, $2)))'
}
bypass=$(git grep -nE "$RULE|$CLIP" -- tools \
	| grep -vE '^[^:]+:[0-9]+:[[:space:]]*//' \
	| inside)
n=$(printf '%s' "$bypass" | grep -c . || true)
echo "selection-rule copies / clipboard writes outside their owners: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$bypass"
	echo "  -> ask selection_range(r, w, es, caret), or cut_selection / copy_selection ($OWNER)."
	exit 1
fi
echo "GREEN -- the selection has one rule, the clipboard one writer."
