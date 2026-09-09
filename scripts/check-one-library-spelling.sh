#!/bin/bash
# check-one-library-spelling.sh — the library-spelling gate.
#
# The platform spelling of a library NAME — the `lib` prefix and the .so /
# .dylib / .dll suffix — has ONE owner: src/madc_modules.cpp
# (madc_target_dso_suffix, madc_module_library_spelling,
# madc_spelled_library_p). `import`, `-l`, the native link closure, the
# -shared artifact name and the cover analysis all read it. The family this
# gate holds closed had three implementations on 2026-09-06 (the -l arm, the
# DT_NEEDED mapping, a host macro), and the one that differed (MADC_DSO_SUFFIX,
# no .dll arm) spelled lib<name>.so on the win64 target — silently. A second
# implementation of the rule is how that comes back.
#
# Rule: a BARE suffix literal (".so" / ".dylib" / ".dll") or the prefix
# concatenation ("lib" + ...) appears in no source or header outside the
# owner. Full image names (libc.so.6, ucrtbase.dll, libSystem.B.dylib) are
# platform runtime constants, not the rule, and are not matched.
#
# Negative control: a synthetic violation must FAIL the scan, else the gate
# itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

OWNER=src/madc_modules.cpp
PATTERN='"\.(so|dylib|dll)"|"lib" *\+'

scan() {
	# $@ = files; prints violations, returns 0 when clean.
	grep -nE "$PATTERN" "$@" /dev/null
	test $? -ne 0
}

# --- negative control -------------------------------------------------------
tmp=$(mktemp)
printf 'const char *sfx(void) { return ".dll"; }\n' > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-library-spelling: NEGATIVE CONTROL FAILED — the scan did not catch a bare suffix literal" >&2
	exit 2
fi
printf 'std::string n(const std::string &x) { return "lib" + x; }\n' > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-library-spelling: NEGATIVE CONTROL FAILED — the scan did not catch a lib-prefix concatenation" >&2
	exit 2
fi
rm -f "$tmp"

# --- the tree ---------------------------------------------------------------
files=$(git ls-files 'src/*.cpp' 'src/*.h' 'include/*.h' 'include/**/*.h' | grep -v "^$OWNER\$")
# shellcheck disable=SC2086
if ! out=$(scan $files 2>&1); then
	echo "check-one-library-spelling: a library spelling outside the one owner ($OWNER):" >&2
	echo "$out" >&2
	echo "  -> read the spelling from madc_modules (madc_target_dso_suffix / madc_module_library_spelling / madc_spelled_library_p)" >&2
	exit 1
fi
echo "check-one-library-spelling: OK — the lib prefix and the .so/.dylib/.dll suffix are spelled only in $OWNER (negative controls bite)"
