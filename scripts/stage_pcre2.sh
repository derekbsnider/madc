#!/bin/bash
# Stage the per-target static PCRE2 the hosted macOS and Windows MODEs link
# into libmadc (split arc 1A.3; design docs/plans/2026-10-10-pcre2-design.md).
#
#   bash scripts/stage_pcre2.sh <x86-64-windows|arm64-macos|x86-64-macos|host>
#   bash scripts/stage_pcre2.sh --path <target>   (print the target's stage dir)
#
# The scripts/stage_cmark_gfm.sh shape: PCRE2 is a DEPENDENCY, never part of
# madc's source. Linux links the distribution's shared libpcre2-8 (configure);
# the hosted macOS and Windows MODEs link THIS stage. The pin is src/Makefile's
# PCRE2_TAG, read through make, and every path below carries it — a bump
# stages afresh. The toolchain is the target's hosted MODE's own, translated
# into CMake's arguments by scripts/stage_cmake_args.sh. Static only, 8-bit
# code units only, Unicode on, the JIT OFF (design §2.3), and only the
# library target builds: pcre2grep, pcre2test and the tests never link.
#
# Layout under $PCRE2_DIR (default /workspace/pcre2), per pinned tag:
#   <tag>/src/                    the pinned source tree (git tag); its
#                                 LICENCE is the notice the packages ship
#   <tag>/<target>/include/pcre2.h  the generated public header
#   <tag>/<target>/libpcre2-8.a   the per-target static archive
#   <tag>/build-<target>/         the per-target CMake build dir
# Idempotent: a present stage is left alone.
set -e

usage="usage: stage_pcre2.sh [--path] <x86-64-windows|arm64-macos|x86-64-macos|host>"
print_path=
if [ "${1:-}" = "--path" ]; then
    print_path=1
    shift
fi
target="${1:?$usage}"
target=$(bash "$(dirname "$0")/stage_cmake_args.sh" --target "$target") || exit 2
cd "$(dirname "$0")/.."

# The pin and the stage directory: src/Makefile's, never a second copy.
export PCRE2_DIR="${PCRE2_DIR:-/workspace/pcre2}"
PCRE2_TAG=$(make -C src -s print-PCRE2_TAG)
STAGE=$(make -C src -s print-PCRE2_STAGE)
[ -n "$PCRE2_TAG" ] && [ -n "$STAGE" ] || { echo "stage_pcre2: could not read PCRE2_TAG/PCRE2_STAGE from src/Makefile" >&2; exit 1; }
SRC="$STAGE/src"
BUILD="$STAGE/build-$target"
OUT="$STAGE/$target"

if [ -n "$print_path" ]; then
    echo "$OUT"
    exit 0
fi

if [ -f "$OUT/libpcre2-8.a" ] && [ -f "$OUT/include/pcre2.h" ]; then
    echo "pcre2 ($target): already staged ($OUT)"
    exit 0
fi

if [ ! -d "$SRC/.git" ]; then
    echo "pcre2 ($target): cloning PCRE2Project/pcre2 $PCRE2_TAG into $SRC"
    mkdir -p "$STAGE"
    git clone --quiet --depth 1 --branch "$PCRE2_TAG" https://github.com/PCRE2Project/pcre2.git "$SRC"
fi
# The pin is the tag, verified on the tree that is actually about to build.
tag=$(git -C "$SRC" describe --tags --exact-match 2>/dev/null || true)
if [ "$tag" != "$PCRE2_TAG" ]; then
    echo "pcre2 ($target): $SRC is not at $PCRE2_TAG (git describe: '${tag:-untagged}')" >&2
    exit 1
fi

toolchain=$(bash scripts/stage_cmake_args.sh "$target")
TOOLCHAIN=()
while IFS= read -r arg; do TOOLCHAIN+=("$arg"); done <<< "$toolchain"	# bash 3.2 has no mapfile

echo "pcre2 ($target): configuring static build"
printf '  %s\n' "${TOOLCHAIN[@]}"
rm -rf "$BUILD"
mkdir -p "$BUILD"
# PCRE2 10.42 declares an old cmake_minimum_required; CMake 4 refuses a
# minimum below 3.5 unless told the policy floor (older CMake ignores it).
cmake -S "$SRC" -B "$BUILD" -G "Unix Makefiles" \
    "${TOOLCHAIN[@]}" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_STATIC_LIBS=ON \
    -DPCRE2_BUILD_PCRE2_8=ON \
    -DPCRE2_BUILD_PCRE2_16=OFF \
    -DPCRE2_BUILD_PCRE2_32=OFF \
    -DPCRE2_SUPPORT_UNICODE=ON \
    -DPCRE2_SUPPORT_JIT=OFF \
    -DPCRE2_BUILD_PCRE2GREP=OFF \
    -DPCRE2_BUILD_TESTS=OFF \
    -DPCRE2_SUPPORT_LIBBZ2=OFF \
    -DPCRE2_SUPPORT_LIBZ=OFF \
    -DPCRE2_SUPPORT_LIBEDIT=OFF \
    -DPCRE2_SUPPORT_LIBREADLINE=OFF

jobs=$(nproc 2>/dev/null || echo 4)
echo "pcre2 ($target): building (-j$jobs)"
cmake --build "$BUILD" -j"$jobs" --target pcre2-8-static

lib=$(find "$BUILD" -maxdepth 1 -name 'libpcre2-8*.a' | head -1)
[ -n "$lib" ] || { echo "pcre2 ($target): no libpcre2-8 archive in $BUILD" >&2; exit 1; }
rm -rf "$OUT"
mkdir -p "$OUT/include"
cp -p "$lib" "$OUT/libpcre2-8.a"
cp -p "$BUILD/pcre2.h" "$OUT/include/"
echo "pcre2 ($target): staged $OUT"
