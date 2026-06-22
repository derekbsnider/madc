# MIR community-fork patch triage (2026-06-02)

Upstream `vnmakarov/mir` has been frozen ~2 years, so several community forks carry
independent fixes/features. We surveyed four against madc's MIR fork (`/workspace/mir`,
branch `develop`, pin `MIR_COMMIT`). Decision (user, 2026-06-02): **adopt genuine bug
fixes freely — upstream divergence is no longer a concern** (we are the maintained line).

> **OUTCOME (fork `8864a73`, pin bumped from `4aa628b`): 5 proven bug fixes ADOPTED**
> — `MAX_INSN_RELOAD_MEM_OPS` 2→4 (op_nums overflow), `addr_regs` rebuild after SSA
> (O2-gated), `jump_opt` lref-label preservation, vararg RET use-after-NULL, NULL
> teardown guards. Each carries an `ADOPTED-FROM: <fork> @ <sha>` comment in the
> source. The GVN-`≥O3` gate was NOT adopted (a shim, not a fix). The O2-viability
> experiment below is what established which are inert at our O1 gate vs genuine —
> all 5 are O1-safe (full torture 1565, zero regressions); SMAUG boots.

## The two structural filters (why most fork content does NOT apply to madc)

1. **madc bypasses c2mir's text parser AND preprocessor.** `cir_builder` builds the
   `cir_node` tree and feeds it to c2mir's *checker/generator* (`c2mir_compile_tree`);
   c2mir never lexes/preprocesses source. → Any fork patch in c2mir's lexer / preprocessor
   / `__asm` text-statement / embedding API is irrelevant to us.
2. **We run MIR-gen at O1** (`madc_opt_level = 1`; the torture runner passes no `-O`).
   O2+ is gated behind a known (undocumented, historical) SSA bug. → Fork fixes that only
   bite at `optimize_level >= 2` are inert at our current gate; they pay off only if/when
   we make O2 viable.

## Forks surveyed

| Fork | Base | Ahead | Theme |
|---|---|---|---|
| `theMackabu/mir` | vnmakarov master | 16 | codegen correctness + aarch64/win32 + atomics + module-remove |
| `vladich/mir-patched` | vnmakarov master | 6 | `__asm` (MIR-IR mini-asm), memory-mgmt/module-unload, aarch64/win32 |
| `MysticalUnicat/mir` | vnmakarov master | 9 | c2mir preprocessor (`,##__VA_ARGS__`, `##`), embedding API, module-remove |
| `cyrilmhansen/mir` | vnmakarov master | 654 | a whole `basic/` BASIC→MIR compiler + runtime + REPL + games |

## Patch-by-patch verdict

### TESTED then NOT ADOPTED (empirically inert / insufficient for madc) — see the O2 experiment below
- **`addr_regs` rebuild after SSA-rebuild** (`mir-gen.c`, before `reg_alloc`). Corroborated
  **independently in theMackabu AND vladich**: `transform_addrs` computes `addr_regs`
  (address-taken vars that must be stack-homed), but `build_ssa`/`ssa_combine`/`undo_build_ssa`
  renumber vars afterward, leaving it stale → `&local` can be wrong. **Genuine correctness fix in
  principle, but inert for madc**: at O1 the renumbering passes don't run; at O2 it recovered
  ZERO of our 8 O2-only failures (full-suite measured). Not our O2 bug. Reverted.
- **GVN store/load gated to `>= O3`** (theMackabu, `gvn_modify`). *Disables* GVN memory
  store/load forwarding below O3 (a shim, not a root-cause fix). Inert at O1 (GVN itself is
  `optimize_level >= 2`-gated at the call site). At O2 it recovered **3 of 8** O2-only failures
  (`alias-1`, `991228-1`, `pr79043`) with zero regressions — so GVN mem-forwarding IS part of
  our O2 bug — but O2 with it (1562) is **still below O1 (1565)**, so it does not make O2 viable,
  and it disables a real optimization. Reverted; recorded as a known partial mitigation for the
  O2-viability track.

### ALREADY HAVE
- **Narrow-load sign/zero-extension on GVN forward** (theMackabu) — our fork already carries
  the `add_ext_p`/`ext_code` logic.

### CHEAP ROBUSTNESS (no current-gate effect; low risk)
- `MAX_INSN_RELOAD_MEM_OPS` 2 → 4 (theMackabu) — reload capacity for complex insns/spilling.
- NULL-guards on module teardown (theMackabu `fab2727c0`) — `vars`/`internal` NULL checks in `remove_item`.
- `jump_opt` preserves `lref` labels (theMackabu) — stops unreachable-bb deletion from removing
  labels referenced by label-address data (computed goto / label-as-value). VERIFY whether our
  `jump_opt` already protects `first_lref` labels before adopting.

