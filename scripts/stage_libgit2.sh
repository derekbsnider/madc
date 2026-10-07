#!/bin/bash
# Stage the per-target MINIMAL static libgit2 the madcgit module links
# (docs/plans/2026-09-15-madcgit-cross-targets-plan.md).
#
#   bash scripts/stage_libgit2.sh <host|x86-64-windows|arm64-macos|x86-64-macos>
#   bash scripts/stage_libgit2.sh --path <target>   (print the archive's path)
#
# ONE recipe for every target, the scripts/stage_darwin_zstd.sh shape:
# libgit2 is a BUILD-TIME REQUIREMENT, never part of our distribution. Its
# object code is STATICALLY linked into OUR libmadcgit.{so,dll,dylib};
# nothing named libgit2 ships (owner framing 2026-09-15), and no package
# depends on a system libgit2. `host` is the Linux build host's own target
# (x86-64-linux, aarch64-linux): Ubuntu 24.04's system libgit2 is 1.7.2,
# below the floor (owner 2026-10-04: 1.8.7 / 1.9.7 or newer), so Linux links
# the staged archive as the bundles do. The pin is src/madcgit.mk's
# LIBGIT2_TAG, read through make, and every path below carries it — a bump
# stages afresh and never mistakes an older tag's archive for the pin.
#
# The module is madc's READ-ONLY view of a LOCAL repository
# (scripts/check-one-git-owner.sh forbids any remote/clone/fetch/push API in
# the module), so this build needs NONE of libgit2's network stack: no https
# (OpenSSL/mbedTLS/SecureTransport), no ssh (libssh2), no http parser, no
# NTLM/GSSAPI, no iconv, no PCRE. zlib is BUNDLED (libgit2's own deps/zlib,
# compiled by the cross toolchain) so the archive is fully self-contained and
# no cross find_package(ZLIB) is needed.
#
# The toolchain is the target's hosted MODE's own, translated into CMake's
# arguments by scripts/stage_cmake_args.sh (the one translation, shared with
# stage_cmark_gfm.sh). Static-only (BUILD_SHARED_LIBS=OFF): CMake only compiles
# .o and archives, so no darwin dylib/install_name link step runs on the Linux
# host.
#
# Layout under $LIBGIT2_DIR (default /workspace/libgit2), per pinned tag:
#   <tag>/src/                       the pinned source tree (git tag)
#   <tag>/src/include/git2.h + git2/ the public headers (arch-independent;
#                                    one include dir serves every target)
#   <tag>/libgit2-<target>.a         the per-target static archive
#   <tag>/build-<target>/            the per-target CMake build dir
# Idempotent: a present archive beside present headers is left alone.
set -e

usage="usage: stage_libgit2.sh [--path] <host|x86-64-windows|arm64-macos|x86-64-macos>"
print_path=
if [ "${1:-}" = "--path" ]; then
    print_path=1
    shift
fi
target="${1:?$usage}"
target=$(bash "$(dirname "$0")/stage_cmake_args.sh" --target "$target") || exit 2
cd "$(dirname "$0")/.."

# The pin and the stage directory: src/madcgit.mk's, never a second copy.
export LIBGIT2_DIR="${LIBGIT2_DIR:-/workspace/libgit2}"
LIBGIT2_TAG=$(make -C src -s print-LIBGIT2_TAG)
STAGE=$(make -C src -s print-LIBGIT2_STAGE)
[ -n "$LIBGIT2_TAG" ] && [ -n "$STAGE" ] || { echo "stage_libgit2: could not read LIBGIT2_TAG/LIBGIT2_STAGE from src/madcgit.mk" >&2; exit 1; }
SRC="$STAGE/src"
BUILD="$STAGE/build-$target"
OUT="$STAGE/libgit2-$target.a"
INCSTAMP="$SRC/include/git2.h"

if [ -n "$print_path" ]; then
    echo "$OUT"
    exit 0
fi

if [ -f "$OUT" ] && [ -f "$INCSTAMP" ]; then
    echo "libgit2 ($target): already staged ($OUT)"
    exit 0
fi

# Pinned source (fetch subsumed into the stage, the stage_darwin_zstd.sh way).
if [ ! -d "$SRC/.git" ]; then
    echo "libgit2 ($target): cloning libgit2/libgit2 $LIBGIT2_TAG into $SRC"
    mkdir -p "$STAGE"
    git clone --quiet --depth 1 --branch "$LIBGIT2_TAG" https://github.com/libgit2/libgit2.git "$SRC"
fi
# The pin is the tag, verified on the tree that is actually about to build.
tag=$(git -C "$SRC" describe --tags --exact-match 2>/dev/null || true)
if [ "$tag" != "$LIBGIT2_TAG" ]; then
    echo "libgit2 ($target): $SRC is not at $LIBGIT2_TAG (git describe: '${tag:-untagged}')" >&2
    exit 1
fi

# The target's toolchain (scripts/stage_cmake_args.sh, one argument per line).
toolchain=$(bash scripts/stage_cmake_args.sh "$target")
TOOLCHAIN=()
while IFS= read -r arg; do TOOLCHAIN+=("$arg"); done <<< "$toolchain"	# bash 3.2 has no mapfile

# zlib policy differs by target. libgit2's BUNDLED zlib (deps/zlib) is ancient
# K&R code: it compiles under mingw but NOT against the macOS SDK headers
# (deps/zlib/zutil.c vs _stdio.h). Windows has no guaranteed system zlib, so
# there we bundle it (the archive stays fully self-contained). macOS ships zlib
# as a SYSTEM library (always present at runtime), so there we compile against
# the SDK's zlib.h and let libmadcgit.dylib resolve -lz at its own link — the
# static libgit2.a carries no zlib objects, only unresolved zlib symbols. The
# SDK is the hosted MODE's (print-MACOS_SDK: the container's staged SDK on a
# cross build, Xcode's on a native darwin host). The host target links the
# system zlib madc's own packages already depend on (zlib1g).
case "$target" in
    *-linux) ZLIB=( -DUSE_BUNDLED_ZLIB=OFF ) ;;
    *-windows) ZLIB=( -DUSE_BUNDLED_ZLIB=ON ) ;;
    *-macos)
        SDK=$(make -C src -s MODE="hosted-$target" print-MACOS_SDK)
        [ -n "$SDK" ] || { echo "libgit2 ($target): could not read MACOS_SDK of MODE=hosted-$target from src/Makefile" >&2; exit 1; }
        ZLIB=( -DUSE_BUNDLED_ZLIB=OFF
               -DZLIB_INCLUDE_DIR="$SDK/usr/include"
               -DZLIB_LIBRARY="$SDK/usr/lib/libz.tbd" )
        ;;
esac

echo "libgit2 ($target): configuring minimal static build"
printf '  %s\n' "${TOOLCHAIN[@]}"
rm -rf "$BUILD"
mkdir -p "$BUILD"
cmake -S "$SRC" -B "$BUILD" -G "Unix Makefiles" \
    "${TOOLCHAIN[@]}" \
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
    "${ZLIB[@]}"

jobs=$(nproc 2>/dev/null || echo 4)
echo "libgit2 ($target): building (-j$jobs)"
cmake --build "$BUILD" -j"$jobs"

lib=$(find "$BUILD" -name 'libgit2.a' -print -quit)
[ -n "$lib" ] || { echo "libgit2 ($target): build produced no libgit2.a under $BUILD" >&2; exit 1; }
cp -p "$lib" "$OUT"
echo "libgit2 ($target): staged $OUT"
