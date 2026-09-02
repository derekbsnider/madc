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
#     AND hosted-windows modes link (forest-carriers S1; windows lane W4.1).
#     The tree itself is the v1.5.5 clone; its three archives ARE staged
#     below (win64 here, the darwin twins via scripts/stage_darwin_zstd.sh
#     once the SDK is present).
#   Staged here (self-healing, fetch scripts own them): the darwin open C
#   headers, the pinned libc++ text + copyright (/workspace/libcxx-headers —
#   the mac packager's ONE copyright source), the UCRT libstdc++ stage.
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
# cpio + unzip: scripts/package_install_gate.sh (PK4) extracts the shipped
# artifact bytes — rpm2cpio|cpio for the .rpm, unzip for the Windows zip;
# zip is the win packager's own dependency. All three would read as green
# if lost (the gate only runs at package time), so they are pinned here.
PKGS_package="rpm cpio zip unzip zstd"
# winlane: the windows release lane (Track 6.4). The -posix flavor is
# deliberate — winpthreads provides the pthread/std::thread surface madc
# uses. wine64 is the interim/isolation runner for cross-built PE binaries
# (real-Windows runs go over the W0.2 ssh channel once the owner enables
# it). NOTE: these packages DEFAULT TO MSVCRT; the UCRT recipe and its
# gate live in scripts/win_ucrt_gate.sh, and the UCRT-flavor libstdc++
# stage (the C++ ABI leaks the CRT via std::mbstate_t, so the prebuilt
# msvcrt-flavor archive cannot serve -D_UCRT builds) is staged by
# scripts/build_win_ucrt_libstdcxx.sh — run below.
PKGS_winlane="g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64 wine64 libz-mingw-w64-dev"
# php-cli is an ORACLE, exactly like g++ and clang++: php::print_r and
# php::var_dump must render a madc value the way PHP renders it, and every
# fixture in tests/testphpprintr*.mad / tests/testphpvardump.mad was captured
# from a real `php` run (docs/plans/2026-08-17-php-print-r-var-dump-plan.md holds
# the captures). Without it the next reader can only compare against a comment.
# Nothing madc BUILDS needs it, which is why its absence would read as green.
PKGS_oracle="php-cli"

ALL="$PKGS_base $PKGS_llvm18 $PKGS_codec $PKGS_storage $PKGS_cross $PKGS_package $PKGS_winlane $PKGS_oracle"

# The binaries that actually have to exist afterwards — the check the build and
# the gates really depend on (a package can install and still not provide the
# versioned name we invoke).
BINS="g++ gcc make autoconf ccache python3 rsync nm gdb valgrind
      clang clang++ clang-18 clang++-18 ld64.lld-18 llvm-ar-18 llvm-nm-18 llvm-objdump-18 llvm-otool-18
      qemu-aarch64-static
      x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ x86_64-w64-mingw32-objdump wine
      php
      rpmbuild rpm2cpio cpio zip unzip"

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
	# The pinned libc++ header text + copyright (darwin-host port D2b/D3):
	# the mac packager ships THIS stage's copyright — its one source.
	local lhome="${LIBCXX_HEADERS_HOME:-/workspace/libcxx-headers}"
	if [ -f "$lhome/.PROVENANCE" ] && [ -s "$lhome/copyright" ]; then
		printf '  ok      pinned libc++ stage (%s)\n' "$(cat "$lhome/.PROVENANCE")"
	else
		printf '  MISSING pinned libc++ stage (%s) — scripts/fetch_libcxx_headers.sh stages it\n' "$lhome"
		missing=1
	fi
	# The win64 zstd stage (W4.1): the hosted-windows MODE links it, and its
	# absence fails that build loudly at link — but report it here so a lost
	# stage is visible before a build is attempted.
	local wzstd="${WIN_ZSTD_DIR:-/workspace/zstd}/libzstd-x86-64-windows.a"
	if [ -f "$wzstd" ]; then
		printf '  ok      win64 zstd stage (%s)\n' "$wzstd"
	else
		printf '  MISSING win64 zstd stage (%s) — staged below from /workspace/zstd\n' "$wzstd"
		missing=1
	fi
	# The darwin zstd twins (forest-carriers S1): the hosted-<arch>-macos
	# MODEs link them; ONE recipe, scripts/stage_darwin_zstd.sh.
	local a dzstd
	for a in arm64 x86-64; do
		dzstd="${DARWIN_ZSTD_DIR:-/workspace/zstd}/libzstd-$a-macos.a"
		if [ -f "$dzstd" ]; then
			printf '  ok      darwin zstd stage (%s)\n' "$dzstd"
		else
			printf '  MISSING darwin zstd stage (%s) — scripts/stage_darwin_zstd.sh %s (needs the SDK)\n' "$dzstd" "$a"
			missing=1
		fi
	done
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

echo "provision_container: staging the pinned libc++ text + copyright (darwin-host port D2b)"
bash "$(dirname "$0")/fetch_libcxx_headers.sh" || exit 1

echo "provision_container: staging UCRT-flavor libstdc++ (windows lane W1)"
bash "$(dirname "$0")/build_win_ucrt_libstdcxx.sh" || exit 1

# win64 zstd stage (windows lane W4.1): the per-target static build the
# hosted-windows MODE links, beside the darwin twins. /workspace/zstd is the
# v1.5.5 source tree the header comment names; idempotent — the .a survives
# container rebuilds on the persistent volume.
WZSTD_A="${WIN_ZSTD_DIR:-/workspace/zstd}/libzstd-x86-64-windows.a"
if [ ! -f "$WZSTD_A" ]; then
	echo "provision_container: staging win64 zstd (windows lane W4.1)"
	if [ ! -d /workspace/zstd/lib ]; then
		echo "provision_container: /workspace/zstd source tree missing (clone facebook/zstd v1.5.5 there)" >&2
		exit 1
	fi
	make -C /workspace/zstd/lib -j8 BUILD_DIR=obj-win64 \
		CC='x86_64-w64-mingw32-gcc-posix -D_UCRT -D__USE_MINGW_ANSI_STDIO=1' \
		AR=x86_64-w64-mingw32-ar libzstd.a || exit 1
	cp -p /workspace/zstd/lib/libzstd.a "$WZSTD_A" || exit 1
	# never leave a target-flavored libzstd.a in lib/ for another target's
	# build to pick up stale
	rm -f /workspace/zstd/lib/libzstd.a
fi

# darwin zstd twins (forest-carriers S1 / darwin-host D3): the hosted MODE's
# own CC/AR build them, which needs the owner-supplied SDK — without it the
# report above already says MISSING; do not turn that into a provisioning
# failure here.
if [ -d "${MACOS_SDK:-/workspace/sdk/MacOSX.sdk}" ]; then
	for a in arm64 x86-64; do
		echo "provision_container: staging darwin zstd ($a)"
		bash "$(dirname "$0")/stage_darwin_zstd.sh" "$a" || exit 1
	done
fi

echo "provision_container: verifying"
report
rc=$?
if [ $rc -eq 0 ]; then
	echo "provision_container: OK"
else
	echo "provision_container: INCOMPLETE (see MISSING above)"
fi
exit $rc
