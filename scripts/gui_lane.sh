#!/bin/bash
# gui_lane.sh — the GUI test lane: tests/gui under a virtual display.
#
# tests/gui exercises the ui web target's surfaces that need a real window
# (the page and its edit nodes, dialogs, the clipboard, the native menu bar,
# theming, the workbench regions), so it runs under Xvfb against a freshly
# built ui_web module. madcide's own window tests live in its repository.
# All three execution lanes run — JIT, --exe and --obj — because a GUI
# module binds differently in each.
#
# WHY IT IS A SCRIPT AND NOT AN INLINE STAGE: this body lived only inside
# remote_build.sh's `gui` stage, which is why the lane had no ledger row for
# weeks — the same blind spot that let gcc c-torture drift five weeks with no
# lane script and no row. A lane nothing can invoke by name is a lane nothing
# re-runs. remote_build.sh's stage now calls this script, so there is ONE body.
#
# The lane is FULL GREEN or RED — no baseline. Every tests/gui test passes on
# every lane today; a failure here is a regression, not a known gap.
#
#   MADC_BIN   binary under test (default bin/madc)
set -u
cd "$(dirname "$0")/.."

BIN="${MADC_BIN:-bin/madc}"

if [ ! -x "$BIN" ]; then
	echo "gui_lane: $BIN missing — build first" >&2
	exit 1
fi
# A missing virtual display is an ERROR, never a skip. A lane that reports
# green because it could not run is the failure mode this whole ledger exists
# to prevent: run it on a host that has Xvfb.
if ! command -v xvfb-run > /dev/null 2>&1; then
	echo "gui_lane: no xvfb-run on this host — the GUI lane cannot run here" >&2
	echo "gui_lane: run it on the desktop container (scripts/remote_build.sh gui)" >&2
	exit 2
fi

mkdir -p tmp/logs
out=tmp/logs/gui-lane-$(date +%Y%m%d-%H%M%S).log

# The module the tests import must be built from CURRENT content.
if ! make -C src ui_web > "$out" 2>&1; then
	echo "gui_lane: ui_web module build failed — see $out" >&2
	tail -20 "$out" >&2
	exit 1
fi

# The runner exports the memory guard's `auto`; a GUI-module test lifts it
# itself at run start (import ui_web — the module's GUI flag), so no
# memory override belongs here. The CPU and wall caps stay.
(
	ulimit -t 30
	MADC_BIN="$BIN" MADC_TEST_DIR=tests/gui MADC_FAIL_DETAIL=20 \
		timeout -k 3 300 xvfb-run -a bash scripts/run_tests.sh --exe --obj
) >> "$out" 2>&1
rc=$?

jit=$(grep -E '^[0-9]+ passed,' "$out" | tail -1)
exe=$(grep -E '^EXE: ' "$out" | tail -1)
obj=$(grep -E '^OBJ: ' "$out" | tail -1)
pass=${jit%% passed,*}

echo "gui: ${jit:-(no summary)} | ${exe:-no EXE line} | ${obj:-no OBJ line}"

# run_tests.sh refuses a run that executed nothing (rc 1), so a wrong
# MADC_TEST_DIR is red through rc; a missing summary means the runner died.
case "$pass" in
	''|*[!0-9]*)
		echo "gui: RED — no runner summary in $out" >&2
		exit 1;;
esac
if [ "$rc" -ne 0 ]; then
	echo "gui: RED — runner rc=$rc, see $out" >&2
	exit 1
fi
exit 0
