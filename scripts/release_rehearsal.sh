#!/bin/bash
# release_rehearsal.sh — the release REHEARSAL (owner 2026-10-24), before the
# tag: every package built on GitHub's fresh runners exactly as the tag push
# will build it, then madcide's whole CI run against those packages.
#
# Why: v0.106.0 needed v0.106.1 for two regressions that only the tag push and
# the products' CI could show. The Windows package failed to build on a fresh
# runner, which has no host madc. A Windows run from madcide printed a pack
# writer report on stderr, and only madcide's Windows suite saw it. Every
# local lane runs in a warm tree with a madc we built ourselves, and the
# products' CI took madc from a PUBLISHED release, so both showed after the
# tag.
#
#   1. release.yml, dispatched with rehearsal=true on the candidate commit
#      (tag = v<VERSION there>): Linux .deb/.rpm/tarball, the Ubuntu 22.04
#      .deb, the Windows zip, both macOS tarballs, each install-gated on its
#      runner, into the DRAFT release. No Homebrew bottle: it builds from the
#      tag's source archive, which does not exist yet (the brew-linux lane
#      covers it). The tag push later rebuilds into the same draft.
#   2. madcide's ci.yml, dispatched with madc_tag = that draft and madc_ref =
#      the candidate commit: madcide built, its suite and window tests run,
#      packaged and installed alone, on Linux, Ubuntu 22.04, Windows under
#      wine, genuine Windows and both Macs.
#
#   bash scripts/release_rehearsal.sh [<candidate>]   (default HEAD; pushed)
#
# Green on both records ledger rows release-ci and products (promote=release)
# when the candidate is HEAD. Runs where gh is authenticated (the NAS); it
# builds and runs nothing locally. Refuses a version already published: a
# rehearsal writes only a draft.
set -u
cd "$(dirname "$0")/.." || exit 9
MADC_REPO=derekbsnider/madc
MADCIDE_REPO=derekbsnider/madcide
OUT=tmp/release-rehearsal
die() { echo "release_rehearsal: $*" >&2; exit 2; }

ref=${1:-HEAD}
sha=$(git rev-parse --verify "$ref^{commit}") || die "no commit $ref"
ver=$(git show "$sha:VERSION") || die "no VERSION at $sha"
tag="v$ver"
[ -n "$(git branch -r --contains "$sha")" ] || die "push $sha first: the runners check it out"
if [ "$(gh release view "$tag" -R "$MADC_REPO" --json isDraft --jq .isDraft 2>/dev/null)" = false ]; then
	die "$tag is already published; rehearse a candidate before its release"
fi
mkdir -p "$OUT" || exit 2
log="$OUT/$tag-${sha:0:9}.log"
echo "release_rehearsal: $tag at ${sha:0:9}; log $log"

# dispatch <repo> <workflow> <ref> <field...> -> the new run's id on stdout
dispatch() {
	local repo=$1 wf=$2 wref=$3 before id i
	shift 3
	before=$(gh run list -R "$repo" --workflow "$wf" --limit 1 --json databaseId --jq '.[0].databaseId // 0')
	gh workflow run "$wf" -R "$repo" --ref "$wref" "$@" >&2 || return 1
	for i in $(seq 1 60); do
		id=$(gh run list -R "$repo" --workflow "$wf" --event workflow_dispatch --limit 1 --json databaseId --jq '.[0].databaseId // 0')
		if [ "$id" != "$before" ] && [ "$id" != 0 ]; then
			echo "$id"
			return 0
		fi
		sleep 5
	done
	return 1
}

# watch <repo> <id> <label> -> 0 green
watch() {
	echo "release_rehearsal: $3 run $2 (https://github.com/$1/actions/runs/$2)"
	gh run watch "$2" -R "$1" --exit-status --interval 60 >> "$log" 2>&1
	local rc=$?
	gh run view "$2" -R "$1" --json conclusion,jobs \
		--jq '"  " + .conclusion + ": " + ([.jobs[] | .name + "=" + (.conclusion // "?")] | join(", "))'
	return $rc
}

id=$(dispatch "$MADC_REPO" release.yml develop -f tag="$tag" -f build_ref="$sha" -f rehearsal=true) ||
	die "release.yml did not start"
watch "$MADC_REPO" "$id" "release.yml rehearsal"
ci=$?
products=1
if [ $ci -eq 0 ]; then
	pid=$(dispatch "$MADCIDE_REPO" ci.yml main -f madc_tag="$tag" -f madc_ref="$sha") ||
		die "madcide's ci.yml did not start"
	watch "$MADCIDE_REPO" "$pid" "madcide ci.yml on the candidate"
	products=$?
fi

if [ "$sha" = "$(git rev-parse HEAD)" ]; then
	[ $ci -eq 0 ] && bash scripts/lane_ledger.sh record release-ci \
		"rehearsal of $tag at ${sha:0:9}: release.yml run $id green (Linux deb/rpm/tarball, 22.04 deb, Windows zip, macOS arm64 + x86_64, each install-gated on a fresh runner; draft)"
	[ $products -eq 0 ] && bash scripts/lane_ledger.sh record products \
		"madcide ci.yml run $pid green on the $tag draft (madc_ref ${sha:0:9}): Linux, 22.04, Windows under wine, genuine Windows, macOS arm64 + x86_64"
else
	echo "release_rehearsal: the candidate is not HEAD; nothing recorded"
fi
if [ $ci -ne 0 ]; then
	echo "release_rehearsal: RED — the release build failed; see $log"
	exit 1
fi
if [ $products -ne 0 ]; then
	echo "release_rehearsal: RED — madcide's CI failed on the candidate; see $log"
	exit 1
fi
echo "release_rehearsal: GREEN — $tag builds on every runner and madcide passes on it"
