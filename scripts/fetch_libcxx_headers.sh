#!/bin/bash
# Stage the EXACT libc++ header text the build container serves as madc's
# C++ world (darwin-host port D2, docs/plans/2026-09-01-darwin-host-port.md).
#
# The frozen C++ groves — and the -stdlib=libc++ parity lane's zero-failure
# corpus — are baselined on the container's apt package, libc++-18-dev
# 1:18.1.3-1ubuntu1 (LLVM 18.1.3). A darwin build host must serve the SAME
# text or its groves drift: brew llvm@18 is 18.1.8, and the first native
# self-freeze on a mac runner measured 837 units / 54 pack parse errors
# against the container's 835 / 64, plus groves referencing constructs madc
# has not met (basic_string::__align_it). Header text is a pinned INPUT,
# not a host convenience — this script is the ONE owner of that pin.
#
# The deb is Ubuntu's build of Apache-2.0-with-LLVM-exception headers (the
# same license text the mac tarball already ships as libc++-copyright.txt);
# every file under c++/v1 belongs to this one package (dpkg -S verified),
# and __config_site carries no arch-specific content, so the amd64 deb IS
# the container's text byte for byte (diff -r against the installed tree:
# empty). Idempotent: a stage whose .PROVENANCE matches the pin is left
# alone. Works on the container (unneeded there — apt's tree is the default)
# and on a Mac with brew coreutils + zstd (`ar` is Xcode's).
#
# Layout under $LIBCXX_HEADERS_HOME (default /workspace/libcxx-headers):
#   libc++-18-dev_<ver>_amd64.deb     the verified pinned artifact
#   include/c++/v1/...                the header tree (what LLVM_LIBCXX_INC names)
#   .PROVENANCE                       one-line stamp: package, version, sha256
#
# Bumping the pin = a served-C++-world change: move the container (apt) and
# this file together, then re-baseline docs/parity/pack-degradation-baseline.txt
# and the libc++ lane — never one side alone.

set -e

DEB_PACKAGE=libc++-18-dev
DEB_VERSION=18.1.3-1ubuntu1
DEB_ARCH=amd64
DEB_SHA256=31223b203414f2a4bb2a115e5626aa7c8ccab031740cf5ccf3d1da21eb6a208d
DEB_URL="http://archive.ubuntu.com/ubuntu/pool/universe/l/llvm-toolchain-18/${DEB_PACKAGE}_${DEB_VERSION}_${DEB_ARCH}.deb"
DEB_TREE=usr/lib/llvm-18/include/c++/v1
EXPECT_FILES=1017

HOME_DIR="${LIBCXX_HEADERS_HOME:-/workspace/libcxx-headers}"
DEB="$HOME_DIR/${DEB_PACKAGE}_${DEB_VERSION}_${DEB_ARCH}.deb"
TREE="$HOME_DIR/include/c++/v1"
STAMP="$DEB_PACKAGE $DEB_VERSION $DEB_ARCH sha256=$DEB_SHA256"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

if [ -f "$HOME_DIR/.PROVENANCE" ] && [ "$(cat "$HOME_DIR/.PROVENANCE")" = "$STAMP" ] \
        && [ -f "$TREE/__config" ]; then
    echo "libc++ headers: already staged ($STAMP)"
    exit 0
fi

for t in ar zstd tar curl; do
    command -v "$t" >/dev/null 2>&1 || { echo "libc++ headers: needs '$t' (a Mac: brew install zstd coreutils; Xcode's ar)" >&2; exit 1; }
done

mkdir -p "$HOME_DIR"
if [ ! -f "$DEB" ] || [ "$(sha256_of "$DEB")" != "$DEB_SHA256" ]; then
    echo "libc++ headers: fetching $DEB_URL"
    curl -fsSL -o "$DEB.part" "$DEB_URL"
    mv "$DEB.part" "$DEB"
fi
# Verify LOUDLY — a mismatched deb must never stage a header tree.
got=$(sha256_of "$DEB")
if [ "$got" != "$DEB_SHA256" ]; then
    echo "libc++ headers: sha256 mismatch for $DEB: $got != $DEB_SHA256" >&2
    exit 1
fi

# A .deb is an ar archive; noble's data member is zstd-compressed tar with
# ./-prefixed paths. Extract only the header tree, into a scratch dir, then
# move it into place atomically so a half-extracted stage never passes.
WORK=$(mktemp -d "$HOME_DIR/.extract.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
( cd "$WORK" && ar x "$DEB" data.tar.zst )
zstd -dc "$WORK/data.tar.zst" | tar -x -C "$WORK" "./$DEB_TREE"
n=$(find "$WORK/$DEB_TREE" -type f | wc -l | tr -d ' ')
if [ "$n" -ne "$EXPECT_FILES" ]; then
    echo "libc++ headers: extracted $n files, expected $EXPECT_FILES — deb layout changed?" >&2
    exit 1
fi
grep -Eq '^#[[:space:]]*define[[:space:]]+_LIBCPP_VERSION[[:space:]]+180100([[:space:]]|$)' "$WORK/$DEB_TREE/__config" \
    || { echo "libc++ headers: __config does not define _LIBCPP_VERSION 180100 (LLVM 18.1.x)" >&2; exit 1; }
rm -rf "$TREE"
mkdir -p "$HOME_DIR/include/c++"
mv "$WORK/$DEB_TREE" "$TREE"
printf '%s\n' "$STAMP" > "$HOME_DIR/.PROVENANCE"
echo "libc++ headers: staged $n files at $TREE ($STAMP)"
