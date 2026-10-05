#!/bin/bash
# seam_battery.sh — TIER 3, the merge-wave battery, in ONE command, fastest
# catches first (.claude/rules/testing-fulltest.md).
#
#   bash scripts/seam_battery.sh            # run it (hours become ~an hour)
#   setsid nohup bash scripts/seam_battery.sh > tmp/logs/seam.out 2>&1 &
#
# Owner 2026-10-02: the full tests/ suite runs ONE way at the seam — on the
# shipped artifact, the -O2 packed madc-release, headerless — and the other
# axes ride that same binary on smaller runs. The -O0 dev binary no longer
# runs the full suite here (it stays the per-fix Tier 1/2 and per-batch
# binary, where incremental builds matter). The order is cheapest-first, so
# a red stage costs minutes, not the whole battery:
#
#   0  pre-build every toolchain (Linux dev + release, packed and hosted
#      win64 PEs, both macOS arches, the aarch64-linux cross) + static gates
#   1  gates      unit tests + every repository gate, no suite (make gates)
#   2  headerless the FULL suite, Linux packed binary, no headers on disk
#      ondisk     the headerless-skipped tests, same binary, headers on disk
#   3  headerless-win / ondisk-win   the win64 twins (packed PE under wine)
#   4  exeobj     the FULL suite as native executables and objects (--exe
#                 --obj), on the packed binary — the longest stage, last
#   5  macos      release-macos (both arches; its pack step is the darwin
#                 list's quiet gate) and aarch64-ld
#
# Every stage runs even after a red one (a battery reports the whole picture);
# the exit status is non-zero when any stage was red. Logs: tmp/logs/seam-*.log.
# Records nothing: copy the printed tallies into scripts/lane_ledger.sh on the
# NAS (linux-battery, headerless-win, wine64, macos, aarch64-ld, tests-jit).
set -u
cd "$(dirname "$0")/.." || exit 9
mkdir -p tmp/logs
SSH="ssh -p 2299 -o BatchMode=yes dev@localhost"
red=0
summary=""

note() {	# note <label> <rc>
	summary="$summary  $1 rc=$2"$'\n'
	[ "$2" -eq 0 ] || red=$((red + 1))
}

stage() {	# stage <label> <remote_build stage...>
	local label="$1"; shift
	echo "=== $label ($(date -u +%H:%M:%SZ)) ==="
	bash scripts/remote_build.sh "$@" > "tmp/logs/seam-$label.log" 2>&1
	local rc=$?
	grep -E '^[0-9]+ passed, |^EXE: |^OBJ: |^FAIL|^RED|headerless_suite: (COMPLEMENT|VACUOUS|CONTROL)' \
		"tmp/logs/seam-$label.log" | tail -12
	echo "$label rc=$rc"
	note "$label" "$rc"
}

remote() {	# remote <label> <command run on the container>
	local label="$1" cmd="$2" rc
	echo "=== $label ($(date -u +%H:%M:%SZ)) ==="
	$SSH "cd /workspace/madc; $cmd" > "tmp/logs/seam-$label.log" 2>&1
	rc=$?
	tail -8 "tmp/logs/seam-$label.log"
	echo "$label rc=$rc"
	note "$label" "$rc"
}

# --- 0. pre-build every toolchain, then the static gates ---------------------
stage prebuild sync build release release-win win
remote macos-build '( ulimit -t 3600; timeout 2400 make -C src -j20 -k hosted-x86-64-macos ) && ( ulimit -t 3600; timeout 2400 make -C src -j20 -k hosted-arm64-macos )'
remote aarch64-build '( ulimit -t 3600; timeout 2400 make -C src -j20 cross-aarch64-linux )'
remote static-gates 'fail=0; for g in scripts/check-*.sh; do out=$( ( ulimit -t 300; timeout 300 bash "$g" ) 2>&1 ); rc=$?; if [ $rc -ne 0 ]; then fail=$((fail+1)); echo "RED  $(basename $g) rc=$rc"; echo "$out" | tail -4 | sed "s/^/       /"; fi; done; echo "static gates: $fail red of $(ls scripts/check-*.sh | wc -l)"; [ $fail -eq 0 ]'

# --- 1. the gates (no suite) -------------------------------------------------
stage gates gates

# --- 2. the full suite on the shipped Linux artifact -------------------------
stage headerless headerless
stage ondisk ondisk
# Chthonia built and tested as its own repository builds it (libmadcide and
# the public headers, never the base's sources), on the shipped binary.
remote chthonia 'MADC_BIN=bin/madc-release bash scripts/chthonia_lane.sh'

# --- 3. the win64 twins ------------------------------------------------------
stage headerless-win headerless-win
stage ondisk-win ondisk-win

# --- 4. native artifacts on the shipped binary (longest) ---------------------
stage exeobj exeobj

# --- 5. the platform builds --------------------------------------------------
stage macos release-macos
stage aarch64-ld aarch64-ld

echo "=== seam battery summary ($(date -u +%H:%M:%SZ)) ==="
printf '%s' "$summary"
echo "seam battery: $red red stage(s)"
echo "=== seam battery done ==="
[ "$red" -eq 0 ]
