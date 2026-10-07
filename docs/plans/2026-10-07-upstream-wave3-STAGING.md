# Upstream wave 3 — MIR (vnmakarov/mir): staging, for OWNER REVIEW

**Date:** 2026-10-07 · **Status:** DRAFT — nothing filed, commented or pushed.
Wave 1: `2026-07-23-upstream-wave1-STAGING.md` (#461 #462 #463). Wave 2 (Aug):
#469 #470 #471. Held for after the master release (owner, 2026-09-05): the six
`pr/*` branches below; v0.102.0 is on master.

## 0. Upstream since our last look

- **No upstream commits since a8ab7c31 (2026-06-19).** Nothing to merge into
  `third_party/mir`. Every branch below sits on a8ab7c31. Re-run 2026-10-07
  on the build container (`tmp/upstream-pr-check.sh`: a clean upstream clone,
  each branch's own commit, `make c2m` + `make test`): all six build rc=0,
  test rc=0.
- Vladimir has not responded to #461–#471 (July–August).
- New from others: Cyan Ogilvie — issues #482, #476, PRs #477–#481 (all
  2026-10-05/07); aardappel — #472–#475; ThePeiLin #467; richarddd #466, #465;
  wshlavacek #468.

Triage, per item (evidence: reducers on the build container, upstream c2m at
a8ab7c31 vs ours vs gcc):

| Item | What | Our fork | Action |
|---|---|---|---|
| #472 aardappel | `extern T x = …;` definition lost | fixed (0a2d3e8e6) | our PR, **Fixes #472** |
| #473 aardappel | anonymous-union members alias apart at -O2 | fixed (497412956) | our PR, **Fixes #473** |
| #482 Cyan | 16-byte stack args: x86-64 long double; blocks carry no alignment | part 1 fixed (4fbffc77e); part 2 open in ours too | our PR fixes part 1 — Cyan offered to send one, so open ours promptly and say so on #482 |
| #481 Cyan PR | va_block_arg all-SSE past the XMM save area | fixed independently (8b005a51c, 08-28, c-testsuite 00204) — same bound | do NOT send ours; optional note on #481 confirming it |
| #480 Cyan PR | GVN: va_* insns as memory clobbers | **bug present** (reducer rc=1 at -O2) | adopt into `third_party/mir` |
| #479 Cyan PR | x86-64 `%al` must count block-arg XMM regs | **bug present** (garbage vs gcc `1.5 -2.25 3.125 4`) | adopt (keep our V128 count, cap 8) |
| #478 Cyan PR | don't combine an address the target can't encode | guard absent; failure not reproduced on aarch64 | adopt (defensive) |
| #477 Cyan PR | aarch64 HFAs per AAPCS64 | **bug present** (qemu: `4 0 -1.09508e+304` vs gcc `1.5 3.125 4`) | adopt — large (+479), conflicts with our aarch64 V128/Apple work; aarch64 + macOS lanes |
| #476 Cyan | inliner growth check: callers >200 insns never inline | present (mir.c:4636) | fix in `third_party/mir` (perf; SMAUG-size functions) |
| #475 aardappel | `op_nums` overflow in try_spilled_reg_mem | fixed (adopted #468, 1cb0c1872) | nothing (same root as #410/#468) |
| #474 aardappel | win64 32-byte spill area at every call | present (code reading; no win64 run) | ours to fix later; optional note: its wrapper_end side note is fixed in ours (c695fe5bc) |
| #467 ThePeiLin | ssa_combine base onto a backedge PHI | fixed (c9e617b4c) | nothing — our #471 already says Fixes #467 |
| #466 / #468 | aarch64 disp gate / spill bounds | adopted (98bee3d93 / 1cb0c1872) | nothing |
| #465 richarddd | stp/ldp prologue | closed by its author (6% slower on M5) | nothing |

## 1. File now — the six held branches (owner review of each body)

Each is one commit on a8ab7c31, authored by us, verified at upstream HEAD,
upstream `make test` green (re-run 2026-10-07). Head: `derekbsnider:<branch>`, base
`vnmakarov:master`.

### PR A — `pr/c2mir-extern-with-initializer-is-a-definition`

**Title:** c2mir: a file-scope `extern` declaration with an initializer is a definition

> Fixes #472.
>
> Thank you for MIR, and thanks to aardappel for the clear report.
>
> C17 6.9.2p1 makes an external object declaration that has an initializer
> an external definition, so `extern int x = 5;` defines `x`; gcc, clang and
> MSVC all define it (gcc and clang with a warning). c2mir treated `extern` as
> decisive here: it emitted an import and dropped the initializer, so the
> object was defined nowhere ("import of undefined item"), or, when another
> unit defined it, the initializer was silently ignored.
>
> The N_SPEC_DECL lowering now recognizes the case (file scope, an object, an
> initializer), skips the import and takes the same definition path as
> `int x = 5;`, with the warning gcc and clang print.
>
> ```c
> extern int x = 5;
> int main (void) { return x == 5 ? 0 : 1; }
> ```
>
> `make test` passes. Happy to rework it if you would prefer a different
> approach.

### PR B — `pr/c2mir-anonymous-union-member-alias-class`

**Title:** c2mir: a member of an anonymous union carries the union's alias class

> Fixes #473.
>
> Thank you for MIR, and to aardappel for the reduced Lobster case.
>
> A member reached through an anonymous union shares storage with the union's
> other members, so its accesses need the union's alias class — which
> `get_type_alias` already registers member conflicts for — exactly as an
> access through a named union gets. The anonymous case took the member
> type's own class, so at -O2 GVN forwarded a stale value across a store
> through the other member:
>
> ```c
> struct { union { long long ival; double fval; }; } v;
> v.fval = 1.5;
> return v.ival;   /* -O2: the value from before the store */
> ```
>
> `enclosing_anon_union_type` follows
> `containing_unnamed_anon_struct_union_member` to the outermost anonymous
> union (an anonymous struct on the way changes nothing — its members do not
> overlap), and both field lowerings (N_FIELD, N_DEREF_FIELD) use that union's
> class when there is one.
>
> `make test` passes. Happy to adjust if you'd like it shaped differently.

### PR C — `pr/x86-64-long-double-stack-slot-alignment`

**Title:** x86-64: a stack-passed long double takes a 16-byte aligned slot

> This fixes the scalar long double half of #482 (thank you, Cyan, for
> writing up both halves; the block-alignment half is not addressed here).
>
> SysV x86-64 (3.2.3) aligns a memory argument to its type, so a long double
> occupies a 16-byte aligned stack slot and the caller pads after an odd
> number of eightbytes; 3.5.7 aligns va_arg's overflow pointer the same way.
> MIR advanced every stack slot by 8 or 16 without rounding — at the caller
> (target_machinize), the callee's parameter load, the interpreter's ff_call
> shim and va_arg_builtin — so after seven integer arguments a long double
> sat 8-byte aligned. Both MIR sides agree with each other, so MIR-only tests
> pass; against gcc- or clang-compiled code it is wrong in both directions:
>
> ```c
> long double f (long a1, long a2, long a3, long a4, long a5, long a6,
>                long a7, long double x, long a8);  /* gcc reads x at rsp+16 */
> ```
>
> All four sites now round to 16 before a long double; win64 is untouched
> (its long double travels by reference). `make test` passes. Happy to rework
> if you'd prefer it organized differently.

