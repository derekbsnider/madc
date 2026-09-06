#!/bin/bash
# tui_scroll_gate (IDE-9c, in fulltest): madcide scrolling a TAB-indented
# document on a REAL pty, the screen reconstructed with a VT100 interpreter
# that honors real tab-stop semantics (a raw 0x09 moves the cursor WITHOUT
# erasing the skipped cells). Every non-chrome row must equal one expected
# tab-expanded document line — stale tail fragments and doubled brace glyphs
# (the 2026-08-26 owner-reported scroll corruption: raw '\t' bytes in grid
# cells desynchronizing grid columns from screen columns) both fail it.
# Then the NEGATIVE CONTROL: with one reconstructed row corrupted, the
# checker must FAIL — proving detection instead of a vacuous pass.
cd "$(dirname "$0")/.." || exit 1

if ! command -v python3 >/dev/null 2>&1; then
    echo "tui_scroll_gate: python3 missing (provision_container.sh installs it)"
    exit 1
fi

out=$( ( ulimit -t 120; timeout 120 python3 scripts/tui_scroll_recon.py ) 2>&1 )
if [ $? -ne 0 ]; then
    echo "$out"
    echo "tui_scroll_gate: FAIL — scroll repaint corrupted the screen"
    exit 1
fi
echo "$out"

neg=$( ( ulimit -t 120; timeout 120 env MADC_TUI_RECON_NEGATIVE=1 python3 scripts/tui_scroll_recon.py ) 2>&1 )
if [ $? -eq 0 ]; then
    echo "$neg"
    echo "tui_scroll_gate: FAIL — the negative control PASSED (the checker detects nothing)"
    exit 1
fi

echo "tui_scroll_gate: PASS (scroll reconstruction + negative control)"
