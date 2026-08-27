#!/bin/bash
# tui_smoke_gate (R5, in fulltest): the hand-rolled VT100 TUI target on a
# REAL pty — alt-screen discipline, drawing, attributes, cursor, and the
# exact semantic event stream (coalesced text, focus cycling, menu
# navigation, choose, resize) — then the NEGATIVE CONTROL: a program that
# never draws must FAIL the harness, proving the gate detects a broken
# terminal path instead of passing vacuously.
cd "$(dirname "$0")/.." || exit 1

if ! command -v python3 >/dev/null 2>&1; then
    echo "tui_smoke_gate: python3 missing (provision_container.sh installs it)"
    exit 1
fi

out=$( ( ulimit -t 120; timeout 90 python3 scripts/tui_pty.py scripts/tui_smoke.mad ) 2>&1 )
if [ $? -ne 0 ]; then
    echo "$out"
    echo "tui_smoke_gate: FAIL — the VT100 target smoke did not pass"
    exit 1
fi

neg=$( ( ulimit -t 120; timeout 60 python3 scripts/tui_pty.py scripts/tui_smoke_negative.mad ) 2>&1 )
if [ $? -eq 0 ]; then
    echo "$neg"
    echo "tui_smoke_gate: FAIL — the negative control PASSED (the harness detects nothing)"
    exit 1
fi

# Terminal death is readable PROGRESS (fd_readable_progress_probe): the
# app must EXIT when its pty master closes, never spin in the input wait.
eofout=$( ( ulimit -t 60; timeout 30 python3 scripts/tui_eof_pty.py ) 2>&1 )
if [ $? -ne 0 ]; then
    echo "$eofout"
    echo "tui_smoke_gate: FAIL — terminal death did not end the input wait"
    exit 1
fi

echo "tui_smoke_gate: PASS (pty smoke + negative control + terminal-death exit)"
