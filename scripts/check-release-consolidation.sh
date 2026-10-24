#!/usr/bin/env bash
# check-release-consolidation.sh — every release REDUCES existing duplication.
#
# Rule (.claude/rules/branching.md, owner 2026-10-16): a release consolidates
# at least one EXISTING duplicate family — found by a whole-subsystem
# /dupaudit sweep, not the diff — and leaves that family's gate behind. Its
# CHANGELOG section says so on a line of the form
#
#   Consolidated: <DupFamily name> — gate scripts/check-<name>.sh
#
# and the gate script it names exists. Every release section after EPOCH must
# carry at least one such line — except a PATCH release (x.y.Z, Z > 0:
# regression fixes on a release; owner 2026-10-24). Between releases (the [Unreleased] section)
# nothing is required, so the per-commit static gate loop stays green; /release
# runs it after the section is versioned, where a release without one fails.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CHANGELOG="${1:-$ROOT/CHANGELOG.md}"
# The last release cut before the rule (move FORWARD only, never back).
EPOCH="0.105.0"

# check_file FILE — prints each offending release, returns 1 when any.
check_file()
{
	local file="$1" bad=0 ver line gate
	while IFS= read -r ver; do
		# Only releases strictly after the epoch.
		[ "$ver" = "$EPOCH" ] && continue
		[ "$(printf '%s\n%s\n' "$EPOCH" "$ver" | sort -V | tail -1)" = "$ver" ] || continue
		# A patch release consolidates nothing by design.
		[ "${ver##*.}" != 0 ] && continue
		local found=0
		while IFS= read -r line; do
			gate=$(printf '%s\n' "$line" | grep -oE 'scripts/check-[A-Za-z0-9_-]+\.sh' | head -1)
			if [ -n "$gate" ] && [ -f "$ROOT/$gate" ]; then
				found=1
			fi
		done < <(awk -v hdr="## [v$ver]" '
			index($0, hdr) == 1 { on = 1; next }
			on && /^## \[/ { exit }
			on && /^Consolidated: / { print }' "$file")
		if [ "$found" -eq 0 ]; then
			echo "check-release-consolidation: release v$ver names no consolidated" \
			     "duplicate family — its CHANGELOG section needs a line" \
			     "'Consolidated: <DupFamily> — gate scripts/check-<name>.sh'" \
			     "naming a gate that exists." >&2
			bad=1
		fi
	done < <(grep -oE '^## \[v[0-9]+\.[0-9]+\.[0-9]+\]' "$file" \
		 | sed -E 's/^## \[v(.*)\]$/\1/')
	return $bad
}

if ! check_file "$CHANGELOG"; then
	exit 1
fi

# Controls: a synthetic post-epoch release without the line must fail, and
# one naming an existing gate must pass.
tmp=$(mktemp)
printf '## [Unreleased]\n\n## [v99.0.0] — 2099-01-01\n\nNo consolidation.\n\n## [v%s] — old\n' \
	"$EPOCH" > "$tmp"
if check_file "$tmp" 2>/dev/null; then
	rm -f "$tmp"
	echo "check-release-consolidation: FAIL — negative control passed (the" \
	     "check went blind)." >&2
	exit 1
fi
printf '## [v99.0.0] — 2099-01-01\n\nConsolidated: synthetic — gate scripts/check-release-consolidation.sh\n' \
	> "$tmp"
if ! check_file "$tmp" 2>/dev/null; then
	rm -f "$tmp"
	echo "check-release-consolidation: FAIL — positive control failed (a" \
	     "valid Consolidated: line was refused)." >&2
	exit 1
fi
printf '## [v99.0.1] — 2099-01-02\n\nA regression fix.\n\n## [v99.0.0] — 2099-01-01\n\nConsolidated: synthetic — gate scripts/check-release-consolidation.sh\n' \
	> "$tmp"
if ! check_file "$tmp" 2>/dev/null; then
	rm -f "$tmp"
	echo "check-release-consolidation: FAIL — patch control failed (a patch" \
	     "release without a Consolidated: line was refused)." >&2
	exit 1
fi
rm -f "$tmp"

echo "check-release-consolidation: OK (every release after v$EPOCH names a consolidated family; a patch release is exempt)"
exit 0
