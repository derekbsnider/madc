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
#   obj       bash scripts/run_tests.sh --obj  (single-object loader lane)
#   libcxx    the whole suite under -stdlib=libc++, JIT + exe + obj (the
#             stdlib-flavor PARITY lane; .libcxx_skip marks out-of-scope tests)
#   libcxxjit the lane's JIT leg only — the per-batch checkpoint during lane
#             burndown (owner 2026-08-05: don't run half a dozen full suites
#             per change); EXE/OBJ legs run at session end / pre-merge
#   release   make -C src release
#   packed    MADC_BIN=bin/madc-release bash scripts/run_tests.sh
#   release-macos  make -C src release-macos (both hosted darwin arches,
#             stripped + forest-verified) + package_release_macos.sh
#             (tarballs into dist/), then pull the tarballs back
#   pull      rsync container-built bin/madc (+ madc-release) back to
#             the NAS (ABI-identical userlands; QNAP never compiles)
#   battery   fulltest + exe + obj + release + packed (the push gate)
#   shell     print the ssh command and exit
#
# Every remote invocation is one ssh call running a generated script, so
# local shell hygiene (no && chains per call) is preserved.

REMOTE="dev@localhost"
PORT=2299
SSH="ssh -p $PORT $REMOTE"
LOCAL_MADC=/workspace/madc
LOCAL_MIR=/workspace/mir

# ALWAYS keep a full transcript, whether or not the caller redirects.
#
# On 2026-07-28 a ~30-minute battery was run as `remote_build.sh battery |
# tail -60`. It reported `total rc=1`, and the stage that produced it had
# already scrolled out of the pipe — the whole run had to be thrown away. The
# fix is not "remember to redirect": stdout here stays SHORT (stage lines plus
# a summary), the full output goes to the log, and the log path is printed at
# both ends. There is nothing left worth piping through `tail`.
if [ -z "$MADC_RB_LOGGING" ]; then
	case " $* " in
		*" shell "*) : ;;   # prints one line; no log needed
		*)
			mkdir -p "$LOCAL_MADC/tmp/logs"
			_log="$LOCAL_MADC/tmp/logs/rb-$(date +%Y%m%d-%H%M%S).log"
			echo "=== full log: $_log"
			MADC_RB_LOGGING=1 "$0" "$@" 2>&1 | tee "$_log"
			_rc=${PIPESTATUS[0]}
			echo "=== full log: $_log"
			exit $_rc
			;;
	esac
fi

stages="$*"
if [ -z "$stages" ]; then
	stages="sync build"
fi
if [ "$stages" = "battery" ]; then
	stages="sync build fulltest exe obj release packed"
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

stage_summary=""

note_stage() {
	# $1 = label, $2 = rc. Recorded so the final summary can name the stage
	# that failed — `total rc=1` on its own sent a whole battery to waste.
	stage_summary="$stage_summary
  $1 rc=$2"
	if [ "$2" -ne 0 ]; then rc_total=1; fi
}

run_remote() {
	# $1 = label, $2 = remote command string
	# /usr/lib/ccache first so clang++/g++ resolve to the ccache wrappers.
	echo "=== $1 ==="
	$SSH "export PATH=/usr/lib/ccache:\$PATH; $2"
	rc=$?
	echo "$1 rc=$rc"
	note_stage "$1" "$rc"
}

