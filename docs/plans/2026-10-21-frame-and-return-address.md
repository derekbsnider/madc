# `__builtin_frame_address` / `__builtin_return_address` — plan

## The defect

Two silent wrong answers, both front-end stubs:

- `__builtin_return_address(level)` is a lexer macro for `0`
  (`src/lexer.cpp`, "unsupported, but many GCC torture tests only need a
  stable sentinel value").
- `__builtin_frame_address` is renamed to `__madc_builtin_frame_address`, a
  host helper (`src/va_helpers.cpp`) that returns ITS OWN frame — a frame
  below the caller's, not the caller's.

Reducer: `int f(void){ ra = __builtin_return_address(0); ... }` — gcc 13 and
clang 18 give the address after `f`'s call in its caller; madc gives NULL on
Linux and Windows alike. `gcc.c-torture/execute/frame-address.c` is in
`docs/parity/torture-skip-manifest.txt` for the same reason.

B205 6.3 step 5 needs the frame address too: the establisher frame a Windows
try passes to `_setjmp` is mingw's `__builtin_frame_address(0)`.

## Canon (recon 2026-10-21: gcc 13 x86-64 and aarch64, mingw gcc 13, clang 18
x86_64-w64-mingw32 / powerpc64le / riscv64 / s390x, `-O2 -S`)

| target         | `__builtin_frame_address(0)`                 | `__builtin_return_address(0)` |
|----------------|----------------------------------------------|-------------------------------|
| x86-64 SysV    | rbp after `push rbp; mov rsp,rbp` = CFA - 16 | `(%rsp)` at entry = CFA - 8   |
| x86-64 Windows | the establisher frame (gcc: rbp with `.seh_setframe %rbp, 0`; clang: `rbp - 128` under `.seh_setframe %rbp, 128`) | CFA - 8 |
| aarch64        | x29, the frame record {caller's x29, x30}    | x30 at entry                  |
| ppc64          | r1 after the prologue (the back chain word)  | LR at entry                   |
| riscv64        | s0 = the CFA (sp at entry)                   | ra at entry                   |
| s390x          | r15 at entry (CFA - 160)                     | r14 at entry                  |

Both force a frame where the function would have none (gcc's x86-64 `fa`
pushes rbp; aarch64 builds a frame record). A nonzero level: the GCC manual
leaves the value unspecified ("may return 0 or garbage", nonzero levels are
`-Wframe-address`); aarch64 gcc returns 0 for any nonzero
`__builtin_return_address` level, and `20010122-1.c` guards its level-1
checks with `save_ret1[3] &&`.

## Design

MIR (no architecture code outside MIR; all five targets):

- Two insns, `MIR_FRAME_ADDR dst` and `MIR_RET_ADDR dst` (one I64 output, no
  inputs), appended to the insn codes, in the I/O tables, the interpreter
  (frame address: the interpreter frame's own address; return address 0 —
  the manual's "unspecified" for a frame no machine code describes) and the
  inliner's never-inline list (inlined, they would name the caller's frame).
- `MIR_RET_ADDR` lowers in each target's machinize, where the value is still
  in hand: a move from the link register at body entry (aarch64 x30, ppc64
  LR, riscv64 ra, s390x r14 — the prologue saves it without changing it), or
  on x86-64 a load from `FP + 8` with the frame pointer kept (the Windows
  frame's `win64_rebase_fp` moves that displacement with every other).
- `MIR_FRAME_ADDR` survives register allocation (no pattern: the RA's
  rewrite leaves it a register output, combine and address folding skip an
  insn the target rejects) and is lowered by each prologue, where the frame is
  known, through one generic helper `gen_lower_frame_addrs (gen_ctx, k)` —
  `dst = FP + k` (a register through the target's temp when the RA left
  memory or `k` does not fit an add): x86-64 SysV k = 0; Windows k = -FO (the
  establisher frame); aarch64 0; ppc64 0; riscv64 frame_size (the CFA);
  s390x frame_size (r15 at entry). A function holding one keeps a frame on
  every target (`gen_ctx->frame_addr_p`, checked by each frameless-leaf
  shortcut; x86-64 also keeps its frame pointer).
- c2mir: `__builtin_frame_address` and `__builtin_return_address` with one
  integer constant argument (an error otherwise, as gcc), type `void *`;
  level 0 emits the insn, a nonzero level the null pointer.

madc front end: the lexer stubs, `__madc_builtin_frame_address` (the helper
and its registration) are deleted; both builtins are registered `void
*(unsigned)` with no symbol, as `__builtin_va_start` is, so the call reaches
c2mir as an N_CALL. `--emit=c11` prints the builtin call, which every C
compiler takes.

## Thread-safety contract

Each builtin reads the calling thread's own frame: no shared state, safe on
any thread.

## Tests

- `tests/testbuiltin_frame_return_address.mad` (gcc/clang oracle): the return
  address is the caller's IP the unwinder reports for that frame; the frame
  address lies between a callee's local and the caller's local, is stable
  within a function and differs per frame. (A nonzero level is not printed:
  its value is unspecified, and gcc's x86-64 walks the rbp chain.)
- `gcc.c-torture/execute/frame-address.c` leaves the skip manifest.
- MIR unit test: both insns through MIR directly on the host target; the
  other targets compile (`check-mir-targets-compile.sh`), and their lowering
  is read against the canon above.
