#!/bin/bash
# stage.sh — Chthonia's installed files, staged into a root: the one staging
# every package uses (package_linux.sh's .deb, .rpm and tarball,
# package_windows.sh's zip, package_macos.sh's tarball).
#
#   scripts/stage.sh LAYOUT ROOT
#
#   LAYOUT  linux    a prefix: bin/chthonia, its desktop entry and icons
#                    under share/, its bundle under share/madcide/plugins,
#                    its licence under share/doc/chthonia
#           macos    the same, without the desktop entry and icons
#           windows  a madc folder: chthonia.exe and its bundle in bin\
#                    (madcide's data sits beside the programs there), its
#                    licence at the top as LICENSE-chthonia.txt
#   ROOT    where the layout goes: the prefix itself (a package's usr/, or
#           the top of a package's folder)
#
#   CHTHONIA  the built program (default: build/chthonia, with the
#             platform's suffix)
#   MADC      as build.sh; the bundle's library is built by the madcide
#             beside it (MADCIDE, a command, overrides)
#
# The bundle installs as one of madcide's shipped plugins, where the plugin
# search path looks, with its code's library built beside it, so madcide's
# --profile chthonia loads it without compiling it first.
set -eu
. "$(dirname "$0")/common.sh"
if [ $# -ne 2 ]; then
	echo "usage: $0 linux|macos|windows ROOT" >&2
	exit 2
fi
layout=$1
root=$2
case "$layout" in
linux|macos) bindir=$root/bin; data=$root/share/madcide; doc=$root/share/doc/chthonia ;;
windows) bindir=$root/bin; data=$root/bin; doc= ;;
*) echo "usage: $0 linux|macos|windows ROOT" >&2; exit 2 ;;
esac
prog=${CHTHONIA:-$here/build/chthonia$exe}
if [ ! -f "$prog" ]; then
	echo "stage.sh: no $prog — scripts/build.sh first" >&2
	exit 1
fi
chthonia_setup || exit 1

mkdir -p "$bindir" "$data/plugins"
install -m 755 "$prog" "$bindir/chthonia$exe"
rm -rf "$data/plugins/chthonia"
cp -R "$bundle" "$data/plugins/chthonia"
# shellcheck disable=SC2046
if ! capped 300 $(madcide_cmd) --build-plugin "$data/plugins/chthonia" > /dev/null; then
	echo "stage.sh: the bundle's library did not build ($(madcide_cmd) --build-plugin)" >&2
	exit 1
fi
plain_modes "$data/plugins/chthonia"
if [ "$layout" = linux ]; then
	mkdir -p "$root/share/applications"
	install -m 644 "$here/chthonia.desktop" "$root/share/applications/chthonia.desktop"
	python3 "$here/scripts/ico_png_images.py" "$here/chthonia.ico" "$root/share/icons/hicolor" chthonia
	find "$root/share/icons/hicolor" -name chthonia.png -exec chmod 0644 {} +
fi
if [ -n "$doc" ]; then
	mkdir -p "$doc"
	install -m 644 "$here/LICENSE" "$doc/copyright"
else
	install -m 644 "$here/LICENSE" "$root/LICENSE-chthonia.txt"
fi
