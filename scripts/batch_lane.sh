#!/bin/bash
# batch_lane.sh — the BATCH tier: the whole tests/ suite, JIT only, once per
# BATCH of fixes, never per fix (owner 2026-09-28), on the -O2 madc-release
# (owner 2026-10-03: full suites run the -O2 build — about 3x the dev
# binary's speed on a C++ test).
#
# The fast tier (scripts/fast_lanes.sh) runs per commit but has no lane over
# tests/*.mad; the seam battery (fulltest) is the only other run of it. A
# BUGS.md burn-down of ~50 fix commits, every one Tier 1 + Tier 2 green,
# carried four tests/ regressions (a defaulted vector move, a returned var
# literal, a libc++ string_view conversion) until one JIT run found them in
# under ten minutes (557 s measured). That run is this lane.
#
# A green run records the `tests-jit` row (promote=batch) in the ledger.
# `lane_ledger.sh check` prints a BATCH reminder while that row is stale; it
# never blocks a commit (the batch, not the fix, is the unit).
#
#   MADC_BIN                 binary under test (default bin/madc-release;
#                            build it with make -C src release)
#   MADC_BATCH_NO_RECORD=1   run and report, do not touch the ledger (the
#                            remote_build.sh `batch` stage: a row written on
#                            the rsync copy gates nothing)
set -u
cd "$(dirname "$0")/.."

BIN="${MADC_BIN:-bin/madc-release}"
if [ ! -x "$BIN" ]; then
	echo "batch_lane: $BIN missing — make -C src release first" >&2
	exit 1
fi
export MADC_BIN="$BIN"

mkdir -p tmp/logs
log=tmp/logs/batch-tests-jit-$(date +%Y%m%d-%H%M%S).log
echo "=== batch lane ($(git rev-parse --short HEAD 2>/dev/null), $(date -u +%FT%TZ)) ==="
t0=$(date +%s)
bash scripts/run_tests.sh > "$log" 2>&1
rc=$?
t1=$(date +%s)
tally=$(grep -E '^[0-9]+ passed, [0-9]+ failed, ' "$log" | tail -1)
[ -z "$tally" ] && tally="(no summary — see $log)"
grep -E '^(FAIL|TIMEOUT|LEAK|NOISY)' "$log" >&2
printf '%-12s rc=%-2s %4ss  %s\n' tests-jit "$rc" "$((t1 - t0))" "$tally"
if [ "$rc" -eq 0 ] && [ "${MADC_BATCH_NO_RECORD:-0}" != "1" ]; then
	bash scripts/lane_ledger.sh record tests-jit \
		"$tally — batch tier, $log" > /dev/null
fi
exit "$rc"
