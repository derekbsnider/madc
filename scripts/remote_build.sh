#!/bin/bash
# remote_build.sh — offload build + test batteries to the desktop container.
#
# The QNAP NAS is not the build/test host; the owner's desktop container is
# (reverse SSH tunnel: ssh -p 2299 dev@localhost). This script rsyncs the
# working trees over the tunnel, builds there, and runs the requested
# battery stages, echoing each stage's rc.
#
# Usage:
#   scripts/remote_build.sh [stage ...]
# Stages (default: sync build):
#   sync      rsync madc + mir deltas to the container
#   build     make MIR libmir.a + configure (once) + make -C src
#   unittest  make -C src test
#   fulltest  make -C src fulltest
#   exe       bash scripts/run_tests.sh --exe
#   release   make -C src release
#   packed    MADC_BIN=bin/madc-release bash scripts/run_tests.sh
#   pull      rsync container-built bin/madc (+ madc-release) back to
#             the NAS (ABI-identical userlands; QNAP never compiles)
#   battery   fulltest + exe + release + packed (the push gate)
#   shell     print the ssh command and exit
#
# Every remote invocation is one ssh call running a generated script, so
# local shell hygiene (no && chains per call) is preserved.

REMOTE="dev@localhost"
PORT=2299
SSH="ssh -p $PORT $REMOTE"
LOCAL_MADC=/workspace/madc
LOCAL_MIR=/workspace/mir

stages="$*"
if [ -z "$stages" ]; then
	stages="sync build"
fi
if [ "$stages" = "battery" ]; then
	stages="sync build fulltest exe release packed"
fi
case " $stages " in
	*" shell "*) echo "ssh -p $PORT $REMOTE"; exit 0;;
esac

# MECHANICAL GUARD — refuse to touch the container while it is busy.
#
# Two rules used to depend on the operator remembering them: "never rebuild
# mid-suite (phantom failures)" and "one heavy container job at a time". Both
# were broken in the same stretch on 2026-07-27 — a relink landed under a live
# suite, then a second suite started beside it — and the damage READ AS GREEN
# (rc=0 twice, both untrustworthy, both discarded). A green you cannot trust is
# worse than a red, so this is a refusal, not a warning.
#
# MADC_ALLOW_CONCURRENT=1 overrides it. If you set that, you own the result.
container_busy_check() {
	case " $stages " in
		*" shell "*) return 0;;
	esac
	if [ "${MADC_ALLOW_CONCURRENT:-0}" = "1" ]; then
		echo "=== container busy-check SKIPPED (MADC_ALLOW_CONCURRENT=1) ==="
		return 0
	fi
	busy=$($SSH 'pgrep -fa "run_tests\.sh|make -C .*/src|make -C /workspace/madc" 2>/dev/null | grep -v pgrep' 2>/dev/null)
	if [ -n "$busy" ]; then
		echo "REFUSING: a build or test run is already live in the container." >&2
		printf '%s\n' "$busy" | sed 's/^/  /' >&2
		echo "" >&2
		echo "Rebuilding or starting a second suite now produces phantom" >&2
		echo "passes AND phantom failures. Wait for it, or override with" >&2
		echo "MADC_ALLOW_CONCURRENT=1 and own the result." >&2
		exit 1
	fi
}
container_busy_check

rc_total=0

run_remote() {
	# $1 = label, $2 = remote command string
	# /usr/lib/ccache first so clang++/g++ resolve to the ccache wrappers.
	echo "=== $1 ==="
	$SSH "export PATH=/usr/lib/ccache:\$PATH; $2"
	rc=$?
	echo "$1 rc=$rc"
	if [ $rc -ne 0 ]; then rc_total=1; fi
}

for stage in $stages; do
	case "$stage" in
	sync)
		echo "=== sync ==="
		# bin/ obj/ lib/ tmp/ are excluded from the transfer, so the
		# directories themselves never arrive — make sure they exist.
		$SSH "mkdir -p /workspace/madc/bin /workspace/madc/obj /workspace/madc/lib /workspace/madc/tmp"
		rsync -az --delete \
			--exclude=tmp/ --exclude=bin/ --exclude=obj/ --exclude=lib/ \
			--exclude=MadSMAUG --exclude=autom4te.cache \
			-e "ssh -p $PORT" "$LOCAL_MADC/" "$REMOTE:/workspace/madc/"
		rc=$?
		echo "sync madc rc=$rc"
		if [ $rc -ne 0 ]; then rc_total=1; fi
		rsync -az --delete --exclude="*.o" --exclude="*.a" --exclude="*.d" \
			-e "ssh -p $PORT" "$LOCAL_MIR/" "$REMOTE:/workspace/mir/"
		rc=$?
		echo "sync mir rc=$rc"
		if [ $rc -ne 0 ]; then rc_total=1; fi
		;;
	build)
		run_remote "build mir" "make -C /workspace/mir -j20 libmir.a"
		run_remote "configure" "cd /workspace/madc; test -f src/config.mk || ./configure"
		run_remote "build madc" "make -C /workspace/madc/src -j20"
		# lib/ is excluded from sync; the soname link the emitted
		# .so's DT_NEEDED resolves through must exist on this side.
		run_remote "soname link" "ln -sf libmadc.so /workspace/madc/lib/libmadc.so.0"
		;;
	unittest)
		run_remote "unittest" "make -C /workspace/madc/src -j20 test"
		;;
	fulltest)
		run_remote "fulltest" "make -C /workspace/madc/src -j20 fulltest"
		;;
	exe)
		run_remote "exe" "cd /workspace/madc; bash scripts/run_tests.sh --exe"
		;;
	release)
		run_remote "release" "make -C /workspace/madc/src -j20 release"
		;;
	packed)
		run_remote "packed" "cd /workspace/madc; MADC_BIN=bin/madc-release bash scripts/run_tests.sh"
		;;
	pull)
		# Bring the container-built binaries back to the NAS: the two
		# userlands are ABI-identical (Ubuntu glibc 2.39, g++ 13.3)
		# and bin/madc links libmadc statically, so pulled binaries
		# run directly. The QNAP never compiles (owner directive
		# 2026-07-23). --no-perms/--no-owner/--no-group: the QNAP ACL
		# rejects chmod on temp files ("Bad address"). Pull only
		# right after building the CURRENT tree state, and never
		# while anything on the NAS is mid-suite.
		echo "=== pull ==="
		rsync -az --no-perms --no-owner --no-group \
			-e "ssh -p $PORT" \
			"$REMOTE:/workspace/madc/bin/madc" "$LOCAL_MADC/bin/madc"
		rc=$?
		echo "pull madc rc=$rc"
		if [ $rc -ne 0 ]; then rc_total=1; fi
		chmod +x "$LOCAL_MADC/bin/madc" 2>/dev/null
		if $SSH "test -f /workspace/madc/bin/madc-release"; then
			rsync -az --no-perms --no-owner --no-group \
				-e "ssh -p $PORT" \
				"$REMOTE:/workspace/madc/bin/madc-release" \
				"$LOCAL_MADC/bin/madc-release"
			rc=$?
			echo "pull madc-release rc=$rc"
			if [ $rc -ne 0 ]; then rc_total=1; fi
			chmod +x "$LOCAL_MADC/bin/madc-release" 2>/dev/null
		fi
		;;
	*)
		echo "unknown stage: $stage" >&2
		rc_total=1
		;;
	esac
done

echo "remote_build total rc=$rc_total"
exit $rc_total
