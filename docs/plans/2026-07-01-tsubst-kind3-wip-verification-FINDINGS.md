# tsubst Kind-3 WIP — verification findings & disposition (2026-07-01)

**Author:** Claude (coordinator), verifying Codex's uncommitted tsubst WIP.
**Verdict:** the WIP is a **net negative** and was **reverted** to clean HEAD
(`114d81b8`, v0.32.0). Codex's diff is preserved at
`tmp/codex-tsubst-kind3-wip-BACKUP.patch`. This doc records exactly why, so the
next tsubst attempt does not repeat it, and pins the real root cause + correct
sequencing.

## What Codex submitted (against the `2026-07-01-templateid-gate-insight-HANDOFF.md`
§5 retargeting: step 0 = Kind-3 dependent member-type resolution, step 1 = harden
the fallback net). Four logical changes, uncommitted in `cir_builder.cpp` + `parser.cpp`:

| # | Change | Site | Verdict |
|---|--------|------|---------|
| A | dependent member-type resolver `resolve_substituted_dependent_type` | `subst_datadef`, cir_builder | **INERT** — 0 fallbacks→hits (identical hit counts with it neutered); + parse-once smell (hand-rolls a type-spelling → re-tokenize → `resolve_typename_type_token`). |
| B | `tsubst_pattern_has_try` → `bail("try/catch in body")` | `tsubst_method_body`, cir_builder | **BAND-AID** concealing C's crash. The −9pt burndown regression. |
| C | expanded `bail_restore` (also restores `m_prog->ast`, `m_output_externs`, `deferred_lazy_bodies`, `pending_funcs`) | `tsubst_method_body`, cir_builder | **THE DEFECT** — crashes map/set at runtime (see below). |
| D | `build_dependent_pattern` restore-centralization + leaked `__pat` lazy-body purge | parser | Not the crash cause; unneeded now, unvalidated. |

## The measured facts (all reproducible)

Gates Codex reported (all TRUE): build clean, flag-off `fulltest` **673/0/0/16**,
census 0, drift gates green, gcc-torture c17 **byte-identical to the 51-name
baseline** (I re-ran all of these clean; production is genuinely byte-identical —
the tsubst path is `MADC_XTEST_DEP_PARSE`-gated).

What Codex's closeout **omitted**: the suite-wide burndown, which **regressed**:

| | HEAD baseline | Codex WIP | Δ |
|---|---|---|---|
| tsubst HIT | 175 | 152 | **−23** |
| tsubst FALLBACK | 90 | 113 | **+23** |
| HIT rate | 66% | 57% | **−9 pts** |

The entire delta is a new `[why: try/catch in body]` class (16 fallbacks). Those
16 are just **two** libstdc++ bodies replicated across tests:
`std::_Rb_tree::_M_construct_node<_Args...>` and
`std::vector::_M_realloc_insert<_Args...>` — both exception-safety
`__try { construct } __catch { destroy; deallocate; rethrow }` blocks.

**At HEAD these two bodies are correct tsubst HITS** — verified by `--emit=c11`:
the try/catch lowers to a complete, faithful SJLJ block
(`setjmp(__madc_try_push(...))` → happy path + `__madc_try_pop()`; else →
destroy + deallocate + `__madc_rethrow()`). All container tests pass at HEAD, and
v0.32.0 shipped them this way. So Codex's stated rationale ("fixes the *unsafe*
`_M_construct_node` hit") **does not hold** — they were correct, shipped hits.

## Root cause of the crash the guard concealed (bisected)

Remove guard B and run flag-on: `testset`/`testmap` **SIGSEGV** at runtime
(`rc=139`) in JIT'd code — `__sigsetjmp` inside the tsubst'd `_M_construct_node`,
reached via the `pair<const string, …>` piecewise ctor. `testcontainerdtor` /
`testsubscript` / `testmadc_ns` (which also hit `_M_construct_node`) do **not**
crash. Bisection (each an independent build):

- Neuter resolver A → still crashes ⇒ **A is not the cause** (and A is inert).
- Revert parser D → still crashes ⇒ **D is not the cause**.
- Revert rollback expansion C → **crash gone, hit counts back to HEAD** ⇒ **C is
  the sole cause.**

**Why C crashes:** `bail_restore` runs when a body **falls back**. Codex made it
wholesale-restore the *emitted program* (`m_prog->ast`, `m_output_externs`,
`deferred_lazy_bodies`). But a **nested** body (e.g. `_M_construct_node`) that was
legitimately emitted as a **HIT** *during* the instantiation of an outer body that
later bails gets **rolled back too** — its definition/extern is dropped. The
map/set code still calls it → the JIT call lands on a stale/garbage address →
`setjmp` on garbage → SIGSEGV. HEAD's `bail_restore` only rolled back
`referenced_funcs` (an ODR-use set — over-inclusion is harmless), which is why
HEAD is safe. This matches the known SJLJ-locals failure class
(`[[project_mir_setjmp_returns_twice]]`).

So the WIP's causal chain is: **C introduces a latent crash → B hides it by never
tsubst-ing try/catch bodies → the metric regresses −9pts, and A (the headline
feature) does nothing.**

## Disposition

Reverted the whole WIP. Net progress = 0 hits, and it shipped a concealed crash;
HEAD is correct and greener on the metric. Nothing salvageable was committable:
A is inert dead weight, B/C are the bug, D is unneeded. Backup patch kept for
reference / future mining of the resolver machinery.

## Correct sequencing for the next attempt (supersedes the optimistic §5 order)

1. **Do NOT harden the fallback net by rolling back the emitted program.** The
   §5.1 "bonus finding" (an over-admitted body escaping to break the compile) is a
   **WIDE-eligibility** problem — it does not exist under today's eligibility. If/when
   widening starts, reject *before emitting* (extend the completeness check to gate
   admission on a cir error), do **not** wholesale-restore `ast`/`externs` after the
   fact — that drops legitimate nested hits (this is exactly C's flaw).
2. **The real lever is step 2 — local-class-in-tsubst (`basic_string::_M_construct`'s
   `_Guard`)** — the single highest-frequency masked body (every string test). That
   converts fallbacks to *hits* (moves the metric the right way), unlike gate
   refinement / resolver work which the data shows is inert. Start there.
3. The dependent member-type capability (Kind 3) is real and still needed for the
   `_Rb_tree` rebind family (step 3), but Codex's string-re-tokenize approach was
   inert here — a structural `make_typename_type`→`lookup_member` in `subst_datadef`
   is the g++-faithful form. Mine the backup patch's machinery only if it earns hits.
4. **A regression like this must not pass green fulltest again.** The burndown is a
   tracker, not a gate — see the flag-on tsubst gate added alongside this doc.
