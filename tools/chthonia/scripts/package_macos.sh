#!/bin/bash
# package_macos.sh — Chthonia's macOS tarball, for a madc folder:
#
#   chthonia-<ver>-macos-<arch>.tar.gz   unpacks into the madc folder (madc's
#                                        tarball's): tar -xzf … -C <madc folder>
#
#   scripts/package_macos.sh [-o DIST]
#
#   MADC, MADCIDE_INCLUDE, MADCIDE   as build.sh and stage.sh
#   -o DIST                          where the tarball goes (default dist/),
#                                    its line in its SHA256SUMS
#
# Run on a Mac of the architecture it packages: the bundle's library is
# built by the madcide installed there. The version is chthonia_version.h's;
# the tarball is checked to hold exactly the staged files.
set -eu
. "$(dirname "$0")/common.sh"
dist=$here/dist
while [ $# -gt 0 ]; do
	case "$1" in
	-o) dist=$2; shift 2 ;;
	*) echo "usage: $0 [-o DIST]" >&2; exit 2 ;;
	esac
done
if [ "$(uname -s)" != Darwin ]; then
	echo "package_macos.sh: run it on a Mac (the bundle's library is built by the madcide there)" >&2
	exit 1
fi
ver=$(sed -n 's/^#define CHTHONIA_VERSION "\(.*\)"$/\1/p' "$here/chthonia_version.h")
[ -n "$ver" ] || { echo "package_macos.sh: no CHTHONIA_VERSION in chthonia_version.h" >&2; exit 1; }

work=$here/tmp/package-macos
rm -rf "$work"
mkdir -p "$work" "$dist"
dist=$(cd "$dist" && pwd)
bash "$here/scripts/build.sh" -o "$work/chthonia"
CHTHONIA="$work/chthonia" bash "$here/scripts/stage.sh" macos "$work/tar"
plain_modes "$work/tar"
tgz=chthonia-$ver-macos-$(uname -m).tar.gz
tar -C "$work/tar" -czf "$dist/$tgz" bin share
if ! diff <(tar -tzf "$dist/$tgz" | grep -v '/$' | LC_ALL=C sort) \
	  <(cd "$work/tar" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) > "$work/diff"; then
	echo "package_macos.sh: $tgz does not hold exactly the staged files:" >&2
	cat "$work/diff" >&2
	exit 1
fi
refresh_sums "$dist" "$tgz"
echo "package_macos.sh: $dist/$tgz"
