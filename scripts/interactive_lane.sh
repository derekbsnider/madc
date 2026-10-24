#!/bin/bash
# interactive_lane.sh — the INTERACTIVE lane (owner 2026-10-12: a change that
# only affects REPL mode must not need a day of testing every part of madc).
#
# The code only `madc -i`, `madc --lsp` and a dialect host (madcide) reach —
# lane_ledger.sh's INTERACTIVE_CODE: the REPL loops, the session and its
# backend wire, the ex core and its dialect face, the history store — is
# tested by what exercises it: the REPL, colon-mode, session and LSP tests of
# tests/ (JIT; the REPL is JIT-only, D19) and the interactive unit binaries.
# A change confined to INTERACTIVE_CODE stales only the interactive lanes;
# any other code change stales them with every other lane.
#
# Usage: scripts/interactive_lane.sh
#   the tests/ subset through run_tests.sh (MADC_BIN as there), then the unit
#   binaries (built here; MADC_INTERACTIVE_UNITS=no skips them). It prints the
#   tally; record it in the NAS checkout's ledger (lane_ledger.sh record
#   interactive ...), as the remote fast lanes' are. A platform runner drives
#   the same subset through its own suite script: win_suite.sh
#   $INTERACTIVE_GLOBS (interactive-win), run_tests.sh on the Macs
#   (interactive-darwin).
set -u
cd "$(dirname "$0")/.." || exit 2

# The tests that drive the interactive code, and those that parse
# <ns_madc_host> (a host program's faces: doc_*, tool_host, ex_session).
INTERACTIVE_GLOBS="testrepl_* testex_* testsession_* testlsp_* testdocument*
	testlang_doc testtool_host"
INTERACTIVE_UNITS="test_ex test_repl_session test_session_backend test_repl_cli
	test_repl_input test_lsp test_document"
# --globs: print the subset, for a platform runner's own suite script
# (scripts/release_candidate.sh: win_suite.sh $(interactive_lane.sh --globs)).
if [ "${1:-}" = --globs ]; then
	echo $INTERACTIVE_GLOBS
	exit 0
fi

# shellcheck disable=SC2086
suite=$(bash scripts/run_tests.sh $INTERACTIVE_GLOBS 2>&1)
src=$?
printf '%s\n' "$suite" | tail -3
tally=$(printf '%s\n' "$suite" | grep -E '^[0-9]+ passed' | tail -1)
units_ok=0 units_red=0
if [ "${MADC_INTERACTIVE_UNITS:-yes}" != no ]; then
	targets=""
	for u in $INTERACTIVE_UNITS; do targets="$targets ../bin/$u"; done
	# shellcheck disable=SC2086
	make -C src $targets > /dev/null || { echo "interactive_lane: unit build failed" >&2; exit 1; }
	for u in $INTERACTIVE_UNITS; do
		if timeout 900 "bin/$u" > /dev/null 2>&1; then
			units_ok=$((units_ok + 1))
		else
			units_red=$((units_red + 1)); echo "interactive_lane: RED unit $u" >&2
		fi
	done
	tally="$tally; units $units_ok/$units_red"
fi
echo "interactive_lane: $tally"
[ "$src" -eq 0 ] && [ "$units_red" -eq 0 ]