### FEATURES — defer to the relevant future track (NOT this parity phase)
- **Module lifecycle for long-running JIT hosts**: `MIR_unload_module` / `MIR_remove_module`,
  `_MIR_compact_item_tab`, thunk recycling, deplist tracking. These recur in **three** forks
  (vladich, theMackabu, MysticalUnicat) — the community converging on what an embedded/REPL host
  needs. → Adopt when we build the **mode-4 REPL / libmadc-embedded** tier.
- **atomics** (theMackabu `127a17dc9`+`8c287da6d`) — future Tier-2 (C11 atomics).
- **module caching** (MysticalUnicat `09fbd3faa`) — incremental-compile tier.

### NOT APPLICABLE
- **c2mir preprocessor/embedding** (MysticalUnicat: `,##__VA_ARGS__`, `##` token-paste,
  headers-from-buffers, programmatic messages) — madc has its own lexer/preprocessor; c2mir
  never preprocesses for us.
- **vladich `__asm`** — a MIR-IR mini-assembler parsed in c2mir's *text* statement grammar
  (you write MIR opcodes like `mov r1, 42` in the string). NOT GNU native-asm, so it does NOT
  address our inline-asm floor gap (those torture tests use native x86). c2mir-text-only (we
  bypass it) and ships WIP cruft (global `asm_labels[256]`, a hardcoded `/tmp/...debug.log`).
- **aarch64 / win32 fixes** (theMackabu, vladich) — wrong target; revisit for the future ARM64 track.

## BASIC (cyrilmhansen/mir) — vision data point, NOT a merge

A complete `basic/` BASIC→MIR compiler (runtime, REPL, fixed64 math, dozens of `.bas` games),
built largely via codex. It validates MIR as a real multi-language backend and is excellent
**prior art** for a BASIC→IR semantic mapping (line numbers→labels, `GOSUB`/`RETURN`, string/array
runtime, `DEF FN`, fixed64) plus a ready-made **conformance corpus** (the `.bas` games, the way
SMAUG is for C). BUT it emits MIR *directly* — a **parallel front-end** that bypasses `cir_node`,
which violates madc's one-IR / no-parallel-implementations invariants. The madc-correct path to
compiled BASIC is **a BASIC front-end that produces `cir_node`** (polyglot Phase 3) — net-new
front-end work, informed by this fork, not an import.

## O2-viability experiment (2026-06-02) — DONE, result: O2 still loses to O1

Goal: do the cross-fork codegen fixes make madc-O2 viable? Method: full gcc.c-torture at O2 (via
a `-O2` wrapper passed as `--madc`), before/after, with an O1 zero-regression guard. Results:

| Config | torture pass | notes |
|---|---|---|
| **O1 (shipped)** baseline | **1565** | the parity gate |
| O1 + addr_regs + GVN-gate | **1565** | identical — both patches inert at O1 |
| O2 baseline | 1559 | O2 is 6 *worse* than O1 (confirms "O2 broken") |
| O2 + addr_regs | 1559 | addr_regs recovers nothing at O2 |
| O2 + addr_regs + GVN-gate | **1562** | GVN-gate recovers `alias-1`,`991228-1`,`pr79043`; 0 regr |

**Conclusion:** O2 (best 1562) stays below O1 (1565), so O2 is not viable even patched, and the
only gain is a perf-disabling shim. Neither patch helps the O1 parity gate. Both reverted; fork
stays at pin `4aa628b` (clean). The O2 bug surface is now scoped: **8 O2-only failures**
(`20141107-1`, `920908-1`, `991228-1`, `alias-1`, `pr43236`, `pr79043`, `stdarg-3`, `strct-varg-1`);
GVN mem-forwarding accounts for 3 of them; the remaining **5** (varargs `stdarg-3`/`strct-varg-1`
+ optimizer-PR value-mismatches `20141107-1`/`920908-1`/`pr43236`) need real root-causing.

## Upstream PR #430 adoption (2026-06-11) — computed-goto RA fixes

