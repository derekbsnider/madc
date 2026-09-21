#!/bin/bash
# fast_lanes.sh — the FAST conformance tier: run it after every commit.
#
# Owner directive 2026-09-20: test suites are tiered BY TIME TO RUN. A suite
# that finishes in minutes or less runs after every commit; the multi-hour
# lanes gate merges and releases. This script is the fast tier.
#
#   c-testsuite   ~4s     220 single-exec C programs, compile+run+stdout
#   c-torture     ~4min   gcc.c-torture/execute, the standard-C conformance set
#   gxx-c++11     ~85s    g++.dg compile-clean subset (a ROADMAP metric, never
#                         a gate — owner ruling 2026-09-04 — but cheap, so it
#                         is measured here rather than drifting)
#
# Total well under six minutes. The reason this exists: gcc c-torture had no
# lane script and no ledger row, so nothing re-ran it for five weeks and it
# drifted 1614 -> 1587 passing while eight other lanes stayed green, carrying
# three standard-C regressions including a parser SIGSEGV. The suite was never
# expensive — it was simply unowned.
#
# Each GREEN lane is recorded in the ledger, so `lane_ledger.sh check --commit`
# reports the fast tier's freshness the same way --promote and --release report
# theirs. A RED gated lane exits nonzero.
#
#   MADC_BIN            binary under test (default bin/madc)
#   MADC_FAST_NO_RECORD=1   run and report, do not touch the ledger
set -u
cd "$(dirname "$0")/.."

BIN="${MADC_BIN:-bin/madc}"
if [ ! -x "$BIN" ]; then
	echo "fast_lanes: $BIN missing — build first" >&2
	exit 1
fi

mkdir -p tmp/logs
STAMP=$(date +%Y%m%d-%H%M%S)
rc_total=0

# run_lane <ledger-name> <label> <gated:yes|no> <script...>
run_lane() {
	local lane=$1 label=$2 gated=$3; shift 3
	local log=tmp/logs/fast-$lane-$STAMP.log
	local t0 t1 rc
	t0=$(date +%s)
	"$@" > "$log" 2>&1
	rc=$?
	t1=$(date +%s)
	local tally
	# FIXED-STRING match, never -E: a lane name carries metacharacters and
	# ERE reads "gxx-c++11" as quantifiers, silently matching nothing and
	# recording an EMPTY tally for a green lane. lane_ledger.sh carries the
	# same warning over its own row-drop; this is the second instance.
	tally=$(grep -F -- "$label: " "$log" | head -2 | tr '\n' ' ')
	[ -z "$tally" ] && tally="(no summary — see $log)"
	printf '%-12s rc=%-2s %4ss  %s\n' "$lane" "$rc" "$((t1 - t0))" "$tally"
	if [ "$rc" -eq 0 ]; then
		if [ "${MADC_FAST_NO_RECORD:-0}" != "1" ]; then
			bash scripts/lane_ledger.sh record "$lane" \
				"$tally— fast tier, $log" > /dev/null
		fi
	else
		sed 's|^|    |' "$log" | tail -12 >&2
		[ "$gated" = yes ] && rc_total=1
	fi
}

echo "=== fast lanes ($(git rev-parse --short HEAD), $(date -u +%FT%TZ)) ==="
run_lane c-testsuite c-testsuite yes bash scripts/c_testsuite_lane.sh
run_lane c-torture   c-torture   yes bash scripts/c_torture_lane.sh
run_lane gxx-c++11   gxx-c++11   no  bash scripts/gxx_lane.sh

if [ "$rc_total" -ne 0 ]; then
	echo "fast_lanes: RED — a gated fast lane failed above" >&2
else
	echo "fast_lanes: GREEN"
fi
exit $rc_total