### PR D — `pr/c2mir-aarch64-linux-unsigned-char`

**Title:** c2mir (aarch64): plain char is unsigned on the Linux ABI

> Thank you for MIR. A small target-description correction:
>
> On AAPCS64 Linux plain `char` is unsigned — aarch64-linux-gnu-gcc and clang
> predefine `__CHAR_UNSIGNED__`, `(char) 200 < 0` is false and `CHAR_MAX` is
> 255 — while `c2mir/aarch64/caarch64.h` declares `mir_char` signed for every
> aarch64 target, so c2m on aarch64 Linux treats plain char as signed and
> disagrees with the platform compiler wherever a char goes negative (a
> gcc-compiled callee reads a c2m caller's `-9` as 247; `'\377'`; `CHAR_MIN`
> / `CHAR_MAX`).
>
> ```c
> #include <limits.h>
> int printf (const char *, ...);
> int main (void) {
>   char c = (char) 200;
>   printf ("CHAR_MIN %d CHAR_MAX %d\n", CHAR_MIN, CHAR_MAX);
>   printf ("c %d neg %d ff %d\n", c, c < 0, '\377');
>   return 0;
> }
> /* gcc: CHAR_MIN 0 CHAR_MAX 255 / c 200 neg 0 ff 255 */
> ```
>
> Apple and Windows arm64 keep char signed. `mir_char` / `MIR_CHAR_MIN` /
> `MIR_CHAR_MAX` now follow the target, the Linux predefined macros gain
> `__CHAR_UNSIGNED__ 1`, and the shipped `<limits.h>` derives `CHAR_MIN` /
> `CHAR_MAX` from it as gcc's does. ppc64 and s390x Linux also have unsigned
> plain char; I left those headers unchanged for you to judge. Happy to
> rework.

### PR E — `pr/apple-interp-shim-fp-stack-imm12`

