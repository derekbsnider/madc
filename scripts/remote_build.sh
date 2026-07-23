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
		rsync -az --delete --exclude="*.o" --exclude="*.a" \
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
