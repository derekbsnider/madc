# madc Template Instantiation — Parse Once, Resolve Generically (the g++ model)

**Date:** 2026-06-27 · **Branch:** `feature/front-end-performance-claude`
The single strategic plan. One screen. Supersedes the per-shape "wall-stack / catalog" framing —
that framing is what exploded Phase 4 into dozens of pieces. We do it the g++ way instead.

---

## The architecture (what "work like g++" means, concretely)

g++ parses each template **once** into a saved tree that keeps dependent expressions in
**un-resolved** form; at instantiation, `tsubst` walks that tree **re-running the same `finish_*`
resolution entry points that parsing uses**, on the substituted types. It never re-lexes and never
re-parses — for *all* of C++. madc adopts exactly this:

- **Tree-1 (static):** each template parsed once → immutable pattern retaining the dependent form.
- **Tree-2 (dynamic):** instantiate = **re-run the one generic resolver** over the pattern with
  concrete type bindings → lowered cir → c2mir. No re-parse.

---

## The finish line is FINITE — a fixed, small set of generic resolution KINDS (never shapes)

A template body only ever needs re-resolution of a fixed set of *kinds*. madc needs the re-resolve
analogue of each, implemented **ONCE** — each mirrors a g++ entry point. This is the entire job:

| Kind | madc generic mechanism | g++ analogue | status |
|------|------------------------|--------------|--------|
| 1. function call | `resolve_copied_dependent_call` | `finish_call_expr` | ✅ landed `1d69ee40` |
| 2. operator (incl. ADL) | route operators through the same resolver | `build_new_op`/`add_operator_candidates` | TODO |
| 3. dependent member-type / `typename` | `subst_datadef` resolves `Alloc::rebind<X>::other` once scope is concrete | `make_typename_type`→`lookup_member` | TODO |
| 4. object construction | one generic ctor re-resolution (defer in pattern, re-resolve on concrete types) | `tsubst` ctor → `finish_call_expr` | TODO (recursion root-caused) |
| router | dependence predicate selecting defer-vs-resolve | `type_dependent_expression_p` | started (`is_type_dependent`) |

When kinds 2–4 + the router land, **every template flows through them.** There is no per-shape list,
no "cover allocator then string then iterator." That collapses Phase 4 from dozens of pieces to four.

---

## Done = re-parse deleted (g++ parity)

The `--show-stats [why:]` fallback profile (landed `6570a849`) is the **burndown chart**. When it is
**empty** across the corpus (tests + SMAUG + self-host): make tsubst the default (drop the
`MADC_XTEST_DEP_PARSE` gate), **delete the re-parse-at-instantiation path**, and replace it with a
**LOUD hard error** on any uncovered construct (gaps must be loud, never silently slow).
**Gate:** full suite + torture + SMAUG green with re-parse removed.

---

## The discipline (this is the whole game — it is what keeps it finite)

