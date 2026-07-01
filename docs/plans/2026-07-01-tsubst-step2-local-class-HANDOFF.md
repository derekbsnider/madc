# tsubst burndown step-2 — local-class-in-tsubst (`_M_construct`'s `_Guard`)

**Status:** scoped + RED test landed; core materialization NOT yet implemented.
**Goal:** convert `basic_string::_M_construct<_InIterator>` (and the reduced
`Box::build<U>`) from a re-parse FALLBACK to a parse-once HIT. This is the
single highest-frequency masked fallback (every string-using test) and the
biggest real burndown lever — see
`docs/plans/2026-07-01-templateid-gate-insight-HANDOFF.md` §5.2 and
`docs/plans/2026-07-01-tsubst-kind3-wip-verification-FINDINGS.md`.

## RED test (landed)
`tests/testlocalclassraii.mad` — a `<`-free member template with a local RAII
class (`Guard`: user ctor + non-empty dtor + a pointer member). Isolates the
local-class wall WITHOUT the `<`-gate entanglement (its body has no template-id,
so it passes `tsubst_eligible`). Today:
- flag-off: PASS (prints 42).
- `MADC_XTEST_DEP_PARSE=1 bin/madc --show-stats`: **0 hit / 1 fallback**,
  `[why: tsubst body calls un-emittable symbol]` (the un-materialized
  `Guard::Guard` / `~Guard`). Runs correctly flag-on only because it falls back
  to re-parse.
**DONE for step-2 = `1 hit / 0 fallback` AND prints 42 flag-on**, plus the
`_M_construct` fallback gone from testset/testmap/testsubscript's `[why:]`.

## The two independent capabilities `_M_construct` needs

### Piece 2A — local-class ctor + non-empty dtor materialization (THE core)
`subst_datadef` (cir_builder.cpp:774) materializes a dependent local struct via
`materialize_local_aggregate_datadef` (704) → `clone_local_aggregate_members`
(620), gated by `tsubst_local_class_clone_supported` (602). That gate REJECTS any
local class with a user ctor (`cdd->has_user_ctor`, line 612), non-empty methods,
or a non-empty dtor (only `tsubst_local_class_has_only_empty_dtor`, 589, is
allowed). `_Guard` has BOTH a user ctor and a non-empty RAII dtor → rejected →
`subst_datadef` returns the un-substituted dependent `Guard` → the body's
`Guard g(this)` ctor call + scope-exit `~Guard()` reference un-emitted symbols →
completeness check bails.

**What must be built:** extend the clone to also clone the ctor + dtor (and any
methods), and INSTANTIATE their bodies with the same `subst` map (T→concrete),
registering the instantiated FuncDefs so `emit_class_struct_with_deps`
(cir_builder.cpp:5987 — local classes already route through the class/ctor/vtable
emit path) emits them. Key facts:
- The clone already substitutes MEMBER types (`clone_local_aggregate_members`);
  extend it to clone `src->ctors` / `src->methods` / the dtor and their bodies.
- **No re-parse shortcut.** flag-off "works" only because it RE-PARSES `build`'s
  body (the path the campaign deletes). tsubst must materialize `Guard` from the
  Tree-1 pattern on the parse-once spine — that is the whole point (parse-once.md).
- The ctor/dtor bodies are NOT templates; when `Box<int>::build<int>` instantiates,
  T is fixed, so `Guard`'s `Box*` → `Box<int>*` and `b->reset()` → `Box<int>::reset`.
  Reuse the body-instantiation the main tsubst body already uses (the copy +
  `subst_datadef` walk), applied to the ctor/dtor TokenFuncs.
- **Scope-exit dtor injection** for the materialized local class must fire on the
  tsubst'd body's normal + early-exit paths (the existing dtor-injection machinery).

### Piece 2B — narrow `<`-gate admission (needed for the REAL `_M_construct`)
The reducer avoids this, but `_M_construct`'s body has `static_cast<size_type>`
(basic_string.tcc:225) → `tsubst_eligible` (parser.cpp:33785) rejects ANY `tkLT`
→ `[why: template-id '<' in body]`. Admitting it needs a NARROW classifier:
admit a `<..>` span that is a cast / holds no dependent template-arg; reject
dependent template-ids. **CAUTION (proven, do not repeat):** the WIDE admit-all
experiment broke the compile via `__and_ : conditional<...>::type` dependent-base
resolution (templateid-gate-insight §4b). The `pnames`-only check is NOT
sufficient (`conditional<_B1,...>`'s params aren't in `_M_construct`'s pnames yet
are dependent). Do 2A first (metric moves via the reducer without touching the
gate); tackle 2B with the dependent-`<` classifier only after 2A is solid.

## Guardrails (learned this session — non-negotiable)
- **NEVER harden the fallback net by rolling back the emitted program**
  (`m_prog->ast` / `m_output_externs` / `deferred_lazy_bodies`) — a fallback of an
  OUTER body drops legitimate NESTED hits → runtime setjmp SIGSEGV. HEAD rolls back
  only the ODR-use `referenced_funcs` set (safe). This is what sank the reverted
  Kind-3 WIP.
- **Never mask a bad hit with a bail/guard.** If a materialized local class
  miscompiles, fix the materialization; do not add a screen that demotes it (that
  regresses the burndown AND can hide a crash — see the FINDINGS doc).
- **Verify by RUNNING flag-on, not just compiling.** The `_Guard`/`_M_realloc`
  crash class is happy-path-clean; run the tests (the new `scripts/tsubst_flagon_gate.sh`
  does this). Every increment: build 0-warn, `make fulltest` (incl. the tsubst
  flag-on gate), gcc-torture c17 byte-identical to the 51-name baseline, and the
  reducer HITS + prints 42 flag-on. Bump `docs/parity/tsubst-flagon-baseline.txt`
  on the resulting hit gains.

## First concrete sub-slice
Make `tests/testlocalclassraii.mad` HIT: in `tsubst_local_class_clone_supported`
stop rejecting user-ctor/non-empty-dtor when the ctor/dtor bodies are
instantiable; teach `clone_local_aggregate_members` to clone `ctors`/dtor and
instantiate their bodies with `subst`; confirm the emit path (5987) picks them up.
Gate green + reducer HIT before touching `_M_construct` (2B).
