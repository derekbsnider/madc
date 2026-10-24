#!/usr/bin/env bash
# check-release-conformance.sh — every release moves C23 and/or C++20 forward.
#
# Rule (.claude/rules/branching.md, owner 2026-10-16): a release raises at
# least one C23 / C++20 passing count and lowers none, against the previous
# release. The counts are the ledger's (docs/parity/conformance-ledger.tsv),
# written only by scripts/conformance_measure.sh. For every CHANGELOG release
# section after EPOCH, with P the release below it:
#   - the ledger holds the release's rows for every corpus P has, each with
#     P's in-scope count (one corpus, measured the same way);
#   - no corpus's passing count is below P's, and at least one is above;
#   - the section carries a line
#       Conformance: <corpus> <P's count> -> <the release's count>[, ...]
#     and every pair it names is in the ledger and rose.
# A PATCH release (x.y.Z, Z > 0 — regression fixes on a release; owner
# 2026-10-24) is held to the first two only: measured, lowering none. It needs
# no rise and no Conformance: line; a line it carries is checked the same way.
# Between releases (the [Unreleased] section) nothing is required, so the
# per-commit static gate loop stays green; /release runs it after the release
# is measured and its section versioned, where a release that moved nothing
# fails.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CHANGELOG="${1:-$ROOT/CHANGELOG.md}"
LEDGER="${2:-$ROOT/docs/parity/conformance-ledger.tsv}"
# The baseline: the last release cut before the rule (move FORWARD only).
EPOCH="0.105.0"

# rows_of LEDGER vX.Y.Z — "corpus passing in_scope" per row.
rows_of()
{
	awk -F'\t' -v r="$2" '$1 == r { print $2, $4, $5 }' "$1"
}

