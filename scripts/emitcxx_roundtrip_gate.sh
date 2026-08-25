#!/usr/bin/env bash
# emitcxx_roundtrip_gate.sh — the --emit=c++ reverse-render round-trip gate
# (madcide AST-4 slice 1; docs/plans/2026-08-25-madcide-ast-arc-design.md
# §3.2). For every tests/fidelity/emitcxx_*.cpp reducer: madc --emit=c++
# must render, BOTH host C++ compilers (g++ AND clang++ — the two-canon
# rule; clang++ skipped only if absent) must recompile the render, and the
# rebuilt binary must print exactly what the same compiler's build of the
# ORIGINAL prints. Negative controls: (a) the output compare must catch an
# injected divergence; (b) a corrupted render must fail recompilation —
# proving the gate exercises a real compiler, not a stub.
set -u
cd "$(dirname "$0")/.."
MADC=${MADC_BIN:-bin/madc}
work=tmp/emitcxx_gate
mkdir -p "$work"

fail() { echo "emitcxx_roundtrip_gate: FAIL — $1"; exit 1; }
compare() { diff -q "$1" "$2" >/dev/null; }

# Negative control (a): the compare harness must detect a divergence.
printf 'a\n' > "$work/na"
printf 'b\n' > "$work/nb"
if compare "$work/na" "$work/nb"; then
	fail "negative control: compare missed an injected divergence"
fi

command -v g++ >/dev/null || fail "g++ missing (it built this tree)"
CXXES="g++"
if command -v clang++ >/dev/null; then
	CXXES="g++ clang++"
fi

n=0
for src in tests/fidelity/emitcxx_*.cpp; do
	[ -f "$src" ] || fail "no emitcxx reducers found"
	b=$(basename "$src" .cpp)
	n=$((n + 1))
	( ulimit -t 60; timeout 90 "$MADC" --emit=c++ "$src" \
		> "$work/$b.emitted.cpp" 2> "$work/$b.emit.err" ) \
		|| { sed -n '1,5p' "$work/$b.emit.err"; fail "$b: --emit=c++ exited nonzero"; }
	[ -s "$work/$b.emitted.cpp" ] || fail "$b: empty render"
	for CXX in $CXXES; do
		"$CXX" -O0 -o "$work/$b.orig.$CXX" "$src" \
			2> "$work/$b.origcc.err" \
			|| { sed -n '1,5p' "$work/$b.origcc.err"; fail "$b: $CXX rejected the ORIGINAL (bad reducer)"; }
		"$CXX" -O0 -o "$work/$b.emit.$CXX" "$work/$b.emitted.cpp" \
			2> "$work/$b.emitcc.err" \
			|| { sed -n '1,8p' "$work/$b.emitcc.err"; fail "$b: $CXX rejected the render"; }
		( ulimit -t 30; timeout 45 "$work/$b.orig.$CXX" > "$work/$b.o1" ) \
			|| fail "$b: the original's run failed under $CXX"
		( ulimit -t 30; timeout 45 "$work/$b.emit.$CXX" > "$work/$b.o2" ) \
			|| fail "$b: the render's run failed under $CXX"
		compare "$work/$b.o1" "$work/$b.o2" \
			|| { diff "$work/$b.o1" "$work/$b.o2" | head -5; fail "$b: behavior diverged under $CXX"; }
	done
done

# Negative control (b): a corrupted render must fail the recompile leg.
first=$(ls "$work"/emitcxx_*.emitted.cpp 2>/dev/null | head -1)
[ -n "$first" ] || fail "no render to corrupt for the negative control"
cp "$first" "$work/corrupt.cpp"
echo "int main;" >> "$work/corrupt.cpp"
if g++ -O0 -fsyntax-only "$work/corrupt.cpp" 2>/dev/null; then
	fail "negative control: g++ accepted a corrupted render"
fi

echo "emitcxx_roundtrip_gate: OK ($n reducers x $(echo $CXXES | wc -w) compilers, both controls bit)"
