# Phase-1 CIR Coverage — Design

**Date:** 2026-05-29
**Branch:** `feature/cir-node`
**Status:** approved, pre-implementation

## Goal

Grow `CirBuilder` (`src/cir_builder.cpp`) integration-test coverage by
implementing the three highest-leverage missing constructs identified by the
2026-05-29 triage (see `tmp/cir_triage.sh`, memory `project_cir_triage`):

1. **Char literals** (`ttChar`) — 32 unhandled-expr failures
2. **Subscript reads** (`ttSubscript`) — 49 unhandled-expr failures
3. **`var_decl` initializers** — `cir_builder.cpp:360` drops them
   (`init_node = ignore()`, silent miscompile); most of the 69
   runtime_mismatch + 3 timeout failures

Baseline at design time: `--backend=cir` passes **55/419** (legacy
`MADC_CIR_OLD=1` path passes 0/419 — it is behind CirBuilder and is NOT a
porting source, only a node-shape reference).

## Non-goals

- Not flipping the default backend (stays MIR/emit-C; CIR is flag-gated).
- Not the c2mir_rejected / other_nonzero correctness tail (struct member
  access, prototype emission) — that is Phase 2, needs systematic-debugging
  per signature, not missing-construct adds.
- Not parser changes. If designated initializers turn out to need parser work
  (no designator field exists on `TokenDecl`), they are deferred.

## Approach

Incremental, **one construct at a time**, each verified against `c2m -d` via
`scripts/cir_diff.sh` BEFORE moving to the next. After each target, re-run
`tmp/cir_triage.sh` and record the histogram delta — reporting both
tests-moved-to-pass AND any newly-surfaced failure modes (no silent caps).

The legacy `madc_cir.cpp::cir_translate_*` functions are the **node-shape
reference** — they emitted all three constructs in a c2mir-acceptable shape.
Port the *shapes* into CirBuilder idioms (`node2`/`id`/`integer`/`list`/
`append`); do NOT wholesale-copy the legacy code (its module walk is dead for
unrelated reasons).

Ordering (cheapest-first, cleanest signal per step):
**char → subscript → initializers.**

## Target 1 — char literals (`ttChar`)

Add a `translate_expr` case for `TokenChar`. C character constants have type
`int`, so the node is an integer constant.

- **Node shape:** confirm N_I vs N_CH with a one-line reducer (`char c = 'A';`)
  through `c2m -d` as step 1.
- **Emit:** `integer(charValue, tb)` (or the confirmed char node).
- **Verify:** `cir_diff.sh` on a reducer using a char literal in initializer
  and assignment position.

## Target 2 — subscript reads (`ttSubscript`)

Add a `translate_expr` case for `TokenSubscript`.

- **Node shape:** `node2(N_IND, base, idx)` (confirmed via legacy
  `madc_cir.cpp:266-276` and `c2m -d`).
- **Improvement over legacy:** translate `base` as a full expression via
  `translate_expr(object_expr)` rather than only `object.name`, so
  `p->arr[i]`, `(a+b)[i]`, and `f()[i]` work. Fall back to `id(object.name)`
  when there is no base sub-expression.
- **idx:** `translate_expr(index)`.
- **Verify:** reducers `a[1]`, `p->arr[i]`, `(a+b)[i]` vs `c2m -d`.

## Target 3 — `var_decl` initializers

Replace `cir_builder.cpp:360-361` (`init_node = ignore(); // TODO`) with real
initializer translation. Data source: the `TokenDecl` (origin at
`translate_stmt:956` is the `TokenDecl`; confirm the `translate_module` global
path also reaches `var_decl` with the `TokenDecl` available — adjust the
signature/threading if it currently passes only `Variable*`).

Initializer shapes (confirmed via legacy `madc_cir.cpp:558-584` + `c2m -d`):

- **scalar** (`int x = 5;`, `char *p = "s";`):
  `init_node = translate_expr(tdecl->initialize)` — bare expression, no wrapper.
- **flat brace** (`int a[] = {1,2,3}`):
  `LIST(INIT(LIST(), val), INIT(LIST(), val), ...)`.
- **nested** (`int m[2][2] = {{1,2},{3,4}}`, array-of-struct):
  recursive helper. A nested element is a `TokenStructLit` (`inits` vector);
  its `INIT` value is itself a `LIST(INIT…)` — recurse.
- **designated** (STRETCH, 2 tests: `testnestdesignatedinit`,
  `testflexarrayemptyinit`): `.field=v` → `INIT(LIST(FIELD_ID(ID name)), v)`;
  `[idx]=v` → `INIT(LIST(const-expr), v)`. **Open question:** `TokenDecl` has
  no designator field, yet the JIT backend handles designated init correctly,
  so the parser captures it somehow (likely positional normalization).
  Investigate at this step: if positional-normalized, emit positional `INIT`s
  (correct semantics); otherwise emit the designator shapes above; **defer the
  2 tests** if the AST does not preserve a CIR-usable representation.

Helper: a recursive `init_from_token(TokenBase*)` that returns the `INIT`
value node, dispatching on scalar-expr vs `TokenStructLit`.

## Verification loop (every target)

1. Minimal `.c` reducer → `scripts/cir_diff.sh [--checked] <r.c>` must MATCH
   (`--checked` where forward-decl folding applies).
2. Add `bin/test_cir` unit case(s); `make -C src test` (or the test_cir
   binary) stays green.
3. Re-run `tmp/cir_triage.sh`; record the histogram delta.
4. `make -C src fulltest` green — CIR is flag-gated, so the default backend
   must be unaffected; confirm rather than assume.
5. Commit per target with the measured delta in the message.

## Risks / open questions

- **Designated-init AST representation** — unknown; JIT handles it but
  `TokenDecl` has no explicit designator field. Investigate at Target 3;
  defer the 2 tests if not cleanly reachable. Does not block scalar/flat/nested.
- **char node code** — N_I vs N_CH; confirmed in Target 1 step 1.
- **Gate-clearing ≠ green** — clearing an "unhandled expression" error node
  moves a test past the validity gate but may surface a c2mir_rejected or
  runtime_mismatch failure instead. Phase-1 success is measured as histogram
  movement, not a guaranteed count of newly-green tests.
- **Global vs statement decl path** — `var_decl` must receive the `TokenDecl`
  (not a bare `Variable*`) on both paths to see initializers; verify the
  `translate_module` global path before implementing.

## Success criteria

- All three `translate_expr`/`var_decl` additions match `c2m -d` on their
  reducers via `cir_diff.sh`.
- `make -C src fulltest` stays green (default backend unaffected).
- `bin/test_cir` green with new unit coverage.
- Measured, reported `--backend=cir` histogram improvement over the 55/419
  baseline, with newly-surfaced failure modes documented for Phase 2.
