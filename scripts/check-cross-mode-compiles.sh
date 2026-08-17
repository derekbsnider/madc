#!/bin/bash
# check-cross-mode-compiles.sh — the cross-* build modes are in NO validation
# lane, so a cross-ONLY code arm can break for days while every suite is green.
#
# The incident (2026-08-14, found by the POSIX P1/P2 pre-merge audit):
# madc_cir.cpp used madc::detail::resolve_real_path inside an
# #ifdef MADC_CROSS_TARGET block without including madc_posix_io.h. A host
# build never compiles that block, so fulltest, the libc++ JIT lane, --exe,
# --obj and the wine suite were ALL green while `make hosted-arm64-macos`
# failed rc=2 — for two days, across a release. The macOS release artifacts
# build through exactly that mode.
#
# This gate compiles the cross arms and nothing else, so the class costs
# seconds instead of a full cross build.
#
# The TU list is DERIVED from the markers, never hardcoded (design-principles
# rule #7): a new TU that grows a cross arm is covered the day it is written,
# with no edit here.
#
# Negative control: revert the madc_posix_io.h include in src/madc_cir.cpp and
# this gate must FAIL naming that TU. Verified 2026-08-14.

set -u

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root" || exit 1

say() { echo "check_cross_mode_compiles: $1"; }
fail() { echo "check_cross_mode_compiles: $1" >&2; exit 1; }

# The concept is "a translation unit with a cross-target-only code arm", not
# one spelling of it — match either macro the cross modes define.
srcs=$(grep -lE 'MADC_CROSS_TARGET|MADC_CROSS_APPLE' src/*.cpp 2>/dev/null | sort)

if [ -z "$srcs" ]; then
	fail "no TU matched the cross-arm markers — the MARKER is stale, not the tree (a null here is a broken gate, not a pass)"
fi

mode="cross-arm64-macos"
objs=""
count=0
for s in $srcs; do
	b=$(basename "$s" .cpp)
	objs="$objs ../obj/$mode/$b.o"
	count=$((count + 1))
done

log=$(mktemp "${TMPDIR:-/tmp}/cross-mode-compiles.XXXXXX")
trap 'rm -f "$log"' EXIT

if ! make -C src MODE="$mode" $objs > "$log" 2>&1; then
	echo "--- $mode compile output ---" >&2
	grep -E "error:|Error [0-9]" "$log" >&2 | head -20
	fail "the $mode arms do not compile — a cross-only block references something the host build never sees"
fi

say "OK — $count cross-arm TU(s) compile in $mode"
