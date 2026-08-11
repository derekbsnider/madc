#!/bin/bash
# Stage the OPEN-LICENSED darwin libc header tree the embedded C prelude is
# flattened from (macos-release-lane plan W0.5).
#
# The tree is Apple's own C/POSIX headers — APSL-2.0 / BSD licensed, from
# Apple's open-source releases — as curated, deduplicated, and publicly
# shipped for years in the Zig project's lib/libc/include/any-darwin-any
# (Zig's position since its 0.8.0 release notes: these headers are APSL,
# freely redistributable). Pinning a Zig source release gives a versioned,
# SHA-verified, arch-neutral tree without curating from scratch.
#
# This script is the ONE owner of the pin (version + SHA256). It is
# idempotent: a staged sysroot whose .PROVENANCE matches the pin is left
# untouched. The container is disposable — provision_container.sh runs this
# so a rebuilt container regains the tree from the network.
#
# Layout under $DARWIN_OPEN_HEADERS_HOME (default /workspace/darwin-open-headers):
#   zig-<ver>.tar.xz                      the verified pinned artifact
#   zig-<ver>/lib/libc/include/...       extracted header tree + zig LICENSE
#   sysroot-any-darwin-any/usr/include   symlink -> the any-darwin-any tree
#   sysroot-any-darwin-any/.PROVENANCE   one-line stamp gen_darwin_prelude.sh
#                                        bakes into the umbrella (the release
#                                        gate greps it out of shipped binaries)
#
# Bumping the pin: update ZIG_VERSION + ZIG_SRC_SHA256 (shasum from
# https://ziglang.org/download/index.json), rerun, then re-run the closure
# license audit and refresh docs/licenses/NOTICE-darwin-prelude.md — the
# audit is a fact about the pinned bytes, so it is redone per bump, not per
# build.

set -e

ZIG_VERSION=0.16.0
ZIG_SRC_SHA256=43186959edc87d5c7a1be7b7d2a25efffd22ce5807c7af99067f86f99641bfdf
ZIG_SRC_URL="https://ziglang.org/download/$ZIG_VERSION/zig-$ZIG_VERSION.tar.xz"

HOME_DIR="${DARWIN_OPEN_HEADERS_HOME:-/workspace/darwin-open-headers}"
TARBALL="$HOME_DIR/zig-$ZIG_VERSION.tar.xz"
TREE="$HOME_DIR/zig-$ZIG_VERSION/lib/libc/include/any-darwin-any"
SYSROOT="$HOME_DIR/sysroot-any-darwin-any"
STAMP="zig-$ZIG_VERSION-any-darwin-any sha256=$ZIG_SRC_SHA256"

if [ -f "$SYSROOT/.PROVENANCE" ] && [ "$(cat "$SYSROOT/.PROVENANCE")" = "$STAMP" ] \
        && [ -f "$SYSROOT/usr/include/stdio.h" ]; then
    echo "darwin open headers: already staged ($STAMP)"
    exit 0
fi

mkdir -p "$HOME_DIR"

if ! echo "$ZIG_SRC_SHA256  $TARBALL" | sha256sum -c - >/dev/null 2>&1; then
    echo "darwin open headers: fetching zig-$ZIG_VERSION source tarball"
    curl -fsSL -o "$TARBALL.part" "$ZIG_SRC_URL"
    mv "$TARBALL.part" "$TARBALL"
fi
# Verify LOUDLY — a mismatched tarball must never stage a sysroot.
echo "$ZIG_SRC_SHA256  $TARBALL" | sha256sum -c -

tar -C "$HOME_DIR" -xf "$TARBALL" \
    "zig-$ZIG_VERSION/lib/libc/include/any-darwin-any" \
    "zig-$ZIG_VERSION/LICENSE"

if [ ! -f "$TREE/stdio.h" ]; then
    echo "Error: extracted tree lacks stdio.h — tarball layout changed?" >&2
    exit 1
fi

mkdir -p "$SYSROOT/usr"
ln -sfn "$TREE" "$SYSROOT/usr/include"
printf '%s\n' "$STAMP" > "$SYSROOT/.PROVENANCE"
echo "darwin open headers: staged $SYSROOT ($STAMP)"
