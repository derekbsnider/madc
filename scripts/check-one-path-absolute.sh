#!/bin/bash
# check-one-path-absolute.sh — the "is this path absolute" gate.
#
# The question has ONE owner: madc::detail::host_path_absolute
# (src/madc_posix_io.cpp), which reads the host's separator set and, on
# Windows, a drive (`C:\x`, `C:/x`, `C:x`). Six hand-rolled copies existed on
# 2026-10-06, every one a '/'-only test: the project manifest's resolve()
# joined a TU's `Z:\work\inc` include directory onto the manifest's own
# directory, so an angle-bracket include through it was never found on
# Windows; the config file, the lexer's include resolvers and the forest's
# include-spelling tail match carried the same rule. A second copy of the rule
# is how that comes back.
#
# Rule: outside the owner, no source tests a path's first character against
# '/' — `p[0] == '/'`, `p[0] != '/'`, `p.front() == '/'`. A two-character
# COMMENT opener — `p[0] == '/' && p[1] == '/'` or `... p[1] == '*'` — is not
# a path test and is not counted.
#
# Negative control: a synthetic violation must FAIL the scan, else the gate
# itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

OWNER=src/madc_posix_io.cpp
PATTERN="(\[0\]|\.front\(\)) *[!=]= *'/'"
COMMENT_OPENER="\[0\] *== *'/' *&& *[A-Za-z_.>-]+\[1\] *== *'[/*]'"

scan() {
	# $@ = files; prints violations, returns 0 when clean.
	# (the opener's own text is removed first, so a path test sharing its
	# line is still seen)
	grep -nE "$PATTERN" "$@" /dev/null | sed -E "s#$COMMENT_OPENER##g" | grep -E "$PATTERN"
	test $? -ne 0
}

# --- negative control -------------------------------------------------------
tmp=$(mktemp)
printf "std::string resolve(const std::string &b, const std::string &p) { if (p[0] == '/') return p; return b + p; }\n" > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-path-absolute: NEGATIVE CONTROL FAILED — the scan did not catch a '/'-only absolute test" >&2
	exit 2
fi
printf "bool rooted(const std::string &p) { return !p.empty() && p.front() == '/'; }\n" > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-path-absolute: NEGATIVE CONTROL FAILED — the scan did not catch a front() test" >&2
	exit 2
fi
printf "if (p[0] == '/' && p[1] == '/') skip(); if (p[0] == '/') rooted();\n" > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-path-absolute: NEGATIVE CONTROL FAILED — a path test beside a comment opener went unseen" >&2
	exit 2
fi
# Positive control: a comment opener alone is not a path test.
printf "if ( p + 1 < e && p[0] == '/' && p[1] == '*' ) comment();\n" > "$tmp"
if ! scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-path-absolute: POSITIVE CONTROL FAILED — a comment opener was counted as a path test" >&2
	exit 2
fi
rm -f "$tmp"

# --- the tree ---------------------------------------------------------------
files=$(git ls-files 'src/*.cpp' 'src/*.h' 'src/**/*.cpp' 'src/**/*.h' \
	'include/*.h' 'include/**/*.h' | grep -v "^$OWNER\$")
# shellcheck disable=SC2086
if ! out=$(scan $files 2>&1); then
	echo "check-one-path-absolute: an absolute-path test outside the one owner ($OWNER):" >&2
	echo "$out" >&2
	echo "  -> ask madc::detail::host_path_absolute(path)" >&2
	exit 1
fi
echo "check-one-path-absolute: OK (one owner: $OWNER host_path_absolute)"
