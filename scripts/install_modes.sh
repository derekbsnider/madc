#!/bin/bash
# install_modes.sh — give a staged install tree the modes an install gives
# every user, whatever the build host's umask and the checkout's modes:
# directories 0755, files 0755 with an execute bit, else 0644. The one owner
# for every packager's stage root (scripts/stage_install.sh — Linux packages
# and the Homebrew keg — and scripts/package_release_macos.sh); the copied
# plugin bundles keep the checkout's modes, and files written by redirection
# (the gzipped man pages, the icons) the umask's.
# scripts/package_install_gate.sh's check_modes proves the result on the
# artifact itself.
#
#   scripts/install_modes.sh <stage-root>
#
# dpkg-deb also requires plain 0755 directories. GNU chmod's NUMERIC modes
# deliberately preserve a directory's setgid bit (inherited from the
# checkout), so it is cleared symbolically first.
set -e
if [ $# -ne 1 ] || [ ! -d "$1" ]; then
	echo "usage: $0 <stage-root>" >&2
	exit 2
fi
find "$1" -type d -exec chmod g-s {} +
find "$1" -type d -exec chmod 0755 {} +
find "$1" -type f -perm -0100 -exec chmod 0755 {} +
find "$1" -type f ! -perm -0100 -exec chmod 0644 {} +