**ADOPTED** (user-queued review): [vnmakarov/mir#430](https://github.com/vnmakarov/mir/pull/430)
(cyanogilvie, OPEN upstream) — two RA bugs breaking computed goto (`laddr`/`jmpi`) under
MIR-gen at `-O2`/`-O3`:

1. **`insn_descs[MIR_LADDR]` missing `OUT_FLAG`** on the destination operand (`mir.c`) —
   RA treats the laddr dest as a *use*: under pressure it reloads before / skips the spill
   after, and a later `jmpi` jumps through stale spill-slot memory.
2. **`split_edge_if_necessary` assumes direct-branch block exits** — a `jmpi` successor
   edge cannot be split; the full RA overwrote jmpi's *register operand with a label*
   (NDEBUG: an N-way indirect jump silently becomes an unconditional jump). Fix: functions
   containing `MIR_JMPI` use the simplified RA; `busy_used_locs` stays allocated in sync
   with `used_locs` since RA mode now varies per function (`mir-gen.c`).

Verification on the fork: the PR's `jmpi-crash.mir` reducer **SIGSEGV'd under
`MIR_TYPE=gen` at pin `2ffebff`** (interp correct: `1 2 3`); after the fix gen prints
`1 2 3` exit 0. Full MIR `make test` green (Tests 1121, Success tests 2242 — identical
to the pre-patch baseline). Both commits are **verbatim cherry-picks preserving the PR
author** (no `ADOPTED-FROM` source comments — kept byte-identical so a future upstream
merge of #430 dedups cleanly).

Gate relevance: inert at madc's O1 torture gate (`optimize_level < 2` already takes the
simplified RA), but madc ships user-facing `-O2`/`-O3`, the fork carries labels-as-values
(computed goto), and bug 2's silent-misexecution mode is exactly the class the O2-viability
thread (open thread 1 below) must not stack on top of. Watch upstream: if #430 lands and
diverges from these shas, re-sync.

## O2-viability campaign (2026-06-11) — DONE: O2 reaches exact O1 parity

The "8 O2-only failures" surface was re-measured at fork `65b99fc` (composition had
shifted: `20141107-1`/`991228-1` had been fixed by fork evolution; `pr41395-2`/`pr41463`
newly exposed) and then **root-caused to FIVE distinct bugs, all fixed** (fork
`65b99fc` → `cc74fef`, all reproduced on stock `c2m -eg` first — pure c2mir/MIR bugs):

1. **`6c83111` — VA_BLOCK_ARG missing from `fixed_place_insn_p`**: GVN CSE'd two
   identical-looking struct `va_arg` fetches; the replace rewrite corrupted SSA edges
   (op 0 is an input address, not a def) → copy_prop SIGSEGV. Fixed `strct-varg-1`,
   `920908-1`.
2. **`058893f` — lost-copy hazard in out-of-SSA** (`make_conventional_ssa`): the
   same-bb fast path renamed phi uses to the congruence reg, but a SELF-LOOP block's
   back-edge copy lands before the tail branch — the branch read the next iteration's
   value (`while (i-- > 0)` ran one short). Fixed `stdarg-3` (gen), `pr43236`. The old
   "GVN mem-forwarding accounts for alias-1/991228-1/pr79043" theory was wrong — no
   GVN gating shim needed.
3. **`7a90960` — union alias-conflict relation**: c2mir gives union-internal accesses
   the union class 'U…' but pointer-deref accesses the member class; flat id-compare
   `may_alias_p` never aliased them. MIR core gains `MIR_add_alias_conflict` /
   `MIR_alias_conflict_p`; c2mir registers union-class↔member-leaf-class conflicts.
   Fixed `pr41463`, `pr41395-2`.
4. **`bcdd0c5` — honor `optimize("-fno-strict-aliasing")`**: per-function TBAA
   suppression (alias class 0), surviving MIR inlining. Fixed `alias-1`, `pr79043`
   under c2m. (madc-side: the lexer attribute allowlist gains `optimize`, the parser
   records the pending flag in `consume_gnu_attributes` + applies it at the function
   parse, and the CIR builder forwards an `N_ATTR` in the FUNC_DEF specs.)
5. **`cc74fef` — ff_call XMM slot over-counting** (bonus, found inside stdarg-3's
   interp leg): `_MIR_get_ff_call` advanced `n_xregs` for INTEGER block eightbytes
   (Win64 copy-paste), so each mixed `{double,long}` vararg struct after the first
   landed its SSE half one XMM too high through the interpreter FFI. Fixed `stdarg-3`
   under `c2m -ei`.

**Result: torture at `-O2` = 1567 = torture at `-O1`, failsets byte-identical** (was
1559/8-worse). O1 failset unchanged (byte-identical), fulltest 572/0/0/18, MIR
`make test` identical baseline (1121/2242), SMAUG soak green. madc `-O2`/`-O3` are no
longer behind a known-broken gate; whether O2 *beats* O1 in code quality/perf is a
separate (open) question.

## Attribution / provenance policy (re-affirmed 2026-06-11)

Every adopted patch must trace back to its origin. The mechanisms, in layers:
1. **Git author metadata** — upstream PRs are cherry-picked preserving the original
   author (`git log --format='%an'` shows e.g. `Cyan Ogilvie` on all five PR commits;
   committer is us). Messages are kept verbatim so the patch-id matches upstream for
   future merge dedup.
2. **Git notes** (`refs/notes/commits`, pushed to the fork) — each adopted commit
   carries an `ADOPTED-FROM: upstream PR vnmakarov/mir#NNN (author)` note, added
   without rewriting hashes (`git log --notes` shows them).
3. **Source comments** — hand-applied (non-cherry-pick) adoptions carry an
   `ADOPTED-FROM: <fork> @ <sha>` comment at the fix site (the theMackabu pattern).
4. **This document** — the narrative record mapping fork shas ↔ upstream PRs/issues.

Context worth knowing: cyanogilvie's burst of high-quality MIR fixes (#430–#434) comes
from building a MIR backend for slimcc —
https://github.com/cyanogilvie/slimcc/tree/mir-backend — another C-frontend-on-MIR
project whose pipeline stresses the same generator paths madc does. **Watch that tree
and their MIR fork for further fixes.**

## Upstream activity sweep + PR #432/#433/#434 adoption (2026-06-11, round 2)

A full sweep of recent upstream `vnmakarov/mir` activity (user-requested) found three
fresh fix-PRs by cyanogilvie (same author as #430) for the three open issues we were
about to fix ourselves — all three **ADOPTED as cherry-picks** (fork `cc74fef` →
`9ab36fb`, pin bumped same-commit):

- **PR #432 → issue #423** (`3de6283`): GVN store-forwarding to a *narrower typed
  reload* forwarded the raw 64-bit stored register, losing the load's sign/zero
  extension (32-bit ops leave upper halves undefined in MIR). Reproduced on the fork
  (gen O2/O3 returned 4294967023 for -273) and root-caused independently before the
  sweep found the PR — the PR's fix materializes the forward temp with the matching
  EXT insn. Verified: the C repro and `c-tests/mir/issue423.mir` both pass.
- **PR #433 → issue #424** (`290024c`): jump_opt could free labels referenced only by
  `laddr` insns / lref data; `gen_setup_lrefs` then read freed insns. Comment-only
  conflict with our existing theMackabu lref-preservation loop (the fork already
  carried HALF of this fix — that's why a valgrind pre/post control showed our fork
  was NOT behaviorally exposed by the issue's C repro). The new `MIR_LADDR` scan is
  defensive completion for direct-MIR producers emitting laddr without lref data.
  Note: `c-tests/mir/issue424.mir` passes via the suite runner but cannot LOAD via
  the m2b → mir-bin-run binary round-trip — that is upstream **issue #426** (label
  refs break `MIR_read`), pre-existing and unchanged on our fork.
- **PR #434 → issue #431** (`9ab36fb`): aarch64 `(x+15) % 16` where round-up-to-16 was
  intended, in `va_arg_builtin` + `_MIR_get_ff_call` (long double past the 8
  v-registers crashed / corrupted stack args). Untestable on this x86-64 box;
  mechanically obvious, serves the ARM64 track (Track 6.1). Its `va-ld-stack.c` test
  passes on x86-64 too.

Also checked from the sweep: **PR #420** (vararg RET error-path use-after-NULL) and
**PR #418** (`MIR_NO_GEN_DEBUG` guards) — **both already carried** by the fork via the
2026-06-02 theMackabu backport (`23fd2678f` adoptions). Remaining upstream items noted
for the future, none urgent for us: **#426** (lref vs `MIR_read` — would matter if madc
ever ships computed-goto code through saved .bmir; in-process JIT unaffected), **#410**
(`try_spilled_reg_mem` error at O1 — could not reproduce locally, no reducer in issue),
**#429** (ARM64 by-value struct >16B — ARM64 track), **#394** (`_Thread_local` feature
gap), **#411** (c2mir memory usage on sqlite3 — perf).

