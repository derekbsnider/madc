#!/bin/bash
# cir_fidelity.sh — localize CIR renderer divergence against gcc.
#
# Usage: scripts/cir_fidelity.sh <file.c|file.mad>
#
# Renders the input to C via `madc --emit=c11`, then:
#   1. flags any <unhandled N_x> markers (a missing renderer construct);
#   2. compiles both the original and the emitted C with
#      `gcc -S -fverbose-asm -O0` and diffs per-function, label-normalized
#      assembly. For plain-C input the two should be near-identical; any
#      divergence localizes the construct madc lowered incorrectly.
#
# Diagnostic tool, not a pass/fail CI gate. Generic — no per-test logic.
set -u

# Corpus mode: run every integration test + fidelity reducer through the gate.
if [ "${1:-}" = "--all" ]; then
	for t in tests/*.mad tests/fidelity/*.c; do
		b=$(basename "$t" | sed 's/\.[^.]*$//')
		[ "$b" = "include_helper" ] && continue
		[ -f "tests/$b.mir_skip" ] && continue
		timeout 20 bash "$0" "$t" 2>/dev/null | head -1
	done
	exit 0
fi

src="$1"
base=$(basename "$src" | sed 's/\.[^.]*$//')
work=tmp/fid
mkdir -p "$work"
emitted="$work/$base.emitted.c"

if ! bin/madc --emit=c11 "$src" > "$emitted" 2>"$work/$base.emit.err"; then
	echo "EMIT-FAIL $base"
	sed -n '1,5p' "$work/$base.emit.err"
	exit 1
fi

if grep -q "<unhandled" "$emitted"; then
	echo "UNHANDLED $base:"
	grep -o "<unhandled [A-Z_0-9]*>" "$emitted" | sort | uniq -c
	exit 2
fi

# Compile original and emitted separately, capturing gcc's exit status so we can
# tell "input isn't standalone C" (leg N/A) from "renderer produced bad C" (bug).
orig_s="$work/$base.orig.s"
emit_s="$work/$base.emit.s"
gcc -S -fverbose-asm -O0 -x c "$src"     -o "$orig_s" 2>/dev/null; src_ok=$?
gcc -S -fverbose-asm -O0 -x c "$emitted" -o "$emit_s" 2>"$work/$base.gcc.err"; emit_ok=$?

if [ $src_ok -ne 0 ]; then
	# The original isn't standalone C (C++/madc constructs): the gcc-fidelity
	# leg cannot apply. Emission itself succeeded with no unhandled markers.
	echo "NONC $base (original not standalone C; asm-fidelity N/A)"
	exit 0
fi
if [ $emit_ok -ne 0 ]; then
	# Original is valid C but our emitted C is rejected by gcc — a real
	# renderer correctness bug (malformed/ill-typed output).
	echo "EMIT-BAD-C $base -> $work/$base.gcc.err"
	exit 4
fi

# Normalize away non-semantic noise: local labels, directives, trailing comments.
norm() {
	sed -E 's/\.L[A-Za-z0-9_]+/.L/g; /^[[:space:]]*\.(file|ident|cfi[_a-z]*|loc|size|type|globl|section|text|align)/d; s/#.*$//; /^[[:space:]]*$/d' "$1"
}

if ! diff <(norm "$orig_s") <(norm "$emit_s") > "$work/$base.asm.diff" 2>/dev/null; then
	echo "ASM-DIVERGE $base ($(grep -c '^[<>]' "$work/$base.asm.diff") lines) -> $work/$base.asm.diff"
	exit 3
fi

echo "FIDELITY-OK $base"
