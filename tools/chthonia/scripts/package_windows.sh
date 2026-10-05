#!/bin/bash
# package_windows.sh — Chthonia's Windows zip, for a madc folder:
#
#   chthonia-<ver>-windows-x86_64.zip   unpacks into the madc folder (madc's
#                                       zip's): chthonia.exe goes beside
#                                       madc.exe, where it binds libmadc-0.dll
#                                       and madcide.dll, and its bundle beside
#                                       madcide's shipped plugins
#
#   scripts/package_windows.sh [-o DIST]
#
#   MADC     the Windows madc, as a command: "wine <madc folder>/bin/madc.exe"
#            on Linux, <madc folder>/bin/madc.exe on Windows
#   MADCIDE_INCLUDE, MADCIDE   as build.sh and stage.sh
#   -o DIST  where the zip goes (default dist/), its line in its SHA256SUMS
#
# The version is chthonia_version.h's. The zip is checked to hold exactly
# the staged files.
set -eu
. "$(dirname "$0")/common.sh"
dist=$here/dist
while [ $# -gt 0 ]; do
	case "$1" in
	-o) dist=$2; shift 2 ;;
	*) echo "usage: $0 [-o DIST]" >&2; exit 2 ;;
	esac
done
if [ "$exe" != .exe ]; then
	echo "package_windows.sh: MADC ($madc) is not a Windows madc (madc.exe)" >&2
	exit 1
fi
ver=$(sed -n 's/^#define CHTHONIA_VERSION "\(.*\)"$/\1/p' "$here/chthonia_version.h")
[ -n "$ver" ] || { echo "package_windows.sh: no CHTHONIA_VERSION in chthonia_version.h" >&2; exit 1; }

work=$here/tmp/package-windows
rm -rf "$work"
mkdir -p "$work" "$dist"
dist=$(cd "$dist" && pwd)
bash "$here/scripts/build.sh" -o "$work/chthonia.exe"
CHTHONIA="$work/chthonia.exe" bash "$here/scripts/stage.sh" windows "$work/zip"
plain_modes "$work/zip"
zipname=chthonia-$ver-windows-x86_64.zip
rm -f "$dist/$zipname"
(cd "$work/zip" && zip -q -r -X "$dist/$zipname" .)
if ! diff <(unzip -Z1 "$dist/$zipname" | grep -v '/$' | LC_ALL=C sort) \
	  <(cd "$work/zip" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) > "$work/diff"; then
	echo "package_windows.sh: $zipname does not hold exactly the staged files:" >&2
	cat "$work/diff" >&2
	exit 1
fi
refresh_sums "$dist" "$zipname"
echo "package_windows.sh: $dist/$zipname"