Gates (round 2): MIR `make test` exit 0 with the three new regression tests counted in
(1124/2248 interp, 1128/2256 gen), fulltest 572/0/0/18, torture O1 failset
byte-identical, **O2 still = O1 byte-identical** (parity preserved), SMAUG soak green.

## Upstream activity sweep + PR #437/#438/#439/#440 adoption (2026-06-13, round 3)

cyanogilvie opened a burst of PRs against frozen upstream on 2026-06-11/12; four
adopted onto fork `develop` (pin bumped `545ad46` → `5df536f`).

- **#438 — x86-64 va_start offsets for functions with block args. ADOPTED — genuine
  live bug in OUR generator.** va_start re-derived the named args' reg/stack layout
  with its own scan that disagreed with the prologue walk in 3 ways (reg-passed BLK
  args uncounted into gp/fp_offset; every block added to the overflow displacement
  without 8-byte rounding; fp exhaustion checked `gp_offset >= 176`). A struct param
  before `...` made `va_arg` read the named struct's register-save slot as a vararg.
  Verified via the 3-way oracle: reducer `tmp/w13_vastruct.mad` returned `60/3.0/3018`
  on `MIR_gen` (and stock `c2m -eg`) vs gcc/`-ei` `330/3.8/3018`; fixed after adoption.
  Conflicted with our existing `VA_BLOCK_ARG` named-arg scan (resolved: the PR's
  prologue-totals approach SUPERSEDES our hand-rolled scan — fewer moving parts).
  Pinned by `tests/testvastruct.mad`. ALSO fixed **`pr117432.c`** in gcc-torture
  (varargs `long long`/`int` tag) — failset 52 → 51, a strict improvement.
