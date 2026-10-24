#!/bin/bash
# mac_release.sh — the release's Mac work and product-tree syncs, driven FROM
# THE NAS (where the trees and the ssh keys are; the NAS itself builds and
# runs nothing).
#
#   mac_release.sh sync <arm64|x86-64>      this madc tree to the Mac's ~/madc
#   mac_release.sh lanes <arm64|x86-64>     sync, then scripts/darwin_lane.sh
#                                           (battery + the FULL suite on the -O2
#                                           madc-release-<arch>-macos: darwin-suite)
#                                           and the interactive subset on the same
#                                           binary (interactive-darwin)
#   mac_release.sh products <arm64|x86-64>  madcide + Chthonia macOS tarballs on
#                                           the Mac (scripts/mac_products.sh), from
#                                           the build container's madc tarball
#                                           (scripts/release_artifacts.sh macos),
#                                           back into its artifact set
#   mac_release.sh products-sync            the NAS's madcide and Chthonia trees
#                                           to the build container
#                                           (scripts/release_products.sh)
#
# Each prints its tallies. Record the lanes once BOTH arches are green
# (lane_ledger.sh record darwin-suite / interactive-darwin "<both tallies>").
#
# Knobs:
#   MADC_MAC_ARM64_SSH  the arm64 Mini (default derek@192.168.1.55: by address,
#                       its mDNS alias hangs)
#   MADC_MAC_X86_SSH    the Intel MacBook (default madc-mac-x86)
#   MADC_MAC_X86_ENV    its lane environment file on the Mac (default
#                       ~/madc-lane-env.sh). Its autoconf is broken and may not
#                       be upgraded, so it gets the build container's generated
#                       configure + include/config.h.in (LANE_PREGENERATED=1)
#   MADC_CONTAINER      the build container's ssh target (default dev@localhost)
#   MADC_CONTAINER_PORT (default 2299)
#   MADC_PRODUCTS_DIR   where madcide/ and chthonia/ live, on the NAS and the
#                       container alike (default /workspace)
set -u
cd "$(dirname "$0")/.." || exit 9
M=$(pwd)
V=$(cat VERSION)
CT=${MADC_CONTAINER:-dev@localhost}
CP=${MADC_CONTAINER_PORT:-2299}
P=${MADC_PRODUCTS_DIR:-/workspace}
X86_ENV=${MADC_MAC_X86_ENV:-'~/madc-lane-env.sh'}
W=$M/tmp/mac-release
mkdir -p "$W" tmp/logs
die() { echo "mac_release: $*" >&2; exit 2; }

host_of() {
	case "$1" in
	arm64) echo "${MADC_MAC_ARM64_SSH:-derek@192.168.1.55}" ;;
	x86-64) echo "${MADC_MAC_X86_SSH:-madc-mac-x86}" ;;
	*) die "arch is arm64 or x86-64, not '$1'" ;;
	esac
}
# the lane environment prefix for a remote command on that Mac
env_of() { [ "$1" = x86-64 ] && echo ". $X86_ENV; " || true; }

sync_mac() {
	local arch=$1 host
	host=$(host_of "$arch")
	echo "=== sync $arch -> $host ($(date -u +%T))"
	rsync -a --delete -e "ssh -o ConnectTimeout=15" \
		--exclude=/obj/ --exclude=/bin/ --exclude=/lib/ --exclude=/dist/ --exclude=/tmp/ \
		--exclude=MadSMAUG --exclude=autom4te.cache --exclude=node_modules \
		--exclude=/configure --exclude=/config.status --exclude=/config.log \
		--exclude=/Makefile --exclude=/config.mk --exclude=/libmadc.pc \
		--exclude=/include/config.h --exclude=/include/config.h.in \
		--exclude=src/sys_include_paths.cpp --exclude=src/predefined_macros.cpp \
		"$M"/ "$host":madc/ || die "sync to $host failed"
	if [ "$arch" = x86-64 ]; then
		scp -q -P "$CP" "$CT:/workspace/madc/configure" "$W/configure" &&
			scp -q -P "$CP" "$CT:/workspace/madc/include/config.h.in" "$W/config.h.in" ||
			die "no generated configure on the build container"
		scp -q "$W/configure" "$host":madc/configure &&
			scp -q "$W/config.h.in" "$host":madc/include/config.h.in &&
			ssh "$host" 'chmod +x ~/madc/configure' || die "pregenerated configure to $host failed"
	fi
	ssh "$host" 'cd ~/madc && git log --oneline -1'
}

