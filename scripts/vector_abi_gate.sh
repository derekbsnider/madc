#!/usr/bin/env bash
# ABI gate: a 128-bit vector crosses a call boundary the way gcc and clang
# pass it — SysV x86-64: an SSE-class value (xmm, or a 16-byte ALIGNED stack
# slot when none is left, %al counting it as a vararg); AAPCS64: a Short
# Vector (v[NSRN], or a 16-byte aligned stack slot); the result in xmm0 / v0;
# the same through `...`, read back by va_arg from the register save area or
# the aligned overflow slot. And long double keeps its 16-byte aligned stack
# slot beside it.
#
# madc-only tests cannot see any of this: a self-consistent caller and callee
# agree with each other whatever the layout, which is exactly how the vector
# ABI was wrong on both targets while every vector test was green. So this
# gate compiles ONE side with the host's own C compiler: tests/abi/libvecnative.c
# becomes a shared object (the native callee, and a native caller of the
# callbacks it receives), tests/abi/vec_ffcall.c is compiled by c2m (-eg, the
# generated code; -ei, the interpreter with its ff_call and interp shims) and by
# madc (through #load), and every lane must print byte-for-byte what the pair
# prints when the host compiler builds BOTH halves. The pair covers vectors
# declared, nine deep, mixed with integers and a stack long, through `...`
# (including the FP registers exhausted), callbacks receiving vectors / nine
# vectors / a vector vararg list / nine doubles / ten ints, and long double on
# the stack and through `...` -- and the stack-argument PACKING probes (ten
# ints / chars / shorts / floats, a mixed run, a vararg function with a named
# stack argument) in both directions: Apple arm64 packs a non-variadic stack
# argument at its natural size where AAPCS64 and SysV give every one 8 bytes,
# so the linux lanes gate the generic rule and the Mac stage the Apple one.
#
# Non-vacuity: the oracle must print the expected line count; a lane that
# prints nothing, or dies, fails.
#
# Run from the repo root (fulltest does).
set -u
cd "$(dirname "$0")/.."

ulimit -t 300 2>/dev/null

MADC="${MADC_BIN:-bin/madc}"
C2M="${C2M_BIN:-obj/mir/host/c2m}"
HOSTCC="${HOST_CC:-cc}"
LINES=28

fail() { echo "vector_abi_gate: $1"; exit 1; }

[ -x "$MADC" ] || fail "no madc at $MADC"
[ -x "$C2M" ] || fail "no c2m at $C2M (make -C src builds it)"
command -v "$HOSTCC" >/dev/null 2>&1 || fail "no host C compiler ($HOSTCC) — the oracle needs one"

dir="$PWD/tmp/vector_abi"
rm -rf "$dir"; mkdir -p "$dir"
"$HOSTCC" -O0 -fPIC -shared -o "$dir/libvecnative.so" tests/abi/libvecnative.c \
	|| fail "the host compiler could not build the native half"
"$HOSTCC" -O0 -o "$dir/oracle" tests/abi/vec_ffcall.c tests/abi/libvecnative.c \
	|| fail "the host compiler could not build the oracle"
timeout 30 "$dir/oracle" > "$dir/expect" 2>&1 || fail "the oracle did not run"
n=$(grep -c . "$dir/expect")
[ "$n" -eq "$LINES" ] || fail "the oracle printed $n lines, expected $LINES — the reducer no longer exercises the pairs"

check() { # lane, output file
	if cmp -s "$dir/expect" "$2"; then
		echo "vector_abi_gate: [$1] OK — $LINES lines identical to the host compiler's"
	else
		echo "vector_abi_gate: [$1] DIFFERS from the host compiler (expected < / got >):"
		diff "$dir/expect" "$2" | head -20
		fail "[$1] the vector / long double calling convention disagrees with gcc-compiled code"
	fi
}

timeout 60 "$C2M" -L "$dir" -l vecnative tests/abi/vec_ffcall.c -eg > "$dir/eg" 2>&1
check "c2m -eg" "$dir/eg"
timeout 60 "$C2M" -L "$dir" -l vecnative tests/abi/vec_ffcall.c -ei > "$dir/ei" 2>&1
check "c2m -ei" "$dir/ei"
printf '#load "%s/libvecnative.so" as vecnative;\n#include "%s/tests/abi/vec_ffcall.c"\n' "$dir" "$PWD" > "$dir/vec_ffcall_madc.mad"
timeout 60 "$MADC" "$dir/vec_ffcall_madc.mad" > "$dir/madc" 2>&1
check "madc" "$dir/madc"
echo "vector_abi_gate: OK"