- **#437 — aarch64 bb-thunk x9 clobber + contiguous code-holder reservation.
  ADOPTED with an x86-64 refinement.** The aarch64 x9-thunk fix is inert on x86-64.
  The shared `mir.c` 128MB code-holder reservation regressed `20040811-1.c` (a
  leaky-VLA torture case — see the madc VLA-backward-goto gap) into an OOM SIGSEGV on
  a memory-pressured host: the reservation subtracts 128MB of commit headroom from the
  jitted program's own ~2GB-peak heap. **Refinement (fork commit `5df536f`, proposed
  back upstream):** skip the reservation on x86-64 — its `rel32` direct branches reach
  ±2GB, so scattered code holders stay in range without it (the reservation only helps
  reach-limited targets like aarch64 ±128MB). Other arches keep it unchanged.
- **#439 — C23 paramless variadic (`int f(...)`). ADOPTED.** Front-end check removal
  only; the backend already supported it (register save areas key on `vararg_p`, not
  `nargs`). No current madc surface needs it; on the C23 arc, trivially safe.
- **#440 — block-arg copy clobbering caller-saved regs (aarch64/riscv64/s390x/ppc64).
  ADOPTED, inert on x86-64.** c2mir lowers big aggregates itself and never emits >2
  qword block call args, so only direct MIR-API users on those arches hit it; kept for
  future targets.

Not adopted this round: **#435** (musl/Alpine test-harness — not our build),
**#405/#383/#341/etc.** (embedding-API / older, not relevant). The remaining
non-adopted older PRs from the 2026-06-11 sweep are unchanged.

Gates (round 3): fork `make c2mir-test` exit 0 (incl. bootstrap lazy-bb); madc
fulltest 556/27 (failure list byte-identical, +`testvastruct`), unit 10/10 binaries
green, gcc-torture **1571 passed, failset 51 names** (`pr117432.c` fixed by #438,
ZERO regressions; `20040811-1.c` restored by the x86-64 reservation arch-gate),
SMAUG `--project` soak green. Baseline `docs/parity/torture-failset-current.txt`
updated 52 → 51.

## Open threads (the actually-valuable derived work)

1. ~~**Make madc-O2 viable**~~ **DONE 2026-06-11** (see the O2-viability campaign above):
   all 8 O2-only failures root-caused to 5 real bugs and fixed; O2 = O1 = 1567 with
   byte-identical failsets. Remaining: evaluate whether O2 actually *helps* (perf), and
   consider flipping madc's default `madc_opt_level` 1 → 2 after a soak period.
2. **REPL/embedded module lifecycle** — adopt the converged unload/recycle/compact APIs (recur in
   3 forks) when the mode-4 REPL / libmadc-embedded tier lands.
3. **BASIC front-end** — polyglot Phase 3; cyrilmhansen as prior art + a `.bas` conformance corpus.
4. **Future ARM64** — revisit the aarch64 fixes (theMackabu/vladich) when that target opens.
