#!/bin/bash
# check-istring-fields.sh — the compiler core holds NAMES as madc::dis::istring.
#
# 2026-10-08 the compiler core (lexer, parser, CIR builder/dump/format/emit,
# madc_cir, cir_freeze, type spelling, mangler, completion, keywords, pch, and
# their headers) moved every std::string to the interned madc::dis::istring;
# only text that is BUILT or EDITED, output text, source text, OS-boundary
# paths and lists, and the libmadc API boundary stayed std::string. The
# keep-list, by kind, is docs/plans/2026-10-08-compile-time-vs-gxx.md
# ("Interned names above the token layer").
#
# Two checks:
#  1. RATCHET — per core file, the count of `std::string` in CODE (comments and
#     string literals excluded) may not grow past scripts/istring_keep.tsv. A
#     new std::string in the core is a keep-list decision: re-record with
#     --record only when it is one of the kept kinds. A drop asks for a
#     re-record so the gain is kept.
#  2. HAZARDS — shapes that compile and are wrong:
#     a. `(istring *)` / `(const istring *)` cast: an istring never travels
#        behind a void *; the eval bridge's void * carries a std::string *.
#     b. an istring REFERENCE data member: bound from a std::string it binds a
#        temporary that dies with the constructor's full-expression.
#     c. `istring(x.str())`: re-interns an istring for nothing.
#     d. a function returning `const istring &` that returns its own
#        `const istring &` parameter: dangles when called with a std::string.
#     e. a function or lambda that stores the ADDRESS of its own
#        `const istring &` parameter (`x = &param;`): called with a
#        std::string, the parameter is a temporary that dies at the call's end.
#
# Negative control: a synthetic file carrying each hazard must be flagged five
# times and a clean one not at all — else the gate is broken and fails loudly.
#
# Usage: scripts/check-istring-fields.sh [--record]

set -u
cd "$(dirname "$0")/.." || exit 2

BASELINE=scripts/istring_keep.tsv
CORE="src/lexer.cpp src/parser.cpp src/cir_builder.cpp src/cir_builder.h
src/madc_cir.cpp src/madc_cir.h src/cir_freeze.cpp src/cir_freeze.h
src/cir_dump.cpp src/cir_emit_c.cpp src/cir_format.cpp
src/madc_type_spelling.cpp src/madc_mangle.cpp src/madc_complete.cpp
src/madc_keywords.cpp src/pch.cpp src/madc.cpp
include/madc.h include/datadef.h include/tokens.h include/datatokens.h
include/madc_mangle.h include/madc_type_spelling.h include/spelling_delim.h"

# scan MODE FILE... — MODE count: "<file>\t<n>" per file; MODE hazards: one
# line per hazard found.
scan() {
	python3 - "$@" <<'PY'
import re, sys
mode, files = sys.argv[1], sys.argv[2:]

def code_only(s):
    # Blank comments and string / char literal bodies, keeping newlines.
    out, i, q = [], 0, None
    while i < len(s):
        c = s[i]
        if q:
            if c == '\\':
                out.append('  '); i += 2; continue
            if c == q:
                q = None
            out.append(c if c in '\n' + (q or '') else ' ')
            i += 1; continue
        if c in '"\'':
            q = c; out.append(c); i += 1; continue
        if s.startswith('//', i):
            j = s.find('\n', i); j = len(s) if j < 0 else j
            out.append(' ' * (j - i)); i = j; continue
        if s.startswith('/*', i):
            j = s.find('*/', i); j = len(s) if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', s[i:j])); i = j; continue
        out.append(c); i += 1
    return ''.join(out)

IS = r'madc::dis::istring'
CAST = re.compile(r'\(\s*(const\s+)?' + IS + r'\s*\*\s*\)|_cast<\s*(const\s+)?' + IS + r'\s*\*\s*>')
REFMEMBER = re.compile(r'^[ \t]*(const\s+)?' + IS + r'\s*&\s*\w+\s*;', re.M)
REINTERN = re.compile(IS + r'\(\s*[A-Za-z_][\w.>\[\]-]*\.str\(\)\s*\)')
REFRET = re.compile(r'const\s+' + IS + r'\s*&\s*([\w:~]+)\s*\(([^;{}()]*)\)\s*(const\s*)?\{')
PARAMFN = re.compile(r'\(([^;{}()]*const\s+' + IS + r'\s*&\s*\w+[^;{}()]*)\)\s*(?:const\s*)?(?:->\s*[\w:]+\s*)?\{')

def body_at(src, start):
    i, d = start, 1
    while d and i < len(src):
        d += {'{': 1, '}': -1}.get(src[i], 0); i += 1
    return src[start:i]

for f in files:
    src = code_only(open(f, encoding='utf-8', errors='replace').read())
    if mode == 'count':
        print('%s\t%d' % (f, len(re.findall(r'\bstd::string\b', src))))
        continue
    line = lambda pos: src.count('\n', 0, pos) + 1
    for m in CAST.finditer(src):
        print('%s:%d: istring pointer cast (a void * bridge carries std::string)' % (f, line(m.start())))
    for m in REFMEMBER.finditer(src):
        print('%s:%d: istring reference data member (hold the istring by value)' % (f, line(m.start())))
    for m in REINTERN.finditer(src):
        print('%s:%d: istring(x.str()) re-interns an istring' % (f, line(m.start())))
    for m in PARAMFN.finditer(src):
        body = body_at(src, m.end())
        for p in re.findall(r'const\s+' + IS + r'\s*&\s*(\w+)', m.group(1)):
            if re.search(r'=\s*&\s*' + p + r'\b(?!\s*[\[.(]|\s*->)', body):
                print('%s:%d: stores the address of its const istring & parameter %s (it may be a temporary)'
                      % (f, line(m.start()), p))
    for m in REFRET.finditer(src):
        params = re.findall(r'const\s+' + IS + r'\s*&\s*(\w+)', m.group(2))
        if not params:
            continue
        i, d = m.end(), 1
        while d and i < len(src):
            d += {'{': 1, '}': -1}.get(src[i], 0); i += 1
        body = src[m.end():i]
        for p in params:
            if re.search(r'\breturn\b[^;]*\b' + p + r'\b', body):
                print('%s:%d: %s returns a reference to its parameter %s (return istring by value)'
                      % (f, line(m.start()), m.group(1), p))
PY
}

