#!/usr/bin/env bash
# resume.sh — rehydration preflight. Run this FIRST on every session resume /
# post-compaction, BEFORE editing files or running git.
#
# A compaction summary tells you what you were DOING; it does NOT tell you the
# live ground truth — what's still running, where git actually is, or whether a
# background task committed while you were away. This prints that ground truth
# in one shot so you can re-orient instead of acting on stale assumptions.
#
# The 2026-06-01 incident: orphaned background tasks survived /compact, kept
# relaunching `make fulltest` (hanging test_cir, pegging the host), AND a
# background subagent committed to the branch mid-edit -> a commit/revert/reapply
# collision. This script surfaces all three classes before they bite.
#
# Usage:
#   bash scripts/resume.sh           # report only
#   bash scripts/resume.sh --kill    # also kill runaway test/torture processes

set -u
KILL=0
[ "${1:-}" = "--kill" ] && KILL=1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

bar() { printf '\n=== %s ===\n' "$1"; }

bar "REPO ($ROOT)"
echo "branch:      $(git rev-parse --abbrev-ref HEAD)"
echo "HEAD:        $(git log --oneline -1)"
up="$(git rev-parse --abbrev-ref --symbolic-full-name @{u} 2>/dev/null)"
if [ -n "$up" ]; then
	echo "upstream:    $up -> $(git rev-parse --short "$up" 2>/dev/null)"
	ab="$(git rev-list --left-right --count HEAD..."$up" 2>/dev/null)"
	echo "ahead/behind upstream: ${ab:-unknown}  (left=ahead, right=behind)"
fi
dirty="$(git status --short --untracked-files=no)"
if [ -n "$dirty" ]; then
	echo "UNCOMMITTED tracked changes:"; echo "$dirty"
else
	echo "working tree: clean (tracked)"
fi

bar "RECENT COMMITS + REFLOG (catch concurrent/collision commits you didn't make)"
git log --oneline -5
echo "--- reflog (last 6) ---"
git reflog -6

bar "MIR FORK (/workspace/mir)"
if [ -d /workspace/mir/.git ]; then
	git -C /workspace/mir log --oneline -1
	echo "branch: $(git -C /workspace/mir rev-parse --abbrev-ref HEAD)"
else
	echo "(not a git repo / not present)"
fi

bar "RUNAWAY PROCESSES (test/torture/madc that survived a compaction)"
# Match the heavy, host-pegging offenders by command, excluding this script.
pat='test_cir|test_c2mir|run_gcc_testsuite|fulltest|loopprobe|bin/madc|[ /]c2m '
runaways="$(ps -eo pid,ppid,etimes,pcpu,args 2>/dev/null | grep -E "$pat" | grep -vE 'grep|resume\.sh')"
if [ -n "$runaways" ]; then
	echo "!! FOUND running test/build processes — likely orphans if you just resumed:"
	echo "$runaways"
	if [ "$KILL" = "1" ]; then
		echo "--kill: terminating them..."
		pkill -9 -f 'test_cir' 2>/dev/null
		pkill -9 -f 'test_c2mir' 2>/dev/null
		pkill -9 -f 'run_gcc_testsuite' 2>/dev/null
		pkill -9 -f 'loopprobe' 2>/dev/null
		pkill -9 -f 'fulltest' 2>/dev/null
		echo "done. survivors:"
		ps -eo pid,etimes,pcpu,args 2>/dev/null | grep -E "$pat" | grep -vE 'grep|resume\.sh' || echo "  (clean)"
	else
		echo "   -> re-run with --kill to terminate, OR verify they are a CURRENT run before killing."
	fi
else
	echo "clean — no runaway test/build processes."
fi
echo "load average: $(cat /proc/loadavg 2>/dev/null)"

bar "BUILD FRESHNESS"
if [ -x bin/madc ]; then
	newer="$(find src include -newer bin/madc -name '*.cpp' -o -newer bin/madc -name '*.h' 2>/dev/null | head -3)"
	if [ -n "$newer" ]; then
		echo "bin/madc is STALE — source changed since last build:"; echo "$newer"
		echo "  -> run: make -C src"
	else
		echo "bin/madc up to date with src/ + include/"
	fi
else
	echo "bin/madc missing -> run: make -C src"
fi

bar "NEXT"
echo "Authoritative state + next tasks: docs/superpowers/plans/2026-06-01-HANDOFF.md (READ FIRST)"
echo "Parity worklist: docs/parity/root-cause-worklist.md   |   canonical: claude_status.json"
echo "ALWAYS cap test runs:  ( ulimit -t 120; timeout 180 <cmd> )   — one heavy job at a time."
