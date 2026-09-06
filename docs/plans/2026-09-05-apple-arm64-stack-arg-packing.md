# Apple arm64: stack-argument PACKING — plan

Status: LANDED (2026-09-05, s156) — d1f6c853 (the packing rule, both machinizer
arms, both shims, va_start and the interp shim's vararg handoff; the interop
pair's eleven packing probes; the gate at 28 lines) and 779bb654 (found by the
wave's qemu stage: c2mir's plain char is UNSIGNED on the AAPCS64 linux target —
see "Landed" below). Planned the same day, found by the vector
calling-convention arc's Mac stage (docs/plans/2026-09-05-aarch64-simd-v128.md,
"The vector calling convention"). KG Gap `mir_aarch64_apple_stack_arg_packing`
FIXED. Owner scope: the MIR fork (`third_party/mir`: mir-aarch64.h,
mir-gen-aarch64.c, mir-aarch64.c, mir-interp.c); nothing in madc.

## Landed

- **One owner** (mir-aarch64.h, beside `fp_class_type_p` / `stack_arg_16_p`):
  `stack_arg_slot_size (type, vararg_p)` — 16 for a 16-byte type, on Apple 1 / 2
  / 4 for a NON-variadic I8 / I16 / I32-or-F, else 8; `stack_arg_slot_start
  (offset, slot_size)` — the running offset rounded up to the slot's alignment
  (its size); `stack_arg_mem_type (type, vararg_p)` — the argument's own type
  for the SIMD/FP class and for a slot narrower than 8 bytes (a typed load
  extends), else I64. The generic targets read the same helpers and move
  nothing (every slot stays 8 or 16; the qemu stage is byte-identical).
- **Machinizer** (mir-gen-aarch64.c): the call site's first-pass accounting and
  stack-store arm, the callee's parameter loads, blocks rounded to 8 first; and
  `va_start`'s `__stack` = the entry sp + the named stack area rounded to 8 on
  BOTH hosts (Apple stored the bare entry sp — wrong for a vararg function with
  a ninth named argument on the stack: Apple's own va_start is `add x8, sp,
  #120` past the packed int at [sp+112]).
- **ff_call** (mir-aarch64.c): first pass and stores by slot size — `strb` /
  `strh` / `str w` / `str x` (`int_st_pat`, imm12 scaled by `slot_scale`), the
  FP store at the type's slot (a non-variadic float packs to 4 on Apple).
- **Apple interp shim**: loads from the caller's stack by size and signedness —
  `ldrsb x` / `ldrb w` / `ldrsh x` / `ldrh w` / `ldrsw x` / `ldr w` / `ldr x`
  (`int_ld_pat`); the handler's own slots stay 8 bytes (`handler_slot_size`);
  the vararg handoff (fdaa33b4) hands `x9 + round8 (named stack area)`.
- **The gate**: tests/abi/libvecnative.c + vec_ffcall.c gain `nisum10`,
  `ncsum10`, `nssum10`, `nfsum10`, `nmixpack` (char / int / char / long / short
  / composite / char / float over the packed area), `nva9` (a ninth named int
  on the stack, then varargs) and the callbacks `capply10`, `sapply10`,
  `fapply10`, `mixapply`, `va9apply` — eleven lines, both directions;
  scripts/vector_abi_gate.sh LINES 28. Negative chars and shorts check the
  callee's sign extension. Verified: x86-64 (fulltest gate; c2m -eg / -ei /
  madc == gcc), qemu aarch64 (== aarch64-gcc, tmp/logs/w11-a64b.log), the Mac
  (== Apple clang under -eg and -ei, iapply10 included; tmp/logs/w11-chain.log).
- **Found by the gate, fixed in 779bb654**: c2mir (caarch64.h) declared plain
  char SIGNED for every aarch64 flavor; the AAPCS64 linux ABI has it UNSIGNED
  (`__CHAR_UNSIGNED__`; `(char) 200 < 0` is false, CHAR_MAX 255) — the two
  probes passing negative chars differed from the aarch64-gcc build of the pair
  under qemu. `mir_char` / `MIR_CHAR_MIN` / `MIR_CHAR_MAX` now follow the
  target (signed under `MIR_TARGET_APPLE_P` / `MIR_TARGET_WINDOWS_P`), the
  linux flavor predefines `__CHAR_UNSIGNED__ 1`, the shipped `<limits.h>` keys
  `CHAR_MIN` / `CHAR_MAX` on it. Reducer tmp/mirq/charlim.c (== gcc under -eg /
  -ei; the x86-64 host c2m and madc keep gcc's signed output). madc's own
  front end has the TWIN for its emit-only cross-aarch64-linux mode
  (`DataType::dtCHAR == dtINT8` — plain char IS the signed 8-bit tag, so a
  target-keyed `is_unsigned()` needs a distinct tag; no lane runs that mode) —
  KG Gap `madc_cross_aarch64_linux_plain_char_signed`, open. Upstream
  vnmakarov/mir carries the same declaration (aarch64, ppc64, s390x): a PR
  candidate, owner review gates it.

## The rule (as planned)

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
