#!/bin/bash
# Gate: an fd is readable-progress when POLLIN OR POLLHUP OR POLLERR — EOF
# and errors ARE progress the read surfaces (DupFamily
# fd_readable_progress_probe; the divergent copy was ui_term.cpp's
# input_ready, whose POLLIN-only test made a dead terminal "not readable"
# so the EOF-surfacing read never ran). Every readable test in src/ either
# spells the full triple or routes through taskio's io_probe_readable.
# Marker: a BARE `revents & POLLIN)` test (the full-triple sites spell
# `revents & (POLLIN | ...`) — zero matches allowed.
set -u
cd "$(dirname "$0")/.."

fail() { echo "check-fd-readable-progress: FAIL — $1"; exit 1; }

# Negative control: the triple-spelling sites must exist (the marker's
# complement matches the known-good rule), or the marker has rotted.
grep -q "POLLIN | POLLHUP | POLLERR" src/madc_task_chan.cpp \
    || fail "negative control broken: the readable-progress triple no longer matches its owner (io_probe_readable)"

bare=$(grep -n "revents & POLLIN)" src/*.cpp)
[ -z "$bare" ] \
    || fail "bare POLLIN readable test (EOF/error would not count as progress) — spell the (POLLIN | POLLHUP | POLLERR) triple or route through io_probe_readable:
$bare"

echo "check-fd-readable-progress: OK (no bare POLLIN readable tests)"
