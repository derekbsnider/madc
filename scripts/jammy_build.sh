#!/bin/bash
# jammy_build.sh — the Ubuntu 22.04 .debs, run INSIDE the jammy root on the
# build container (as dev; scripts/release_artifacts.sh and
# scripts/release_products.sh copy it in and chroot to it). One .deb per Ubuntu
# release: 22.04's glibc 2.35 and libstdc++, its g++ 11 headers in the forest.
#
#   jammy_build.sh madc [dry]                 madc's 22.04 .deb (package_release.sh
#                                             --deb; dry = skip the packed suite,
#                                             which already ran on this content)
#   jammy_build.sh madcide <madc 22.04 .deb>  madcide built, tested and packaged
#                                             against that madc (installed)
#   jammy_build.sh chthonia <madcide .deb>    Chthonia likewise against madcide
#
# The trees are /home/dev/{madc,madcide,chthonia}, synced in by the caller.
set -u
export HOME=/home/dev
step() { echo "=== $1 ($(date -u +%T))"; }

case "${1:-}" in
madc)
	cd /home/dev/madc || exit 2
	export LIBGIT2_DIR=/home/dev/stage/libgit2 CMARK_GFM_DIR=/home/dev/stage/cmark-gfm
	[ "${2:-}" = dry ] && export MADC_PKG_SKIP_SUITE=1
	step "madc .deb"
	bash scripts/package_release.sh --deb > tmp/jammy-deb.log 2>&1
	r=$?
	echo "package rc=$r"
	grep -E "passed|failed|packaged|install gate|error" tmp/jammy-deb.log | tail -12
	ls -la dist/
	exit "$r" ;;
madcide)
	cd /home/dev/madcide || exit 2
	rm -rf dist build tmp/package-linux
	export MADC=madc MADC_SRC=/home/dev/madc MADC_PACKAGE=${2:?the madc 22.04 .deb} MADCIDE_BUILD=/home/dev/madcide/build ;;
chthonia)
	cd /home/dev/chthonia || exit 2
	rm -rf dist build tmp/package-linux
	export MADC=/usr/lib/madcide/bin/madc MADCIDE_PACKAGE=${2:?the madcide 22.04 .deb} ;;
*)
	echo "usage: $0 madc [dry] | madcide <madc .deb> | chthonia <madcide .deb>" >&2
	exit 2 ;;
esac

# WebKit's bubblewrap sandbox cannot start inside the chroot.
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
echo "madc: $($MADC --version | head -1)"
fails=0
if [ "$1" = madcide ]; then
	bash scripts/build.sh > tmp-build.log 2>&1
	r=$?; echo "build rc=$r"; [ "$r" -eq 0 ] || exit "$r"
fi
bash scripts/run_tests.sh > tmp-tests.log 2>&1
r=$?; echo "tests rc=$r"; fails=$((fails + (r != 0)))
grep -aE "^[0-9]+ passed, " tmp-tests.log
xvfb-run -a bash scripts/run_tests.sh --gui > tmp-gui.log 2>&1
r=$?; echo "window tests rc=$r"; fails=$((fails + (r != 0)))
grep -aE "^[0-9]+ passed, " tmp-gui.log
bash scripts/package_linux.sh --deb > tmp-package.log 2>&1
r=$?
echo "package rc=$r"
tail -n 4 tmp-package.log
ls -l dist/
[ "$r" -eq 0 ] || exit "$r"
exit "$fails"
