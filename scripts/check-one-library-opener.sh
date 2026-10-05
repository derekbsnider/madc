#!/bin/bash
# check-one-library-opener.sh — a library a program names opens through ONE
# opener, madc_module_open (src/madc_modules.cpp).
#
# `import`, `#load`, a manifest's "libs", the command line's -l and a frozen
# forest's recorded libraries all name a library by its spelling, and all
# must find it the same way: madc's own lib directory first (<exe
# dir>/../lib, the relocatable install's shape), then the loader. On
# 2026-10-05 the command line's -l called the platform loader directly
# (madcdl_open_global), so an installed madc's `-lmadcide` missed the
# libmadcide beside it while the manifest's "libs" found it, and a forest
# frozen with -l reopened its libraries the same divergent way.
#
# Rule: madcdl_open_global( appears only in its definition (src/madc_dl.cpp,
# include/madc_dl.h), in the opener (src/madc_modules.cpp), and on a line
# marked `// system runtime` (a fixed runtime image, e.g. the C++ standard
# library's, which no user names).
#
# Negative control: a synthetic unmarked call must FAIL and a marked one
# must PASS — else the gate itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

# scan FILE... — prints each call outside the rule; 0 = clean.
scan() {
	local out
	out=$(grep -n -H 'madcdl_open_global(' "$@" 2>/dev/null | grep -v '// system runtime')
	[ -z "$out" ] && return 0
	echo "$out"
	return 1
}

tmpd=$(mktemp -d)
printf 'void f(const char *l) { madcdl_open_global(l, true); }\n' > "$tmpd/bad.cpp"
printf 'void g(const char *l) { madcdl_open_global(l); }	// system runtime: libstdc++\n' > "$tmpd/good.cpp"
if scan "$tmpd/bad.cpp" >/dev/null; then
	rm -rf "$tmpd"
	echo "check-one-library-opener: NEGATIVE CONTROL FAILED — an unmarked direct open passed" >&2
	exit 2
fi
if ! scan "$tmpd/good.cpp" >/dev/null; then
	rm -rf "$tmpd"
	echo "check-one-library-opener: POSITIVE CONTROL FAILED — a marked system-runtime open was flagged" >&2
	exit 2
fi
rm -rf "$tmpd"

files=$(git ls-files 'src/*.cpp' 'src/*.h' 'include/*.h' 'include/**/*.h' \
	| grep -v -E '^(src/madc_dl\.cpp|include/madc_dl\.h|src/madc_modules\.cpp)$')
# shellcheck disable=SC2086
if ! out=$(scan $files); then
	echo "check-one-library-opener: a library opens past madc_module_open:" >&2
	echo "$out" >&2
	echo "  -> open it with madc_module_open(spelling, err[, bind_now]) — madc's lib directory first, then the loader" >&2
	exit 1
fi
echo "check-one-library-opener: OK (every named library opens through madc_module_open)"
