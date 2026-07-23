# Parse-Once — No Re-Parse for New Support (reasoning)

## The law

New C++ support is built on the parse-once generic spine. Re-parse is not a
design target — it is a transitional fallback slated for deletion. This is the
reasoning behind `.claude/rules/parse-once.md`.

## Why g++'s model is the only acceptable one

A real C++ compiler parses a template **once** into an AST. To instantiate it
for concrete arguments, it runs *template substitution* (`tsubst`): it re-walks
the saved tree, substitutes the template parameters, and re-runs the same
semantic-analysis entry points (`finish_call_expr`, overload resolution, ADL,
member-type rebinding, object construction) on the substituted forms. It never
goes back to the source text and re-lexes/re-parses it.

madc's two-tree design mirrors this: Tree-1 is the immutable parsed pattern
(with placeholder template params); Tree-2 is the per-instantiation materialized
`cir_node` tree fed to c2mir. Instantiation = re-running the generic resolver
over Tree-1 on substituted args. (See `mc11-ir.md`: the `cir_node` tree is the
single IR, carrying both the lowered view and the originating tokens/parse
subtree — instantiation reads the saved structure, it does not re-derive it.)

## Why re-parse is a dead end (not just "slower")

- **It is not how compilers work.** No production C++ front end re-parses to
  instantiate. Building on re-parse builds a thing that cannot reach parity.
- **Cost.** Re-parsing the same template body N times is repeated lexing +
  parsing + sema; it shows up directly in the front-end-performance work
  (re-parse on macro-heavy bodies is a measured O(n²)-class cost).
- **Divergence risk.** A re-parsed instantiation and a generically-substituted
  one are two code paths that can disagree — exactly the parallel-implementation
  hazard `no-parallel-implementations.md` exists to prevent.

## How we get there (the deprecation roadmap)

The finish line is **finite** — not a per-shape catalog (that catalog is what
exploded the old Phase 4). It is the set of generic resolution KINDs plus the
dependence router:

1. **Call** — a dependent call re-resolves on substituted arg types
   (`resolve_copied_dependent_call`, free-function branch). [landed]
2. **Operator / ADL** — free/member operators re-resolve and instantiate on the
   concrete operand types (incl. scalar-returning iterator operators). [landed
   for the scalar/iterator family]
3. **Dependent member-type / rebind** — `typename T::x`, `rebind<>`, nested
   member calls re-resolve and instantiate their bodies.
4. **Object construction** — constructor selection + materialization on
   substituted types.
- plus the **dependence router** (is-this-still-dependent) that decides defer
  vs resolve, exactly like g++'s `type_dependent_expression_p`.

Each KIND, once complete, makes a class of bodies stop bailing and start
hitting. When all KINDs are covered, no body in the suite bails. Then:
flip `MADC_XTEST_DEP_PARSE` to default-on → re-parse is unreachable dead code →
delete it. That is g++ parse-once parity.

**Status (2026-07-03): parse-once tsubst is UNCONDITIONAL.** The burndown
reached 0 on 2026-07-02 (268 hit / 0 fallback; 296/0 after the multi-type
member-template fix) and the flip became the default; Phase-5 slice 4a then
DELETED the `MADC_XTEST_DEP_PARSE=0` escape hatch
(`madc_tsubst_dep_parse_enabled()`) and the slice-2/3 soak levers
(`MADC_XTEST_TSUBST_NO_BODY_SKIP`, `MADC_XTEST_TSUBST_FORCE_BAIL=1`) — the
parallel "pure re-parse" MODE no longer exists.
(`MADC_XTEST_TSUBST_FORCE_BAIL=covered` survives as a fault-injection hook:
it forces the slice-3 covered-shape LOUD-error arm, which no real shape can
reach by construction, so the hook is that arm's only unit-test exerciser.)
Slice 4b completed the deletion (2026-07-04): the first-eager requirement
fell (the arming sites skip FIRST instantiations too — the delete gate was
all first-skip walls cleared + burndown 0 FALLBACK + an eligibility census
EMPTY across the suite), and the captured-span re-parse fallback
(materialize_tsubst_skipped_body + the per-instantiation token capture) is
deleted outright — a tsubst bail on a skipped body is a LOUD pre-c2mir
error. What remains of the instantiation body parse is the ONLY parse for
its shapes, not a fallback: pattern-INELIGIBLE sources (tsubst_eligible
rejects — dependent member types, dependent returns, non-type scalar
params) and auto-return deduction (reads the parsed statements). Widening
those is per-KIND eligibility coverage work, tracked by the fallback
profile exactly like the KINDs before it.

## How we track distance (the burndown)

`scripts/tsubst_burndown.sh` runs the suite with `--show-stats` and aggregates:

- **Suite-wide HIT / FALLBACK totals** — the headline number. FALLBACK is the
  count of template-body instantiations that bailed to re-parse. Goal: 0.
- **Distinct `[why:]` reason-classes** — the remaining KIND worklist, ranked by
  frequency. Each line is "this many bodies bail for this reason"; the reason is
  the KIND still to implement. This is the prioritized to-do list.

The metric is monotonic by gate: every change must move FALLBACK down or flat
(never up). So the single number is a true distance-to-goal, and an empty
`[why:]` tally is the deletion trigger.

**Caveat — the denominator is the test suite, not all of C++.** Burndown = 0
means "nothing we test re-parses," which is the practical deprecation gate;
coverage is only as wide as the tests. Widen the corpus (more template-heavy
tests, real-header compiles) as confidence in the gate grows.

## What "done" looks like for a new feature

- Implemented as a KIND on the spine; it never bails, so it never re-parses.
- It does **not** appear in the `[why:]` fallback tally for any test that uses
  it.
- It does not depend, even transitively, on the re-parse fallback being present
  — removing the fallback would not change its behavior.
- The fix is generic (keyed on KIND/shape), not on a callee/template/operator
  name.

A feature that only passes because the re-parse net catches it is logged as
incomplete, not shipped.
