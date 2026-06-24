# Two-Tree Phase 3 — CONSUME the recipe (tsubst) — HANDOFF

**Date:** 2026-06-24 · **Branch:** `feature/front-end-performance-claude`
**Governing plan:** `2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md` (read §11 + §0 RESUME).
**This doc:** the imperative execute-to-done for Phase 3 — the FIRST slice that delivers a
real win (a template method instantiated by copy+substitute instead of re-parse).

---

## 0. STATE (verify, do not trust blindly — run `scripts/resume.sh`)

DONE + gated + committed on this branch:
- **`b35cef2`** scoped template-param registry (`Program::template_param_scopes` +
  `TemplateParamScope` RAII + `intern_template_param` + `resolve_template_param`,
  consulted first in `resolve_current_class_type_alias`).
- **`2104299`** the PRODUCER + defer guard: `build_dependent_pattern(FuncDef*)`
  (parser.cpp) parses a member-fn-template body ONCE with param→`DataDefTemplateParam`
  placeholder and captures it as **`FuncDef::dependent_pattern`** (a TokenFunc parse-tree
  RECIPE), removing it from `pending_funcs`. `dependent_parse_in_progress` gates the
  instantiation entry points so nested calls don't eagerly instantiate.
- **`20bbf92`** widening step 1: the per-call dependence test
  (`call_involves_placeholder` / `datadef_involves_placeholder`, parser.cpp) replaced the
  global bail — the instantiation entry points now defer ONLY genuinely type-dependent
  calls (g++'s `any_type_dependent_arguments_p` analogue, pt.cc:30555).
- **✅ PHASE 3 FIRST SLICE DONE — the recipe is now CONSUMED (3 commits):**
  - **`e4dda75`** `tsubst_cir` core: `copy_cir_subtree` gains an optional
    `{placeholder→concrete}` map (default NULL = byte-identical); `subst_datadef` rewrites a
    `DataDefTemplateParam` directly or under ptr/ref/const layers via the canonical builders.
  - **`6c301f9`** `FuncDef::tsubst_source`: a concrete instance links back to its SOURCE
    member template (stamped in `instantiate_member_fn_template_for_call`).
  - **`2bf8696`** the SEAM (`func_def`, cir_builder.cpp): `tsubst_method_body` builds a
    covered method's BODY by `tsubst_cir` of the recipe (cir-built ONCE into a memoized
    Tree-1 pattern, `m_tsubst_body_patterns`) instead of `translate_block`. Binding recovered
    by aligning recipe placeholder params with the instance's concrete params. CONSERVATIVE
    capability gate: if the pattern build trips the `append_type_specs` guard (a body cast
    `(U)x` / local `U tmp;` lowers a placeholder TYPE → error node), memo NULL and FALL BACK
    to re-parse (not yet covered). Hybrid B: concrete signature/shell stays on the parse path;
    re-parse stays the fallback (PLAN §5).
  - **`2459a7e`** WIDENING — deferred type-spec MARKER (g++ TEMPLATE_TYPE_PARM-in-saved-tree):
    in pattern mode `append_type_specs` leaves a placeholder used as a TYPE (`(T)x`, `T tmp;`)
    as an N_IGNORE marker carrying the placeholder datadef (instead of erroring); tsubst
    EXPANDS it to the concrete specs via the SAME `append_type_specs` (byte-identical), or an
    error node → fall back. Unit-tested (expansion: test_cir 58/0).

Producer + consumption run ONLY behind env hook **`MADC_XTEST_DEP_PARSE`** (off by default →
production byte-identical). VALIDATED firing (not silent fallback): `Holder::set<int>` and
`testoutoflinemembertemplate::store<int>` build bodies by tsubst, BYTE-IDENTICAL emit to
re-parse. Gate every commit: build clean; fulltest flag-OFF AND flag-ON 669/0/0/18; --emit=c11
byte-identical; torture flag-off byte-identical by construction.

⛔ **NEXT BLOCKER (the real next slice) — the DEPENDENT-PARSE-of-`T`-as-a-TYPE gap.** The
marker (emission + expansion) is built + the expansion unit-tested, BUT it cannot fire
end-to-end yet: `build_dependent_pattern`'s dependent PARSE THROWS (std::exception, caught →
recipe NULL) the moment the body uses `T` as a TYPE — a `(T)x` cast or a `T tmp;` local
(confirmed: `set` is eligible, parse throws; only bodies that use `T` purely as a VALUE, e.g.
`member = (int)v;`, produce a recipe today). So the marker EMISSION branch (`append_type_specs`
pattern mode) is currently unreached on the corpus. FIX = make the dependent parse tolerate a
template-parameter placeholder in TYPE position (cast target, local decl type): find what
throws in `parseFunction` on `(T)`/`T tmp` with `T` = `DataDefTemplateParam` (likely
size()/rawtype()/conversion-check on the placeholder), and make that path placeholder-tolerant
(it only needs to record the type as the placeholder, not size/convert it). Reducer:
`tmp/tsubst_marker.mad` (`template<class T> void set(T v){ member=(int)(T)v+1; }`) — currently
falls back; after the fix it must tsubst + emit byte-identical (proves the marker emission).

THEN the later widening (PLAN §11.5c / §Phase 4, one construct per gated commit): ptr/ref
params (`subst_datadef` peeling already handles the datadef side; extend the binding recovery
in `tsubst_method_body` to peel pdd/cdd to the base placeholder); dependent return type
(relax `tsubst_eligible`); typeless placeholder + ADL bit; real `is_type_dependent`. The
re-parse deletion (Phase 5) waits for full coverage.

