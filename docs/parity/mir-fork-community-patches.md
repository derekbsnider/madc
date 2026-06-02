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

## Open threads (the actually-valuable derived work)

1. **Make madc-O2 viable** (orthogonal to the O1 parity gate; codegen-quality, not promotion).
   Root-cause the GVN mem-forwarding bug (deepest fix, vs the ≥O3 shim) + the 5 remaining O2-only
   failures above. Only worth it once O2 can actually beat O1. Not the current priority.
2. **REPL/embedded module lifecycle** — adopt the converged unload/recycle/compact APIs (recur in
   3 forks) when the mode-4 REPL / libmadc-embedded tier lands.
3. **BASIC front-end** — polyglot Phase 3; cyrilmhansen as prior art + a `.bas` conformance corpus.
4. **Future ARM64** — revisit the aarch64 fixes (theMackabu/vladich) when that target opens.
