#!/bin/bash
# brew_formula.sh — the Homebrew formula, rendered from its one template
# (packaging/homebrew/madc.rb.in) for a source tarball.
#
#   scripts/brew_formula.sh <tarball-url> <sha256> [version]
#
# Prints Formula/madc.rb on stdout. The version defaults to the VERSION file.
# A release renders it for the tag's tarball (the tap's copy); the brew lane
# (scripts/brew_lane.sh) for a local file:// tarball of this tree.
set -e
cd "$(dirname "$0")/.."
if [ $# -lt 2 ] || [ $# -gt 3 ]; then
	echo "usage: $0 <tarball-url> <sha256> [version]" >&2
	exit 2
fi
url="$1"
sum="$2"
ver="${3:-$(cat VERSION)}"
case "$sum" in
	*[!0-9a-f]*|"") echo "brew_formula: '$sum' is not a sha256 (64 hex digits)" >&2; exit 2 ;;
esac
if [ "${#sum}" -ne 64 ]; then
	echo "brew_formula: '$sum' is not a sha256 (64 hex digits)" >&2
	exit 2
fi
sed -e "s|@URL@|$url|" -e "s|@SHA256@|$sum|" -e "s|@VERSION@|$ver|" \
	packaging/homebrew/madc.rb.in
