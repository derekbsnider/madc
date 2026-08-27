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

echo "check-madcide-single-owners: OK (one fresh-row owner: push_buffer_row)"
exit 0
