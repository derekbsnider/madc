#!/bin/bash
# Stage the per-target static zstd the hosted-<arch>-macos MODE links
# (forest-carriers plan S1; darwin-host port D3,
# docs/plans/2026-09-01-darwin-host-port.md).
#
#   bash scripts/stage_darwin_zstd.sh <arm64|x86-64>      (from the repo root)
#
# zstd is the forest / PCH container codec the release pack ships; the hosted
# binary must READ the containers the freezer packs, so the codec is a PINNED
# input (v1.5.5 — the same tag the win64 stage builds), not the build host's
# package. brew zstd stays the darwin-host PROBE default (src/Makefile
# DARWIN_ZSTD_PREFIX); the shipped artifact links THIS stage: an explicit
# DARWIN_ZSTD_DIR wins over the brew derivation on a darwin host.
#
# ONE recipe for both build hosts. The compiler and archiver are the hosted
# MODE's own (`make print-CC` / `print-AR`: cross clang-18 + llvm-ar-18 on the
# container, brew llvm@18 on a Mac) — the codec is built by the SAME compiler
# line madc is, never by a copy of it kept here.
#
# Layout under $DARWIN_ZSTD_DIR (default /workspace/zstd):
#   the v1.5.5 source tree          lib/zstd.h is what DARWIN_ZSTD_INC names
#   libzstd-<arch>-macos.a          the per-target archive (DARWIN_ZSTD_LIB)
# Idempotent: a present archive beside a present header is left alone. A
# target-flavored lib/libzstd.a is never left behind for another target's
# build to pick up stale (the provision_container.sh win64 discipline).
set -e

arch="${1:?usage: stage_darwin_zstd.sh <arm64|x86-64>}"
case "$arch" in
    arm64|x86-64) ;;
    x86_64) arch=x86-64 ;;
    *) echo "stage_darwin_zstd: unknown arch '$arch' (arm64 | x86-64)" >&2; exit 2 ;;
esac
cd "$(dirname "$0")/.."

ZSTD_TAG=v1.5.5
DIR="${DARWIN_ZSTD_DIR:-/workspace/zstd}"
OUT="$DIR/libzstd-$arch-macos.a"

if [ -f "$OUT" ] && [ -f "$DIR/lib/zstd.h" ]; then
    echo "darwin zstd: already staged ($OUT)"
    exit 0
fi

if [ ! -d "$DIR/lib" ]; then
    echo "darwin zstd: cloning facebook/zstd $ZSTD_TAG into $DIR"
    git clone --quiet --depth 1 --branch "$ZSTD_TAG" https://github.com/facebook/zstd.git "$DIR"
fi
# The pin is the tag, verified on the tree that is actually about to build —
# a drifted checkout must never stage as v1.5.5.
tag=$(git -C "$DIR" describe --tags --exact-match 2>/dev/null || true)
if [ "$tag" != "$ZSTD_TAG" ]; then
    echo "darwin zstd: $DIR is not at $ZSTD_TAG (git describe: '${tag:-untagged}')" >&2
    exit 1
fi

# The hosted MODE's compiler + archiver, read from the one definition.
MODE="hosted-$arch-macos"
CC=$(make -C src -s MODE="$MODE" DARWIN_ZSTD_DIR="$DIR" print-CC)
AR=$(make -C src -s MODE="$MODE" DARWIN_ZSTD_DIR="$DIR" print-AR)
[ -n "$CC" ] && [ -n "$AR" ] || { echo "darwin zstd: could not read CC/AR of MODE=$MODE from src/Makefile" >&2; exit 1; }
echo "darwin zstd: building $OUT"
echo "  CC=$CC"
echo "  AR=$AR"
jobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
rm -f "$DIR/lib/libzstd.a"
make -C "$DIR/lib" -j"$jobs" BUILD_DIR="obj-$arch-macos" CC="$CC" AR="$AR" libzstd.a
cp -p "$DIR/lib/libzstd.a" "$OUT"
rm -f "$DIR/lib/libzstd.a"
echo "darwin zstd: staged $OUT"
