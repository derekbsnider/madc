#!/bin/bash
# Stage the per-target MINIMAL static libgit2 the hosted-<target> madcgit
# module links (docs/plans/2026-09-15-madcgit-cross-targets-plan.md).
#
#   bash scripts/stage_libgit2.sh <x86-64-windows|arm64-macos|x86-64-macos>
#
# ONE recipe for every cross target, the scripts/stage_darwin_zstd.sh shape:
# libgit2 is a BUILD-TIME REQUIREMENT, never part of our distribution. The
# Windows/macOS bundles have no package manager to supply libgit2 at runtime,
# so its object code is STATICALLY linked into OUR libmadcgit.{dll,dylib};
# nothing named libgit2 ships (owner framing 2026-09-15). This stages the
# per-target static archive the module link consumes.
#
# The module is madc's READ-ONLY view of a LOCAL repository
# (scripts/check-one-git-owner.sh forbids any remote/clone/fetch/push API in
# the module), so this build needs NONE of libgit2's network stack: no https
# (OpenSSL/mbedTLS/SecureTransport), no ssh (libssh2), no http parser, no
# NTLM/GSSAPI, no iconv, no PCRE. zlib is BUNDLED (libgit2's own deps/zlib,
# compiled by the cross toolchain) so the archive is fully self-contained and
# no cross find_package(ZLIB) is needed.
#
# The compiler + archiver are the hosted MODE's OWN (make print-CC/print-AR:
# mingw-w64 UCRT over specs on the container for windows, cross clang-18 +
# llvm-ar-18 for macOS) — libgit2 is built by the SAME toolchain line madc is,
# never a copy of it kept here. Static-only (BUILD_SHARED_LIBS=OFF): CMake only
# compiles .o and archives, so no darwin dylib/install_name link step runs on
# the Linux host.
#
# Layout under $LIBGIT2_DIR (default /workspace/libgit2):
#   src/                             the pinned v1.7.2 source tree (git tag)
#   src/include/git2.h + git2/       the public headers (arch-independent;
#                                    one include dir serves every target)
#   libgit2-<target>.a               the per-target static archive
#   build-<target>/                  the per-target CMake build dir
# Idempotent: a present archive beside present headers is left alone.
set -e

target="${1:?usage: stage_libgit2.sh <x86-64-windows|arm64-macos|x86-64-macos>}"
case "$target" in
    x86-64-windows|arm64-macos|x86-64-macos) ;;
    x86_64-windows) target=x86-64-windows ;;
    x86_64-macos)   target=x86-64-macos ;;
    *) echo "stage_libgit2: unknown target '$target' (x86-64-windows | arm64-macos | x86-64-macos)" >&2; exit 2 ;;
esac
cd "$(dirname "$0")/.."

LIBGIT2_TAG=v1.7.2
DIR="${LIBGIT2_DIR:-/workspace/libgit2}"
SRC="$DIR/src"
BUILD="$DIR/build-$target"
OUT="$DIR/libgit2-$target.a"
INCSTAMP="$SRC/include/git2.h"

if [ -f "$OUT" ] && [ -f "$INCSTAMP" ]; then
    echo "libgit2 ($target): already staged ($OUT)"
    exit 0
fi

# Pinned source (fetch subsumed into the stage, the stage_darwin_zstd.sh way).
if [ ! -d "$SRC/.git" ]; then
    echo "libgit2 ($target): cloning libgit2/libgit2 $LIBGIT2_TAG into $SRC"
    git clone --quiet --depth 1 --branch "$LIBGIT2_TAG" https://github.com/libgit2/libgit2.git "$SRC"
fi
# The pin is the tag, verified on the tree that is actually about to build.
tag=$(git -C "$SRC" describe --tags --exact-match 2>/dev/null || true)
if [ "$tag" != "$LIBGIT2_TAG" ]; then
    echo "libgit2 ($target): $SRC is not at $LIBGIT2_TAG (git describe: '${tag:-untagged}')" >&2
    exit 1
fi