# --- negative / positive control -------------------------------------------
ctl=$(mktemp -d "${TMPDIR:-/tmp}/istring-gate.XXXXXX")
trap 'rm -rf "$ctl"' EXIT
cat > "$ctl/bad.cpp" <<'EOF'
struct G {
	const madc::dis::istring &k;
};
void f(void *p) { const madc::dis::istring &s = *(const madc::dis::istring *)p; }
madc::dis::istring g(const madc::dis::istring &x) { return madc::dis::istring(x.str()); }
const madc::dis::istring &h(const madc::dis::istring &a) { return a; }
void w(const madc::dis::istring *&out) { auto f = [&](const char *k, const madc::dis::istring &body) -> bool { out = &body; return false; }; }
EOF
cat > "$ctl/good.cpp" <<'EOF'
struct G {
	madc::dis::istring k;
};
void f(void *p) { const std::string &s = *(const std::string *)p; }
madc::dis::istring g(const madc::dis::istring &x) { return x; }
const madc::dis::istring &h(const madc::dis::istring &a) { static madc::dis::istring e; return e; }
void w(const std::string *&out) { auto f = [&](const char *k, const std::string &body) -> bool { out = &body; return false; }; }
// (const madc::dis::istring *) in a comment, "(madc::dis::istring *)" in a literal
EOF
nbad=$(scan hazards "$ctl/bad.cpp" | wc -l)
ngood=$(scan hazards "$ctl/good.cpp" | wc -l)
if [ "$nbad" -ne 5 ]; then
	echo "check-istring-fields: NEGATIVE CONTROL FAILED — $nbad of 5 hazards flagged" >&2
	exit 1
fi
if [ "$ngood" -ne 0 ]; then
	echo "check-istring-fields: POSITIVE CONTROL FAILED — a clean file was flagged" >&2
	exit 1
fi

# --- hazards -----------------------------------------------------------------
# shellcheck disable=SC2086
haz=$(scan hazards $CORE)
if [ -n "$haz" ]; then
	echo "check-istring-fields: interned-name hazards in the compiler core:" >&2
	echo "$haz" >&2
	exit 1
fi

# --- ratchet ------------------------------------------------------------------
# shellcheck disable=SC2086
now=$(scan count $CORE)
if [ "${1:-}" = "--record" ]; then
	{
		echo "# check-istring-fields.sh baseline: std::string mentions in core CODE, per file."
		echo "# Growth fails the gate; record only a keep-list kind (see the script header)."
		echo "$now"
	} > "$BASELINE"
	echo "check-istring-fields: recorded $(echo "$now" | wc -l) files into $BASELINE"
	exit 0
fi
if [ ! -f "$BASELINE" ]; then
	echo "check-istring-fields: no baseline $BASELINE — run with --record" >&2
	exit 1
fi
red=0; gain=0
while IFS=$'\t' read -r f n; do
	base=$(awk -F'\t' -v f="$f" '$1 == f { print $2 }' "$BASELINE")
	if [ -z "$base" ]; then
		echo "check-istring-fields: $f has no baseline row — run with --record" >&2
		red=1
	elif [ "$n" -gt "$base" ]; then
		echo "check-istring-fields: $f holds $n std::string (baseline $base) — a NAME is an istring;" \
		     "only a keep-list kind may be recorded (--record)" >&2
		red=1
	elif [ "$n" -lt "$base" ]; then
		echo "check-istring-fields: $f dropped to $n std::string (baseline $base) — re-record (--record) to keep the gain"
		gain=1
	fi
done <<< "$now"
[ "$red" -eq 0 ] || exit 1
[ "$gain" -eq 1 ] || echo "check-istring-fields: OK ($(echo "$now" | wc -l) core files, no hazards)"
exit 0
