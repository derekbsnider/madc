#!/bin/bash
# check-expect-twins.sh — a domain twin fixture (tests/X.<domain>_expect)
# REPLACES tests/X.expect on that domain, so a change to the base that is not
# a domain difference must land in the twin too — or the twin silently keeps
# the OLD answer and the domain lane goes red long after the change merged
# (2026-09-08: the colour-unification slice fixed testmadcide.expect's
# `spans:` line and left the win64 twin behind; the wine lane caught it two
# hours later, in the merge-wave battery).
#
# The rule, for LABELLED lines (`label: rest` — the clause labels a
# headless IDE test prints): every base label must appear in the twin with
# the IDENTICAL line, unless the label is listed in the twin's `.domain`
# sidecar (tests/X.<domain>_expect.domain — the labels whose answer differs
# on that domain, or which that domain does not print: the differences are
# named, not silent). Unlabelled lines are out of scope (the twin's job).
# Twins whose base has no labelled line are not gated.
#
# Negative controls: a synthetic pair whose twin differs on an unlisted
# label must FAIL; the same pair with the label listed must PASS.
set -u
cd "$(dirname "$0")/.." || exit 2

# $1 = base expect, $2 = twin, $3 = the twin's .domain list (may be absent).
# Prints the violations; returns 0 when clean.
check_pair() {
	local base="$1" twin="$2" domain="$3" rc=0
	local labels
	labels=$(grep -oE '^[a-z0-9_-]+:' "$base" | sort -u)
	[ -n "$labels" ] || return 0
	while IFS= read -r label; do
		[ -n "$label" ] || continue
		if [ -f "$domain" ] && grep -qxF -- "${label%:}" "$domain"; then
			continue
		fi
		local bline tline
		bline=$(grep -m1 -F -- "$label" "$base")
		tline=$(grep -m1 -F -- "$label" "$twin" || true)
		if [ "$bline" != "$tline" ]; then
			if [ -z "$tline" ]; then
				echo "  $twin: label '${label%:}' missing (base: $bline)"
			else
				echo "  $twin: label '${label%:}' differs from the base"
				echo "      base: $bline"
				echo "      twin: $tline"
			fi
			rc=1
		fi
	done <<<"$labels"
	return $rc
}

# --- negative controls -------------------------------------------------------
tmpd=$(mktemp -d)
printf 'alpha: one\nbeta: two\nplain text\n' > "$tmpd/t.expect"
printf 'alpha: one\nbeta: TWO\nplain other\n' > "$tmpd/t.win64_expect"
if check_pair "$tmpd/t.expect" "$tmpd/t.win64_expect" "$tmpd/none" >/dev/null 2>&1; then
	rm -rf "$tmpd"
	echo "check-expect-twins: NEGATIVE CONTROL FAILED — an unlisted differing label passed" >&2
	exit 2
fi
printf 'beta\n' > "$tmpd/t.win64_expect.domain"
if ! check_pair "$tmpd/t.expect" "$tmpd/t.win64_expect" "$tmpd/t.win64_expect.domain" >/dev/null 2>&1; then
	rm -rf "$tmpd"
	echo "check-expect-twins: POSITIVE CONTROL FAILED — a listed label was still flagged" >&2
	exit 2
fi
rm -rf "$tmpd"

# --- the tree ------------------------------------------------------------------
fail=0
for twin in tests/*_expect; do
	case "$twin" in *.expect) continue ;; esac
	base="${twin%.*_expect}.expect"
	[ -f "$base" ] || continue
	out=$(check_pair "$base" "$twin" "$twin.domain") || {
		echo "check-expect-twins: $twin drifted from $base:" >&2
		echo "$out" >&2
		echo "  -> update the twin's line, or list the label in $twin.domain if the domain's answer differs" >&2
		fail=1
	}
done
[ "$fail" -eq 0 ] || exit 1
echo "check-expect-twins: OK — every labelled line agrees across the domain twins, or is named in a .domain sidecar (negative controls bite)"
