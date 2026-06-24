# Two-Tree Phase 3 — CONSUME the recipe (tsubst) — HANDOFF

**Date:** 2026-06-24 · **Branch:** `feature/front-end-performance-claude`
**Governing plan:** `2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md` (read §11 + §0 RESUME).
**This doc:** the imperative execute-to-done for Phase 3 — the FIRST slice that delivers a
real win (a template method instantiated by copy+substitute instead of re-parse).

---

## 0. STATE (verify, do not trust blindly — run `scripts/resume.sh`)

✅ **KG SYNCED (2026-06-24).** FalkorDB was briefly unreachable mid-session; once back,
`madc-knowledge` was reconciled via `scripts/kg_query.sh`: Feature
`two_tree_tsubst_instantiation` (status in_progress) + Session
`session_2026_06_24_two_tree_phase4`, linked `LANDED` + `IMPLEMENTS_DECISION
→ cir_c2mir_backend_adr`. Captures: two-tree **Phase 3 COMPLETE**; **Phase 4 constructs
1–4 landed** (ptr/ref/const params `b5e9d86`; bare-T return `6117ccd`; pointer return
`c5adec2`; multiple type params + completeness guard `2204d19`); **data-grounded next
targets** (pack 87 dominant > template-id 14) → next two design-level steps = cir-node
**pack expansion** and **direct type-arg binding** (capture resolved args out of
`try_instantiate_namespace_fn_template`, retire `recover_param_binding`). All gated
flag-off AND flag-on 669/0/0/18; torture byte-identical by construction (env-gated).


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

✅ **2026-06-24 LOCAL CODEX UPDATE — DEPENDENT-PARSE-of-`T`-as-a-TYPE gap RESOLVED
(uncommitted).** Root cause was C-style cast disambiguation in `Program::parseExpr_operatorArm`:
identifier cast heads only checked `datatype_map` / `struct_map`, so `(T)v` fell through as a
parenthesized expression and reported `T` as an undeclared identifier before the dependent
recipe could reach the type-spec marker. The parser now also consults
`resolve_current_class_type_alias`, which reaches the scoped `DataDefTemplateParam` registry,
for cast heads, cast-token consumption, and nested-cast lookahead. This records the placeholder
type without asking for concrete size/rawtype/conversion. `T tmp` locals were already accepted
through the declared-type path; regression coverage now pins both `member = (int)(T)v + 1;`
and `T tmp = v; member = (int)tmp + 1;` in dependent pattern parse.

Validation for the local update:
- `make -C src` green.
- `bin/test_cir` green: 59 test cases, 739 assertions, 4 skipped.
- `make -C src fulltest` green: 669 passed, 0 failed, 0 timed out, 18 skipped.
- `env MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` green: 669 passed, 0 failed,
  0 timed out, 18 skipped.
- Focused reducers: `tmp/tsubst_marker.mad`, `tmp/tsubst_scalar.mad`,
  `tmp/tsubst_local.mad` clean.
- `--emit=c11` flag-off vs flag-on output byte-identical for marker/local reducers; no raw
  `T`, `tsubst:`, or `unbound template` markers in emitted C.
- GDB breakpoint confirmed `CirBuilder::tsubst_cir` is reached from
  `CirBuilder::tsubst_method_body` for the marker reducer, proving this is not silent fallback.

