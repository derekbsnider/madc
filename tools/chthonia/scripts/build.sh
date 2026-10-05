#!/bin/bash
# build.sh — build Chthonia with a madc.
#
#   scripts/build.sh [-o OUT]
#
#   MADC             the madc to build with, as a command (default: madc on
#                    PATH; "wine madc.exe" builds the Windows executable)
#   MADCIDE_INCLUDE  madcide's public headers (default: the installed ones,
#                    <prefix>/share/madcide/include, beside MADC)
#   -o OUT           the executable (default: build/chthonia)
#
# Chthonia is two units (chthonia.json): its plugin's code and its main.
# Both include madcide's public headers, and the program links libmadcide,
# madcide's base as a library (the manifest's "libs"), which the madc install
# provides.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
madc=${MADC:-madc}
out=$here/build/chthonia
while [ $# -gt 0 ]; do
	case "$1" in
	-o) out=$2; shift 2 ;;
	*) echo "usage: $0 [-o OUT]" >&2; exit 2 ;;
	esac
done
inc=${MADCIDE_INCLUDE:-}
if [ -z "$inc" ]; then
	bin=$(command -v "${madc%% *}" || true)
	if [ -z "$bin" ]; then
		echo "build.sh: no madc ($madc) — set MADC" >&2
		exit 1
	fi
	inc=$(cd "$(dirname "$bin")/.." && pwd)/share/madcide/include
fi
if [ ! -f "$inc/madcide/product" ]; then
	echo "build.sh: no madcide headers in $inc — set MADCIDE_INCLUDE" >&2
	exit 1
fi
mkdir -p "$(dirname "$out")"
# shellcheck disable=SC2086
exec $madc -I "$inc" --project "$here/chthonia.json" -o "$out"
