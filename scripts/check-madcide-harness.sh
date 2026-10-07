#!/bin/bash
# check-madcide-harness.sh — every function <madcide/harness> declares is
# defined by madcide's base with the same signature.
#
# <madcide/harness> (tools/madcide/include/madcide/harness, installed as
# share/madcide/include/madcide/harness) is a product test's view of the
# base: it DECLARES the base functions a test calls, and the test links
# libmadcide, which defines them. A declaration that drifts from its
# definition (a parameter's type changed, a function renamed) still compiles
# in the test — a C++ redeclaration with other parameters is an overload —
# and fails only when the test links, with an unresolved symbol, in the
# product's repository. This gate finds the drift here.
#
# Rule: each declaration line in the harness (`T name(params);`) matches,
# whitespace aside, the head of a definition in the base's sources
# (tools/madcide/*.inc, tools/texteditor/*.inc): the same text, followed by
# the body or the end of the line.
#
# Negative control: a synthetic harness declaring a base function with
# another parameter type must FAIL, and one declaring it as defined must
# PASS — else the gate itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

HARNESS=tools/madcide/include/madcide/harness

# check HARNESS — prints each declaration with no matching definition; 0 = clean.
check() {
	python3 - "$1" tools/madcide/*.inc tools/texteditor/*.inc <<'PY'
import re, sys
harness, sources = sys.argv[1], sys.argv[2:]
norm = lambda t: re.sub(r'\s+', ' ', t).strip()
heads = set()
for f in sources:
    for line in open(f, encoding='utf-8', errors='replace'):
        m = re.match(r'^([A-Za-z_][^;{()]*\([^;{]*\))\s*(\{.*)?$', line.rstrip('\n'))
        if m:
            heads.add(norm(m.group(1)))
bad = 0
for i, line in enumerate(open(harness, encoding='utf-8'), 1):
    m = re.match(r'^([A-Za-z_][^;{()]*\([^;{]*\));\s*$', line.rstrip('\n'))
    if m and norm(m.group(1)) not in heads:
        print(f"{harness}:{i}: {line.strip()}")
        bad += 1
sys.exit(1 if bad else 0)
PY
}

# --- controls -------------------------------------------------------------
tmpd=$(mktemp -d)
printf 'long es_int(long w, long es, const char *key, long dflt);\n' > "$tmpd/good"
printf 'long es_int(long w, long es, var &key, long dflt);\n' > "$tmpd/bad"
if check "$tmpd/bad" >/dev/null; then
	rm -rf "$tmpd"
	echo "check-madcide-harness: NEGATIVE CONTROL FAILED — a declaration with another parameter type passed" >&2
	exit 2
fi
if ! check "$tmpd/good" >/dev/null; then
	rm -rf "$tmpd"
	echo "check-madcide-harness: POSITIVE CONTROL FAILED — a declaration as the base defines it was refused" >&2
	exit 2
fi
rm -rf "$tmpd"

# --- the tree ---------------------------------------------------------------
if ! out=$(check "$HARNESS"); then
	echo "check-madcide-harness: <madcide/harness> declares a function the base does not define so:" >&2
	echo "$out" >&2
	echo "  -> declare it exactly as madcide's base defines it (a product test links libmadcide by that signature)" >&2
	exit 1
fi
n=$(grep -cE '^[A-Za-z_][^;{()]*\([^;{]*\);\s*$' "$HARNESS")
echo "check-madcide-harness: OK ($n declarations, each defined by madcide's base)"
