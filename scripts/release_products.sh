#!/bin/bash
# release_products.sh — madcide and Chthonia on this release's madc, ON the
# build container, into tmp/v<VERSION>-artifacts/ beside madc's packages
# (scripts/release_artifacts.sh made those). Each product is built by the madc
# in the package it is paired with: madcide by madc's Linux tarball, Chthonia
# by the madc madcide's tarball carries. Each is tested, packaged, and its
# tarball checked as a user runs it, on Linux, then Windows (wine), then the
# Ubuntu 22.04 .debs in the jammy root. The macOS tarballs are
# scripts/mac_release.sh's (on the Macs). Nothing public.
#
#   bash scripts/release_products.sh [step...]
#     steps: madcide_linux chthonia_linux madcide_windows chthonia_windows
#            jammy_madcide jammy_chthonia   (default: all, in that order)
#   MADC_PRODUCTS_DIR  where madcide/ and chthonia/ live (default /workspace),
#                      synced from the NAS first (scripts/mac_release.sh
#                      products-sync)
#   MADC_JAMMY_ROOT    the Ubuntu 22.04 root (default /workspace/jammy)
set -u
cd "$(dirname "$0")/.." || exit 9
M=$(pwd)
P=${MADC_PRODUCTS_DIR:-/workspace}
I=$P/madcide
C=$P/chthonia
J=${MADC_JAMMY_ROOT:-/workspace/jammy}
V=$(cat VERSION)
IV=$(sed -n 's/^#define MADCIDE_VERSION "\(.*\)"$/\1/p' "$I/tools/madcide/madcide_version.h")
CV=$(sed -n 's/^#define CHTHONIA_VERSION "\(.*\)"$/\1/p' "$C/chthonia_version.h")
[ -n "$IV" ] && [ -n "$CV" ] || { echo "release_products: no madcide or Chthonia version under $P" >&2; exit 2; }
OUT=$M/tmp/v$V-artifacts
U=$M/tmp/v$V-unpacked
L=$M/tmp/logs/products-$V
mkdir -p "$U" "$OUT" tmp/logs
step() { echo "=== $1 ($(date -u +%T))"; }
rc() { echo "$1 rc=$2"; [ "$2" -eq 0 ] || exit "$2"; }
echo "release_products: madc $V, madcide $IV, Chthonia $CV"

madcide_linux() {
	step madcide-linux
	[ -f "$OUT/madc-$V-linux-x86_64.tar.gz" ] || { echo "no madc $V tarball in $OUT (release_artifacts.sh linux)"; exit 3; }
	rm -rf "$U/madc-$V-linux-x86_64"
	tar -xzf "$OUT/madc-$V-linux-x86_64.tar.gz" -C "$U" || exit 3
	cd "$I" || exit 2
	rm -rf dist build tmp/package-linux
	export MADC=$U/madc-$V-linux-x86_64/bin/madc MADC_SRC=$M MADC_PACKAGE=$OUT/madc-$V-linux-x86_64.tar.gz
	echo "madc: $($MADC --version | head -1)"
	bash scripts/build.sh > "$L-madcide-build.log" 2>&1; rc "madcide build" $?
	bash scripts/gates.sh > "$L-madcide-gates.log" 2>&1
	local r=$?; tail -n 3 "$L-madcide-gates.log"; rc "madcide gates" $r
	bash scripts/run_tests.sh > "$L-madcide-tests.log" 2>&1
	r=$?; grep -aE "^[0-9]+ passed, " "$L-madcide-tests.log"; rc "madcide tests" $r
	xvfb-run -a bash scripts/run_tests.sh --gui > "$L-madcide-gui.log" 2>&1
	r=$?; grep -aE "^[0-9]+ passed, " "$L-madcide-gui.log"; rc "madcide window tests" $r
	MADCIDE_BUILD=build bash scripts/package_linux.sh > "$L-madcide-pkg.log" 2>&1
	r=$?; tail -n 4 "$L-madcide-pkg.log"; rc "madcide packages" $r
	rm -rf "$U/madcide-$IV-linux-x86_64"
	tar -xzf "dist/madcide-$IV-linux-x86_64.tar.gz" -C "$U"
	bash scripts/check_install.sh "$U/madcide-$IV-linux-x86_64" > "$L-madcide-check.log" 2>&1
	r=$?; tail -n 3 "$L-madcide-check.log"; rc "madcide check_install (tarball)" $r
	cp dist/madcide-$IV-linux-x86_64.tar.gz dist/madcide_$IV-*ubuntu24.04_amd64.deb dist/madcide-$IV-*.x86_64.rpm "$OUT"/
	unset MADC MADC_SRC MADC_PACKAGE
	cd "$M"
}

