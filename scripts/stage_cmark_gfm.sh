#!/bin/bash
# Stage the per-target static cmark-gfm the madcmark module links
# (src/madcmark.mk; plan docs/plans/2026-10-03-chthonia-windows-macos.md §7d).
#
#   bash scripts/stage_cmark_gfm.sh <host|x86-64-windows|arm64-macos|x86-64-macos>
#   bash scripts/stage_cmark_gfm.sh --path <target>   (print the target's stage dir)
#
# The scripts/stage_libgit2.sh shape: cmark-gfm is a BUILD-TIME REQUIREMENT,
# never part of our distribution. Its object code is STATICALLY linked into
# OUR libmadcmark.{so,dll,dylib}; nothing named cmark ships, and no package
# depends on a system cmark-gfm. The pin is src/madcmark.mk's CMARK_GFM_TAG,
# read through make, and every path below carries it — a bump stages afresh.
# Linux links the stage too: the distributions' cmark-gfm lags the upstream
# fixes (Ubuntu 24.04 carries 0.29.0.gfm.6).
#
# The toolchain is the target's hosted MODE's own, translated into CMake's
# arguments by scripts/stage_cmake_args.sh. Static-only (CMARK_SHARED=OFF),
# and only the two library targets build: the cmark-gfm program and the tests
# never link.
#
# Layout under $CMARK_GFM_DIR (default /workspace/cmark-gfm), per pinned tag:
#   <tag>/src/                         the pinned source tree (git tag); its
#                                      COPYING is the notice every package ships
#   <tag>/<target>/include/            the public headers, the generated
#                                      cmark-gfm_export.h + _version.h included
#   <tag>/<target>/libcmark-gfm.a      the per-target static archives
#   <tag>/<target>/libcmark-gfm-extensions.a
#   <tag>/build-<target>/              the per-target CMake build dir
# Idempotent: a present stage is left alone.
set -e

usage="usage: stage_cmark_gfm.sh [--path] <host|x86-64-windows|arm64-macos|x86-64-macos>"
print_path=
if [ "${1:-}" = "--path" ]; then
    print_path=1
    shift
fi
target="${1:?$usage}"
target=$(bash "$(dirname "$0")/stage_cmake_args.sh" --target "$target") || exit 2
cd "$(dirname "$0")/.."

# The pin and the stage directory: src/madcmark.mk's, never a second copy.
export CMARK_GFM_DIR="${CMARK_GFM_DIR:-/workspace/cmark-gfm}"
CMARK_GFM_TAG=$(make -C src -s print-CMARK_GFM_TAG)
STAGE=$(make -C src -s print-CMARK_GFM_STAGE)
[ -n "$CMARK_GFM_TAG" ] && [ -n "$STAGE" ] || { echo "stage_cmark_gfm: could not read CMARK_GFM_TAG/CMARK_GFM_STAGE from src/madcmark.mk" >&2; exit 1; }
SRC="$STAGE/src"
BUILD="$STAGE/build-$target"
OUT="$STAGE/$target"

if [ -n "$print_path" ]; then
    echo "$OUT"
    exit 0
fi

if [ -f "$OUT/libcmark-gfm.a" ] && [ -f "$OUT/libcmark-gfm-extensions.a" ] && [ -f "$OUT/include/cmark-gfm_export.h" ]; then
    echo "cmark-gfm ($target): already staged ($OUT)"
    exit 0
fi

if [ ! -d "$SRC/.git" ]; then
    echo "cmark-gfm ($target): cloning github/cmark-gfm $CMARK_GFM_TAG into $SRC"
    mkdir -p "$STAGE"
    git clone --quiet --depth 1 --branch "$CMARK_GFM_TAG" https://github.com/github/cmark-gfm.git "$SRC"
fi
# The pin is the tag, verified on the tree that is actually about to build.
tag=$(git -C "$SRC" describe --tags --exact-match 2>/dev/null || true)
if [ "$tag" != "$CMARK_GFM_TAG" ]; then
    echo "cmark-gfm ($target): $SRC is not at $CMARK_GFM_TAG (git describe: '${tag:-untagged}')" >&2
    exit 1
fi

toolchain=$(bash scripts/stage_cmake_args.sh "$target")
TOOLCHAIN=()
while IFS= read -r arg; do TOOLCHAIN+=("$arg"); done <<< "$toolchain"	# bash 3.2 has no mapfile

echo "cmark-gfm ($target): configuring static build"
printf '  %s\n' "${TOOLCHAIN[@]}"
rm -rf "$BUILD"
mkdir -p "$BUILD"
# cmark-gfm declares cmake_minimum_required(VERSION 3.0); CMake 4 refuses a
# minimum below 3.5 unless told the policy floor (older CMake ignores the
# variable, and only warns about the old minimum).
cmake -S "$SRC" -B "$BUILD" -G "Unix Makefiles" \
    "${TOOLCHAIN[@]}" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMARK_SHARED=OFF \
    -DCMARK_STATIC=ON \
    -DCMARK_TESTS=OFF \
    -DCMARK_LIB_FUZZER=OFF

jobs=$(nproc 2>/dev/null || echo 4)
echo "cmark-gfm ($target): building (-j$jobs)"
cmake --build "$BUILD" -j"$jobs" --target libcmark-gfm_static libcmark-gfm-extensions_static

rm -rf "$OUT"
mkdir -p "$OUT/include"
cp -p "$BUILD/src/libcmark-gfm.a" "$BUILD/extensions/libcmark-gfm-extensions.a" "$OUT/"
cp -p "$SRC/src/cmark-gfm.h" "$SRC/src/cmark-gfm-extension_api.h" \
      "$BUILD/src/cmark-gfm_export.h" "$BUILD/src/cmark-gfm_version.h" \
      "$SRC/extensions/cmark-gfm-core-extensions.h" "$OUT/include/"
echo "cmark-gfm ($target): staged $OUT"
