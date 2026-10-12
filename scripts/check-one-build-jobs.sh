#!/bin/bash
# check-one-build-jobs.sh — a script's parallel build takes its job count from
# scripts/build_jobs.sh (memory-budgeted), never from the cores or a literal.
#
# The build container is the owner's WSL2 VM (16 GB, no cgroup cap): `-j20`
# and `-j$(nproc)` sized builds by cores alone, and two -O2 builds at once
# took the whole WSL down (2026-10-11). A hand job count is `-j<digits>`,
# `-j <digits>` or any `nproc` on a non-comment line of scripts/*.sh.
# Negative control: a synthetic script with `make -j20` and `jobs=$(nproc)`
# must FAIL; the same through build_jobs.sh must PASS.
#
#   scripts/check-one-build-jobs.sh [DIR]   (DIR: the scripts to scan)
set -u
cd "$(dirname "$0")/.." || exit 2

hand_jobs='(-j ?[0-9]+\b|\bnproc\b)'

# Prints the hand job counts in the scripts under $1; returns 0 when none.
scan() {
	local dir=$1 f hits found=0
	for f in "$dir"/*.sh; do
		[ -f "$f" ] || continue
		case "$(basename "$f")" in
			# the owner; the gate; benchmark_lane reports the host's cpus
			build_jobs.sh|check-one-build-jobs.sh|benchmark_lane.sh) continue ;;
		esac
		hits=$(grep -n -E "$hand_jobs" "$f" | grep -v -E '^[0-9]+:[[:space:]]*#')
		if [ -n "$hits" ]; then
			found=1
			printf '%s\n' "$hits" | sed "s|^|  $f:|"
		fi
	done
	return $found
}

mkdir -p tmp
ctl=$(mktemp -d tmp/build-jobs-ctl.XXXXXX) || exit 2
trap 'rm -rf "$ctl"' EXIT
mkdir -p "$ctl/bad" "$ctl/good"
printf 'make -C src -j20 release\njobs=$(nproc)\n' > "$ctl/bad/build.sh"
printf 'make -C src -j"$(bash scripts/build_jobs.sh)" release\n' > "$ctl/good/build.sh"
if scan "$ctl/bad" > /dev/null; then
	echo "check-one-build-jobs: CONTROL FAILED — a hand -j20 / nproc went undetected" >&2
	exit 1
fi
if ! scan "$ctl/good" > /dev/null; then
	echo "check-one-build-jobs: CONTROL FAILED — build_jobs.sh was flagged" >&2
	exit 1
fi

out=$(scan "${1:-scripts}")
if [ -n "$out" ]; then
	echo "check-one-build-jobs: a script sizes a build by hand —" >&2
	echo "use -j\"\$(bash scripts/build_jobs.sh)\" (memory-budgeted):" >&2
	printf '%s\n' "$out" >&2
	exit 1
fi
echo "check-one-build-jobs: OK (one memory-budgeted job count; control both ways)"
