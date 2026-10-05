#!/bin/bash
# build.sh — build Chthonia with a madc.
#
#   scripts/build.sh [-o OUT]
#
#   MADC             the madc to build with, as a command (default: madc on
#                    PATH; "wine madc.exe" builds the Windows executable)
#   MADCIDE_INCLUDE  madcide's public headers (default: the installed ones,
#                    beside MADC; scripts/common.sh)
#   -o OUT           the executable (default: build/chthonia, with the
#                    platform's suffix)
#
# Chthonia is two units (chthonia.json): its plugin's code and its main.
# Both include madcide's public headers, and the program links libmadcide,
# madcide's base as a library (the manifest's "libs"), which the madc install
# provides.
set -eu
. "$(dirname "$0")/common.sh"
out=$here/build/chthonia$exe
while [ $# -gt 0 ]; do
	case "$1" in
	-o) out=$2; shift 2 ;;
	*) echo "usage: $0 [-o OUT]" >&2; exit 2 ;;
	esac
done
chthonia_setup || exit 1
mkdir -p "$(dirname "$out")"
# shellcheck disable=SC2086
exec $madc -I "$inc" --project "$here/chthonia.json" -o "$out"
