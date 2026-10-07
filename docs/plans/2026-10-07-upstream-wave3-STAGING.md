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

**Open upstream issues since September, against the PRs (re-checked
2026-10-07 via `gh api`, owner request):**

| Issue | Opened | A PR that resolves it | Our side |
|---|---|---|---|
| #472 `extern T x = …` (08-31) | — | none filed yet | our PR A, unfiled |
| #473 anonymous-union aliases (08-31) | — | none filed yet | our PR B, unfiled |
| #474 win64 32-byte spill area per call (09-01) | — | **none** | open in ours: performance (sub/add rsp around every call, frame pointer forced), not wrong code; the fix is msvc/clang's shape, the outgoing area reserved once in the prologue — ours to fix in `third_party/mir`, win64 lanes, then file |
| #475 `op_nums` overflow (09-03) | — | **#468** (wshlavacek, open since 08-18) — same root | adopted (`1cb0c1872`); optional note on #475 pointing at #468 |
| #476 inliner growth check (10-05) | — | none (Cyan offered one) | adopt Cyan's `625be9104`; file nothing |
| #482 part 1, x86-64 stack long double (10-07) | — | none filed yet | our PR C, unfiled |
| #482 part 2, block alignment (10-07) | — | **none** — Cyan asked Vladimir for the design first | **MEASURED open in ours, silent**: a gcc-compiled callee taking `struct { long double }` by value after seven `long`s — gcc+gcc `take_s 2.5`; upstream c2m, our c2m AND `bin/madc` callers `take_s -nan`, exit 0 (container `tmp/blk482/`). `MIR_T_BLK` carries a size only. Our part-1 work (`4fbffc77e` x86-64, `d123c3b0a` / `62e86e2db` aarch64, `d1f6c8539` Apple) aligns 16-byte SCALAR and VECTOR slots, never a block |

Unresolved by any PR, ours or theirs: **#474** and **#482 part 2** — part 2 is a
silent wrong answer for madc programs calling C libraries, so it outranks #474
(performance). Proposal: fix it in `third_party/mir` now with the alignment
carried on the block type (the option our draft #482 comment backs), offer it
upstream once Vladimir answers Cyan.

Found on the way (2026-10-07): c2m's PARSER rejects `_Alignas (16)` on a
struct member (`syntax error on struct`), upstream and ours; gcc accepts it and
madc's own parser does too (`bin/madc --std=c11` rc 0), so it is c2m-only —
a small standalone upstream candidate.

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

**Amendment (measured 2026-10-07): D as staged BREAKS upstream's aarch64
bootstrap; file PR G first and stack D on it.** Upstream's
`c2mir-bootstrap-test` recipe, run on aarch64 under qemu (aarch64 c2m built by
gcc with MIR's flags; it compiles MIR's sources, then runs itself from the
.bmir to compile them again):

| upstream tree | stage 1 | stage 2 | result |
|---|---|---|---|
| a8ab7c31 (control) | rc 0 | rc 0 | Passed |
| + D | rc 0 | rc 1 (`features.h:472: wrong result of ##`) | FAIL |
| + D + G's two source edits | rc 0 | rc 0 | Passed |

Cause: MIR's sources assume signed plain char (its gcc/CMake builds force
`-fsigned-char`); D makes c2m compile them with aarch64-linux's unsigned char.
Two dependencies, both found and fixed in G: c2mir's line buffer (`cs_unget`
pushes EOF into a `char` buffer, `cs_get` pops it as 255) and `out_insn`'s hex
scan (`char d` holding `hex_value()`'s -1). The recon's riscv64 site is NOT one:
its `d` holds a non-negative displacement. A `-fsigned-char` option for c2m was
rejected: plain char's sign is read at ~90 context-free `signed_integer_type_p`
sites. D's body gains one sentence: "This builds on #G, without which MIR's own
bootstrap on aarch64 Linux stops in stage 2."

### PR G — `pr/c2mir-source-byte-ff-is-not-eof` (NEW; file before D)

**Title:** c2mir: a 0xFF source byte is not EOF

> Thank you for MIR. A small lexer fix, found while checking plain char's sign
> on aarch64 Linux:
>
> c2mir's line buffer is a `VARR (char)`, and `cs_get` returns the popped
> `char` as an `int`. Where char is signed, a source byte 0xFF pops as -1, which
> is `EOF`, so the file ends there: a Latin-1 'ÿ' in a comment gives
> "unfinished comment" on x86-64 today; gcc and clang accept it. Where char is
> unsigned, the reverse: `cs_unget (EOF)` stores -1 in the buffer and it pops
> back as 255 ("syntax error on 255").
>
> `cs_get` and `str_getc` now return the byte as `unsigned char` (as `fgetc`
> does), and `cs_unget` leaves EOF out of the buffer (the next `cs_get` finds
> the end again). The same commit makes `out_insn` in `mir-gen-aarch64.c` hold
> `hex_value ()` in an `int`: as a `char` its -1 never compares below zero
> when char is unsigned. Together these let MIR compile itself where plain
> char is unsigned. A test is in `c-tests/new/source-byte-ff.c`. Happy to rework.

Evidence: reducer under gcc rc 0, clang rc 0, upstream c2m `-ei`/`-eg` rc 1
"unfinished comment", fixed c2m rc 0 (both). Our side: `e3359770c` on
`fix/mir-plain-char-claude` with the gate `scripts/check-mir-plain-char.sh`.
The clean upstream branch is still to build (one commit on a8ab7c31, then
`tmp/upstream-pr-check.sh`); D is then rebased onto it.

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

**Clash rule (owner, 2026-10-07):** where an external hunk clashes with a fix
of ours, OURS STAYS if it is more correct — the gcc/clang oracle on a reducer
decides, never provenance; take theirs where theirs is more correct; merge
where each covers a different case. Each adoption commit names every kept-ours
hunk and the oracle that decided it.

Adoption sources, per §5 (each PR's branch head IS its PR head; nothing
moved past it): #480 `35185c926`, #479 `976096d37`, #478 `f7594a507`,
#477 `079a47da8`, #476 Cyan's `625be9104` (byte-equivalent to our planned
one-liner — adopt HIS with credit; file no PR of our own for #476, he has
offered one). Merge notes from the recon (read, not run):

