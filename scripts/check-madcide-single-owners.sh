#!/usr/bin/env bash
# check-madcide-single-owners.sh — madcide's consolidated single owners
# stay single (DupFamily madcide_buffer_row_shape, consolidated IDE-10b).
#
# The buffer-table row shape {doc, path, caret, mark, bend} has ONE fresh-
# row owner: push_buffer_row. The one deliberate exception is edit_file's
# seed row (it carries LIVE interaction state, not fresh zeroes). Marker:
# '"bend"] = -1' sites in tools/madcide/madcide_core.inc == 2 (the owner +
# the seed). A new open path must call push_buffer_row, not restate the
# shape.
set -u

FILE="$(dirname "$0")/../tools/madcide/madcide_core.inc"

count_rows()
{
	grep -c '"bend"\] = -1' "$1"
}

n=$(count_rows "$FILE")
if [ "$n" -ne 2 ]; then
	echo "check-madcide-single-owners: FAIL — $n buffer-row shape sites" \
	     "in tools/madcide/madcide_core.inc (expected 2: push_buffer_row" \
	     "+ edit_file's live-state seed). Route new rows through" \
	     "push_buffer_row." >&2
	exit 1
fi

# Negative control: a synthetic third shape site must fail the count.
tmp=$(mktemp)
cat "$FILE" > "$tmp"
echo '    b["bend"] = -1;	// synthetic' >> "$tmp"
if [ "$(count_rows "$tmp")" -ne 3 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic row site (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# The return-key pause has ONE owner: terminal_return_pause (DupFamily
# terminal_return_pause, consolidated with the fork-Run slice). Marker:
# the prompt spelling appears exactly once — a second pause prompt means
# a run flow restated the pause instead of calling the owner.
count_pause()
{
	grep -c 'press enter to return' "$1"
}

n=$(count_pause "$FILE")
if [ "$n" -ne 1 ]; then
	echo "check-madcide-single-owners: FAIL — $n return-pause prompts" \
	     "in tools/madcide/madcide_core.inc (expected 1:" \
	     "terminal_return_pause). Call the owner, never restate the" \
	     "pause." >&2
	exit 1
fi

# Negative control for the pause marker.
tmp=$(mktemp)
cat "$FILE" > "$tmp"
echo '    println("[madcide] press enter to return");	// synthetic' >> "$tmp"
if [ "$(count_pause "$tmp")" -ne 2 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic pause prompt (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

echo "check-madcide-single-owners: OK (one fresh-row owner: push_buffer_row;" \
     "one pause owner: terminal_return_pause)"
exit 0
