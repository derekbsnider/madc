#!/bin/bash
# check-one-path-stem.sh — the "a path's extension / stem" gate.
#
# The rule has ONE owner: madc::detail::host_path_drop_extension and
# host_path_stem (src/madc_posix_io.cpp): the extension follows the last dot
# of the FINAL component, never a dot in a directory and never a leading dot.
# Six hand-rolled copies existed on 2026-10-10 and disagreed: --emit-pch's
# default output searched the whole path (`../tool` became `..madh`), a
# --project -c object's name and a TU's init symbol split on '/' only (a
# Windows path kept its directories), and %build's, %call's and -c's copies
# each spelled the guards again. A second copy of the rule is how that comes
# back.
#
# Rule: outside the owner, no source looks for a string's LAST dot —
# `rfind('.')`, `rfind(".")`, `find_last_of('.')`, `find_last_of(".")`.
# (file_kinds.cpp reads an extension with strrchr over a basename it splits
# on both separators everywhere — a kind guessed from a name; not a path
# operation on the host.)
#
# Negative control: a synthetic violation must FAIL the scan, else the gate
# itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

OWNER=src/madc_posix_io.cpp
PATTERN="(rfind|find_last_of) *\( *('\.'|\"\.\")"

scan() {
	# $@ = files; prints violations, returns 0 when clean.
	grep -nE "$PATTERN" "$@" /dev/null
	test $? -ne 0
}

# --- negative control -------------------------------------------------------
tmp=$(mktemp)
printf "std::string out = in; size_t dot = out.rfind('.'); if (dot != npos) out.erase(dot);\n" > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-path-stem: NEGATIVE CONTROL FAILED — the scan did not catch rfind('.')" >&2
	exit 2
fi
printf "size_t d = name.find_last_of(\".\");\n" > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-path-stem: NEGATIVE CONTROL FAILED — the scan did not catch find_last_of(\".\")" >&2
	exit 2
fi
# Positive control: another character's last occurrence is not the rule.
printf "size_t sl = path.rfind('/');\n" > "$tmp"
if ! scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-path-stem: POSITIVE CONTROL FAILED — rfind('/') was counted" >&2
	exit 2
fi
rm -f "$tmp"

# --- the tree ---------------------------------------------------------------
files=$(git ls-files 'src/*.cpp' 'src/*.h' 'src/**/*.cpp' 'src/**/*.h' \
	'include/*.h' 'include/**/*.h' | grep -v "^$OWNER\$")
# shellcheck disable=SC2086
if ! out=$(scan $files 2>&1); then
	echo "check-one-path-stem: a path's extension found outside the one owner ($OWNER):" >&2
	echo "$out" >&2
	echo "  -> madc::detail::host_path_drop_extension(path) / host_path_stem(path)" >&2
	exit 1
fi
echo "check-one-path-stem: OK (one owner: $OWNER host_path_drop_extension / host_path_stem)"
