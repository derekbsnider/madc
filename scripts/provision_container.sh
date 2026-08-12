#!/bin/bash
# provision_container.sh — install everything the madc dev container needs to
# build and gate, on top of a stock Ubuntu 24.04 image.
#
# WHY THIS EXISTS: the toolchain used to live only in a running container's
# writable layer, so recreating the container (a WSL crash did exactly this on
# 2026-07-26) silently lost it — and the loss reads as GREEN, because
# macho_obj_gate SKIPS when its llvm-18 / SDK prerequisites are absent. The
# build environment is a property of the project, so it belongs in the project.
#
# Idempotent: apt-get install on an already-installed package is a no-op, so
# re-running after any container rebuild is the whole recovery procedure.
#
#   bash scripts/provision_container.sh          # install
#   bash scripts/provision_container.sh --check  # report only, install nothing
#
# NOT provisioned here (deliberately):
#   - /workspace/sdk/MacOSX.sdk — the owner's macOS SDK. NEVER committed,
#     synced, or downloaded by us; it comes from the owner's Mac and lives on
#     the workspace volume.
#   - /workspace/madc — the git tree (MIR included at third_party/mir),
#     restored by scripts/remote_build.sh sync (or a clone).
#   - /workspace/zstd — the per-target static zstd builds the hosted darwin
#     modes link (forest-carriers S1); rebuild from that tree if lost.
set -u

CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

# Package groups, each with the reason it is here. Keep the reasons: a bare
# list rots into "why is qdbm in our build?" within one session.
#
# base       compiler + build plumbing for the native Linux build
# llvm18     clang-18 drives the hosted/cross darwin builds and the embedded
#            darwin prelude generator; ld64.lld-18 links Mach-O; the llvm-*
#            binutils are macho_obj_gate's independent authority; libc++-dev is
#            the standard library madc's second ABI flavor targets
# codec      zstd: the forest / PCH container codec (HAVE_ZSTD)
# storage    madcdat backends configure probes for
# cross      qemu-user-static: runs aarch64-linux artifacts on x86-64
# gdb: parser-crash attribution — a stack-overflow SIGSEGV's recursion cycle
# is visible in three backtrace frames (how the libc++ <string> allocator
# CRTP loop was found, 2026-07-28); the built-in handler prints raw
# addresses only.
# valgrind: front-end PERFORMANCE attribution (callgrind). gcc is the
# performance baseline too, and scripts/perf_vs_gcc.sh callgrinds any file
# where madc is slower — with valgrind absent that script degrades to a bare
# timing number. Found needed 2026-08-02: testsubscript's front end takes
# ~20 s under -stdlib=libc++ vs ~1.3 s default, and --show-stats accounts
# for only 2.3 s of it, so the remaining ~17 s is invisible without a
# profiler.
PKGS_base="build-essential g++-13 autoconf ccache make git rsync python3 pkg-config gdb valgrind"
# libc++-18-dev / libc++abi-18-dev are NOT darwin-only tooling: libc++ is a
# standard library (Apple, Android NDK, FreeBSD, and clang on Linux all use
# it), so madc's libc++ ABI flavor is developed and gated HERE, on Linux,
# via -stdlib=libc++ with clang++-18 as the oracle. Owner hardware then only
# has to prove the darwin target plumbing, not the library semantics.
# The UNVERSIONED clang/clang++ drivers matter too: gate scripts invoke the
# canon compilers by plain name (class_pattern_equivalence.sh needs
# `clang++`), and a container with only clang++-18 fails that gate — loudly,
# which is how this was found after the 2026-07-26 container rebuild.
PKGS_llvm18="clang-18 clang lld-18 llvm-18 libc++-18-dev libc++abi-18-dev"
PKGS_codec="libzstd-dev zlib1g-dev"
# libxqdbm-dev is QDBM's C++ API and a SEPARATE package from libqdbm-dev:
# without it configure reports xqdbm=0 and the build quietly loses a
# backend (and its unit-test surface) versus the pre-crash config.
PKGS_storage="libdb-dev libgdbm-dev libsqlite3-dev libqdbm-dev libxqdbm-dev"
PKGS_cross="qemu-user-static"
# rpm supplies rpmbuild for scripts/package_release.sh (.rpm leg); dpkg-deb
# is part of the base image but rpm is not — its absence 127'd the v0.69.0
# promote's package build after a container rebuild dropped the apt layer.
PKGS_package="rpm"
# winlane: the windows release lane (Track 6.4). The -posix flavor is
# deliberate — winpthreads provides the pthread/std::thread surface madc
# uses. wine64 is the interim/isolation runner for cross-built PE binaries
# (real-Windows runs go over the W0.2 ssh channel once the owner enables
# it). NOTE: these packages DEFAULT TO MSVCRT; the UCRT recipe and its
# gate live in scripts/win_ucrt_gate.sh.
PKGS_winlane="g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64 wine64 libz-mingw-w64-dev"

