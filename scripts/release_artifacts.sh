#!/bin/bash
# release_artifacts.sh — madc's release packages for the owner's test builds,
# built ON the build container from this tree, into tmp/v<VERSION>-artifacts/
# with a SHA256SUMS. Order matters: Linux first (package_release.sh's restore
# rebuild deletes the macOS packs), then Windows (the packed PE under wine),
# then the macOS cross tarballs, then the Ubuntu 22.04 .deb in the jammy root.
# Nothing public: no push, tag or release. The published assets are
# release.yml's (rehearsed by scripts/release_rehearsal.sh); these are the set
# the owner tests on three environments before a push, and the madc that
# scripts/release_products.sh builds madcide and Chthonia with.
#
#   bash scripts/release_artifacts.sh [linux|windows|macos|jammy ...]
#                                     (default: every step, in that order)
#   MADC_JAMMY_ROOT   the Ubuntu 22.04 root (default /workspace/jammy), with
#                     /proc and /dev/pts mounted
set -u
cd "$(dirname "$0")/.." || exit 9
M=$(pwd)
V=$(cat VERSION)
J=${MADC_JAMMY_ROOT:-/workspace/jammy}
OUT=$M/tmp/v$V-artifacts
L=$M/tmp/logs/artifacts-$V
mkdir -p "$OUT" tmp/logs
step() { echo "=== $1 ($(date -u +%T))"; }
rc() { echo "$1 rc=$2"; [ "$2" -eq 0 ] || exit "$2"; }
JOBS=$(bash scripts/build_jobs.sh)

linux() {
	step madc-linux
	bash scripts/package_release.sh > "$L-linux.log" 2>&1
	local r=$?
	grep -aE "^[0-9]+ passed, |package_install_gate: (PASS|FAIL)" "$L-linux.log" | tail -6
	rc "madc linux" $r
	cp dist/madc-$V-linux-x86_64.tar.gz dist/madc_$V-*ubuntu24.04_amd64.deb dist/madc-$V-*.x86_64.rpm "$OUT"/
}

windows() {
	step madc-windows
	make -C src -j"$JOBS" release-windows > "$L-winbuild.log" 2>&1; rc "release-windows" $?
	bash scripts/package_release_windows.sh > "$L-winzip.log" 2>&1
	local r=$?
	grep -aE "package_install_gate: (PASS|FAIL)" "$L-winzip.log" | tail -2
	rc "madc windows zip" $r
	cp "dist/madc-$V-windows-x86_64.zip" "$OUT"/
}

macos() {
	step madc-macos
	make -C src -j"$JOBS" release-macos > "$L-macbuild.log" 2>&1; rc "release-macos" $?
	bash scripts/package_release_macos.sh > "$L-macpkg.log" 2>&1
	local r=$?
	tail -n 4 "$L-macpkg.log"
	rc "madc macos tarballs" $r
	cp dist/madc-$V-macos-*.tar.gz "$OUT"/
}

jammy() {
	step jammy-madc
	# Without /proc, /dev/fd is gone and gen_embedded_headers.sh refuses.
	mountpoint -q "$J/proc" && mountpoint -q "$J/dev/pts" ||
		{ echo "jammy: mount /proc and /dev/pts in $J first"; exit 4; }
	rsync -a --delete --exclude=/tmp/ --exclude=/bin/ --exclude=/obj/ --exclude=/lib/ --exclude=/dist/ \
		--exclude=MadSMAUG --exclude=autom4te.cache \
		--exclude=src/sys_include_paths.cpp --exclude=src/predefined_macros.cpp \
		--exclude=/config.mk --exclude=/config.status --exclude=/config.log \
		--exclude=/include/config.h --exclude=/Makefile --exclude=/libmadc.pc \
		"$M"/ "$J"/home/dev/madc/
	rc "jammy madc sync" $?
	cp scripts/jammy_build.sh "$J"/home/dev/jammy_build.sh
	sudo -n chroot --userspec=dev:dev "$J" /bin/bash -c 'cd /home/dev/madc; ./configure --enable-madcdat=no' > "$L-jammy-configure.log" 2>&1
	rc "jammy configure" $?
	sudo -n chroot --userspec=dev:dev "$J" /bin/bash /home/dev/jammy_build.sh madc dry > "$L-jammy.log" 2>&1
	local r=$?
	tail -n 8 "$L-jammy.log"
	rc "jammy madc .deb" $r
	local mdeb
	mdeb=$(cd "$J/home/dev/madc/dist" && ls madc_$V-*ubuntu22.04_amd64.deb | tail -1)
	sudo -n chroot "$J" dpkg -i "/home/dev/madc/dist/$mdeb" > "$L-jammy-install.log" 2>&1
	rc "jammy madc install" $?
	sudo -n chroot "$J" /bin/bash -c 'madc --version | head -1; printf "int main(void){return 0;}\n" > /tmp/j.c; madc /tmp/j.c; echo "jammy run rc=$?"' > "$L-jammy-smoke.log" 2>&1
	tail -n 2 "$L-jammy-smoke.log"
	grep -q "jammy run rc=0" "$L-jammy-smoke.log"
	rc "jammy madc smoke" $?
	cp "$J/home/dev/madc/dist/$mdeb" "$OUT"/
}

for s in ${*:-linux windows macos jammy}; do
	case "$s" in
	linux|windows|macos|jammy) "$s" ;;
	*) echo "release_artifacts: unknown step '$s'" >&2; exit 2 ;;
	esac
done
step done
# the sums file never lists itself
( cd "$OUT" && rm -f SHA256SUMS && sha256sum -- * > ../SHA256SUMS.$$ && mv ../SHA256SUMS.$$ SHA256SUMS )
ls -l "$OUT"
