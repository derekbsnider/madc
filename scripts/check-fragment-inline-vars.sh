#!/bin/bash
# check-fragment-inline-vars.sh — a variable an embedded fragment defines at
# namespace scope is `inline`.
#
# An embedded fragment (include/madc/ns_*, include/madc/bits/*: extensionless
# headers the auto-include serves) is included by every unit that names its
# namespace. A plain namespace-scope variable in it is then defined once per
# unit: madc's --project lane tolerated the copies, but a multi-object link
# (`madc -c` per unit, then `madc -o prog a.o b.o`) refuses the second one as
# a duplicate symbol, as ld does ("multiple definition"). On 2026-10-05 the
# web and ws UI hosts each carried one (ui_web::__last_host,
# ui_ws::__pending), and madcide's base linked as an object beside a
# product's units (the static libmadcide) would not link. `inline` makes the
# copies one variable (C++17 [basic.def.odr]; the CIR builder binds it
# linkonce). A fragment's FUNCTION bodies need no keyword: they arrive
# through the deferred-body machinery, which gives them vague linkage.
#
# Rule: at namespace (or file) scope in a fragment, a variable definition is
# inline, static, extern, constexpr or const. tests/testfragmentobjects is the
# behaviour this guards (two units, objects, one link).
#
# Negative control: a synthetic fragment with a plain variable must FAIL,
# and one with each allowed form must PASS — else the gate itself is broken
# and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

# scan FILE... — prints each plain namespace-scope variable; 0 = clean.
scan() {
	python3 - "$@" <<'PY'
import re, sys
ALLOWED = re.compile(r'^(inline|static|extern|constexpr|const|typedef|using|template|friend)\b')
DECL = re.compile(r'^[A-Za-z_][\w:<>,\s\*&]*[\s\*&]\**[A-Za-z_]\w*(\[[^\]]*\])?\s*(=.*)?;$')
NOTVAR = re.compile(r'^(return|case|default|goto|break|continue|import|namespace|struct|class|union|enum|public|private|protected)\b')
bad = 0
for f in sys.argv[1:]:
    lines = open(f, encoding='utf-8', errors='replace').read().split('\n')
    depth, scopes, in_comment = 0, [], False
    for i, raw in enumerate(lines):
        s = raw.strip()
        if in_comment:
            if '*/' in s:
                in_comment = False
            continue
        if s.startswith('/*') and '*/' not in s:
            in_comment = True
            continue
        code = re.sub(r'"(\\.|[^"\\])*"', '""', re.sub(r'//.*', '', s)).strip()
        # Only namespace / extern "C" braces may be open: the variable is
        # then at namespace scope, never a member, a local or an initializer.
        if code and depth == len(scopes) and not ALLOWED.match(code) \
           and not NOTVAR.match(code) and not code.startswith('#') \
           and '(' not in code.split('=')[0] and DECL.match(code):
            print(f"{f}:{i+1}: {s}")
            bad += 1
        opens_scope = re.match(r'^(inline\s+)?namespace\b', code) or re.match(r'^extern\s+"C"', code)
        prev = re.sub(r'//.*', '', lines[i-1]).strip() if i else ''
        if (opens_scope and '{' in code) or (code == '{' and (re.match(r'^(inline\s+)?namespace\b', prev) or re.match(r'^extern\s+"C"', prev))):
            scopes.append(depth)
        depth += code.count('{') - code.count('}')
        scopes = [d for d in scopes if d < depth]
sys.exit(1 if bad else 0)
PY
}

# --- negative control -------------------------------------------------------
tmpd=$(mktemp -d)
printf 'namespace demo {\n    long *slot = 0;\n}\n' > "$tmpd/bad"
printf 'namespace demo {\n    inline long *slot = 0;\n    static long n;\n    extern long e;\n    constexpr long c = 1;\n    struct s { long m; };\n    long f(long x)\n    {\n\tlong local = x;\n\treturn local;\n    }\n    long g(long x);\n}\nstatic bool reg = true;\n' > "$tmpd/good"
if scan "$tmpd/bad" >/dev/null; then
	rm -rf "$tmpd"
	echo "check-fragment-inline-vars: NEGATIVE CONTROL FAILED — a plain namespace-scope variable passed" >&2
	exit 2
fi
if ! scan "$tmpd/good" >/dev/null; then
	scan "$tmpd/good" >&2
	rm -rf "$tmpd"
	echo "check-fragment-inline-vars: POSITIVE CONTROL FAILED — an allowed form was flagged" >&2
	exit 2
fi
rm -rf "$tmpd"

# --- the tree ---------------------------------------------------------------
frags=$(git ls-files include/madc | grep -E '^include/madc/(ns_[^./]*|bits/[^.]*)$')
# shellcheck disable=SC2086
if ! out=$(scan $frags); then
	echo "check-fragment-inline-vars: a fragment defines a plain namespace-scope variable:" >&2
	echo "$out" >&2
	echo "  -> mark it inline: every unit that includes the fragment defines it, and a multi-object link refuses the copies" >&2
	exit 1
fi
echo "check-fragment-inline-vars: OK ($(echo "$frags" | wc -w) fragments, every namespace-scope variable inline or internal)"
