#!/bin/bash
# TOTALITY GATE — every `__builtin_` alias target has a known return type.
#
# The rule: src/lexer.cpp rewrites a `__builtin_X` spelling to some other name Y
# (a libc function, or one of madc's own runtime helpers). The parser then has to
# type a call to Y with no declaration in scope, and it asks
# madc_libc_return_class(Y) (src/libc_signatures.cpp). If Y has no entry there it
# is declared `int` by default — right for most of libc, and silently WRONG for
# every pointer, `double`, `size_t` and `long long` return.
#
# Why a gate. The table this replaces was maintained by finding the bugs: six of
# the fifty-six C99 math roots were registered with real signatures, each one
# added after a separate bug report, while the other fifty read `xmm0` out of
# `rax` and returned the previous call's leftovers. `floor(2.7)` was 1.0 and
# `strcmp("abc","abd") < 0` was FALSE through a whole release, with 1084 green
# tests, because every test that calls those includes the header.
#
# What it CANNOT catch: a wrong class (strlen tagged CharPtr). That is what
# tests/testlibcnoheader.mad measures against the gcc oracle. This gate only
# asserts that nothing is left to the default by accident.
set -u
cd "$(dirname "$0")/.."

LEXER=src/lexer.cpp
TABLE=src/libc_signatures.cpp

# Excluded, each for a stated reason — never to make a count go down.
#
#   the conj/creal/cimag family: typed by is_implicit_complex_builtin_name() /
#     make_implicit_complex_builtin_func() in src/parser.cpp, which takes
#     precedence over the dlsym path and supplies an ARGUMENT type too (a
#     _Complex through the variadic ABI corrupts the call). Claiming them here
#     would give one name two owners.
#   va_arg: a keyword rewrite, not a call.
#   __attribute__: the _Alignas / alignas keyword rewrite, not a call.
EXCLUDE='^(conj|creal|cimag)[fl]?$|^va_arg$|^__attribute__$'

check_table() {
	# $1 = path to the table source to check against
	python3 - "$LEXER" "$1" "$EXCLUDE" <<'PY'
import re, sys
lexer, table, exclude = sys.argv[1], sys.argv[2], sys.argv[3]

src = open(table).read()
have = set(re.findall(r'\{\s*"([A-Za-z0-9_]+)"\s*,\s*LibcRet::', src))

# The math families are covered BY CONSTRUCTION: the lexer expands the same
# math_roots[] list this table expands, so a root plus its f/l suffixes is
# known whenever the root is in the list.
m = re.search(r'const char \*const math_roots\[\] = \{(.*?)NULL', src, re.S)
if not m:
	print("REGRESSION - math_roots[] not found in %s" % table)
	sys.exit(1)
for r in re.findall(r'"([a-z0-9_]+)"', m.group(1)):
	have |= {r, r + 'f', r + 'l'}

targets = set(re.findall(r'define_map\["(?:__builtin_|__bswap_)[A-Za-z0-9_]*"\]\s*=\s*"([A-Za-z0-9_]+)"',
			 open(lexer).read()))
missing = sorted(t for t in targets
		 if t not in have and not re.search(exclude, t))
print("%d alias targets, %d missing" % (len(targets), len(missing)))
for t in missing:
	print("  MISSING %s" % t)
sys.exit(1 if missing else 0)
PY
}

out=$(check_table "$TABLE")
rc=$?
echo "$out"
if [ $rc -ne 0 ]; then
	echo "REGRESSION — an alias target has no entry in $TABLE."
	echo "Add its real return class (or exclude it WITH a reason in this script)."
	exit 1
fi

# NEGATIVE CONTROL. A gate nobody has seen fail is a gate nobody knows works.
# Drop one known entry from a copy of the table and require the check to notice.
ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
sed 's/{ "strcmp", LibcRet::Int }/{ "strcmp_ctl", LibcRet::Int }/' "$TABLE" > "$ctl"
if ! grep -q 'strcmp_ctl' "$ctl"; then
	echo "REGRESSION — negative control could not perturb the table (entry renamed?)."
	exit 1
fi
if check_table "$ctl" >/dev/null 2>&1; then
	echo "REGRESSION — negative control PASSED. The gate does not detect a missing entry."
	exit 1
fi

echo "OK — every alias target has a return class; negative control fails as designed."
exit 0
