#!/bin/bash
# check-gates-see-new-files.sh -- a gate sees the files a commit adds.
#
# A gate that lists its files with a bare `git ls-files` scans TRACKED files
# only, so a file not yet committed escapes it until the commit that adds it
# has landed: the Tier-2 gates pass, then fail one commit later. Every
# check-*.sh lists files with `git ls-files --cached --others
# --exclude-standard` -- the tracked files and the new ones git does not
# ignore. Comment lines are prose, not listings.
# Two-sided: the negative control proves the pattern still bites.
set -u
cd "$(dirname "$0")/.."

BARE='git ls-files( |$|\))'
SEES='git ls-files --cached --others --exclude-standard'

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<CTL
files=\$(git ls-files 'src/*.cpp')
files=\$($SEES 'src/*.cpp')
# a bare git ls-files in a comment is prose
CTL
c=$(grep -nE "$BARE" "$ctl" | grep -vE '^[0-9]+:[[:space:]]*#' | grep -vF -- "$SEES" | grep -c .)
if [ "$c" -ne 1 ]; then
	echo "check-gates-see-new-files: NEGATIVE CONTROL FAILED -- matched $c of 1"
	exit 1
fi

bad=$(grep -nE "$BARE" scripts/check-*.sh \
	| grep -vE '^[^:]+:[0-9]+:[[:space:]]*#' \
	| grep -vF -- "$SEES" \
	| grep -v '^scripts/check-gates-see-new-files\.sh:')
n=$(printf '%s' "$bad" | grep -c . || true)
echo "gates listing tracked files only: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$bad"
	echo "  -> list a gate's files with: $SEES <pathspec>..."
	exit 1
fi
echo "GREEN -- every gate sees the files a commit adds."
