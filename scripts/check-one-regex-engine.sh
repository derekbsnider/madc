#!/bin/bash
# DRIFT-PREVENTION GATE -- madc has ONE regex engine.
#
# Every regex madc itself evaluates -- madc::regex_*, perl::grep,
# perl::split, the ex core's :s / :g / /re/ -- goes through madc::regex
# (include/madc_regex.h, src/madc_regex.cpp), which owns PCRE2 (split arc
# 1A.3, docs/plans/2026-10-10-pcre2-design.md). A second engine is the bug
# this replaced: std::regex differed between libstdc++ and libc++, recursed
# per character, and each copy answered an invalid pattern differently.
# So: pcre2.h is included ONLY by src/madc_regex.cpp, and madc's own C++
# (src/, the engine headers in include/, the unit tests) never includes
# <regex> or names std::regex. A user's program is not madc's source: the
# embedded script headers (include/madc/) are not scanned.
#
# Two-sided: the negative control proves both patterns still bite.
set -u
cd "$(dirname "$0")/.."

PCRE='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]pcre2\.h[>"]'
STDRE='^[[:space:]]*#[[:space:]]*include[[:space:]]*<regex>|\bstd::(regex|wregex|sregex_[a-z_]+|smatch|cmatch|regex_(match|search|replace|error))\b'
OWNER='src/madc_regex.cpp'

ctl_dir=$(mktemp -d)
trap 'rm -rf "$ctl_dir"' EXIT
cat > "$ctl_dir/ctl.cpp" <<'CTL'
#include <pcre2.h>
#include <regex>
static bool arm(const std::string &s) { return std::regex_search(s, std::regex("a")); }
CTL
p=$(grep -cE "$PCRE" "$ctl_dir/ctl.cpp")
r=$(grep -cE "$STDRE" "$ctl_dir/ctl.cpp")
if [ "$p" -ne 1 ] || [ "$r" -ne 2 ]; then
	echo "check-one-regex-engine: NEGATIVE CONTROL FAILED -- pcre2.h matched $p of 1, std::regex $r of 2"
	exit 1
fi

files=$(git ls-files 'src/*.cpp' 'src/*.h' 'src/*.inc' 'include/*.h' 'tests/unit/*.cpp')
bad_pcre=$(grep -nE "$PCRE" $files 2>/dev/null | grep -v "^$OWNER:")
bad_std=$(grep -nE "$STDRE" $files 2>/dev/null)
n=$(printf '%s\n%s' "$bad_pcre" "$bad_std" | grep -c . || true)
echo "regex engines outside madc::regex: $n (target 0)"
if [ "$n" -ne 0 ]; then
	[ -n "$bad_pcre" ] && printf '%s\n' "$bad_pcre"
	[ -n "$bad_std" ] && printf '%s\n' "$bad_std"
	echo "  -> compile and match through madc::regex (include/madc_regex.h)."
	exit 1
fi
# The owner still exists and still owns PCRE2.
if ! grep -qE "$PCRE" "$OWNER"; then
	echo "check-one-regex-engine: $OWNER no longer includes pcre2.h -- where did the engine go?"
	exit 1
fi
echo "GREEN -- every regex goes through madc::regex (PCRE2)."
