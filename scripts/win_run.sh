#!/usr/bin/env bash
# win_run.sh — run a Windows executable on the REAL Windows host and relay
# its output.  This is the MADC_WIN_RUNNER for win_ucrt_gate.sh and the
# win battery legs:
#
#   MADC_WIN_RUNNER="bash scripts/win_run.sh" bash scripts/win_ucrt_gate.sh
#
# Channel (docs/plans/2026-08-12-windows-release-lane.md, W0.2): the build
# container reaches the owner's Ubuntu WSL distro sshd at host.docker.internal
# (Docker Desktop VM topology); WSL interop then runs the .exe as a GENUINE
# Windows process (real PE loader / ntdll / ucrtbase — not wine).
#
# Usage: win_run.sh prog.exe [args...]
#   - prog.exe and every argument naming an existing local file are copied to
#     a per-invocation directory under a Windows-visible /mnt/c path (a WSL
#     cwd would be a UNC path Windows rejects); those args are rewritten to
#     their basenames.
#   - stdout/stderr are relayed; the remote exit code is returned when the
#     channel delivers one.  A crashed Windows process dies SILENTLY (WER
#     swallows the banner) and interop exit codes are unreliable — callers
#     classify by OUTPUT MARKER, not rc (same rule as the Mac battery).
#   - args containing whitespace are not supported (gate/battery never use
#     them); keep invocations to simple tokens.
#
# Knobs: MADC_WIN_SSH (default derek@host.docker.internal),
#        MADC_WIN_DIR (default /mnt/c/Users/Public/madcwin),
#        MADC_WIN_TIMEOUT (seconds, default 120),
#        MADC_WIN_KEEP=1 to keep the per-invocation directory for debugging.
set -u

WIN_SSH="${MADC_WIN_SSH:-derek@host.docker.internal}"
WIN_BASE="${MADC_WIN_DIR:-/mnt/c/Users/Public/madcwin}"
TIMEOUT="${MADC_WIN_TIMEOUT:-120}"

if [ $# -lt 1 ]; then
	echo "usage: win_run.sh prog.exe [args...]" >&2
	exit 2
fi

dir="$WIN_BASE/run.$$.$RANDOM"
files=()
cmd=()
for a in "$@"; do
	if [ -f "$a" ]; then
		files+=("$a")
		cmd+=("$(basename "$a")")
	else
		cmd+=("$a")
	fi
done
case "${cmd[0]}" in
*/*) ;;
*) cmd[0]="./${cmd[0]}" ;;
esac

if ! ssh -o BatchMode=yes "$WIN_SSH" "mkdir -p '$dir'"; then
	echo "win_run: channel down ($WIN_SSH)" >&2
	exit 3
fi
if [ ${#files[@]} -gt 0 ]; then
	if ! scp -q -o BatchMode=yes "${files[@]}" "$WIN_SSH:$dir/"; then
		echo "win_run: copy failed" >&2
		exit 3
	fi
fi
timeout "$TIMEOUT" ssh -o BatchMode=yes "$WIN_SSH" "cd '$dir' && ${cmd[*]}"
rc=$?
if [ "${MADC_WIN_KEEP:-0}" != 1 ]; then
	ssh -o BatchMode=yes "$WIN_SSH" "rm -rf '$dir'" >/dev/null 2>&1
fi
exit $rc