## 1. GOAL (one sentence)

At cir-build of a concrete instantiated member-template method, if a Tree-1 recipe exists
for it, build the method body by **`tsubst`** (copy the recipe's cir pattern + substitute
the placeholder with the concrete arg) and feed THAT to c2mir — instead of
`translate_block` over the re-parsed body. Capability-gated; re-parse fallback stays.

## 2. THE SEAM

`CirBuilder::func_def(TokenFunc *tf)` calls `node_t body = translate_block((TokenCpnd*)tf);`
at **`src/cir_builder.cpp:11458`** (verify the line — it drifts). That is where a concrete
method's body becomes cir. Phase 3 replaces that call, for covered methods, with the tsubst
result.

## 3. STEPS (each its own gated commit; gate = §5)

1. **cir-build the recipe → a Tree-1 cir pattern.** `translate_block` over the recipe's
   placeholder body produces cir nodes whose `datadef` is the `DataDefTemplateParam`
   placeholder. The Phase 1.5 `append_type_specs` guard (cir_builder.cpp) turns a
   placeholder reaching type-lowering into an error node — that is correct for a pattern
   that is NEVER compiled directly. So either (a) build the pattern in a mode where the
   guard is suppressed and the pattern is held un-lowered, or (b) fuse build+substitute so
   the placeholder is replaced BEFORE `append_type_specs` runs. **(b) is simpler and safer**
   — never hold a placeholder-bearing cir tree that could reach c2mir.
2. **`tsubst_cir(pattern, {placeholder → concrete})`.** Reuse **`copy_cir_subtree`**
   (cir_builder.cpp:379, Phase 1 — it already makes a private node_t base) and, in the
   copy, replace any `datadef` that `is_template_param()` with the concrete arg type.
   Handle DERIVED types: ptr-to-placeholder → `getPointerType(concrete)`,
   ref-to-placeholder → `getReferenceType(concrete)`, const → `getConstType`. The concrete
   arg comes from the instantiation's type-arg binding.
3. **Method↔pattern link.** A concrete instantiated method's `FuncDef` must find its
   recipe + the concrete type args. Record the SOURCE template identity on the instantiated
   `FuncDef` when it is produced (the member-fn-template instantiation path:
   `instantiate_member_fn_template_for_call` / `try_instantiate_namespace_fn_template`).
   The recipe lives on the SOURCE template's `FuncDef::dependent_pattern`; map
   instantiated → source by (owner, member display name) or a recorded back-pointer.
4. **Wire the seam (cir_builder.cpp:11458).** For a covered method (pattern exists +
   capability-eligible), use the tsubst result; else `translate_block` (fallback). Keep the
   re-parse path — its deletion trigger is full coverage (PLAN §5).
5. **Cache + ownership.** The Tree-1 cir pattern is built ONCE per template member and
   reused across instantiations — own it in the CirBuilder Tree-1 arena (immutable, never
   freed mid-compile). Key by the source FuncDef (PLAN §11.5b).

Start SCALAR-FIRST: the narrowest covered method (one type param, scalar body, e.g.
`void set(T v){ member = v; }` on a concrete class) so the §5 equivalence check is trivial
to satisfy and debug. Widen one construct per commit after.

## 4. THE CRUX RISK (read before coding)

This is the FIRST slice where **output actually changes** for covered methods. The
`--emit=c11` gate then means **tsubst output must be byte-identical to the re-parse
output** for those methods. Any divergence is a real bug — debug it against the re-parsed
body (they must lower to the same cir). De-risk by starting with the most trivial covered
method and diffing emitted C tsubst-vs-reparse on JUST that method before widening.

## 5. GATE (every commit — correctness only, never perf-gate)
- `make -C src` clean, no new warnings.
- `make -C src fulltest` → 669/0/0/18 (flag-off, production).
- **flag-ON** `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` → 669/0/0/18 (isolation +,
  once consumption is wired, correctness of covered methods).
- gcc.c-torture failset byte-identical to the 51-name baseline (0 timeouts).
- `--emit=c11` byte-identical vs the prior commit on representative TUs — for a covered
  method this is the tsubst==reparse equivalence proof.

## 6. SETTLED — do not re-litigate
- Hybrid B (concrete shell at parse, tsubst the BODIES at cir-build). NOT full cir-level
  deferral (A), NOT parse-layer copy (C). (PLAN §11.3.)
- Deferral primitive = the PER-CALL dependence test (`call_involves_placeholder`), g++'s
  per-node model — NOT a global "defer everything in a template" flag.
  `dependent_parse_in_progress` is now only the "am I in a dependent parse" gate. Before
  widening eligibility to bodies that MIX dependent + non-dependent template-id calls,
  finish widening per §11.5c (typeless placeholder + ADL bit + real `is_type_dependent`).
- The env-gated flag-on byte-identical harness is THE validator. Keep it until the re-parse
  path is deleted (PLAN §5 trigger).

## 7. WIDENING BACKLOG (after scalar consumption works) — PLAN §11.5c
1. typeless placeholder for a deferred call (madc `unknown_type_node` analogue) + ADL bit.
2. real `is_type_dependent(expr)` predicate from operand types; at tsubst re-run the NORMAL
   call-resolution entry on concrete args (reuse, don't fork) — retires the flag.
3. then dependent member access (`T::x`), template-ids (`Foo<T>`), packs — one per commit,
   each relaxing a `tsubst_eligible` constraint with the gate green.
