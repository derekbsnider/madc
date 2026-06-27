# Two-Tree End-State & Re-Parse Deprecation — Plan of Action

**Date:** 2026-06-27 · **Branch:** `feature/front-end-performance-claude`
This is the strategic umbrella. The deep detail for Phase 1 lives in
`2026-06-27-tsubst-construction-deferral-PLAN.md`. Keep this page to one screen.

---

## The end-state (the two trees)

- **Tree-1 — the static forest.** Parsed ONCE (user code + the full header closure),
  immutable, **templates retained in un-expanded form** (placeholder-typed patterns). The single
  parsed source of truth.
- **Tree-2 — the dynamic tree.** Built per-compile by **copy + substitute** from Tree-1
  (templates instantiated by binding concrete types), lowered, fed to c2mir. **Never re-parsed.**

`cir_node` is the node type for both (MC11-IR, set in stone); "two trees" = one immutable recipe
forest + one materialized output. Consistent with the MC11-IR rule.

---

## The single hard dependency (and why this need not take a month)

Re-parse (re-running `parseFunction` on a template body per instantiation) can be deleted only when
the **substitute** covers every instantiation the **target corpus** needs (tests + SMAUG + the
self-host source). That completeness is the one deep pole. It stays **bounded** iff it is built as a
**small set of GENERIC capabilities — never a per-shape catalog.** The `--show-stats [why:]`
fallback profile is the finite, measurable burndown: a generic capability collapses a whole *class*
of `[why:]` entries at once; a per-shape fix collapses one. **If a slice is keyed on a
template/callee NAME, it is wrong** (Rule #7) and it is how this turns into a month.

---

## Critical path (no detours)

### Phase 1 — Complete the GENERIC substitute  *(the only deep work)*
Each capability is one gated slice and is generic by construction:
1. **Completeness-fallback** — any un-emittable/un-resolvable symbol ref in a substituted body →
   clean fallback (not a broken compile). Makes every later slice incrementally safe.
2. **Construction deferral** — defer dependent-arg object constructions and re-lower them with
   concrete types at substitute time (covers ALL constructions; fixes the `class_ctor_call ⇄
   object_arg_addr` recursion at root).
3. **ADL operator re-resolve** — route operators (`__gnu_cxx::operator-`, …) through the one generic
   resolver `resolve_copied_dependent_call` (covers ALL operators).
4. **Dependent member-type resolution** — `rebind`/typename, a `make_typename_type`→`lookup_member`
   analogue in `subst_datadef` (covers ALL dependent member-types).
   *(+ local-class-in-body and multi-pack ONLY if the corpus `[why:]` still lists them — do not
   pre-build them.)*
**DONE gate:** flag-on `[why:]` profile EMPTY across the test corpus + SMAUG + self-host, AND
flag-on output == flag-off output (the env-gated byte-identical harness).

### Phase 2 — Delete re-parse
Flip tsubst to default (drop the `MADC_XTEST_DEP_PARSE` gate), **delete the re-parse-at-instantiation
path**, and replace it with a **LOUD hard error** if an uncovered construct ever appears (coverage
gaps must be loud, never silently slow). **Gate:** full suite + torture + SMAUG green with re-parse
removed.

### Phase 3 — Snapshot the forest  *(the compile-speed payoff)*
Serialize Tree-1 (the parsed header closure + the un-expanded template patterns) → **parse once,
LOAD thereafter** — this kills the ~78%-of-wall header-parse cost (the real perf lever). Clean only
*after* Phase 2: a loaded forest has no live parser to re-parse with. **Gate:** cold-compile ≈ load
time; byte-identical output.

---

## Why this stays on track
- **Generic-only Phase-1 slices.** The `[why:]` list is the burndown chart; Phase 1 is finished when
  it hits zero **for the corpus** — a measurable finish line, not "cover every template ever."
- **Env-gate safety throughout.** flag-off stays byte-identical; per-slice 670/670 + torture; never
  perf-gate a correctness slice.
- **Ordering is fixed by dependency:** Phase 3 (forest/perf) sits behind Phase 2 (delete re-parse),
  because a loaded forest cannot re-parse. The only alternative — forest-with-retained-token-reparse
  — is messier and keeps a parser alive; take it ONLY if the perf win is needed before Phase 1/2
  land. Default: the clean order above.

## Status
Phase 1 in progress: completeness-fallback + construction deferral are scoped + the recursion
root-caused (`tsubst-construction-deferral-PLAN.md`, Slices A–D); system-header free-call re-resolve
already landed (`1d69ee40`) and is the model for the ADL slice. Phases 2–3 not started.
