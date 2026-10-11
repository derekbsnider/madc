#!/bin/bash
# check-one-madcide-staging.sh — the "stage madcide's data into an install" gate.
#
# The staging has ONE owner: scripts/stage_madcide_data.sh, which every
# platform's packaging calls with its own data directory (share/madcide on
# Linux and macOS, the exe's directory on Windows). On 2026-10-04 it was
# written twice — scripts/stage_install.sh and scripts/package_release_windows.sh
# each copied the profiles, plugins, plugin headers (and then the line
# editor's verbs and checks, retired in split arc 1D) — and a macOS tarball
# was about to need a third copy. A copy that misses one directory ships an
# editor with no key profiles or one that cannot build a plugin (no headers)
# on that one platform.
#
# Rule: outside the owner, no packaging script copies madcide's data out of
# the tree: an `install` or `cp` naming tools/madcide/profiles or
# tools/madcide/include.
#
# Negative control: a synthetic copy of each kind must FAIL the scan, else
# the gate itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

OWNER=scripts/stage_madcide_data.sh
PATTERN="(^|[;&|( ])(install|cp)( [^#]*)? [\"']?tools/madcide/(profiles|include)"

scan() {
	# $@ = files; prints violations, returns 0 when clean.
	grep -nE "$PATTERN" "$@" /dev/null
	test $? -ne 0
}

# --- negative control -------------------------------------------------------
tmp=$(mktemp)
printf 'install -m 644 tools/madcide/include/madcide/* "$p/share/madcide/include/madcide/"\n' > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-madcide-staging: NEGATIVE CONTROL FAILED — the scan did not catch an install of the headers" >&2
	exit 2
fi
printf '    cp -R tools/madcide/profiles "$dest/"\n' > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-madcide-staging: NEGATIVE CONTROL FAILED — the scan did not catch a cp of the profiles" >&2
	exit 2
fi
rm -f "$tmp"

# --- the tree ---------------------------------------------------------------
# The gate itself is excluded: its negative controls spell violations.
files=$(git ls-files --cached --others --exclude-standard 'scripts/*.sh' 'packaging/*' 'packaging/**' '.github/workflows/*' \
	| grep -v -e "^$OWNER\$" -e "^scripts/check-one-madcide-staging.sh\$")
# shellcheck disable=SC2086
if ! out=$(scan $files 2>&1); then
	echo "check-one-madcide-staging: madcide's data copied outside the one owner ($OWNER):" >&2
	echo "$out" >&2
	echo "  -> scripts/stage_madcide_data.sh <data-dir> <plugins-dir>" >&2
	exit 1
fi
echo "check-one-madcide-staging: OK (one owner: $OWNER)"
