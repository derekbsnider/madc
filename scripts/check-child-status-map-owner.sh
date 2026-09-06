#!/usr/bin/env bash
# check-child-status-map-owner.sh — the waitpid-status → exit-code mapping
# has ONE owner.
#
# Rule (DupFamily child_status_exit_mapping, consolidated with MT-3b):
# mapping a reaped child's status to the 128+signal exit shape happens only
# in map_child_status (madc_process.cpp) — Process::wait and
# Process::wait_or_kill both route through it. run_and_wait deliberately
# stays apart (its contract turns abnormal termination into -1 + err, and
# it carries no signal mapping). A new reap path is where a copy is born.
#
# Marker: `128 + WTERMSIG` in src/*.cpp — exactly one (the owner).
set -u

SRCDIR="$(dirname "$0")/../src"
MARKER='128 + WTERMSIG'

n=$(grep -rF "$MARKER" "$SRCDIR" --include='*.cpp' --include='*.c' 2>/dev/null | wc -l | tr -d ' ')
if [ "$n" -ne 1 ]; then
	echo "check-child-status-map-owner: FAIL — $n '$MARKER' site(s) in" \
	     "src/ (expected 1: map_child_status). A new reap path must" \
	     "route through it — see the DupFamily" \
	     "child_status_exit_mapping." >&2
	exit 1
fi

# Negative control: a synthetic second mapping must be detected.
tmp=$(mktemp --suffix=.cpp)
echo "	status = 128 + WTERMSIG(child_status);	// synthetic" > "$tmp"
if [ "$(grep -cF "$MARKER" "$tmp")" -ne 1 ]; then
	rm -f "$tmp"
	echo "check-child-status-map-owner: FAIL — negative control did not" \
	     "match a synthetic copy (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

echo "check-child-status-map-owner: OK (one mapping owner: map_child_status)"
exit 0
