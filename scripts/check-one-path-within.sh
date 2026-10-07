#!/bin/bash
# check-one-path-within.sh — the "is this path inside that directory" gate.
#
# The question has ONE owner: madc::detail::host_path_within
# (src/madc_posix_io.cpp), which reads the host's separator set — '/' and
# '\' on Windows, '/' elsewhere — and requires a separator after the
# directory's spelling. Two hand-rolled copies existed on 2026-10-04: the
# lexer's system-header classification (a strncmp against prefixes with a
# '/' appended to a Windows canonical spelling, so no mingw header was ever
# "system" and guard-less ones like <pshpack1.h> were silently skipped on
# re-inclusion — B167) and madcgit's working-tree test (correct, but its own
# copy). A second copy of the rule is how that comes back.
#
# Rule: outside the owner, no source tests a path against a directory by
#   - a separator check at the directory's length: `p[wd.size()] != '/'`;
#   - a strncmp against a prefix table: `strncmp(path, prefix, plen)`,
#     `strncmp(path, canon[i].c_str(), ...)`, `strncmp(..., sys_paths...)`.
#
# Negative control: a synthetic violation of each shape must FAIL the scan,
# else the gate itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

OWNER=src/madc_posix_io.cpp
PATTERN="\[[A-Za-z_.]+(\.size\(\)|len)\] *[!=]= *'(/|\\\\\\\\)'|strncmp\([^;]*(sys_paths|canon\[|, *prefix,)"

scan() {
	# $@ = files; prints violations, returns 0 when clean.
	grep -nE "$PATTERN" "$@" /dev/null
	test $? -ne 0
}

# --- negative control -------------------------------------------------------
tmp=$(mktemp)
printf "bool in(const std::string &p, const std::string &wd) { return p[wd.size()] == '/'; }\n" > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-path-within: NEGATIVE CONTROL FAILED — the scan did not catch a separator check at the directory's length" >&2
	exit 2
fi
printf "bool in(const char *path, const char *prefix, size_t plen) { return strncmp(path, prefix, plen) == 0; }\n" > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-path-within: NEGATIVE CONTROL FAILED — the scan did not catch a strncmp against a prefix" >&2
	exit 2
fi
rm -f "$tmp"

# --- the tree ---------------------------------------------------------------
files=$(git ls-files 'src/*.cpp' 'src/*.h' 'src/**/*.cpp' 'src/**/*.h' \
	'include/*.h' 'include/**/*.h' | grep -v "^$OWNER\$")
# shellcheck disable=SC2086
if ! out=$(scan $files 2>&1); then
	echo "check-one-path-within: a directory-containment test outside the one owner ($OWNER):" >&2
	echo "$out" >&2
	echo "  -> ask madc::detail::host_path_within(dir, path[, &rel_at])" >&2
	exit 1
fi
echo "check-one-path-within: OK (one owner: $OWNER host_path_within)"
