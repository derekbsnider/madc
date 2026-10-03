#!/bin/bash
# brew_formula.sh — the Homebrew formula, rendered from its one template
# (packaging/homebrew/madc.rb.in) for a source tarball, and for the bottle
# built from it.
#
#   scripts/brew_formula.sh [--bottle <root-url> <tag> <sha256> <cellar>] \
#           <tarball-url> <sha256> [version]
#
# Prints Formula/madc.rb on stdout. The version defaults to the VERSION file.
# A release renders it for the tag's tarball and the bottle the release
# workflow built (the tap's copy); the brew lane (scripts/brew_lane.sh) for a
# local file:// tarball of this tree and the bottle it built from it. Both
# bottles come from scripts/brew_bottle.sh. Without --bottle the formula has
# no bottle block, and every install builds from source.
#
# --bottle takes what `brew bottle --json` recorded: <tag> (x86_64_linux),
# the bottle's <sha256> and its <cellar> — any or any_skip_relocation (a
# relocatable keg), or the absolute Cellar it was built in, which the block
# names unless it is Homebrew's default Cellar for the tag's platform.
set -e
cd "$(dirname "$0")/.."
usage() {
	echo "usage: $0 [--bottle <root-url> <tag> <sha256> <cellar>] <tarball-url> <sha256> [version]" >&2
	exit 2
}
is_sha256() {
	case "$1" in
		*[!0-9a-f]*|"") return 1 ;;
	esac
	[ "${#1}" -eq 64 ]
}

bottle=""
if [ "$1" = "--bottle" ]; then
	[ $# -ge 5 ] || usage
	broot="$2"
	btag="$3"
	bsum="$4"
	bcellar="$5"
	shift 5
	if ! is_sha256 "$bsum"; then
		echo "brew_formula: bottle '$bsum' is not a sha256 (64 hex digits)" >&2
		exit 2
	fi
	# Homebrew's default Cellar per platform (Homebrew::DEFAULT_*_CELLAR); a
	# bottle built there carries no cellar parameter.
	case "$btag" in
		*_linux) default_cellar=/home/linuxbrew/.linuxbrew/Cellar ;;
		arm64_*) default_cellar=/opt/homebrew/Cellar ;;
		*) default_cellar=/usr/local/Cellar ;;
	esac
	case "$bcellar" in
		any|any_skip_relocation) cellar="cellar: :$bcellar, " ;;
		"$default_cellar") cellar="" ;;
		/*) cellar="cellar: \"$bcellar\", " ;;
		*)
			echo "brew_formula: bottle cellar '$bcellar' is not any, any_skip_relocation or an absolute path" >&2
			exit 2
			;;
	esac
	bottle="  bottle do
    root_url \"$broot\"
    sha256 $cellar$btag: \"$bsum\"
  end

"
fi
if [ $# -lt 2 ] || [ $# -gt 3 ]; then
	usage
fi
url="$1"
sum="$2"
ver="${3:-$(cat VERSION)}"
if ! is_sha256 "$sum"; then
	echo "brew_formula: '$sum' is not a sha256 (64 hex digits)" >&2
	exit 2
fi
sed -e "s|@URL@|$url|" -e "s|@SHA256@|$sum|" -e "s|@VERSION@|$ver|" \
	packaging/homebrew/madc.rb.in |
	BOTTLE_BLOCK="$bottle" awk '$0 == "  @BOTTLE@" { printf "%s", ENVIRON["BOTTLE_BLOCK"]; next } { print }'