✅ **PHASE 4 — construct 1 DONE (ptr/ref/const params).** The binding RECOVERY now peels
`T*` / `T&` / `const T` param layers in lockstep with the concrete instance param, binding the
BARE placeholder (so `subst_datadef`'s existing peel rebuilds the derived type on body nodes).
New file-static `recover_param_binding` (cir_builder.cpp, beside `subst_datadef`) replaces the
scalar-only inline loop in `tsubst_method_body`; the old `is_template_param()` case is its
recursion base. A layer mismatch recovers nothing → safe re-parse fallback. VERIFIED: a
temporary `[TSUBST] … recovered N binding(s)` trace showed `T*`/`T&`/`const T&` go 0→1 binding
(fire tsubst, not silent fallback); trace removed. test_cir 60/60 (new derived-type case pins
`T*`→`int*`, `T&`→`int&`, Tree-1 immutability). Reducers `tmp/tsubst_{ptr,ref,constref}.mad`
run 2/4/6 and emit byte-identical flag-off vs flag-on. Torture byte-identical BY CONSTRUCTION
(all new code behind the `MADC_XTEST_DEP_PARSE` env gate — production never reaches it).

✅ **PHASE 4 — construct 2 DONE (bare-`T` dependent return type).** `tsubst_eligible`
(parser.cpp) now ACCEPTS a member template whose return type is the bare placeholder
(`T echo(T v)`). The return type lives in the concrete SHELL (resolved on the normal parse
path — hybrid B); the recipe parse passes the placeholder return type to `parseFunction`
(which already tolerates it — NO parser change needed), and tsubst substitutes the BODY via
the param binding. KEY TRAP avoided: `return_value_type().is_template_param()` is FALSE for a
bare-`T` return (it is a return TOKEN naming T, not a `DataDefTemplateParam`), and
byte-identical emit MASKS a silent re-parse fallback — so the gate discriminates the bare case
at the TOKEN level (return-token stream == the single identifier `T`) and firing was reproven
with the `[TSUBST]` trace, NOT byte-identical alone. A NON-bare dependent return (`T*`, `T&`,
`vector<T>`, `T::type`) stays REJECTED (token scan) → re-parse fallback. VERIFIED: trace shows
`echo`/`twice` fire (1 binding), `addr` (T* return) does NOT fire. test_cir 61/61 (new case:
`echo` eligible via funcdef_map key, `T*` `addr` rejected, exactly 1 pattern). Reducers
`tmp/tsubst_{retdep,retdep2,retptr}.mad` run 8/14/7 byte-identical. Torture byte-identical BY
CONSTRUCTION (`tsubst_eligible`'s only caller is `build_dependent_pattern`, gated behind
`MADC_XTEST_DEP_PARSE`).

✅ **PHASE 4 — construct 3 DONE (POINTER dependent return `T*`).** `tsubst_eligible` now
also accepts a return that is the bare param T wrapped in POINTER layers (`T`, `T*`, `T**`).
Detected on the return-token stream (recon via a temporary `[RETTOK]` dump: `id=60`=ident,
`id=13`=`tkStar`, `id=24`=`tkBand`; `const` is NOT in these tokens): exactly one identifier
(== T), every other token a `*` (`tkStar`). A `<` (template-id `vector<T>`), `::` (`T::type`),
or a second identifier stays REJECTED. VERIFIED (trace, not byte-identical alone): `T* passthru(T* p)`
fires (1 binding), runs 42, byte-identical; `vector<T>` return does NOT fire. test_cir 61/61
(updated case: `echo` bare-T + `addr` `T*` both eligible, `ref` `T&` not). Reducers
`tmp/tsubst_retptr2.mad` (42) + `tmp/tsubst_retptr.mad` (7) byte-identical. Torture byte-identical
BY CONSTRUCTION (gated). ⚠️ REFERENCE returns (`T&`/`const T&`) are deliberately EXCLUDED:
`tmp/tsubst_retref.mad` / `tsubst_retcref.mad` fail to compile **flag-off too** ("undeclared
identifier `Holder__passref`" + "lvalue required") — a SEPARATE pre-existing
ref-return-member-template instance-symbol bug, NOT tsubst. Revisit `T&` returns after that bug
is fixed (it's a call-site/instantiation-naming issue, a different track).

✅ **PHASE 4 — construct 4 DONE (MULTIPLE type params).** `tsubst_eligible` now accepts a
member template with ≥1 type param (was hard-capped at exactly 1). The `is_type`/`is_pack`
checks were generalized from index `[0]` to ALL indices (so a mixed `<class A, int N>` cannot
slip through), and the return / body `T`-scans now test a param-name SET (`pnames`) rather than
a single `T`. `recover_param_binding` already builds one binding per param.
VERIFIED (trace): `template<class A, class B> void set2(A a, B b)` fires with **2 bindings**
(runs 7, byte-identical); single-param `echo` still fires (1 binding). test_cir 62/62 (new case:
`set2` + `echo` both eligible, 2 patterns). Torture byte-identical BY CONSTRUCTION (gated).
🛠 **COMPLETENESS GUARD (the flag-on gate caught a real bug — do not remove):** multi-param
first crashed 10 flag-on tests (incl. `testvectorptr`). Cause: a real multi-param std method
where a type param appears only in the BODY (not as a parameter type) recovered a PARTIAL
binding (non-empty), so tsubst fired leaving UNSUBSTITUTED placeholders → wrong code no error
node caught. Fix (`tsubst_method_body`, deepest layer): require
`binding.size() >= source->template_param_names.size()` — every param recovered from the
signature, else fall back to re-parse. Single-param behavior unchanged (need=1 == old
binding.empty check).
⚠️ NON-TYPE params stay rejected (correct — they need value substitution, not built); they also
can't currently be exercised end-to-end because the explicit non-type member-template call
syntax (`f<int,5>(...)`) fails to PARSE (a separate pre-existing gap — see `tmp/tsubst_mixed.mad`).

📊 **DATA-GROUNDED NEXT TARGETS (measured 2026-06-24, not guessed).** A temporary
reason-tagged `tsubst_eligible` (env `MADC_XTEST_ELIG`, since reverted) was run flag-on over the
container/template corpus (testvector, testvectorptr, testmap, testset, teststdmapint,
testtemplatecontainer, testcontainerdtor, testoutoflinemembertemplate, testmadc_ns,
testnestedtemplatedtor). Rejection tally:
- **`pack` 87** — DOMINANT (~58%). Variadic templates (emplace, `make_*`, forwarding ctors).
- `OK` 47 — already eligible.
- `template-id-or-lt` 14 — a body/return `<` (a real `Foo<T>` template-id OR a `<` comparison;
  the scan can't tell them apart — conservatively rejects both).
- `nontype` / `dep-return` / `dep-name` — ~0 in this corpus (rare; don't prioritize).
⇒ The CHEAP relaxations are EXHAUSTED (params, ptr/ref returns, multi-param — all landed). The
next two are DESIGN-LEVEL (traced 2026-06-24, both confirmed NOT a gate relaxation):

1. **PACKS (do first — dominant).** The existing pack machinery (`pack_subst`,
   `first_template_pack_index`, the `pack_name...` token splice ~parser.cpp:2815) is ALL
   TOKEN-LEVEL — it serves the RE-PARSE path (splice N tokens, then parse). The tsubst path
   operates on the already-built cir tree, so it needs cir-node **PACK EXPANSION**: one recipe
   node carrying `args...` expands to N nodes by the instantiation's concrete pack arity — g++'s
   `tsubst_pack_expansion`. New mechanism (node fan-out, not just datadef swap). Multi-commit.
2. **DIRECT TYPE-ARG BINDING (prerequisite cleanup — do alongside).** Today the binding is
   REVERSE-ENGINEERED from signature param positions (`recover_param_binding`), which is why a
   body-only param can't bind and the completeness guard has to bail. g++ feeds tsubst the arg
   list directly. The resolved concrete type args are computed INSIDE
   `try_instantiate_namespace_fn_template` (from deduction/explicit args on `tc`) but are NOT
   stored — `FnTemplateDef` (madc.h:1746) carries only param NAMES/flags. FIX: capture the
   resolved `std::vector<DataDef*>` (param-index order) onto the instance `FuncDef` beside
   `tsubst_source`, and bind from it directly in `tsubst_method_body` — subsumes
   `recover_param_binding`, covers body-only params, makes the completeness guard trivially hold.

Lower priority / blocked: template-id body/return (`vector<T>`, `Foo<T>` — 14; needs
dependent-template-id handling, AND disambiguating `<` template-id from `<` comparison);
reference dependent return (`T&` — blocked on the pre-existing ref-return instance bug);
non-type params (value substitution + blocked on the `f<int,5>` call-parse gap); `T::x`;
typeless placeholder + ADL; real `is_type_dependent`. The re-parse deletion (Phase 5 = g++
lex/parse/sema parity) waits for full coverage; Phase 6 (serialize Tree-1 / header-forest) waits
for Phase 5 (user directive 2026-06-24).

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
