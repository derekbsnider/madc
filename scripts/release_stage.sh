#!/bin/bash
# release_stage.sh — madcide's and Chthonia's release assets, staged ON THE
# NAS from the build container's verified artifact set (scripts/
# release_artifacts.sh + release_products.sh + mac_release.sh products) under
# GitHub's asset names ('~' is stored as '.'), each product with its own
# SHA256SUMS, and every sum checked against the artifact set's. madc's own
# assets are release.yml's (CI-built on the tag), never staged here.
#
#   bash scripts/release_stage.sh
#     -> tmp/release-stage/madcide-<IV>/ and tmp/release-stage/chthonia-<CV>/
#        (8 files each: rpm, linux tarball, macOS arm64 + x86_64, Windows zip,
#        the 22.04 and 24.04 .debs, SHA256SUMS), ready for
#        gh release create v<IV> -R derekbsnider/madcide <dir>/*
#   MADC_CONTAINER / MADC_CONTAINER_PORT / MADC_PRODUCTS_DIR as mac_release.sh
set -u
cd "$(dirname "$0")/.." || exit 9
V=$(cat VERSION)
CT=${MADC_CONTAINER:-dev@localhost}
CP=${MADC_CONTAINER_PORT:-2299}
P=${MADC_PRODUCTS_DIR:-/workspace}
IV=$(sed -n 's/^#define MADCIDE_VERSION "\(.*\)"$/\1/p' "$P/madcide/tools/madcide/madcide_version.h")
CV=$(sed -n 's/^#define CHTHONIA_VERSION "\(.*\)"$/\1/p' "$P/chthonia/chthonia_version.h")
A=tmp/v$V-artifacts
S=tmp/release-stage
die() { echo "release_stage: $*" >&2; exit 2; }

# The artifact set, copied without owners or modes (the NAS refuses to set them).
mkdir -p "$A"
rsync -rt -e "ssh -p $CP" "$CT:/workspace/madc/$A/" "$A/" || die "copy of the artifact set failed"
( cd "$A" && sha256sum -c --quiet SHA256SUMS ) || die "the artifact set does not match its SHA256SUMS"

rm -rf "$S"
red=0
for p in "madcide-$IV" "chthonia-$CV"; do
	n=${p%-*}
	v=${p##*-}
	d=$S/$p
	mkdir -p "$d"
	for f in "$A"/${n}[-_]${v}[-_.]*; do
		[ -e "$f" ] || continue
		b=$(basename "$f")
		cp -p "$f" "$d/${b//\~/.}"
	done
	( cd "$d" && sha256sum -- $(ls | grep -v '^SHA256SUMS$') > SHA256SUMS )
	count=$(ls "$d" | grep -vc '^SHA256SUMS$')
	echo "== $p: $count packages"
	[ "$count" -eq 7 ] || { echo "   expected 7 packages"; red=1; }
	# every staged sum equals the artifact set's (under the package's own name)
	while read -r h b; do
		o=${b//.ubuntu/\~ubuntu}
		grep -q "^$h  $o\$" "$A/SHA256SUMS" || { echo "   MISMATCH against the artifact set: $b"; red=1; }
	done < "$d/SHA256SUMS"
done
[ $red -eq 0 ] || exit 1
echo "release_stage: staged under $S"
ls -l "$S"/*