- **Every slice implements a generic resolution KIND. If a slice is keyed on a template/callee
  NAME, it is wrong** (Rule #7) — that name-keying *is* the catalog that exploded. No exceptions.
  Existing name-ish eligibility entries (`tsubst_has_placement_new_ctor_pack_expansion`, …) are debt
  to **fold into the generic predicate**, never to extend.
- **`[why:]` is the burndown, not a to-do list of shapes.** A landed kind clears a whole class at once.
- **Env-gate keeps flag-off byte-identical** throughout; per-slice 670/670 + torture; never perf-gate.

---

## The one honest cost (not hidden)

madc **resolves during parsing**; g++ **parses, then resolves**. So madc's saved pattern carries
placeholder-typed resolution baked in, and re-resolving it is genuinely more friction than walking
g++'s purpose-built dependent AST. That friction *is* the work in kinds 2–4. It is **finite —
bounded by the four kinds, not open-ended** — focused capability work, not a sprawl of shapes.
Hold the discipline and it converges; break it (one per-shape entry) and it explodes again.

---

## Sequence (dependency-ordered, not a phase tree)

1. **Router + completeness-fallback first** — the dependence predicate routes defer-vs-resolve, and
   any not-yet-resolvable ref degrades to a *clean fallback* (not a broken compile). Makes every
   later kind landable incrementally.
2. **Kind 4 (construction)** — root-caused; the recursion is pattern-mode eager construction of a
   dependent arg. Defer in pattern, re-resolve on concrete types.
3. **Kinds 2 & 3 (operator/ADL, member-type/rebind)** — extend the same spine.
Each kind: its own gated commit; `[why:]` shrinks by a class each time.

## References (subordinate to this page)
- **Implementation detail** (machinery map at current file:line, gcc citations, the construction
  root-cause + reducers): `2026-06-27-tsubst-construction-deferral-PLAN.md` — read as "kind 4 + the
  shared re-resolve machinery", NOT a catalog.
- **Forest** (parse-once for the HEADER closure — the compile-speed perf track, ~1.9 s/compile):
  `2026-06-22-embedded-header-forest-execution-plan.md`. **Separate and uncoupled** — prereqs are
  headers-parse-cleanly + typeids, not this. Lands on its own schedule.

---

## Deprecating re-parse: how we get there + how we track it (2026-06-28)

**The law** is now codified: `.claude/rules/parse-once.md` (+ reasoning in
`docs/rules/parse-once.md`). New C++ support resolves on the parse-once generic
spine; re-parse is a transitional fallback slated for deletion. Every change
moves the suite-wide `[why:]` fallback count down or flat, never up.

### The tracking metric — `scripts/tsubst_burndown.sh`
Runs the whole suite under `MADC_XTEST_DEP_PARSE=1 --show-stats` and aggregates
suite-wide tsubst HIT vs FALLBACK + the distinct `[why:]` reason-classes. The
FALLBACK total is the distance-to-goal; the `[why:]` tally is the ranked KIND
worklist. 0 fallback → flip the default → delete re-parse.

### Baseline (HEAD `fef9b3a0`, 2026-06-28)
```
total HIT               : 161
total FALLBACK (reparse): 104
HIT rate                : 60%
```
KIND worklist, ranked (the data REORDERS priorities — aim by frequency):
```
 70  [why: template-id '<' in body]            <-- DOMINANT (67% of all fallbacks)
  7  [why: tsubst body calls un-emittable symbol]
  6  [why: non-type template param]
  6  [why: >1 pack param]
  3  [why: unresolved dependent member call]   <-- Kind 3 (allocator_traits::destroy handoff)
  5  [why: no matching constructor for _Rb_tree...]  (3+1+1 — Kind 4 construction)
  1  [why: reference-param value-read]
  1  [why: recipe parse failed]
```

### What the data says
- **`template-id '<' in body` (70×) is the single biggest lever** — knocking out
  that one KIND drops fallback 104 → ~34 (hit rate 60% → ~83%). It is a
  `tsubst_eligible` gate rejection that fans into sub-cases: concrete class-param
  id (safe to admit), `static_cast<>` (safe), and the genuine wall —
  method-param dependent template-id + forwarding-pack in a `::`-qualified static
  call (`_Alloc_traits::construct(std::forward<_Args>...)`). Refining the gate
  admits the safe cases; the wall needs the dependent member-type/rebind KIND.
- The queued **Kind-3 `allocator_traits::destroy` handoff** is valid and finishes
  `testvector`, but is a small lever (≈3×). After it lands, the strategic next
  target is the **template-id-in-body class**, not more small KINDs.
- Construction (`_Rb_tree ... no matching constructor`, ≈5×) is Kind 4.

### Sequence toward burndown = 0
1. Kind 3 dependent-member-call (queued — finishes vector). 
2. **template-id-in-body class** (the 70× lever — gate refinement + dependent
   member-type/rebind KIND). Biggest single drop.
3. construction KIND (`_Rb_tree` ctor selection, non-type params, packs).
4. mop-up (`un-emittable symbol` completeness, `reference-param value-read`,
   `recipe parse failed`).
Re-run `scripts/tsubst_burndown.sh` after each to confirm monotonic descent.
