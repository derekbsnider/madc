#!/bin/bash
# emit_layout_gate.sh — the emitted C is INDENTED by block depth and carries
# only the parentheses the language (and gcc's / clang's -Wparentheses)
# require. Owner ruling 2026-09-09: every emitted code view is auto-indented
# and coloured; the EMITTER (src/cir_emit_c.cpp) is the one layout owner, so
# `--emit=c11` on the CLI stands in for every IDE lens over the same text.
#
# Positive: tests/testemitindent.mad's render compiles under
# `-Werror=parentheses` with gcc (and clang when present — its group also
# rejects the pre-ruling `if ((a == b))` as -Wparentheses-equality), and its
# function bodies are tab-indented with case labels one level out.
# Negative control: a hand-written file carrying the PRE-RULING shapes
# (`if (a = b)` / `if ((a == b))`, a flush-left body) must FAIL the same
# checks — proof the checks bite. Generic: no per-test logic beyond the one
# reducer the fixture names.
set -u
cd "$(dirname "$0")/.."
work=tmp/emit_layout
mkdir -p "$work"
fail() { echo "emit_layout_gate: FAIL — $*" >&2; exit 1; }

reducer=tests/testemitindent.mad
emitted="$work/emitted.c"
bin/madc --emit=c11 "$reducer" > "$emitted" 2>"$work/emit.err" \
	|| fail "--emit=c11 refused the reducer: $(head -3 "$work/emit.err")"

# 1. Indentation follows the tree: the function body's statements sit one
#    tab in, a nested block's two, and a case label one level OUT from the
#    statements it heads.
grep -q $'^\treturn s - b;$' "$emitted" \
	|| fail "the function body is not tab-indented (no '\\treturn s - b;')"
grep -q $'^\t\ts = a - b;$' "$emitted" \
	|| fail "the nested block is not indented two levels (no '\\t\\ts = a - b;')"
grep -q $'^\tcase 0:$' "$emitted" \
	|| fail "the case label is not one level out from its statements (no '\\tcase 0:')"
grep -q $'^\t\ts = (a | b) << 1;$' "$emitted" \
	|| fail "the case body is not indented under its label"
# 2. Minimal parentheses: the redundant pre-ruling shapes are gone …
grep -q 'if (a > b) {' "$emitted" || fail "'if (a > b) {' not rendered minimal"
grep -q $'^\treturn s - b;$' "$emitted" || fail "'return s - b;' carries parentheses"
grep -q 's = a + b \* 2;' "$emitted" || fail "'a + b * 2' carries parentheses"
grep -q 'if ((s = a % 7))' "$emitted" \
	|| fail "an assignment used as a condition lost its parentheses"
grep -q 's = s || (a && b);' "$emitted" || fail "'&&' within '||' lost its parentheses"
grep -q 's = a & (b == 1);' "$emitted" || fail "a comparison inside '&' lost its parentheses"
grep -q 's = -(-b);' "$emitted" || fail "'-(-b)' would re-lex as '--b'"
grep -q 's -= (a + b) \* 2;' "$emitted" || fail "'(a + b) * 2' lost its parentheses"

# 3. … and both canons compile the render with -Werror=parentheses.
check_compiles() {	# compiler file
	"$1" -fsyntax-only -Werror=parentheses -x c "$2" 2>"$work/$(basename "$1").err"
}
check_compiles gcc "$emitted" \
	|| fail "gcc -Werror=parentheses rejects the render: $(head -3 "$work/gcc.err")"
have_clang=0
if command -v clang >/dev/null 2>&1; then
	have_clang=1
	check_compiles clang "$emitted" \
		|| fail "clang -Werror=parentheses rejects the render: $(head -3 "$work/clang.err")"
fi

# 4. Negative control — the pre-ruling shapes must fail these checks.
neg="$work/negative.c"
cat > "$neg" <<'NEG'
int f(int a, int b)
{
if (a = b)
return (a - b);
if ((a == b))
return 1;
return 0;
}
NEG
grep -q $'^\treturn s - b;$' "$neg" && fail "negative control: the indentation check passed a flush-left body"
check_compiles gcc "$neg" && fail "negative control: gcc accepted 'if (a = b)' under -Werror=parentheses"
if [ $have_clang = 1 ]; then
	check_compiles clang "$neg" && fail "negative control: clang accepted 'if ((a == b))' under -Werror=parentheses"
fi

echo "emit_layout_gate: OK (indented, minimal parentheses; gcc$( [ $have_clang = 1 ] && echo ' + clang') -Werror=parentheses clean; negative control bites)"
exit 0