chthonia_linux() {
	step chthonia-linux
	cd "$C" || exit 2
	rm -rf dist build tmp/package-linux
	export MADC=$U/madcide-$IV-linux-x86_64/bin/madc MADCIDE_PACKAGE=$I/dist/madcide-$IV-linux-x86_64.tar.gz
	echo "madc: $($MADC --version | head -1)"
	bash scripts/run_tests.sh > "$L-chthonia-tests.log" 2>&1
	local r=$?; grep -aE "^[0-9]+ passed, " "$L-chthonia-tests.log"; rc "chthonia tests" $r
	xvfb-run -a bash scripts/run_tests.sh --gui > "$L-chthonia-gui.log" 2>&1
	r=$?; grep -aE "^[0-9]+ passed, " "$L-chthonia-gui.log"; rc "chthonia window tests" $r
	bash scripts/package_linux.sh > "$L-chthonia-pkg.log" 2>&1
	r=$?; tail -n 4 "$L-chthonia-pkg.log"; rc "chthonia packages" $r
	rm -rf "$U/chthonia-$CV-linux-x86_64"
	tar -xzf "dist/chthonia-$CV-linux-x86_64.tar.gz" -C "$U"
	bash scripts/check_install.sh "$U/chthonia-$CV-linux-x86_64" > "$L-chthonia-check.log" 2>&1
	r=$?; tail -n 3 "$L-chthonia-check.log"; rc "chthonia check_install (tarball)" $r
	cp dist/chthonia-$CV-linux-x86_64.tar.gz dist/chthonia_$CV-*ubuntu24.04_amd64.deb dist/chthonia-$CV-*.x86_64.rpm "$OUT"/
	unset MADC MADCIDE_PACKAGE
	cd "$M"
}

madcide_windows() {
	step madcide-windows
	rm -rf "$U/madc-$V-windows-x86_64"
	(cd "$U" && python3 -m zipfile -e "$OUT/madc-$V-windows-x86_64.zip" .) || exit 3
	cd "$I" || exit 2
	rm -rf tmp/package-windows dist/madcide-*-windows-x86_64.zip
	WINEDEBUG=-all MADC="wine $U/madc-$V-windows-x86_64/bin/madc.exe" MADC_PACKAGE=$OUT/madc-$V-windows-x86_64.zip \
		bash scripts/package_windows.sh > "$L-madcide-win.log" 2>&1
	local r=$?; tail -n 3 "$L-madcide-win.log"; rc "madcide windows zip" $r
	rm -rf "$U/madcide-$IV-windows-x86_64"
	(cd "$U" && python3 -m zipfile -e "$I/dist/madcide-$IV-windows-x86_64.zip" .)
	cp "dist/madcide-$IV-windows-x86_64.zip" "$OUT"/
	cd "$M"
}

chthonia_windows() {
	step chthonia-windows
	cd "$C" || exit 2
	rm -rf tmp/package-windows dist/chthonia-*-windows-x86_64.zip
	WINEDEBUG=-all MADC="wine $U/madcide-$IV-windows-x86_64/bin/madc.exe" MADCIDE_PACKAGE=$I/dist/madcide-$IV-windows-x86_64.zip \
		bash scripts/package_windows.sh > "$L-chthonia-win.log" 2>&1
	local r=$?; tail -n 3 "$L-chthonia-win.log"; rc "chthonia windows zip" $r
	cp "dist/chthonia-$CV-windows-x86_64.zip" "$OUT"/
	cd "$M"
}

jammy_ready() {
	mountpoint -q "$J/proc" && mountpoint -q "$J/dev/pts" ||
		{ echo "jammy: mount /proc and /dev/pts in $J first"; exit 4; }
	cp "$M/scripts/jammy_build.sh" "$J"/home/dev/jammy_build.sh
}