for stage in $stages; do
	case "$stage" in
	sync)
		echo "=== sync ==="
		# bin/ obj/ lib/ tmp/ are excluded from the transfer, so the
		# directories themselves never arrive — make sure they exist.
		$SSH "mkdir -p /workspace/madc/bin /workspace/madc/obj /workspace/madc/lib /workspace/madc/tmp"
		# HOST-PROBED generated sources must NEVER cross the tunnel.
		# They are written by probing the LOCAL compiler ($CXX -E -v for
		# the include search list and stdlib flavor table, $CXX -dM -E
		# for the predefined macros), so the NAS's copies describe the
		# NAS — which never builds anything. Syncing them sent a stale
		# pre-flavor-table sys_include_paths.cpp (543 bytes, Jul 22) over
		# the container's freshly generated one (1507 bytes) right after
		# every build. Nothing broke only because the copy also carried
		# the older mtime, so make saw the .o as current and skipped it;
		# delete that .o and the next build compiles the stale format and
		# fails to link madc_stdlib_flavors. rsync does not delete
		# excluded files on the receiver, so the container keeps its own.
		rsync -az --delete \
			--exclude=tmp/ --exclude=bin/ --exclude=obj/ --exclude=lib/ \
			--exclude=MadSMAUG --exclude=autom4te.cache \
			--exclude=src/sys_include_paths.cpp \
			--exclude=src/predefined_macros.cpp \
			-e "ssh -p $PORT" "$LOCAL_MADC/" "$REMOTE:/workspace/madc/"
		rc=$?
		echo "sync madc rc=$rc"
		note_stage "sync madc" "$rc"
		rsync -az --delete --exclude="*.o" --exclude="*.a" --exclude="*.d" \
			-e "ssh -p $PORT" "$LOCAL_MIR/" "$REMOTE:/workspace/mir/"
		rc=$?
		echo "sync mir rc=$rc"
		note_stage "sync mir" "$rc"
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
	tests)
		# TARGETED subset — the inner loop. TESTS holds basename globs.
		# Scope a run to the blast radius of the change and save the full
		# suite for pre-merge (.claude/rules/build.md, build-test-efficiency).
		if [ -z "$TESTS" ]; then
			echo "stage 'tests' needs TESTS='<glob> [glob...]'" >&2
			note_stage "tests" 1
		else
			run_remote "tests" "cd /workspace/madc; bash scripts/run_tests.sh $TESTS"
		fi
		;;
	tests-all)
		# The same subset across every execution lane (JIT + exe + obj) —
		# the targeted equivalent of the battery, for the surface touched.
		if [ -z "$TESTS" ]; then
			echo "stage 'tests-all' needs TESTS='<glob> [glob...]'" >&2
			note_stage "tests-all" 1
		else
			run_remote "tests jit" "cd /workspace/madc; bash scripts/run_tests.sh $TESTS"
			run_remote "tests exe" "cd /workspace/madc; bash scripts/run_tests.sh --exe $TESTS"
			run_remote "tests obj" "cd /workspace/madc; bash scripts/run_tests.sh --obj $TESTS"
		fi
		;;
	exe)
		run_remote "exe" "cd /workspace/madc; bash scripts/run_tests.sh --exe"
		;;
	obj)
		run_remote "obj" "cd /workspace/madc; bash scripts/run_tests.sh --obj"
		;;
	libcxx)
		# The PARITY lane: the whole suite under the alternate stdlib
		# flavor, in all three execution lanes. This is how "libc++
		# behaves like the default flavor" is MEASURED — a flavor-specific
		# fixture proves only that one fixture works. Out-of-scope tests
		# carry tests/<base>.libcxx_skip with a reason.
		run_remote "libcxx jit" "cd /workspace/madc; bash scripts/run_tests.sh --stdlib=libc++"
		run_remote "libcxx exe" "cd /workspace/madc; bash scripts/run_tests.sh --stdlib=libc++ --exe"
		run_remote "libcxx obj" "cd /workspace/madc; bash scripts/run_tests.sh --stdlib=libc++ --obj"
		;;
	libcxxjit)
		# JIT leg only — the per-batch lane checkpoint; the EXE/OBJ legs
		# move to session end / pre-merge (they are ~2/3 of the lane's
		# wall time and rarely flip for front-end work).
		run_remote "libcxx jit" "cd /workspace/madc; bash scripts/run_tests.sh --stdlib=libc++"
		;;
	release)
		run_remote "release" "make -C /workspace/madc/src -j20 release"
		;;
	release-macos)
		# The two arches build SEQUENTIALLY inside the target (shared
		# per-arch generated tables); -j parallelizes within each.
		run_remote "release-macos" "make -C /workspace/madc/src -j20 release-macos"
		run_remote "package-macos" "cd /workspace/madc; bash scripts/package_release_macos.sh"
		mkdir -p "$LOCAL_MADC/dist"
		rsync -az --no-perms --no-owner --no-group \
			-e "ssh -p $PORT" \
			--include='madc-*-macos-*.tar.gz' --include='SHA256SUMS' \
			--exclude='*' \
			"$REMOTE:/workspace/madc/dist/" "$LOCAL_MADC/dist/"
		rc=$?
		echo "pull macos tarballs rc=$rc"
		note_stage "pull macos tarballs" "$rc"
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
		note_stage "pull madc" "$rc"
		chmod +x "$LOCAL_MADC/bin/madc" 2>/dev/null
		if $SSH "test -f /workspace/madc/bin/madc-release"; then
			rsync -az --no-perms --no-owner --no-group \
				-e "ssh -p $PORT" \
				"$REMOTE:/workspace/madc/bin/madc-release" \
				"$LOCAL_MADC/bin/madc-release"
			rc=$?
			echo "pull madc-release rc=$rc"
			note_stage "pull madc-release" "$rc"
			chmod +x "$LOCAL_MADC/bin/madc-release" 2>/dev/null
		fi
		;;
	*)
		echo "unknown stage: $stage" >&2
		rc_total=1
		;;
	esac
done

echo ""
echo "=== stage summary ===$stage_summary"
echo "remote_build total rc=$rc_total"
exit $rc_total