# check_release CHANGELOG LEDGER VER PREV — prints each problem, returns 1
# when any.
check_release()
{
	local log="$1" ledger="$2" ver="$3" prev="$4" bad=0 rose=0
	local corpus p_pass p_scope c_pass c_scope line pairs pair old new
	if [ -z "$(rows_of "$ledger" "v$prev")" ]; then
		echo "check-release-conformance: v$ver: the ledger has no rows for" \
		     "v$prev — measure it on its archived binary" \
		     "(scripts/conformance_measure.sh --release v$prev --record)" >&2
		return 1
	fi
	while read -r corpus p_pass p_scope; do
		read -r c_pass c_scope < <(rows_of "$ledger" "v$ver" \
			| awk -v c="$corpus" '$1 == c { print $2, $3 }')
		if [ -z "${c_pass:-}" ]; then
			echo "check-release-conformance: v$ver: no ledger row for" \
			     "$corpus (scripts/conformance_measure.sh --release" \
			     "v$ver --record)" >&2
			bad=1
		elif [ "$c_scope" != "$p_scope" ]; then
			echo "check-release-conformance: v$ver: $corpus has $c_scope" \
			     "tests in scope, v$prev $p_scope — not one corpus;" \
			     "re-measure v$prev on its archived binary" >&2
			bad=1
		elif [ "$c_pass" -lt "$p_pass" ]; then
			echo "check-release-conformance: v$ver: $corpus FELL," \
			     "$p_pass -> $c_pass" >&2
			bad=1
		elif [ "$c_pass" -gt "$p_pass" ]; then
			rose=1
		fi
		c_pass="" c_scope=""
	done < <(rows_of "$ledger" "v$prev")
	local patch=0
	[ "${ver##*.}" != 0 ] && patch=1
	if [ "$rose" -eq 0 ] && [ "$patch" -eq 0 ]; then
		echo "check-release-conformance: v$ver raises no C23 or C++20" \
		     "count over v$prev" >&2
		bad=1
	fi
	line=$(awk -v hdr="## [v$ver]" '
		index($0, hdr) == 1 { on = 1; next }
		on && /^## \[/ { exit }
		on && /^Conformance: / { print; exit }' "$log")
	if [ -z "$line" ]; then
		[ "$patch" -eq 1 ] && return $bad
		echo "check-release-conformance: v$ver: its CHANGELOG section needs" \
		     "a line 'Conformance: <corpus> <old> -> <new>'" >&2
		return 1
	fi
	pairs="${line#Conformance: }"
	local named=0
	while IFS= read -r pair; do
		read -r corpus old arrow new <<< "$pair"
		p_pass=$(rows_of "$ledger" "v$prev" | awk -v c="$corpus" '$1 == c { print $2 }')
		c_pass=$(rows_of "$ledger" "v$ver" | awk -v c="$corpus" '$1 == c { print $2 }')
		if [ "${arrow:-}" != "->" ] || [ -z "$p_pass" ] || [ -z "$c_pass" ] \
		   || [ "$old" != "$p_pass" ] || [ "$new" != "$c_pass" ] \
		   || [ "$c_pass" -le "$p_pass" ]; then
			echo "check-release-conformance: v$ver: '$pair' is not a gain" \
			     "the ledger records (v$prev ${p_pass:-none}, v$ver" \
			     "${c_pass:-none})" >&2
			bad=1
		fi
		named=1
	done < <(printf '%s\n' "$pairs" \
		 | awk -F', *' '{ for (i = 1; i <= NF; i++) if ($i ~ /[^[:space:]]/) print $i }')
	if [ "$named" -eq 0 ]; then
		echo "check-release-conformance: v$ver: its Conformance: line names" \
		     "no gain" >&2
		bad=1
	fi
	return $bad
}

# check_file CHANGELOG LEDGER — every release after EPOCH; returns 1 when any
# fails.
check_file()
{
	local log="$1" ledger="$2" bad=0 i
	local -a vers
	mapfile -t vers < <(grep -oE '^## \[v[0-9]+\.[0-9]+\.[0-9]+\]' "$log" \
			    | sed -E 's/^## \[v(.*)\]$/\1/')
	for ((i = 0; i < ${#vers[@]}; i++)); do
		local ver="${vers[$i]}"
		[ "$ver" = "$EPOCH" ] && continue
		[ "$(printf '%s\n%s\n' "$EPOCH" "$ver" | sort -V | tail -1)" = "$ver" ] \
			|| continue
		if [ $((i + 1)) -ge ${#vers[@]} ]; then
			echo "check-release-conformance: v$ver has no release below it" >&2
			bad=1
			continue
		fi
		check_release "$log" "$ledger" "$ver" "${vers[$((i + 1))]}" || bad=1
	done
	return $bad
}

if ! check_file "$CHANGELOG" "$LEDGER"; then
	exit 1
fi

# Controls: a synthetic release over the epoch, against synthetic ledgers.
tmpd=$(mktemp -d)
log="$tmpd/CHANGELOG.md" led="$tmpd/ledger.tsv"
base_rows()
{
	printf 'v%s\tc23-compile\tc23\t38\t72\tabc\td\n' "$EPOCH"
	printf 'v%s\tc23-run\tc23\t35\t70\tabc\td\n' "$EPOCH"
	printf 'v%s\tcpp2a\tc++20\t350\t651\tabc\td\n' "$EPOCH"
}
# control NAME WANT CONFORMANCE-LINE NEW-ROWS... — WANT is pass or fail.
control()
{
	local name="$1" want="$2" cline="$3" got
	shift 3
	{
		printf '## [Unreleased]\n\n## [v%s] — 2099-01-01\n\n' "${CTRL_VER:-99.0.0}"
		[ -n "$cline" ] && printf '%s\n' "$cline"
		printf '\n## [v%s] — old\n' "$EPOCH"
	} > "$log"
	{ base_rows; printf '%s\n' "$@"; } > "$led"
	if check_file "$log" "$led" 2>/dev/null; then got=pass; else got=fail; fi
	if [ "$got" != "$want" ]; then
		rm -rf "$tmpd"
		echo "check-release-conformance: FAIL — control '$name' should" \
		     "$want and did not (the check went blind)" >&2
		exit 1
	fi
}
T=$'\t'
rise=("v99.0.0${T}c23-compile${T}c23${T}38${T}72${T}x${T}d"
      "v99.0.0${T}c23-run${T}c23${T}36${T}70${T}x${T}d"
      "v99.0.0${T}cpp2a${T}c++20${T}350${T}651${T}x${T}d")
flat=("v99.0.0${T}c23-compile${T}c23${T}38${T}72${T}x${T}d"
      "v99.0.0${T}c23-run${T}c23${T}35${T}70${T}x${T}d"
      "v99.0.0${T}cpp2a${T}c++20${T}350${T}651${T}x${T}d")
fell=("v99.0.0${T}c23-compile${T}c23${T}38${T}72${T}x${T}d"
      "v99.0.0${T}c23-run${T}c23${T}36${T}70${T}x${T}d"
      "v99.0.0${T}cpp2a${T}c++20${T}349${T}651${T}x${T}d")
rescoped=("v99.0.0${T}c23-compile${T}c23${T}38${T}72${T}x${T}d"
	  "v99.0.0${T}c23-run${T}c23${T}36${T}71${T}x${T}d"
	  "v99.0.0${T}cpp2a${T}c++20${T}350${T}651${T}x${T}d")
control "a measured gain, named" pass "Conformance: c23-run 35 -> 36" "${rise[@]}"
control "no Conformance: line" fail "" "${rise[@]}"
control "nothing rose" fail "Conformance: c23-run 35 -> 35" "${flat[@]}"
control "a corpus fell" fail "Conformance: c23-run 35 -> 36" "${fell[@]}"
control "a gain the ledger lacks" fail "Conformance: c23-run 35 -> 40" "${rise[@]}"
control "a changed in-scope set" fail "Conformance: c23-run 35 -> 36" "${rescoped[@]}"
control "the release unmeasured" fail "Conformance: c23-run 35 -> 36" \
	"${rise[0]}" "${rise[1]}"
# A patch release: measured and lowering none passes with no line; a fall or
# a missing measure still fails.
CTRL_VER=99.0.1 control "a patch holding every count" pass "" \
	"${flat[@]//v99.0.0/v99.0.1}"
CTRL_VER=99.0.1 control "a patch that fell" fail "" "${fell[@]//v99.0.0/v99.0.1}"
CTRL_VER=99.0.1 control "a patch unmeasured" fail "" \
	"${flat[0]//v99.0.0/v99.0.1}"
rm -rf "$tmpd"

echo "check-release-conformance: OK (every release after v$EPOCH raises a" \
     "C23 or C++20 count and lowers none; a patch release lowers none)"
exit 0
