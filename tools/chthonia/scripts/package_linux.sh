#!/bin/bash
# package_linux.sh — Chthonia's Linux packages, for a madc installation:
#
#   chthonia_<ver>-<rel>_<arch>.deb       install under /usr beside madc's
#   chthonia-<ver>-<rel>.<arch>.rpm       package, which they require
#   chthonia-<ver>-linux-<arch>.tar.gz    unpacks into a madc folder (madc's
#                                         relocatable tarball): tar -xzf … -C <madc folder>
#
#   scripts/package_linux.sh [-o DIST]
#
#   MADC, MADCIDE_INCLUDE, MADCIDE   as build.sh and stage.sh
#   PKG_RELEASE                      the package revision (default 1)
#   -o DIST                          where the packages go (default dist/),
#                                    their lines in its SHA256SUMS
#
# The version is chthonia_version.h's. The packages require the madc that
# built them, or a newer one: chthonia links madc's libraries (libmadc,
# libmadcide), and an older madc lacks what it was built against. A window
# is Chthonia's default, so they also require the WebKitGTK 6.0 and GTK 4
# libraries madc's GUI module binds. Each package is checked to hold exactly
# the staged files, each readable by every user.
set -eu
. "$(dirname "$0")/common.sh"
dist=$here/dist
while [ $# -gt 0 ]; do
	case "$1" in
	-o) dist=$2; shift 2 ;;
	*) echo "usage: $0 [-o DIST]" >&2; exit 2 ;;
	esac
done
ver=$(sed -n 's/^#define CHTHONIA_VERSION "\(.*\)"$/\1/p' "$here/chthonia_version.h")
rel=${PKG_RELEASE:-1}
# shellcheck disable=SC2086
mver=$($madc --version | sed -n 's/^madc \([0-9]*\.[0-9]*\.[0-9]*\).*/\1/p' | head -1)
if [ -z "$ver" ] || [ -z "$mver" ]; then
	echo "package_linux.sh: no version (chthonia_version.h: '$ver', $madc --version: '$mver')" >&2
	exit 1
fi
case "$(uname -m)" in
x86_64) deb_arch=amd64; rpm_arch=x86_64 ;;
aarch64) deb_arch=arm64; rpm_arch=aarch64 ;;
*) echo "package_linux.sh: no package architecture for $(uname -m)" >&2; exit 1 ;;
esac
maint="Derek Snider <coding@psychedeliccanada.ca>"
home=$(sed -n 's/^#define CHTHONIA_HOME_PAGE "\(.*\)"$/\1/p' "$here/chthonia_version.h")
summary="An easy IDE to learn C and C++"
desc="Chthonia is a window to learn C and C++ in: the editor, a C shell
below it (a REPL) and the Symbols view beside it, Run and Stop on the
toolbar. It is built on madc's IDE, madcide, and runs on madc's engine."

work=$here/tmp/package-linux
rm -rf "$work"
mkdir -p "$work" "$dist"
dist=$(cd "$dist" && pwd)
bash "$here/scripts/build.sh" -o "$work/chthonia"
strip --strip-unneeded "$work/chthonia"
stage() {	# ROOT — the staged prefix
	CHTHONIA="$work/chthonia" bash "$here/scripts/stage.sh" linux "$1"
}
# The staged files of PREFIX, one path per line, relative to it.
staged() {
	(cd "$1" && find . -type f | sed 's|^\./||' | LC_ALL=C sort)
}
# check NAME LISTING STAGE — the package holds exactly the staged files.
check() {
	if ! diff <(LC_ALL=C sort <<< "$2") <(staged "$3") > "$work/diff"; then
		echo "package_linux.sh: $1 does not hold exactly the staged files:" >&2
		cat "$work/diff" >&2
		exit 1
	fi
}
# check_modes NAME LONG-LISTING — every entry is a 0755 directory or
# executable, or a 0644 file (readable by every user).
check_modes() {
	local bad
	bad=$(awk '$1 !~ /^(drwxr-xr-x|-rwxr-xr-x|-rw-r--r--)$/' <<< "$2")
	if [ -n "$bad" ]; then
		echo "package_linux.sh: $1 installs entries with other modes:" >&2
		echo "$bad" >&2
		exit 1
	fi
}

# ---- deb ----
debroot=$work/deb
stage "$debroot/usr"
mkdir -p "$debroot/DEBIAN"
cat > "$debroot/DEBIAN/control" << EOF
Package: chthonia
Version: $ver-$rel
Section: devel
Priority: optional
Architecture: $deb_arch
Maintainer: $maint
Depends: madc (>= $mver), libwebkitgtk-6.0-4, libgtk-4-1
Homepage: $home
Description: $summary
$(printf '%s\n' "$desc" | sed 's/^/ /')
EOF
plain_modes "$debroot"
deb=chthonia_$ver-${rel}_$deb_arch.deb
dpkg-deb --build --root-owner-group "$debroot" "$dist/$deb" > /dev/null
check "$deb" "$(dpkg-deb -c "$dist/$deb" | awk '$1 !~ /^d/ { print $6 }' | sed 's|^\./usr/||')" "$debroot/usr"
check_modes "$deb" "$(dpkg-deb -c "$dist/$deb")"

# ---- rpm ----
rpmtop=$work/rpm
mkdir -p "$rpmtop/BUILD" "$rpmtop/RPMS" "$rpmtop/SPECS" "$rpmtop/SOURCES"
buildroot=$rpmtop/BUILDROOT/chthonia-$ver-$rel.$rpm_arch
stage "$buildroot/usr"
plain_modes "$buildroot"
cat > "$rpmtop/SPECS/chthonia.spec" << EOF
Name: chthonia
Version: $ver
Release: $rel
Summary: $summary
License: MPL-2.0
URL: $home
AutoReqProv: no
Requires: madc >= $mver
Requires: webkitgtk6.0
Requires: gtk4
%define debug_package %{nil}
%define __strip /bin/true
%define _build_id_links none

%description
$desc

%files
/usr/bin/chthonia
/usr/share/applications/chthonia.desktop
/usr/share/icons/hicolor/*/apps/chthonia.png
/usr/share/madcide/plugins/chthonia
/usr/share/doc/chthonia
EOF
rpmbuild --define "_topdir $rpmtop" --buildroot "$buildroot" -bb "$rpmtop/SPECS/chthonia.spec" > "$work/rpmbuild.log" 2>&1 \
	|| { tail -20 "$work/rpmbuild.log" >&2; exit 1; }
rpm=chthonia-$ver-$rel.$rpm_arch.rpm
cp "$rpmtop/RPMS/$rpm_arch/$rpm" "$dist/"
# rpmbuild removes its build root; the deb's is the same staging.
check "$rpm" "$(rpm -qlvp "$dist/$rpm" | awk '$1 !~ /^d/ { print $NF }' | sed 's|^/usr/||')" "$debroot/usr"
check_modes "$rpm" "$(rpm -qlvp "$dist/$rpm")"

# ---- tarball: the prefix's own paths, so it unpacks into a madc folder ----
tarstage=$work/tar
stage "$tarstage"
plain_modes "$tarstage"
tgz=chthonia-$ver-linux-$rpm_arch.tar.gz
tar -C "$tarstage" --owner=0 --group=0 -czf "$dist/$tgz" bin share
check "$tgz" "$(tar -tzf "$dist/$tgz" | grep -v '/$')" "$tarstage"
check_modes "$tgz" "$(tar -tzvf "$dist/$tgz")"

refresh_sums "$dist" "$deb" "$rpm" "$tgz"
echo "package_linux.sh: $dist/{$deb,$rpm,$tgz} (madc >= $mver)"
