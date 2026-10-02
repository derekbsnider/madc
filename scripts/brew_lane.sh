#!/bin/bash
# brew_lane.sh — madc built and installed by Homebrew from THIS tree, then the
# suite run against the installed madc (plan §41.11a step 9: a full-suite
# lane on a Homebrew install; packaging arc PK6).
#
#   bash scripts/brew_lane.sh [run_tests.sh args...]
#
# Needs Homebrew (`brew` on PATH, or /home/linuxbrew/.linuxbrew/bin/brew).
# Steps, each a gate:
#   1. a source tarball of the tree as it stands (tracked files with their
#      edits, and new files git does not ignore);
#   2. scripts/brew_bottle.sh over it — the formula in a LOCAL tap with no
#      remote (madc-local/madc; nothing is published), `brew install
#      --build-bottle`, `brew bottle`, then the keg POURED back from that
#      bottle file and `brew test` — the release workflow's bottle recipe, so
#      what the suite runs on is a bottle install, as a user's is;
#   3. the runpath a program compiled by the installed madc carries names
#      HOMEBREW_PREFIX/lib, and with the keg's own directory unreachable (the
#      state after `brew upgrade` removes it) the loader resolves libmadc.so.0
#      there and the program runs;
#   4. the packed suite (scripts/run_tests.sh) with MADC_BIN the linked madc.
# The bottle and its formula stay in tmp/brew-lane/bottles.
# The keg is uninstalled at the end (BREW_LANE_KEEP=1 keeps it, to rerun a
# failing test against it); the local tap stays for the next run.
set -e
cd "$(dirname "$0")/.."

BREW=$(command -v brew || true)
[ -n "$BREW" ] || BREW=/home/linuxbrew/.linuxbrew/bin/brew
if [ ! -x "$BREW" ]; then
	echo "brew_lane: no Homebrew (install it: https://brew.sh)" >&2
	exit 2
fi
# Formulae resolve through Homebrew's API, as on a user's machine (no
# homebrew/core clone); the local tap is the one formula read from disk.
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ANALYTICS=1 HOMEBREW_NO_ENV_HINTS=1
BPREFIX=$("$BREW" --prefix)
VER=$(cat VERSION)
TAP=madc-local/madc
WORK=tmp/brew-lane
mkdir -p "$WORK"

# 1. The tarball: the tree as it stands — tracked files with their edits, and
# new files git does not ignore.
tarball="$PWD/$WORK/madc-$VER.tar.gz"
git ls-files --cached --others --exclude-standard -z |
	tar --null -T - --transform "s|^|madc-$VER/|" -czf "$tarball"
sum=$(sha256sum "$tarball" | awk '{ print $1 }')
echo "brew_lane: source tarball of the tree at $(git rev-parse --short HEAD), its edits included"

# 2. Built, bottled, poured back from the bottle and tested.
bottles="$PWD/$WORK/bottles"
scripts/brew_bottle.sh "file://$tarball" "$sum" "file://$bottles" "$bottles"
madc="$BPREFIX/bin/madc"
keg=$("$BREW" --cellar madc)/$VER

# 3. A compiled program outlives its keg's directory.
probe="$PWD/$WORK/probe"
mkdir -p "$probe"
cat > "$probe/rt.c" << 'EOF'
#include <stdio.h>
int main(int argc, char **argv) { int a[argc + 1]; a[argc] = 42; printf("runs %d\n", a[argc]); return 0; }
EOF
"$madc" -o "$probe/rt" "$probe/rt.c"
if ! readelf -d "$probe/rt" | grep -q "$BPREFIX/lib"; then
	echo "brew_lane: FAIL — the program's runpath does not name $BPREFIX/lib:" >&2
	readelf -d "$probe/rt" | grep -i 'runpath\|rpath' >&2
	exit 1
fi
# The keg's lib unreachable, as after `brew upgrade` removes the keg: in a
# private mount namespace an empty tmpfs masks the keg's lib, and a stand-in
# HOMEBREW_PREFIX/lib (its links copied, libmadc.so.0 a real file — the new
# keg's, behind the link) is bound over the real one. The check is WHICH
# libmadc.so.0 the loader resolves (ldd), not only that the program runs: a
# libmadc elsewhere on the host (an older /usr/local install, through
# ld.so.cache) would run it too.
resolved() {	# the libmadc.so.0 the loader resolves for $1 (ldd), or "none"
	ldd "$1" 2>/dev/null | awk '$1 == "libmadc.so.0" { print ($3 == "" || $3 == "not") ? "none" : $3 }'
}
# The control: unmasked, the runpath's keg directory comes first.
plain=$(resolved "$probe/rt")
case "$plain" in
	"$keg"/*) ;;
	*) echo "brew_lane: FAIL — control: unmasked, libmadc.so.0 resolves to '$plain', not the keg's ($keg)" >&2; exit 1 ;;
esac
standin="$PWD/$WORK/standin"
rm -rf "$standin"
mkdir -p "$standin"
cp -a "$BPREFIX/lib/." "$standin/"
rm -f "$standin/libmadc.so.0"
cp "$keg/lib/libmadc.so.0" "$standin/libmadc.so.0"
masked=$(unshare -rm sh -c "mount -t tmpfs none '$keg/lib' && mount --bind '$standin' '$BPREFIX/lib' && ldd '$probe/rt' | awk '\$1 == \"libmadc.so.0\" { print \$3 }' && '$probe/rt'" 2>&1) || true
rm -rf "$standin"
want="$BPREFIX/lib/libmadc.so.0
runs 42"
if [ "$masked" != "$want" ]; then
	echo "brew_lane: FAIL — with the keg's lib masked, expected libmadc.so.0 from $BPREFIX/lib and 'runs 42'; got:" >&2
	echo "$masked" >&2
	exit 1
fi
echo "brew_lane: libmadc.so.0 resolves from the keg ($plain), and with the keg's lib masked from $BPREFIX/lib, where the program runs"

# 4. The suite against the installed madc.
rc=0
MADC_BIN="$madc" bash scripts/run_tests.sh "$@" || rc=$?
if [ -n "$BREW_LANE_KEEP" ]; then
	echo "brew_lane: keg kept (BREW_LANE_KEEP): $madc"
else
	"$BREW" uninstall --force "$TAP/madc" > /dev/null 2>&1 || true
fi
exit $rc