**Title:** aarch64 (Apple): encode a stack FP argument's offset in the interp shim

> Thank you for MIR. A one-line encoding fix:
>
> `_MIR_get_interp_shim`'s Apple arm loads the ninth and later floating-point
> arguments from the caller's stack with `pat |= stack_arg_sp_offset |
> temp_reg;`, which places the raw byte offset in the low register fields
> rather than the scaled imm12 in bits [21:10]. Only the first stack FP
> argument (offset 0) is read correctly. The integer arm beside it already
> encodes `((stack_arg_sp_offset >> scale) << 10)`; the FP arm now does the
> same. Reproducer (Apple arm64, `c2m -ei`): a native function calling an
> interpreted one with nine doubles, the ninth on the stack.
>
> Happy to adjust.

### PR F — `pr/canonicalize-register-and-proto-types`

**Title:** Canonicalize register and proto types like every other typed slot

> Thank you for MIR. On a host whose long double is double (Apple arm64,
> win64 — `canon_type` maps `MIR_T_LD` to `MIR_T_D`), types are canonicalized
> on data items, a func's result and var types, memory operands and insn codes,
> but not on a register created by `MIR_new_func_reg`, nor on a proto's result
> and argument types. A front end that asks for an ldouble register there gets
> an LD register the type checker then refuses against the canonical D
> operand (`in instruction 'i2d': unexpected operand mode ... Got 'ldouble',
> expected 'double'`; the same for a call's result against an LD proto).
> `new_func_reg` and `new_proto` now canonicalize too; nothing changes where
> long double is 16 bytes. Happy to rework.

Submission order (after approval): A, B, C first (each answers an open issue;
C before Cyan sends a duplicate), then D, E, F. Mechanics as wave 1:
`gh pr create -R vnmakarov/mir --head derekbsnider:<branch> --base master`.

## 2. Comments (owner review)

- **#482** (after PR C is open): "Thank you for the careful write-up. The
  scalar long double half is in #NNN (the four sites you list round to 16).
  On the block half, an alignment carried on the block type (your option 2)
  would suit us too — we meet the same gap for 16-byte aligned aggregates."
- **#481** (optional): "Confirming from our side — we hit this through
  c-testsuite 00204 and carry the same one-line bound in our fork. Thank you."
- **#474** (optional): "The `_MIR_get_wrapper_end` side note is real — we
  carry a fix in our fork (an add that keeps the frame 16-byte aligned and the
  xmm saves clear of the shadow space) and can send it if useful."

## 3. Adopt into `third_party/mir` (our own codebase; each its own commit, Tier 1 + 2)

#480, #479, #478 (small), #476 (our fix), then #477 (large: careful merge
with our aarch64 V128 / Apple va_list work; the aarch64 and macOS lanes).
These are madc changes, not filings; they ride the next release.

## 4. New candidates of ours — clean branches still to build

Fifteen of our `third_party/mir` fixes since 2026-07-23 reproduce at upstream
HEAD (reducers on the container under `/tmp/mir-audit/`, upstream c2m vs gcc).
Each needs a clean port onto a8ab7c31 (fork-only parts dropped), `make test`,
and a body. Five edit overlapping c2mir initializer / bit-field code, so each
is ported to stand alone.

Silent wrong code: 8819bb058 float → uint64 ≥ 2^63 · a9b384511 a string
initializes a whole char-array sub-object · 7a9fcd871 a later designated
initializer overrides an earlier one · 4ae0489c0 static bit-field initializers
across type units · 06850da9d conversion to `_Bool` tests != 0 · 48d1a0120 a
volatile local survives longjmp · 115ad0fa3 zero-fill after a bit-field in a
partial local initializer · 376f4cba1 `_Alignas` on automatic objects ·
029a55a0f packed bit-fields straddling their unit (largest, lowest priority).
Crashes / valid C rejected: 86f1577de `1[arr]` (SIGSEGV) · 8d85488f8 nested
compound literal (SIGSEGV) · d79fbad67 a statement expression ending in a
non-expression · df8e67a93 file-scope `_Thread_local` · 5c722ff90 `_Generic`
lvalue conversion · 822512573 a designator inside an anonymous member's braces.

Not runnable here, unverified: six aarch64 and six win64 fixes (no qemu /
win64 c2m on the container); the win64 `_MIR_get_wrapper` / wrapper_end pair
(c695fe5bc, f626e7b3d) are the strongest of them.

**Pacing (owner's call):** six of ours are already open and unanswered since
July–August. Proposal: file section 1 now; build all fifteen branches; file
them in waves of about five, silent wrong code first, as the open ones move.