ALL="$PKGS_base $PKGS_llvm18 $PKGS_codec $PKGS_storage $PKGS_cross $PKGS_package $PKGS_winlane"

# The binaries that actually have to exist afterwards — the check the build and
# the gates really depend on (a package can install and still not provide the
# versioned name we invoke).
BINS="g++ gcc make autoconf ccache python3 rsync nm gdb valgrind
      clang clang++ clang-18 clang++-18 ld64.lld-18 llvm-ar-18 llvm-nm-18 llvm-objdump-18 llvm-otool-18
      qemu-aarch64-static
      x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ x86_64-w64-mingw32-objdump wine"

report() {
	local missing=0
	for b in $BINS; do
		if command -v "$b" > /dev/null 2>&1; then
			printf '  ok      %s\n' "$b"
		else
			printf '  MISSING %s\n' "$b"
			missing=1
		fi
	done
	# The SDK is not ours to install, but its absence silently downgrades the
	# darwin gates to skips — so always say whether it is there.
	local sdk="${MACOS_SDK:-/workspace/sdk/MacOSX.sdk}"
	if [ -d "$sdk/usr/include/c++/v1" ]; then
		printf '  ok      macOS SDK with libc++ (%s)\n' "$sdk"
	elif [ -d "$sdk" ]; then
		printf '  PARTIAL macOS SDK present but no usr/include/c++/v1 (%s)\n' "$sdk"
		missing=1
	else
		printf '  MISSING macOS SDK (%s) — owner-supplied, never downloaded here\n' "$sdk"
		missing=1
	fi
	# The open darwin libc header tree (W0.5, the embedded prelude's input) IS
	# ours to install — self-healing via the fetch script — but its absence
	# fails every hosted/cross darwin build, so report it beside the SDK.
	local dprov="${DARWIN_OPEN_HEADERS_HOME:-/workspace/darwin-open-headers}/sysroot-any-darwin-any/.PROVENANCE"
	if [ -f "$dprov" ]; then
		printf '  ok      darwin open headers (%s)\n' "$(cat "$dprov")"
	else
		printf '  MISSING darwin open headers — scripts/fetch_darwin_open_headers.sh stages them\n'
		missing=1
	fi
	return $missing
}

if [ $CHECK_ONLY -eq 1 ]; then
	echo "provision_container: checking"
	report
	rc=$?
	[ $rc -eq 0 ] && echo "provision_container: OK" || echo "provision_container: INCOMPLETE"
	exit $rc
fi

echo "provision_container: apt-get update"
sudo apt-get update -qq || exit 1
echo "provision_container: installing"
# One transaction: a partial install is harder to reason about than a failure.
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq $ALL || exit 1

echo "provision_container: staging darwin open headers (W0.5 prelude input)"
bash "$(dirname "$0")/fetch_darwin_open_headers.sh" || exit 1

echo "provision_container: verifying"
report
rc=$?
if [ $rc -eq 0 ]; then
	echo "provision_container: OK"
else
	echo "provision_container: INCOMPLETE (see MISSING above)"
fi
exit $rc
