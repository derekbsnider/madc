#!/bin/bash
# release_candidate.sh — a release candidate's validation and packaging ON THE
# BUILD CONTAINER, one job at a time, in the order the release procedure
# (.claude/commands/release-candidate.md) fixes. Cheapest and most telling
# first:
#   diff         the candidate against the previous release (release_diff.sh;
#                its packages stage runs later, once both asset sets exist)
#   fast         build + the commit tier (fast_lanes.sh, which records itself)
#   battery      the seam battery (seam_battery.sh is NAS-driven: this stage
#                only REMINDS — run it from the NAS, never here)
#   lanes        wine64 JIT, interactive (Linux), genuine Windows + its
#                interactive subset (win_suite.sh), the brew lane
#   conformance  the release binary and conformance_measure.sh --record
#   packages     madc's packages (release_artifacts.sh), the release archive
#                and its callgrind map (release_bins.sh archive/map/check)
#   products     madcide + Chthonia on those packages (release_products.sh;
#                sync the trees first: mac_release.sh products-sync, NAS)
# The Mac lanes and products, the rehearsal and the release-asset staging run
# from the NAS (mac_release.sh, release_rehearsal.sh, release_stage.sh).
#
#   bash scripts/release_candidate.sh [stage...]   (default: every stage, in order)
#
# Every stage prints its tallies; record each green on the NAS
# (lane_ledger.sh record <lane> "<tally>") at the release commit. Run it from
# a file with stdin closed (win_suite.sh reads stdin):
#   bash scripts/release_candidate.sh < /dev/null > tmp/logs/rc-<V>.log 2>&1
set -u
cd "$(dirname "$0")/.." || exit 9
V=$(cat VERSION)
L=tmp/logs/rc-$V
J=$(bash scripts/build_jobs.sh)
mkdir -p tmp/logs
step() { echo "=== $1 ($(date -u +%T))"; }

diff_prev() {
	step "diff (v$V against the previous release)"
	make -C src -j"$J" release > "$L-relbuild.log" 2>&1; echo "release build rc=$?"
	bash scripts/release_diff.sh > "$L-diff.log" 2>&1; echo "release_diff rc=$?"
	sed -n '/^=== release_diff/,$p' "$L-diff.log"
}

fast() {
	step build
	make -C src -j"$J" > "$L-build.log" 2>&1; echo "build rc=$?"
	step fast-lanes
	bash scripts/fast_lanes.sh > "$L-fast.log" 2>&1; echo "fast rc=$?"
	grep -aE "^(c-testsuite|c-torture|c2mir-tests|gui|index-c|gxx-c\+\+11|commonmark)[: ]" "$L-fast.log" | tail -12
}

battery() {
	step battery
	echo "the seam battery is NAS-driven: on the NAS, bash scripts/seam_battery.sh (after the sync and the post-sync steps)"
}

lanes() {
	local globs
	step wine64
	{ make -C src -j"$J" hosted-x86-64-windows && make -C src -j"$J" modules-windows; } > "$L-wine-build.log" 2>&1
	echo "wine build rc=$?"
	WINEDEBUG=-all wineserver -p < /dev/null > /dev/null 2>&1
	WINEDEBUG=-all MADC_BIN=bin/madc-hosted-x86-64-windows.exe MADC_WRAPPER=wine MADC_SKIP_EXT='win64 wine64' \
		bash scripts/run_tests.sh > "$L-wine.log" 2>&1 < /dev/null
	echo "wine64 rc=$?"
	grep -aE "passed|^FAIL" "$L-wine.log" | tail -4

	step interactive
	bash scripts/interactive_lane.sh > "$L-interactive.log" 2>&1 < /dev/null
	echo "interactive rc=$?"
	grep -aE "passed|^FAIL|units" "$L-interactive.log" | tail -5

	step genuine-win
	make -C src -j"$J" release-windows > "$L-winbuild.log" 2>&1; echo "release-windows rc=$?"
	bash scripts/win_suite.sh > "$L-genwin.log" 2>&1 < /dev/null
	echo "genuine-win rc=$?"
	grep -aE "passed|^FAIL" "$L-genwin.log" | tail -4

	step interactive-win
	globs=$(bash scripts/interactive_lane.sh --globs)
	# shellcheck disable=SC2086
	bash scripts/win_suite.sh $globs > "$L-genwin-int.log" 2>&1 < /dev/null
	echo "interactive-win rc=$?"
	grep -aE "passed|^FAIL" "$L-genwin-int.log" | tail -3

	step brew
	bash scripts/brew_lane.sh > "$L-brew.log" 2>&1 < /dev/null
	echo "brew rc=$?"
	grep -aE "passed|^FAIL|brew_lane" "$L-brew.log" | tail -6
}

conformance() {
	step conformance
	make -C src -j"$J" release > "$L-relbuild.log" 2>&1; echo "release build rc=$?"
	bash scripts/conformance_measure.sh --release "v$V" --madc bin/madc-release --record > "$L-conf.log" 2>&1
	echo "conformance rc=$?"
	tail -n 12 "$L-conf.log"
	echo "copy docs/parity/conformance-ledger.tsv back to the NAS checkout"
}

packages() {
	step packages-linux
	rm -rf "tmp/v$V-artifacts"
	bash scripts/release_artifacts.sh linux || exit 4
	step archive
	bash scripts/release_bins.sh archive > "$L-archive.log" 2>&1; echo "archive rc=$?"
	tail -n 4 "$L-archive.log"
	cat "tmp/release-bins/v$V/PROVENANCE"
	step packages-other
	bash scripts/release_artifacts.sh windows macos jammy || exit 6
	step map
	bash scripts/release_bins.sh map "v$V" > "$L-map.log" 2>&1; echo "map rc=$?"
	tail -n 8 "$L-map.log"
	bash scripts/release_bins.sh check; echo "check rc=$?"
}

products() {
	step products
	bash scripts/release_products.sh
}

for s in ${*:-diff fast battery lanes conformance packages products}; do
	case "$s" in
	diff) diff_prev ;;
	fast|battery|lanes|conformance|packages|products) "$s" ;;
	*) echo "release_candidate: unknown stage '$s'" >&2; exit 2 ;;
	esac
done
step all-done
