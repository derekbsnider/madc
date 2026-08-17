#!/bin/bash
# warn_scan_lanes.sh — prove the zero-warnings law on EVERY build lane, from
# scratch. Runs on the BUILD HOST (the container); the NAS never builds.
#
# Owner law (2026-08-14): no warnings are allowed anywhere. -Werror enforces it
# at compile time, so a warning is normally a build failure — but that only
# holds for code the compiler actually SEES. This script exists because of how
# the law was nearly declared satisfied while it wasn't:
#
#   An INCREMENTAL build compiles only the objects it touches, so each run
#   reports a different subset of warnings. On 2026-08-14 two successive
#   incremental darwin builds reported three DIFFERENT unique warnings each,
#   and only a full wipe-and-rebuild showed the real set. A lane can look
#   clean and be carrying warnings in files that simply did not recompile.
#
# So: wipe each mode's object tree, rebuild it, and report the count. With
# -Werror active a nonzero count should be impossible — which is the point.
# If a lane ever reports warnings AND still builds, -Werror is not reaching
# that lane's compile lines and the gate is vacuous there.
#
# Usage:
#   bash scripts/warn_scan_lanes.sh [lane ...]      (default: every lane)
#   bash scripts/warn_scan_lanes.sh --list
#
# Exit status: 0 only if every scanned lane built AND reported zero warnings.

set -u

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root" || exit 1

# label:make-target:objdirs   — an empty target means the default goal.
# objdirs is comma-separated because the Apple lanes build a cross compiler
# (which RUNS here and TARGETS the Mac) plus the hosted product, in separate
# per-mode trees; wiping only one leaves the other incremental.
LANES="
host::develop
release:release:release
debug:debug:debug
win64:hosted-x86-64-windows:hosted-x86-64-windows
arm64-macos:hosted-arm64-macos:hosted-arm64-macos,cross-arm64-macos
x86-64-macos:hosted-x86-64-macos:hosted-x86-64-macos,cross-x86-64-macos
"

lane_labels() { echo "$LANES" | sed '/^$/d' | cut -d: -f1; }

if [ "${1:-}" = "--list" ]; then
	lane_labels
	exit 0
fi

want="$*"
[ -z "$want" ] && want="$(lane_labels | tr '\n' ' ')"

log_dir="$repo_root/tmp/warn-scan"
mkdir -p "$log_dir"

rc_total=0
summary=""

for lane in $want; do
	row="$(echo "$LANES" | sed '/^$/d' | grep "^$lane:")"
	if [ -z "$row" ]; then
		echo "warn_scan_lanes: unknown lane '$lane' — try --list" >&2
		exit 2
	fi
	target="$(echo "$row" | cut -d: -f2)"
	objdirs="$(echo "$row" | cut -d: -f3 | tr ',' ' ')"
	log="$log_dir/$lane.log"

	for d in $objdirs; do
		rm -rf "$repo_root/obj/$d"
	done

	echo "=== $lane (clean) ==="
	# Capped like every other automated invocation in this repo, so a hang
	# fails the scan instead of pegging the host.
	( ulimit -t 9000; timeout 7200 make -C "$repo_root/src" -j8 $target ) \
		> "$log" 2>&1
	build_rc=$?

	warns=$(grep -c "warning:" "$log")
	echo "$lane: build rc=$build_rc warnings=$warns  ($log)"

	if [ "$build_rc" -ne 0 ] || [ "$warns" -ne 0 ]; then
		rc_total=1
		echo "--- first warnings/errors in $lane ---" >&2
		grep -E "warning:|error:" "$log" | head -10 >&2
	fi
	summary="$summary
  $lane rc=$build_rc warnings=$warns"
done

echo "=== warn_scan_lanes summary ===$summary"

if [ "$rc_total" -eq 0 ]; then
	echo "warn_scan_lanes: OK — every scanned lane built clean with ZERO warnings"
else
	echo "warn_scan_lanes: FAILED — a lane did not build, or emitted warnings" >&2
fi
exit "$rc_total"
