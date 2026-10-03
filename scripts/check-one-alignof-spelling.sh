#!/bin/bash
# check-one-alignof-spelling.sh — the alignof-spelling gate.
#
# The alignof operator's spellings (`alignof`, `_Alignof`, `__alignof__`,
# `__alignof`) have ONE owner: is_alignof_identifier (src/parser.cpp,
# declared in include/madc.h). On 2026-09-29 four readers spelled the set by
# hand and each differed: the owner lacked `__alignof` (so `__alignof(T)` was
# refused), the member-declarator specifier list lacked `_Alignof`, and two
# more knew only `alignof` (BUGS.md B72).
#
# Rule: no source or header compares a name against an alignof spelling
# (`== "alignof"` and its siblings); ask is_alignof_identifier. The owner holds
# its spellings in a table, and keyword-registry lists are not comparisons.
#
# Negative control: a synthetic violation must FAIL the scan, else the gate
# itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

PATTERN='== *"(alignof|_Alignof|__alignof__|__alignof)"|"(alignof|_Alignof|__alignof__|__alignof)" *=='

scan() {
	# $@ = files; prints violations, returns 0 when clean.
	grep -nE "$PATTERN" "$@" /dev/null
	test $? -ne 0
}

# --- negative control -------------------------------------------------------
tmp=$(mktemp)
printf 'bool q(const std::string &n) { return n == "__alignof"; }\n' > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-alignof-spelling: NEGATIVE CONTROL FAILED — the scan did not catch a hand-spelled comparison" >&2
	exit 2
fi
rm -f "$tmp"

# --- the tree ---------------------------------------------------------------
files=$(git ls-files 'src/*.cpp' 'src/*.h' 'include/*.h' 'include/**/*.h')
# shellcheck disable=SC2086
if ! out=$(scan $files 2>&1); then
	echo "check-one-alignof-spelling: an alignof spelling compared outside its owner:" >&2
	echo "$out" >&2
	echo "  -> ask is_alignof_identifier (src/parser.cpp)" >&2
	exit 1
fi
echo "check-one-alignof-spelling: OK — alignof spellings are compared only through is_alignof_identifier (negative control bites)"
