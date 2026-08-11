#!/usr/bin/env bash
# Emit-C ABI gate: `--emit=c11` output calls a MANGLED-DIRECT by-value
# non-trivial class return through the target-neutral struct-return shape,
# never through a leading pointer argument.
#
# The JIT lane says "indirect result" in the IR (rblk — sret_abi_gate.sh owns
# that). Portable C has no such marker: the ONLY C spelling both SysV x86-64
# and AAPCS64 compile to the C++ convention is a >16-byte struct RETURN whose
# destination is the declared object itself:
#
#   struct __madc_ret_K X __attribute__((cleanup(...))) = SYM(args);
#
# (gcc x86-64 -O0 passes &X in %rdi; clang arm64 -O0 sets x8 = &X; zero
# memcpy in both — any assignment spelling materializes a temporary and
# block-copies it, which corrupts a self-referential small-string.)
#
# CirBuilder::emitc_lower_indirect_returns rewrites the tree at emit time.
# Behaviour cannot gate the rewrite on x86-64 — BOTH shapes run correctly
# there, which is how the JIT bug reached a Mac — so this gate asserts the
# MECHANISM in the emitted text:
#
#   every `extern void SYM(... *__retbuf, ...)` whose symbol has NO
#   definition in the emitted file is an UNFUSED true external — fail.
#
# (A __retbuf extern WITH a definition is a madc-emitted function using the
# __retbuf convention on both sides — portable on any target, left alone.)
#
# Legs:
#   1. default lane (libstdc++), >16-byte class (std::string): operator+,
#      substr, ostringstream::str, assignment into an existing object, a
#      madc-defined by-value function (must NOT be rewritten), and the
#      trivially-copyable iterator negative control (no pad struct may
#      appear for it). The emitted C must compile with gcc, RUN, and print
#      exactly what g++ -O0 prints for the same source (the emit-C oracle
#      rule), and must compile for arm64 with the x8 setup adjacent to each
#      rewritten call.
#   2. -stdlib=libc++, ≤16-byte class (std::locale, 8 bytes — the size class
#      C would return in registers): ios_base::getloc must be rewritten to a
#      padded return, and the binary must run against real libc++.
#   3. negative control for the CHECKER itself: with the rewrite disabled
#      (MADC_XTEST_NO_SRET_LOWER=1) the audit MUST report offenders —
#      otherwise the audit is blind and this gate proves nothing.
#
# KNOWN RESIDUALS (kept on the first-argument shape, counted by a stderr
# warning the fused legs assert ABSENT): a destination that is not a local
# declaration (global initialization), a symbol whose address is taken, and
# calls through function pointers. See the plan doc for the list.
#
# Run from the repo root (fulltest does).
set -u
cd "$(dirname "$0")/.."

ulimit -t 480 2>/dev/null

MADC="${MADC_BIN:-bin/madc}"

fail() { echo "emitc_sret_gate: $1"; exit 1; }

[ -x "$MADC" ] || fail "no madc at $MADC"

mkdir -p tmp
src=tmp/emitc_sret_gate.mad
cat > "$src" <<'EOF'
#include <string>
#include <sstream>
#include <vector>
#include <stdio.h>
std::string twice(const std::string &s) { return s + s; }
int main()
{
	std::string t = twice("z");
	std::string a = "ab";
	std::string b = "cd";
	std::string c = a + b;
	std::string d = c.substr(1, 2);
	std::ostringstream os;
	os << "x";
	std::string e = os.str();
	c = b + a;
	std::vector<int> v;
	v.push_back(7);
	std::vector<int>::iterator it = v.begin();
	printf("%s %s %s %s %d\n", c.c_str(), d.c_str(), e.c_str(), t.c_str(), *it);
	return 0;
}
EOF

