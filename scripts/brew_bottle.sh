#!/bin/bash
# brew_bottle.sh — madc's Homebrew bottle: built by Homebrew from a source
# tarball, bottled, then POURED back from the bottle file and tested, so the
# bottle it hands on is one Homebrew has installed (packaging arc PK6; plan
# §41.11a step 9 — owner 2026-10-02: the Linux bottle ships with the master
# release, the macOS bottle is held).
#
#   scripts/brew_bottle.sh <tarball-url> <sha256> <root-url> <outdir>
#
# Steps, each a gate:
#   1. the formula for the tarball (scripts/brew_formula.sh) into a LOCAL tap
#      with no remote (madc-local/madc: `brew tap-new --no-git`; nothing is
#      published), and `brew install --build-bottle` of it;
#   2. `brew bottle --json` in <outdir>, the bottle file renamed to the name
#      Homebrew fetches — <root-url>/madc-<ver>.<tag>.bottle.tar.gz, the
#      bottle's url_encode, one dash where the local file has two;
#   3. the formula rendered with a bottle block naming file://<outdir>, the
#      keg uninstalled, and `brew install` of it: the keg's install receipt
#      must say it was POURED from that bottle, not built; then `brew test`;
#   4. <outdir>/madc.rb — the formula with the bottle block naming
#      <root-url>: the tap's copy at a release.
# The poured keg stays installed and linked for the caller: scripts/brew_lane.sh
# runs the suite against it; the release workflow attaches the bottle and
# madc.rb. The last line printed is the bottle's path.
set -e
cd "$(dirname "$0")/.."
if [ $# -ne 4 ]; then
	echo "usage: $0 <tarball-url> <sha256> <root-url> <outdir>" >&2
	exit 2
fi
url="$1"
sum="$2"
root="$3"
out="$4"

BREW=$(command -v brew || true)
[ -n "$BREW" ] || BREW=/home/linuxbrew/.linuxbrew/bin/brew
if [ ! -x "$BREW" ]; then
	echo "brew_bottle: no Homebrew (install it: https://brew.sh)" >&2
	exit 2
fi
# Formulae resolve through Homebrew's API, as on a user's machine (no
# homebrew/core clone); the local tap is the one formula read from disk.
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ANALYTICS=1 HOMEBREW_NO_ENV_HINTS=1
VER=$(cat VERSION)
TAP=madc-local/madc
mkdir -p "$out"
out=$(cd "$out" && pwd)
taproot="$("$BREW" --repository)/Library/Taps/madc-local/homebrew-madc"
if [ ! -d "$taproot" ]; then
	"$BREW" tap-new --no-git "$TAP" > /dev/null
fi
mkdir -p "$taproot/Formula"
formula="$taproot/Formula/madc.rb"

# 1. Built from source, ready to bottle.
scripts/brew_formula.sh "$url" "$sum" "$VER" > "$formula"
"$BREW" uninstall --force "$TAP/madc" > /dev/null 2>&1 || true
"$BREW" install --build-bottle "$TAP/madc"

# 2. The bottle, under the name its URL fetches.
rm -f "$out"/madc-*.bottle.tar.gz "$out"/madc--*.bottle.json
( cd "$out" && "$BREW" bottle --json --root-url="$root" "$TAP/madc" )
json=$(ls "$out"/madc--*.bottle.json)
# tag, sha256, cellar, the file brew wrote, the file its URL names
info=$(python3 -c '
import json, sys
(entry,) = json.load(open(sys.argv[1])).values()
b = entry["bottle"]
((tag, t),) = b["tags"].items()
print(tag, t["sha256"], t.get("cellar", b.get("cellar")), t["local_filename"], t["filename"])
' "$json")
read -r btag bsum bcellar localname urlname <<< "$info"
if [ "$localname" != "$urlname" ]; then
	mv "$out/$localname" "$out/$urlname"
fi
echo "brew_bottle: $urlname ($btag, sha256 $bsum, cellar $bcellar)"

# 3. Poured back from the bottle file, then tested.
scripts/brew_formula.sh --bottle "file://$out" "$btag" "$bsum" "$bcellar" \
	"$url" "$sum" "$VER" > "$formula"
"$BREW" uninstall --force "$TAP/madc" > /dev/null
"$BREW" install "$TAP/madc"
receipt="$("$BREW" --cellar madc)/$VER/INSTALL_RECEIPT.json"
if ! python3 -c '
import json, sys
sys.exit(0 if json.load(open(sys.argv[1])).get("poured_from_bottle") else 1)
' "$receipt"; then
	echo "brew_bottle: FAIL — brew install built madc from source instead of pouring $urlname ($receipt)" >&2
	exit 1
fi
"$BREW" test "$TAP/madc"
echo "brew_bottle: poured from $urlname and brew test passed"

# 4. The tap's copy: the bottle block names the release's root URL.
scripts/brew_formula.sh --bottle "$root" "$btag" "$bsum" "$bcellar" \
	"$url" "$sum" "$VER" > "$out/madc.rb"
echo "brew_bottle: the tap's formula is $out/madc.rb (bottle root $root)"
echo "$out/$urlname"
