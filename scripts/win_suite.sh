#!/bin/bash
# Run the integration JIT suite on the REAL Windows host from one staged tree.
# The Linux runner remains the sole fixture/counting owner; MADC_WRAPPER sends
# each madc invocation through win_run.sh's stage-once mode. This is native
# Windows execution (PE loader / ntdll / UCRT), not Wine.
#
# Usage: win_suite.sh [test-basename-glob ...]
#
# Knobs: MADC_WIN_SSH (default derek@host.docker.internal),
#        MADC_WIN_DIR (default /mnt/c/Users/Public/madcwin),
#        MADC_WIN_TIMEOUT (per invocation, default 120),
#        MADC_WIN_KEEP=1 (keep the staged tree),
#        MADC_BIN (packed PE to stage; default release Windows artifact).
set -u
cd "$(dirname "$0")/.."

WIN_SSH="${MADC_WIN_SSH:-derek@host.docker.internal}"
WIN_BASE="${MADC_WIN_DIR:-/mnt/c/Users/Public/madcwin}"
TIMEOUT="${MADC_WIN_TIMEOUT:-120}"
PRODUCT="${MADC_BIN:-bin/madc-release-x86-64-windows.exe}"
STAGE="$WIN_BASE/suite.$$.$RANDOM"

if [ ! -f "$PRODUCT" ]; then
	echo "win_suite: $PRODUCT missing — run make -C src release-windows first" >&2
	exit 1
fi

cleanup()
{
	if [ "${MADC_WIN_KEEP:-0}" != 1 ]; then
		ssh -o BatchMode=yes "$WIN_SSH" "rm -rf '$STAGE'" >/dev/null 2>&1
	fi
}
trap cleanup EXIT

if ! ssh -o BatchMode=yes "$WIN_SSH" "mkdir -p '$STAGE/bin'"; then
	echo "win_suite: channel down ($WIN_SSH)" >&2
	exit 3
fi
if ! scp -q -o BatchMode=yes "$PRODUCT" "$WIN_SSH:$STAGE/bin/madc.exe"; then
	echo "win_suite: compiler copy failed" >&2
	exit 3
fi

# PE runtime adjacency is discovered by convention, matching win_run.sh and
# the release package. No DLL-name table belongs in the suite runner.
runtime_files=()
for runtime_file in "$(dirname "$PRODUCT")"/*.dll; do
	if [ -f "$runtime_file" ]; then
		runtime_files+=("$runtime_file")
	fi
done
if [ ${#runtime_files[@]} -gt 0 ]; then
	if ! scp -q -o BatchMode=yes "${runtime_files[@]}" "$WIN_SSH:$STAGE/bin/"; then
		echo "win_suite: runtime DLL copy failed" >&2
		exit 3
	fi
fi
# The suite's inputs beyond tests/: the editor tests include
# ../tools/texteditor/lined_core.inc and the ui tests read
# examples/adventure/adventure.world — the same trees the Mac suite stage
# carries. A missing tree fails six tests as "Failed to open include file" /
# "cannot read" on the box while wine, run from the repo root, never sees it.
if ! scp -q -r -o BatchMode=yes tests tools examples "$WIN_SSH:$STAGE/"; then
	echo "win_suite: test/tools/examples tree copy failed" >&2
	exit 3
fi

echo "win_suite: staged $PRODUCT + tests at $WIN_SSH:$STAGE"
MADC_WIN_STAGE="$STAGE" \
MADC_WIN_SSH="$WIN_SSH" \
MADC_WIN_TIMEOUT="$TIMEOUT" \
MADC_FOREST_ENV_CHECK=0 \
MADC_BIN=bin/madc.exe \
MADC_WRAPPER="bash scripts/win_run.sh" \
MADC_SKIP_EXT="${MADC_SKIP_EXT:-win64 win}" \
	bash scripts/run_tests.sh "$@"
rc=$?
if [ "$rc" -eq 0 ]; then
	echo "win_suite: native Windows suite OK"
else
	echo "win_suite: native Windows suite FAILED (rc=$rc)" >&2
fi
exit "$rc"
