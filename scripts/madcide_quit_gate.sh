#!/bin/bash
# madcide_quit_gate (in fulltest): madcide EXITS when told to quit with its
# REPL tab live. The real TUI on a pty (scripts/madcide_quit_pty.py): F5
# runs the buffer in the REPL tab's session, then ^K q must end the process
# within 10s. The pumps a session spawns are tasks inside run_tui's serve
# scope, so its join waits on them; IdeSession::stop_tasks (called for every
# client by run_ide's teardown) is what stops them first. Then the NEGATIVE
# CONTROL: the same run with no quit sent must FAIL, proving the exit check
# detects a process that is still running.
cd "$(dirname "$0")/.." || exit 1

if ! command -v python3 >/dev/null 2>&1; then
    echo "madcide_quit_gate: python3 missing (provision_container.sh installs it)"
    exit 1
fi

out=$( ( ulimit -t 120; timeout 120 python3 scripts/madcide_quit_pty.py ) 2>&1 )
if [ $? -ne 0 ]; then
    echo "$out"
    echo "madcide_quit_gate: FAIL — madcide did not exit after F5 then quit"
    exit 1
fi

neg=$( ( ulimit -t 120; timeout 120 python3 scripts/madcide_quit_pty.py --no-quit ) 2>&1 )
if [ $? -eq 0 ]; then
    echo "$neg"
    echo "madcide_quit_gate: FAIL — the negative control PASSED (the exit check detects nothing)"
    exit 1
fi

echo "madcide_quit_gate: PASS ($out; negative control: no quit, still running)"
