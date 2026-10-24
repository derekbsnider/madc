#!/bin/bash
# mac_products.sh — madcide and Chthonia for macOS, ON a Mac (scripts/
# mac_release.sh products copies the madc tarball in and syncs ~/madcide and
# ~/chthonia first). madcide is built by the madc in this release's madc
# tarball, Chthonia by the madc madcide's tarball carries. Each is tested and
# packaged, then its tarball is unpacked FRESH and checked as a user runs it.
# Nothing of madc is built here.
#
#   bash ~/madc/scripts/mac_products.sh <arm64|x86_64> <work dir> [env-file]
#     <work dir> holds madc-<V>-macos-<arch>.tar.gz; the product tarballs land
#     beside it
set -u
arch=$1
W=$2
[ -n "${3:-}" ] && . "$3"
step() { echo "=== $1 ($(date -u +%T))"; }
rc() { echo "$1 rc=$2"; [ "$2" -eq 0 ] || exit "$2"; }
V=$(cat "$HOME/madc/VERSION")
IV=$(sed -n 's/^#define MADCIDE_VERSION "\(.*\)"$/\1/p' "$HOME/madcide/tools/madcide/madcide_version.h")
CV=$(sed -n 's/^#define CHTHONIA_VERSION "\(.*\)"$/\1/p' "$HOME/chthonia/chthonia_version.h")
ide=madcide-$IV-macos-$arch
chth=chthonia-$CV-macos-$arch
echo "mac_products: madc $V, madcide $IV, Chthonia $CV ($arch)"
rm -rf "$W/unpack"
mkdir -p "$W/unpack"
tar -xzf "$W/madc-$V-macos-$arch.tar.gz" -C "$W/unpack"; rc "unpack madc" $?

step madcide
cd "$HOME/madcide" || exit 2
rm -rf dist build tmp/package-macos
export MADC=$W/unpack/madc-$V-macos-$arch/bin/madc MADC_SRC=$HOME/madc MADC_PACKAGE=$W/madc-$V-macos-$arch.tar.gz MADCIDE_BUILD=$HOME/madcide/build
echo "madc: $($MADC --version | head -1)"
bash scripts/build.sh > "$W/madcide-build.log" 2>&1; rc "madcide build" $?
bash scripts/run_tests.sh > "$W/madcide-tests.log" 2>&1
r=$?; grep -aE "^[0-9]+ passed, " "$W/madcide-tests.log"; rc "madcide tests" $r
bash scripts/package_macos.sh > "$W/madcide-pkg.log" 2>&1
r=$?; tail -n 3 "$W/madcide-pkg.log"; rc "madcide package" $r
tar -xzf "dist/$ide.tar.gz" -C "$W/unpack"; rc "unpack madcide" $?
bash scripts/check_install.sh "$W/unpack/$ide" > "$W/madcide-check.log" 2>&1
r=$?; tail -n 3 "$W/madcide-check.log"; rc "madcide check_install" $r
cp "dist/$ide.tar.gz" "$W/"
unset MADC MADC_SRC MADC_PACKAGE MADCIDE_BUILD

step chthonia
cd "$HOME/chthonia" || exit 2
rm -rf dist build tmp/package-macos
export MADC=$W/unpack/$ide/bin/madc MADCIDE_PACKAGE=$W/$ide.tar.gz
echo "madc: $($MADC --version | head -1)"
bash scripts/run_tests.sh > "$W/chthonia-tests.log" 2>&1
r=$?; grep -aE "^[0-9]+ passed, " "$W/chthonia-tests.log"; rc "chthonia tests" $r
bash scripts/package_macos.sh > "$W/chthonia-pkg.log" 2>&1
r=$?; tail -n 3 "$W/chthonia-pkg.log"; rc "chthonia package" $r
tar -xzf "dist/$chth.tar.gz" -C "$W/unpack"; rc "unpack chthonia" $?
bash scripts/check_install.sh "$W/unpack/$chth" > "$W/chthonia-check.log" 2>&1
r=$?; tail -n 3 "$W/chthonia-check.log"; rc "chthonia check_install" $r
cp "dist/$chth.tar.gz" "$W/"
unset MADC MADCIDE_PACKAGE

step done
ls -l "$W"/*.tar.gz