# The hosted MODE's compiler + archiver, read from the one definition. print-CC
# yields "<program> <flags...>"; CMake wants the program and its flags apart.
MODE="hosted-$target"
CCLINE=$(make -C src -s MODE="$MODE" LIBGIT2_DIR="$DIR" print-CC)
AR=$(make -C src -s MODE="$MODE" LIBGIT2_DIR="$DIR" print-AR)
[ -n "$CCLINE" ] && [ -n "$AR" ] || { echo "libgit2 ($target): could not read CC/AR of MODE=$MODE from src/Makefile" >&2; exit 1; }
CC_PROG=${CCLINE%% *}
CC_FLAGS=${CCLINE#* }
[ "$CC_FLAGS" = "$CCLINE" ] && CC_FLAGS=
# ranlib beside the archiver (llvm-ar-18 -> llvm-ranlib-18; *-ar -> *-ranlib).
RANLIB=$(printf '%s' "$AR" | sed 's/\(.*\)ar/\1ranlib/')

# CMAKE_SYSTEM_NAME switches libgit2's platform backends (win32 vs posix). The
# macOS targets are unix-like; Darwin picks the posix path with the SDK sysroot.
# zlib policy differs by target. libgit2's BUNDLED zlib (deps/zlib) is ancient
# K&R code: it compiles under mingw but NOT against the macOS SDK headers
# (deps/zlib/zutil.c vs _stdio.h). Windows has no guaranteed system zlib, so
# there we bundle it (the archive stays fully self-contained). macOS ships zlib
# as a SYSTEM library (always present at runtime), so there we compile against
# the SDK's zlib.h and let libmadcgit.dylib resolve -lz at its own link — the
# static libgit2.a carries no zlib objects, only unresolved zlib symbols.
case "$target" in
    *-windows) SYSNAME=Windows ; SYS_EXTRA=( -DUSE_BUNDLED_ZLIB=ON ) ;;
    *-macos)
        arch=${target%-macos}
        SYSNAME=Darwin
        # The darwin cross linker (the one madc's own build uses): without it
        # clang falls back to the host GNU ld, which cannot link Mach-O
        # ("unrecognised emulation mode: llvm") and CMake's compiler + feature
        # checks fail. -fuse-ld=lld is the DARWIN_LD_FLAGS the hosted MODE sets;
        # read it from the one definition rather than hardcode it here.
        LDF=$(make -C src -s MODE="$MODE" LIBGIT2_DIR="$DIR" print-DARWIN_LD_FLAGS)
        # The macOS SDK sysroot comes from the hosted MODE, never a hardcode:
        # on the container MACOS_SDK falls back to /workspace/sdk/MacOSX.sdk,
        # but a NATIVE darwin host (the darwin-probe.yml GitHub runner, which
        # also stages this archive before `make release-macos`) has its Xcode
        # SDK elsewhere. print-MACOS_SDK yields whichever this host uses
        # (`xcrun --show-sdk-path` on darwin, the /workspace fallback on the
        # container), so the same recipe serves the cross build and the native
        # build. zlib's headers/tbd live under that same SDK on both.
        SDK=$(make -C src -s MODE="$MODE" LIBGIT2_DIR="$DIR" print-MACOS_SDK)
        [ -n "$SDK" ] || { echo "libgit2 ($target): could not read MACOS_SDK of MODE=$MODE from src/Makefile" >&2; exit 1; }
        SYS_EXTRA=( -DCMAKE_OSX_SYSROOT="$SDK"
                    -DCMAKE_OSX_ARCHITECTURES="$(printf %s "$arch" | sed 's/x86-64/x86_64/')"
                    -DCMAKE_OSX_DEPLOYMENT_TARGET=12
                    -DCMAKE_EXE_LINKER_FLAGS="$LDF"
                    -DCMAKE_SHARED_LINKER_FLAGS="$LDF"
                    -DCMAKE_MODULE_LINKER_FLAGS="$LDF"
                    -DUSE_BUNDLED_ZLIB=OFF
                    -DZLIB_INCLUDE_DIR="$SDK/usr/include"
                    -DZLIB_LIBRARY="$SDK/usr/lib/libz.tbd" )
        ;;
esac

echo "libgit2 ($target): configuring minimal static build"
echo "  CC=$CC_PROG   FLAGS=$CC_FLAGS"
echo "  AR=$AR   RANLIB=$RANLIB   SYSTEM=$SYSNAME"
rm -rf "$BUILD"
mkdir -p "$BUILD"
cmake -S "$SRC" -B "$BUILD" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME="$SYSNAME" \
    -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_C_COMPILER="$CC_PROG" \
    -DCMAKE_C_FLAGS="$CC_FLAGS" \
    -DCMAKE_AR="$(command -v "$AR" || echo "$AR")" \
    -DCMAKE_RANLIB="$(command -v "$RANLIB" || echo "$RANLIB")" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTS=OFF \
    -DBUILD_CLI=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DUSE_SSH=OFF \
    -DUSE_HTTPS=OFF \
    -DUSE_HTTP_PARSER=builtin \
    -DUSE_NTLMCLIENT=OFF \
    -DUSE_GSSAPI=OFF \
    -DUSE_ICONV=OFF \
    -DREGEX_BACKEND=builtin \
    "${SYS_EXTRA[@]}"

jobs=$(nproc 2>/dev/null || echo 4)
echo "libgit2 ($target): building (-j$jobs)"
cmake --build "$BUILD" -j"$jobs"

lib=$(find "$BUILD" -name 'libgit2.a' -print -quit)
[ -n "$lib" ] || { echo "libgit2 ($target): build produced no libgit2.a under $BUILD" >&2; exit 1; }
cp -p "$lib" "$OUT"
echo "libgit2 ($target): staged $OUT"
