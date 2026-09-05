# Apple arm64: stack-argument PACKING — plan

Status: PLANNED (2026-09-05, s156), found by the vector calling-convention arc's
Mac stage (docs/plans/2026-09-05-aarch64-simd-v128.md, "The vector calling
convention"). KG Gap `mir_aarch64_apple_stack_arg_packing`. Owner scope: the MIR
fork (`third_party/mir`: mir-gen-aarch64.c, mir-aarch64.c); nothing in madc.

## The rule

AAPCS64 (the generic procedure call standard) rounds every stack argument up to
8 bytes (C.12/C.14). Apple's arm64 ABI ("Writing ARM64 Code for Apple
Platforms") deviates for NON-variadic arguments: a stack argument occupies its
natural size, aligned to its natural alignment — the ninth and tenth `int` of a
ten-int function sit at [sp+0] and [sp+4], a `char` takes one byte, a `float`
four — and the whole area is padded to keep sp 16-byte aligned. Variadic
arguments keep 8-byte slots (that is what Apple's `va_arg` advances; 16-byte
types 16-byte aligned).

MIR uses the generic 8-byte slot for every integer-class stack argument on both
sides of a call and in both shims. MIR<->MIR calls agree with themselves; a
clang-compiled caller or callee does not.

## Evidence

The gcc-interop pair (tests/abi/vec_ffcall.c + libvecnative.c) on the Mac
through a cross-built arm64-macos c2m (tmp/vabi-mac.sh, tmp/logs/vabi-mac-3.log):
`iapply10` — a clang-compiled function calling back a c2m function with ten
ints — printed 788400285 under -eg and 463669405 under -ei for 385. Every other
line of the pair matches Apple clang. A SILENT wrong answer for any MIR
function with more than eight integer arguments of a sub-8-byte type (or more
than eight `float`s) called from or calling clang-compiled code; unreachable
from the madc test suite (madc<->madc).

## Layers (all Apple arms, `#if defined(__APPLE__)`)

1. `mir-gen-aarch64.c` target_machinize, call side: the stack slot of a
   non-variadic argument is sized and aligned by its MIR type (I8/U8 1, I16/U16
   2, I32/U32/F 4, else 8; the 16-byte types as today) — `blk_offset` /
   `mem_size` accounting and the store's memory type (an I32 argument is a
   4-byte store, not an 8-byte one). Variadic positions (beyond `nargs`) keep 8.
2. `mir-gen-aarch64.c` callee side: the parameter load mirrors it (a 4-byte
   load of an int parameter from its 4-byte slot).
3. `mir-aarch64.c` `_MIR_get_ff_call`: the stack stores by size (strb / strh /
   str w / str x) with the same accounting; varargs (i >= arg_vars_num) keep 8.
4. `mir-aarch64.c` `_MIR_get_interp_shim` (Apple arm): the loads from the
   caller's stack by size; the handler's slot area stays 8-byte slots (its
   va_arg walk), the packing is on the CALLER side only.
5. c2mir already names the types (an `int` parameter is `i32`); nothing
   changes there. Blocks (aggregates) keep their 8-byte rounding (Apple: a
   composite on the stack is 8-byte aligned).

One predicate per fact: `apple_stack_slot_size (type, vararg_p)` beside
`stack_arg_16_p` in mir-aarch64.h, read by the machinizer and both shims.

## Gate

Extend the interop pair: `nisum10 (int × 10)`, `ncsum10 (char × 10)`,
`nssum10 (short × 10)`, `nfsum10 (float × 10)` on the native side called from
c2m (the caller side), and the matching callbacks (`iapply10` exists; add
`capply10`, `sapply10`, `fapply10`) for the callee side; a vararg control
(`nvi10 (int n, ...)` with ten ints: 8-byte slots). The Mac stage
(tmp/vabi-mac.sh) is the gate; the linux lanes must stay byte-identical (the
generic rule is untouched — the predicate returns 8 there).

## Estimate

Half a session: the four arms are the ones the vector arc just rewrote, the
loop is pinned (cross c2m + the Mac), and the oracle is Apple clang.
