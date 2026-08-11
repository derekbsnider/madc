#!/usr/bin/env bash
# ABI gate: a call through an UNPROTOTYPED function type is NOT a variadic call.
#
# gcc and clang pass such a call by the ORDINARY convention — the actual
# arguments, default-promoted, in the ordinary argument registers. c2mir used to
# describe it as a VARARG proto carrying ZERO fixed args (a way to let any
# argument count through without an arity check). Those two are
# indistinguishable on x86-64 SysV, where varargs travel in the same registers
# as fixed args — and fatal on Apple arm64, where EVERY vararg goes on the
# stack: the callee, an ordinary C function, read the argument registers and
# found only residue, including the callee's own address left in x0.
#
# That is not a corner case on darwin. The macOS SDK's <secure/_string.h>
# rewrites memcpy / memset / strcpy / strcat / sprintf into
# `__builtin___*_chk(...)`, which madc serves from runtime helpers that no
# header declares — so the ENTIRE memory-writing C surface was unprototyped,
# and every one of those calls SIGBUS'd on Apple silicon while reads worked.
#
# Behaviour cannot gate this: on x86-64 both shapes run correctly. So the gate
# asserts the IR shape, and carries its own negative control — a genuinely
# variadic callee in the SAME translation unit must STILL come out variadic, so
# the gate cannot be satisfied by blanket-disabling varargs.
#
# Run from the repo root (fulltest does).
set -u
cd "$(dirname "$0")/.."

ulimit -t 120 2>/dev/null

C2M="${C2M:-/workspace/mir/c2m}"
MIRDIR="$(dirname "$C2M")"

fail() { echo "unprototyped_call_abi_gate: $1"; exit 1; }

# c2m is c2mir's own driver — the same c2mir madc links. Build it EVERY run,
# never just when absent: c2m is a build artifact that the tree sync can
# replace with an older copy, and a stale driver silently answers for a c2mir
# that is not the one under test (it produced a false failure of this very gate
# on 2026-08-10). make relinks only when something is newer, so this is cheap.
make -C "$MIRDIR" c2m >/dev/null 2>&1
[ -x "$C2M" ] || fail "no c2m at $C2M (build it: make -C $MIRDIR c2m)"

mkdir -p tmp
src=tmp/unproto_abi_gate.c
cat > "$src" <<'EOF'
extern long unproto();            /* K&R / implicit: no parameter information */
extern long variadic(int, ...);   /* genuinely variadic — the negative control */
long g(void) {
    char b[32];
    return unproto(b, 122, 3, 32) + variadic(1, 2);
}
EOF

mir="$(timeout 120 "$C2M" -S -o /dev/stdout "$src" 2>&1)" \
    || fail "c2m failed on $src"

# The proto each call references, then that proto's own declaration line.
call_proto_of() {
    printf '%s\n' "$mir" | sed -nE "s/^[[:space:]]*call[[:space:]]+(proto[0-9]+),[[:space:]]*$1,.*/\1/p" \
        | head -1
}
proto_line_of() {
    printf '%s\n' "$mir" | sed -nE "s/^$1:[[:space:]]*(.*)$/\1/p" | head -1
}

up="$(call_proto_of unproto)"
va="$(call_proto_of variadic)"
[ -n "$up" ] || fail "no call to 'unproto' in the MIR (gate reducer stopped compiling?)"
[ -n "$va" ] || fail "no call to 'variadic' in the MIR (gate reducer stopped compiling?)"

up_line="$(proto_line_of "$up")"
va_line="$(proto_line_of "$va")"

# The correct shape has BOTH halves, and this gate asserts both, because each
# ABI cares about a different one and dropping either breaks a real target:
#
#   fixed args = the actual arguments  ->  AArch64-Darwin stack-banishes only
#       what sits BEYOND them, so with all of them fixed they travel in
#       registers, which is where an ordinary C callee looks. (The original bug:
#       a vararg proto with ZERO fixed args sent every argument to the stack.)
#   vararg flag still set             ->  SysV x86-64 requires %al, the vector-
#       register count, for PROTOTYPE-LESS calls precisely because the callee
#       may turn out to be variadic. (Declaring it non-vararg dropped %al and
#       broke an undeclared printf — tests/testnegzerostatic.mad, caught only by
#       the EXE/OBJ legs; the JIT suite passed all 1018.)
nargs=$(printf '%s\n' "$up_line" | tr ',' '\n' | grep -cE ':p$')
[ "$nargs" -eq 4 ] \
    || fail "unprototyped call proto describes $nargs fixed arg(s), expected 4 (the actual args) — with fewer, Apple arm64 passes the rest on the stack while the callee reads registers: $up_line"
case "$up_line" in
    *...*) : ;;
    *) fail "unprototyped call proto lost its vararg flag — SysV x86-64 needs %al for a prototype-less call (an undeclared printf misreads its args): $up_line";;
esac

# 3. Negative control: a real `...` callee stays variadic.
case "$va_line" in
    *...*) : ;;
    *) fail "negative control broke — a genuinely variadic callee lost its vararg proto: $va_line";;
esac

rm -f "$src"
echo "unprototyped_call_abi_gate: OK (unprototyped call carries its $nargs actual args as FIXED args and keeps the vararg flag; '...' callee unchanged)"