lanes() {
	local arch=$1 host log pre=""
	host=$(host_of "$arch")
	sync_mac "$arch"
	log=tmp/logs/mac-$V-$arch
	[ "$arch" = x86-64 ] && pre="$(env_of x86-64)LANE_PREGENERATED=1 "
	echo "=== darwin lane $arch ($(date -u +%T))"
	# shellcheck disable=SC2029
	ssh "$host" "${pre}bash ~/madc/scripts/darwin_lane.sh $arch $([ "$arch" = x86-64 ] && echo "$X86_ENV")" > "$log.log" 2>&1
	echo "darwin lane rc=$?"
	grep -a -E "passed|battery|^FAIL|rc=" "$log.log" | tail -12
	ssh "$host" 'head -10 ~/madc/tmp/suite-failures.txt 2>/dev/null'
	echo "=== interactive $arch ($(date -u +%T))"
	# shellcheck disable=SC2029
	ssh "$host" "$(env_of "$arch")cd ~/madc && MADC_BIN=bin/madc-release-$arch-macos MADC_INTERACTIVE_UNITS=no MADC_SKIP_EXT=\"darwin libcxx darwin-$arch macos-\$(sw_vers -productVersion | cut -d. -f1)\" bash scripts/interactive_lane.sh" > "$log-interactive.log" 2>&1
	echo "interactive rc=$?"
	grep -a -E "passed|^FAIL" "$log-interactive.log" | tail -4
}

products() {
	local arch=$1 host tarch p IV CV
	host=$(host_of "$arch")
	tarch=${arch/x86-64/x86_64}
	IV=$(sed -n 's/^#define MADCIDE_VERSION "\(.*\)"$/\1/p' "$P/madcide/tools/madcide/madcide_version.h")
	CV=$(sed -n 's/^#define CHTHONIA_VERSION "\(.*\)"$/\1/p' "$P/chthonia/chthonia_version.h")
	local art="$CT:/workspace/madc/tmp/v$V-artifacts"
	sync_mac "$arch"
	scp -q -P "$CP" "$art/madc-$V-macos-$tarch.tar.gz" "$W/" || die "no madc $V macOS $tarch tarball on the build container (release_artifacts.sh macos)"
	ssh "$host" "mkdir -p ~/release-$V" || die "ssh $host failed"
	scp -q "$W/madc-$V-macos-$tarch.tar.gz" "$host:release-$V/" || die "madc tarball to $host failed"
	for p in madcide chthonia; do
		rsync -a --exclude .git --exclude /tmp/ --exclude /build/ --exclude /dist/ "$P/$p/" "$host":$p/ ||
			die "sync $p to $host failed"
	done
	echo "=== products $arch ($(date -u +%T))"
	# shellcheck disable=SC2029
	ssh "$host" "bash ~/madc/scripts/mac_products.sh $tarch ~/release-$V $([ "$arch" = x86-64 ] && echo "$X86_ENV")" > "tmp/logs/mac-$V-$arch-products.log" 2>&1
	local r=$?
	grep -aE '^=== |rc=|passed' "tmp/logs/mac-$V-$arch-products.log" | tail -20
	[ $r -eq 0 ] || die "products on $host rc=$r"
	for p in "madcide-$IV" "chthonia-$CV"; do
		scp -q "$host:release-$V/$p-macos-$tarch.tar.gz" "$W/" &&
			scp -q -P "$CP" "$W/$p-macos-$tarch.tar.gz" "$art/" || die "$p tarball back failed"
	done
	echo "mac products $arch done"
}

products_sync() {
	local p
	for p in madcide chthonia; do
		echo "=== $p -> the build container"
		rsync -a --delete -e "ssh -p $CP" --exclude=/build/ --exclude=/dist/ --exclude=/tmp/ \
			"$P/$p/" "$CT:$P/$p/" || die "sync $p failed"
	done
}

case "${1:-}" in
sync) sync_mac "${2:?arch}" ;;
lanes) lanes "${2:?arch}" ;;
products) products "${2:?arch}" ;;
products-sync) products_sync ;;
*) echo "usage: $0 sync|lanes|products <arm64|x86-64> | products-sync" >&2; exit 2 ;;
esac