# Unfused true externs in an emitted-C file: `extern void ...__retbuf...`
# whose symbol never appears as a definition. Prints one offender per line.
audit() {
	awk '
	{ line = $0; gsub(/__attribute__\(\([^)]*\)\)/, "", line) }
	line ~ /__retbuf/ && line ~ /^extern void / {
		if (match(line, /[A-Za-z_][A-Za-z0-9_]*\(/)) {
			sym = substr(line, RSTART, RLENGTH - 1)
			externs[sym] = $0
		}
		next
	}
	line ~ /__retbuf/ && line ~ /\{$/ {
		if (match(line, /[A-Za-z_][A-Za-z0-9_]*\(/))
			defined[substr(line, RSTART, RLENGTH - 1)] = 1
	}
	END { for (s in externs) if (!(s in defined)) print externs[s] }
	' "$1"
}

# ---- Leg 1: default lane, >16-byte class --------------------------------
emitted=tmp/emitc_sret_gate.c
errlog=tmp/emitc_sret_gate.err
timeout 240 "$MADC" --emit=c11 "$src" > "$emitted" 2> "$errlog" \
	|| fail "[default] --emit=c11 failed: $(head -3 "$errlog")"
grep -q "kept the first-argument result shape" "$errlog" \
	&& fail "[default] the reducer left residual unfused calls: $(grep 'first-argument' "$errlog")"

offend="$(audit "$emitted")"
[ -z "$offend" ] \
	|| fail "[default] unfused true-extern indirect return(s) — the x86-64-only shape:
$offend"

fused=$(grep -c "^extern struct __madc_ret_" "$emitted")
[ "$fused" -ge 3 ] 2>/dev/null \
	|| fail "[default] only $fused rewritten extern(s) — the reducer stopped exercising the path, so this gate proves nothing"

# Negative control: the trivially-copyable iterator return must NOT grow a
# pad struct (C's size rule is correct for it and must be left alone).
grep "^extern struct __madc_ret_" "$emitted" | grep -qi "iterator" \
	&& fail "[default] negative control broke — a TRIVIALLY-copyable class return was rewritten: $(grep '^extern struct __madc_ret_' "$emitted" | grep -i iterator)"

# The emitted C must run and print exactly what g++ prints for the source.
# libmadc provides the compiler runtime the emitted template bodies call
# (__madc_try_push and friends — the exception/cleanup lowering).
gcc -std=c11 -O0 -w -o tmp/emitc_sret_gate.bin "$emitted" \
	-Llib -lmadc "-Wl,-rpath,$PWD/lib" -lstdc++ -lm 2> "$errlog" \
	|| fail "[default] gcc rejected the emitted C: $(head -3 "$errlog")"
got="$(timeout 60 ./tmp/emitc_sret_gate.bin 2>&1)" \
	|| fail "[default] the emitted-C binary failed: $got"
g++ -O0 -o tmp/emitc_sret_gate.oracle -x c++ "$src" 2> "$errlog" \
	|| fail "[default] g++ rejected the oracle source: $(head -3 "$errlog")"
want="$(timeout 60 ./tmp/emitc_sret_gate.oracle 2>&1)"
[ "$got" = "$want" ] \
	|| fail "[default] emitted-C output diverges from the g++ oracle: got [$got] want [$want]"

# arm64 cross: the same emitted C must compile for AArch64 with the x8
# (indirect-result register) setup adjacent to every rewritten call.
if command -v clang >/dev/null 2>&1; then
	asm=tmp/emitc_sret_gate.arm64.s
	clang -std=c11 -O0 -w -S --target=arm64-apple-macos13 -o "$asm" "$emitted" 2> "$errlog" \
		|| fail "[arm64] clang cross-compile of the emitted C failed: $(head -3 "$errlog")"
	for s in $(sed -nE 's/^extern struct __madc_ret_[A-Za-z0-9_]+ ([A-Za-z_][A-Za-z0-9_]*)\(.*/\1/p' "$emitted"); do
		ok=$(awk -v sym="bl\t_$s" '
			{ if (index($0, sym)) { print (win > 0) ? "hit" : "miss"; exit } }
			/x8/ { win = 8; next }
			{ if (win > 0) win-- }' "$asm")
		[ "$ok" = "miss" ] \
			&& fail "[arm64] no x8 setup before the call to $s — the destination address is not riding the indirect-result register"
	done
	echo "emitc_sret_gate: [arm64] OK — x8 rides every rewritten call"
else
	echo "emitc_sret_gate: [arm64] SKIP — no clang on this host"
fi
echo "emitc_sret_gate: [default] OK — $fused rewritten extern(s), 0 unfused, output matches the g++ oracle"

# ---- Leg 2: -stdlib=libc++, ≤16-byte class ------------------------------
src2=tmp/emitc_sret_gate2.mad
cat > "$src2" <<'EOF'
#include <iostream>
#include <locale>
#include <cstdio>
int main()
{
	std::locale l = std::cout.getloc();
	printf("loc ok\n");
	return 0;
}
EOF
emitted2=tmp/emitc_sret_gate2.c
timeout 240 "$MADC" -stdlib=libc++ --emit=c11 "$src2" > "$emitted2" 2> "$errlog" \
	|| fail "[libc++] --emit=c11 failed: $(head -3 "$errlog")"
grep -q "kept the first-argument result shape" "$errlog" \
	&& fail "[libc++] residual unfused calls: $(grep 'first-argument' "$errlog")"
grep -q "^extern struct __madc_ret_locale " "$emitted2" \
	|| fail "[libc++] getloc was not rewritten — an 8-byte class is exactly the size C returns in registers, the case the pad exists for"
offend="$(audit "$emitted2")"
[ -z "$offend" ] \
	|| fail "[libc++] unfused true-extern indirect return(s):
$offend"
gcc -std=c11 -O0 -w -o tmp/emitc_sret_gate2.bin "$emitted2" -lc++ -lm 2> "$errlog" \
	|| fail "[libc++] gcc rejected the emitted C: $(head -3 "$errlog")"
got2="$(timeout 60 ./tmp/emitc_sret_gate2.bin 2>&1)" \
	|| fail "[libc++] the emitted-C binary failed: $got2"
[ "$got2" = "loc ok" ] \
	|| fail "[libc++] wrong output: [$got2]"
echo "emitc_sret_gate: [libc++] OK — 8-byte locale rides a padded return and runs"

# ---- Leg 3: the checker's own negative control --------------------------
# With the rewrite disabled the SAME audit must report offenders; if it
# cannot see the unlowered shape, every green above is vacuous.
MADC_XTEST_NO_SRET_LOWER=1 timeout 240 "$MADC" --emit=c11 "$src" > tmp/emitc_sret_gate.raw.c 2>/dev/null \
	|| fail "[negctl] --emit=c11 (rewrite disabled) failed"
offend="$(audit tmp/emitc_sret_gate.raw.c)"
[ -n "$offend" ] \
	|| fail "[negctl] the audit found NOTHING with the rewrite disabled — the checker is blind, so this gate proves nothing"
echo "emitc_sret_gate: [negctl] OK — audit detects the unlowered shape"

rm -f "$src" "$src2" "$emitted" "$emitted2" "$errlog" \
	tmp/emitc_sret_gate.bin tmp/emitc_sret_gate.oracle \
	tmp/emitc_sret_gate2.bin tmp/emitc_sret_gate.arm64.s \
	tmp/emitc_sret_gate.raw.c
echo "emitc_sret_gate: OK"
