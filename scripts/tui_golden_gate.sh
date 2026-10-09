#!/bin/bash
# tui_golden_gate (TUI facelift S0, in fulltest): madcide's golden screens —
# text and colours, driven by keys at 120x36 and 80x24 (scripts/tui_golden.py,
# goldens in tests/tui_golden/). Then the NEGATIVE CONTROL: one cell's colour
# flipped on one screen (startup), which must FAIL — proving the check detects
# a colour change. The screens run side by side (MADC_TUI_GOLDEN_JOBS).
# A deliberate look change re-records: python3 scripts/tui_golden.py --record
cd "$(dirname "$0")/.." || exit 1

if ! command -v python3 >/dev/null 2>&1; then
    echo "tui_golden_gate: python3 missing (provision_container.sh installs it)"
    exit 1
fi

out=$( ( ulimit -t 300; timeout 300 python3 scripts/tui_golden.py ) 2>&1 )
if [ $? -ne 0 ]; then
    echo "$out"
    echo "tui_golden_gate: FAIL — a screen differs from its golden"
    exit 1
fi
echo "$out"

neg=$( ( ulimit -t 300; timeout 300 env MADC_TUI_GOLDEN_NEGATIVE=1 python3 scripts/tui_golden.py --only startup ) 2>&1 )
if [ $? -eq 0 ]; then
    echo "$neg"
    echo "tui_golden_gate: FAIL — the negative control PASSED (the check detects nothing)"
    exit 1
fi

echo "tui_golden_gate: PASS (golden screens + negative control)"