jammy_madcide() {
	step jammy-madcide
	jammy_ready
	local mdeb ideb r
	mdeb=$(cd "$J/home/dev/madc/dist" && ls madc_$V-*ubuntu22.04_amd64.deb | tail -1)
	[ -n "$mdeb" ] || { echo "no madc $V 22.04 .deb in the jammy root (release_artifacts.sh jammy)"; exit 3; }
	sudo -n chroot "$J" dpkg -i "/home/dev/madc/dist/$mdeb" > "$L-jammy-madc-install.log" 2>&1; rc "jammy madc install" $?
	rsync -a --delete --exclude=/build/ --exclude=/dist/ --exclude=/tmp/ --exclude=/madc-src "$I"/ "$J"/home/dev/madcide/
	rc "jammy madcide sync" $?
	sudo -n chroot --userspec=dev:dev "$J" /bin/bash /home/dev/jammy_build.sh madcide "/home/dev/madc/dist/$mdeb" > "$L-jammy-madcide.log" 2>&1
	r=$?; grep -aE "rc=|passed" "$L-jammy-madcide.log"; rc "jammy madcide" $r
	ideb=$(cd "$J/home/dev/madcide/dist" && ls madcide_$IV-*ubuntu22.04_amd64.deb | tail -1)
	sudo -n chroot "$J" dpkg -i "/home/dev/madcide/dist/$ideb" > "$L-jammy-madcide-install.log" 2>&1; rc "jammy madcide install" $?
	sudo -n chroot "$J" /bin/bash -c 'mkdir -p /tmp/ci-home; cd /home/dev/madcide; HOME=/tmp/ci-home bash scripts/check_install.sh /usr/lib/madcide' > "$L-jammy-madcide-check.log" 2>&1
	r=$?; tail -n 3 "$L-jammy-madcide-check.log"; rc "jammy madcide check_install" $r
	cp "$J/home/dev/madcide/dist/$ideb" "$OUT"/
}

jammy_chthonia() {
	step jammy-chthonia
	jammy_ready
	local ideb cdeb r
	ideb=$(cd "$J/home/dev/madcide/dist" && ls madcide_$IV-*ubuntu22.04_amd64.deb | tail -1)
	rsync -a --delete --exclude=/.claude/ --exclude=/build/ --exclude=/dist/ --exclude=/tmp/ "$C"/ "$J"/home/dev/chthonia/
	rc "jammy chthonia sync" $?
	sudo -n chroot --userspec=dev:dev "$J" /bin/bash /home/dev/jammy_build.sh chthonia "/home/dev/madcide/dist/$ideb" > "$L-jammy-chthonia.log" 2>&1
	r=$?; grep -aE "rc=|passed" "$L-jammy-chthonia.log"; rc "jammy chthonia" $r
	cdeb=$(cd "$J/home/dev/chthonia/dist" && ls chthonia_$CV-*ubuntu22.04_amd64.deb | tail -1)
	sudo -n chroot "$J" dpkg -i "/home/dev/chthonia/dist/$cdeb" > "$L-jammy-chthonia-install.log" 2>&1; rc "jammy chthonia install" $?
	sudo -n chroot "$J" /bin/bash -c 'mkdir -p /tmp/ci-home; cd /home/dev/chthonia; HOME=/tmp/ci-home bash scripts/check_install.sh /usr/lib/chthonia /usr' > "$L-jammy-chthonia-check.log" 2>&1
	r=$?; tail -n 3 "$L-jammy-chthonia-check.log"; rc "jammy chthonia check_install" $r
	cp "$J/home/dev/chthonia/dist/$cdeb" "$OUT"/
}

for s in ${*:-madcide_linux chthonia_linux madcide_windows chthonia_windows jammy_madcide jammy_chthonia}; do
	case "$s" in
	madcide_linux|chthonia_linux|madcide_windows|chthonia_windows|jammy_madcide|jammy_chthonia) "$s" ;;
	*) echo "release_products: unknown step '$s'" >&2; exit 2 ;;
	esac
done
step done
( cd "$OUT" && rm -f SHA256SUMS && sha256sum -- * > ../SHA256SUMS.$$ && mv ../SHA256SUMS.$$ SHA256SUMS )
ls -l "$OUT"