- **#479:** ours counts V128 into `xmm_args` (mir-gen-x86_64.c:393, used :671);
  his `fp_arg_num < 8 ? fp_arg_num : 8` already covers V128 because our
  `get_arg_reg` (:272–291) bumps `fp_arg_num` for F, D and V128 — drop
  `xmm_args`, take his line.
- **#478:** defence in depth on ours. Our #466 adoption (`98bee3d93`) fixed the
  same root from the gate side (`target_memory_ok_p` compares the byte size,
  mir-gen-aarch64.c:2724–2744); his c-test should pass on ours without it —
  RUN it on the aarch64 lane before and after. Cost (one `target_insn_ok_p`
  per combined mem operand) unmeasured; measure on the index-c lane.
- **#477:** fixes an OPEN bug of ours — struct HFAs still travel in GPRs
  (our `93010447b` records it; `caarch64-ABI-code.c:95–141` splits only
  `_Complex`). Hand merge: `mir-aarch64.c` 4 hunks, `mir-gen-aarch64.c` 3,
  `caarch64-ABI-code.c` 2, against `d1f6c8539` / `fdaa33b42` / `b70903ed3`.
  Ours must win on: the `ff_call` long double stride (his hunk reintroduces
  `sizeof (long double)`), the Apple x9 va_list handoff in the interp shim,
  and HFA stack placement after our packed Apple ints (needs
  `stack_arg_slot_start(offset, 8)`). After the merge make `_Complex` and
  `struct { float, float }` take the same path when FP registers run out.
  His Apple arms are untested by him: aarch64-linux AND both macOS lanes.

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

## 5. Cyan's fork (github.com/cyanogilvie/mir) — recon 2026-10-07

Read-only recon (remote `mir-cyan` on the madc repo; full matrix with shas
and file:line in the container-side scratch `tmp/cyan-mir-recon.md`, not
tracked). 21 branches: 8 are heads of his already-merged PRs (#430–#440
series, all in our tree); 5 are the heads of #477–#481 (branch head == PR
head, `mergeable_state: clean`); `debug-support` is his integration branch,
37 commits over a8ab7c31, of which 21 are on no new PR: the June debug series
(line map — already in our tree), #439 (paramless vararg — ours no longer
refuses it), a cherry-pick of another contributor's #420, and the October
items below.

**Our six PRs stand.** Nothing of his touches A, B, D (see the amendment), E
or F; no textual collision. PR C: his #482 names exactly our four x86-64
long double sites and he has NO commit for it ("happy to send a PR for that
part alone if you agree") — file C first, say so on #482. The block half of
#482 is unimplemented on both sides. #474 (win64 spill area) — he has no
win64 work. Our Apple stack packing (`d1f6c8539`) and aarch64 V128 are ahead
of his tree. #481 stays a duplicate of our `8b005a51c` (equivalent bounds).

**Bugs new to us:** #480, #479, #477 — adoptions in §3. #476 is the same
one-liner we planned (adopt his).

**Not bugfixes — owner's call, none planned:**

| Commit | What | Shape |
|---|---|---|
| `5b70140f3` | `MIR_func.cfi` + `.debug_frame`: gdb unwinds JIT frames | +338 over 8 files; 5 conflicts in our grown `mir-debug.c` (loader, ELF/PE). Value: madcide / gdb backtraces through JIT code |
| `bf72acee4` | calls through forward/import items become direct `bl`/`call` (skip the thunk) | x86 part small (+13 mir-gen.c, 2 lines x86); aarch64 +72 with 8 conflicts and a `gen_record_cfi` dependency. Relevant: c2mir emits `MIR_new_forward` items (c2mir.c:17668), so our calls never go direct today. Interplay with redefinition / lazy gen / bb versioning NOT analysed |
| `95e7441ae` | micro-inline past the growth limit, 2000-insn budget per caller | heuristic; changes compile time and size for every large function (SMAUG-size) — measure before considering |
| `b538e8147` | inlining glue stamped with the call site's line | +8 mir.c, debug quality |

**Open from the recon:** whether madc's CIR path ever touches va_list fields
directly (decides how real #480 is for madc — c2mir does not); #478's compile
cost; none of his c-tests were run on ours.
