# Two-Tree Phase 4 — tsubst Construction-Deferral & the Container-Construction Wall Stack

**Date:** 2026-06-27 · **Branch:** `feature/front-end-performance-claude`
**Parent handoff:** `2026-06-24-two-tree-phase3-tsubst-consume-HANDOFF.md` (§0 🔴 block)
**Governing plan:** `2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md` (§11.5c)

This is the **comprehensive, self-contained** execution reference for making the STL
container *node-construction* member-template bodies (`_Rb_tree::*`, `vector::_M_realloc_insert`,
`basic_string::_M_construct`) instantiate by **tsubst** instead of re-parse. It consolidates the
session's research (madc machinery map + gcc/clang model), the GDB-confirmed root cause, the full
wall stack, and an ordered slice plan. Read this before touching the cluster — it prevents
re-deriving and re-walking the two SIGSEGV dead-ends already proven this session.

---

## 0. QUICK-START / STATE (verify with `scripts/resume.sh`)

- **HEAD `3692a172`** (docs). Binary -O0, reflects code at **`1d69ee40`**. Tree clean (only
  untracked `mir-debug-support.md`, not ours).
- **Flag-on engagement (clean baseline):** testsubscript **22 hit / 13 fallback**, testvector
  **12 / 3**, testmap **5 / 6**, testset **4 / 6**. (`MADC_XTEST_DEP_PARSE=1 bin/madc --show-stats <t>.mad`)
- **Gates green:** flag-on `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` = 670/0/0/18;
  flag-off `make -C src fulltest` = 670/0/0/18 + drift gates green.
- **Worklist instrument (landed `6570a849`):** `--show-stats` prints `[why: ...]` per fallback
  shape (the rejecting `tsubst_eligible` clause, or the `tsubst_method_body` bail, or the first
  cir error_msg). This is the steering signal — drive the ranked `[why:]` list down.
- **WIP saved:** `tmp/construction-deferral-guard-WIP.patch` (the eligibility extension + the
  recursion guard — validated to fix the SIGSEGV; see §6.1/§7 Slice A).
- **Reducers:** `tmp/vec_recurse.mad` (vector<string> push_back — the crash repro),
  `tmp/rbtree_recurse.mad` (map<string,int>), `tmp/veci_recurse.mad` (vector<int>, no-crash control).

---

## 1. GOAL

Retire the re-parse fallback for the container node-construction cluster by making their
member-template bodies build via tsubst (copy Tree-1 recipe + substitute) and feed c2mir/MIR
correct CIR. This is the §11.5c generic-re-resolve work — the path that ultimately deletes
re-parse (PLAN §5). Capability-gated by `MADC_XTEST_DEP_PARSE`; the re-parse path stays until full
coverage. **Every tsubst change is production-safe by construction** (flag-off returns at the
`getenv` gate at the top of `tsubst_method_body`, so flag-off output is byte-identical; torture
runs flag-off ⇒ byte-identical too).

---

## 2. LANDED THIS SESSION (do not redo)

- **`1d69ee40`** — tsubst re-resolves **system-header free-function dependent calls**. Retired the
  "simple scalar/pointer types only" pre-resolve gate in `resolve_copied_dependent_call`; a
  system-header call now re-resolves on substituted args via the same
  `find_namespace_function_overload` + instantiate-on-miss path as a local call, with the
  post-resolve body-availability check as the safety net. `std::__uninitialized_copy::__uninit_copy`
  became a hit (21→22 subscript, 11→12 vector). **This is the MODEL for Slice C (ADL operator).**
- **`6570a849`** — self-diagnosing fallback worklist (`[why:]` reasons; see §0).
- **`560cf165`** — data-grounded retarget: the remaining fallbacks are rejected at
  `tsubst_eligible` (not the copy path) — see §3.5.

---

## 3. RESEARCH — the madc tsubst machinery (current file:line, `src/cir_builder.cpp` unless noted)

### 3.1 The seam
`CirBuilder::func_def(TokenFunc*)` lowers a concrete method body. For a covered instantiated
member-template it calls **`tsubst_method_body(tf, fd, &reason)`**; on NULL it falls back to
`translate_block((TokenCpnd*)tf)` (re-parse). The hit/fallback counters + `[why:]` profile are
recorded here.

### 3.2 `tsubst_method_body` — **13323**
Gate: `if (!getenv("MADC_XTEST_DEP_PARSE")) return NULL;` (flag-off = production, untouched).
Then: find `source = fd->tsubst_source`; if no `dependent_pattern`, re-check `tsubst_eligible` to
report ineligible-clause vs parse-failed. Bail predicates (each sets a `[why:]` reason via the
`bail()` lambda): `__destroy` guard; `tsubst_body_has_unsupported_reference_param`;
`tsubst_pattern_has_dependent_call`. Then **build the Tree-1 pattern ONCE** via
`translate_block((TokenCpnd*)recipe)` with `m_tsubst_pattern_mode = true` (memoized in
`m_tsubst_body_patterns[source]`); if the pattern has a cir error → memo NULL → fallback (the
first error_msg becomes the `[why:]`). Then bind {placeholder→concrete} from
`fd->tsubst_type_args`/`tsubst_type_arg_packs` and **`tsubst_cir(pattern, binding)`** (= copy +
substitute). If the result has a cir error → fallback.

### 3.3 `tsubst_eligible(FuncDef*, const char **why)` — `parser.cpp:33507`
Capability predicate (token-scan of `fd->member_template_decl`). `return no("...")` clauses:
not-a-member-template; `no type params`; `non-type template param`; `>1 pack param`;
`dependent return type (not T/T*)`; **`template-id '<' in body`** (the dominant blocker — UNLESS a
covered pack exception, `parser.cpp:33600-33603`); `dependent name P:: in body`. The pack
exception = `template_pack_body_from_system_header && (placement-new-ctor-pack ||
member-call-pack)`; **Slice A adds unqualified-call-pack** (`tsubst_has_unqualified_call_pack_expansion`,
in the WIP patch).

### 3.4 `build_dependent_pattern(FuncDef*)` — `parser.cpp:33701`
Re-parses the retained body in `dependent_parse_in_progress` mode (placeholder-typed params) → the
Tree-1 recipe `fd->dependent_pattern`. Dependent calls are left unresolved because
`call_involves_placeholder` (`parser.cpp:31455` → `is_type_dependent` 31437) makes
`try_instantiate_*` bail during the dependent parse (31476, 33809).

### 3.5 The copy/substitute path
- **`tsubst_cir` (1797)** → thin wrapper over **`copy_cir_subtree(src, &subst)` (1126)**.
- `copy_cir_subtree` deep-copies the cir tree, substituting `datadef`s via `subst_datadef`. Special
  cases: **placement-new deferral (~1164–1637)** — N_IGNORE w/ TokenNEW origin: re-lowers a
  placement-new construction with concrete types (THE PRECEDENT for construction deferral, Slice B);
  destroy markers; **N_CALL dependent call (~1639)** → `resolve_copied_dependent_call`; **N_ID
  rewrite (~1810)** → `rewrite_copied_dependent_call_id` (1088).
- **`resolve_copied_dependent_call` (819)** — re-resolves a dependent call on substituted arg types
  (`find_namespace_function_overload` + instantiate-on-miss), member path + namespace path. The
  **post-resolve body-availability check** (964; system-header 1021) is the safety net. *(The old
  "simple types" pre-gate was removed in `1d69ee40`.)* THE KEYSTONE for call re-resolution.
- **`subst_datadef` (425)** — substitutes template params in a `DataDef`: peels `DataDefREF`/`PTR`/
  `CONST` layers + direct subst-map hits ONLY. **NO dependent member-type resolution** (no
  `Alloc::rebind<X>::other` / `make_typename_type` analogue). **This is the rebind gap — Slice D.**

### 3.6 The construction lowering (THE RECURSION — §5)
- **`object_arg_addr(TokenBase *arg, DataDefCLASS *target)` (2611)** — produce the `void*` address of
  an object `arg` for a by-pointer/by-ref ctor parameter of class `target`. Early direct-bind guards
  (ref-returning call; derived→base; `is_class_object_value`; object-returning call; `TokenObjTemp`;
  rvalue trivially-copyable) — **all require `as_class_instance(arg->datadef())` to resolve to a
  concrete class.** Materializing tail (**2717**): `__madc_objtmp_N` temp of `target` + var_decl +
  `class_ctor_call(tmp, target, {arg})`.
- **`object_arg_value` (2733)** — by-value analogue; same materializing tail (~2754).
- **`class_ctor_call(Variable*, DataDefCLASS *cdd, ctor_args, origin)` (7084)** — lowers a ctor call;
  `select_ctor_overload`; for an object-class ctor parameter it calls **`object_arg_addr(arg, pc)`
  (7158)** — the back-edge.

### 3.7 Diagnostics (landed `6570a849`)
`cir_first_error_msg(node_t)` (15638) surfaces the first error_msg; `tsubst_method_body` threads a
`reason` out-param; `tsubst_eligible` threads a `why` out-param; printed as `[why:]` in
`src/madc.cpp`'s `--show-stats` profile.

---

## 4. RESEARCH — the gcc/clang two-phase model (citations from `/workspace/gcc/gcc/cp/`)

The target shape: **one resolver, two phases.** Dependence is detected by ONE predicate; at
instantiation, tsubst re-runs the SAME `finish_*`/resolution entry points on substituted args.

- **`finish_call_expr` (semantics.cc:3315)** — the single shared call-resolution entry. Branch:
  `if (type_dependent_expression_p(fn) || any_type_dependent_arguments_p(args))` → build a typeless
  deferred `CALL_EXPR` (`build_min_nt_call_vec`, no TREE_TYPE); else real overload resolution.
- **`tsubst_expr` CALL_EXPR case (pt.cc:22158)** — substitutes fn + args; if `koenig_p` and the call
  became non-dependent, `perform_koenig_lookup` (phase-2 ADL); then `finish_call_expr`.
- **`type_dependent_expression_p` (pt.cc:30086)** — the dependence arbiter (IDENTIFIER/USING always
  dependent; resolved FUNCTION_DECL from non-dependent scope not dependent; catch-all
  `dependent_type_p(TREE_TYPE(e))`).
- **ADL (Slice C precedent):** `perform_koenig_lookup` (semantics.cc:3250) →
  `lookup_arg_dependent` (name-lookup.cc:1778) → `search_adl`. Operators:
  `add_operator_candidates` (call.cc:7113) uses **saved phase-1 lookups**
  (`DEPENDENT_OPERATOR_TYPE_SAVED_LOOKUPS`) + a fresh `lookup_arg_dependent` on the concrete arg
  types. This is how `__gnu_cxx::operator-` on a `__normal_iterator` resolves at instantiation.
- **Dependent member-type (Slice D precedent):** `tsubst` TYPENAME_TYPE (pt.cc:17707) →
  `make_typename_type` (decl.cc:5020) → `lookup_member` **once the qualifying scope is concrete**.
  `__alloc_traits<_Alloc>::rebind_alloc<_Rb_tree_node<_Val>>` resolves this way once `_Alloc` is
  concrete.

madc analogue: `resolve_copied_dependent_call` IS the `finish_call_expr`-at-phase-2 for calls.
The cluster needs the same for **operators** (Slice C), **dependent member-TYPES** (Slice D), and
**constructions** (Slice B) — and a completeness check (Slice A) so an un-re-resolvable ref falls
back instead of emitting a dangling symbol.

---

## 5. ROOT CAUSE — the construction recursion (GDB + trace CONFIRMED)

**Repro:** `tmp/vec_recurse.mad` (`vector<string>; push_back("a"); push_back("b");`) + the
eligibility extension ⇒ **SIGSEGV**. `vector<int>` does NOT crash ⇒ needs CLASS elements.

**Trace** (`MADC_TRACE_OAA` instrument at the `object_arg_addr` materializing tail, since reverted):
```
[OAA] depth=4 pattern_mode=1 target=basic_string        arg_dd=_Args
[OAA] depth=5+ pattern_mode=1 target=allocator<char>    arg_dd=_Args   (repeats forever)
```
**Mechanism:** in pattern mode, `object_arg_addr` is reached with a **dependent** arg (`_Args`
template-param placeholder) and a **concrete** target (`allocator<char>`). Because `arg` is
dependent, `as_class_instance(arg->datadef())` is NULL ⇒ none of the early direct-bind guards fire
⇒ it materializes a temp and calls `class_ctor_call(tmp, allocator<char>, {_Args})`. Overload
resolution **cannot reject candidates for an unknown arg type**, so it selects the copy ctor (param
`const allocator<char>&`) ⇒ `object_arg_addr(_Args, allocator<char>)` again ⇒ ∞. The
backtrace is a perfectly periodic 2-frame cycle (`object_arg_addr+0xb33 ⇄ class_ctor_call+0xcf32`),
i.e. invariant `(arg, target)`. (Lead-in: depth-4 constructs `basic_string` from `_Args`, matching
a `basic_string(…, const allocator&)` ctor ⇒ drops to constructing the allocator param from `_Args`.)

**This is a symptom.** The root cause is that **pattern-mode lowering eagerly resolves overloads and
lowers a construction whose arg is still dependent** — semantically impossible. Pattern mode already
defers dependent CALLS; the missing analogue is deferring dependent CONSTRUCTIONS.

---

## 6. THE WALL STACK (4 separate deep capabilities — why this is not a bounded slice)

### 6.1 Construction deferral — *guard validated; deferral-to-hit needed*
- **Blocks:** the SIGSEGV above; all `_Rb_tree`/vector-element bodies transitively construct.
- **Guard (validated, in WIP patch):** in `object_arg_addr`/`object_arg_value`, before the
  materializing tail — `if (m_tsubst_pattern_mode && arg && template_param_under_type_layers(arg->datadef())) return error_node(...)`.
  This is the construction-level analogue of `tsubst_pattern_has_dependent_call` (a dependent-arg
  construction is not lowerable in the recipe ⇒ fall back). **Result: SIGSEGV gone, testset clean.**
  *Cannot regress current hits* (any currently-covered body that reached this would already crash).
- **Deferral-to-HIT (Slice B):** instead of falling back, emit a deferred-construction node that
  `copy_cir_subtree` re-lowers (re-invoke `class_ctor_call` with concrete types) after substitution
  — modeled on the **placement-new deferral (3.5, ~1164)**. gcc precedent: tsubst re-runs the ctor
  resolution on concrete args.

### 6.2 ADL free-operator emission — *Slice C*
- **Blocks:** `vector::_M_realloc_insert` → `MIR error: import of undefined item __ns___gnu_cxx_operator_mi`
  (the `__gnu_cxx::operator-` on `__normal_iterator`, iterator pointer subtraction).
- **Fix:** route the operator through `resolve_copied_dependent_call` so it re-resolves+instantiates
  on concrete arg types and the body-availability check fires (MODEL: `1d69ee40`). gcc precedent:
  `add_operator_candidates` with saved phase-1 lookups + `lookup_arg_dependent` (§4).

### 6.3 Rebind / dependent member-type symbol emission — *Slice D*
- **Blocks:** `map`/`_Rb_tree` → `import of undefined item _Rb_tree_…` (the
  `rebind_alloc<_Rb_tree_node<_Val>>` node-allocator type not emitted).
- **Fix:** add dependent-qualified member-type resolution to `subst_datadef` (425) — a
  `make_typename_type`→`lookup_member` analogue: once the qualifying scope (`__alloc_traits<_Alloc>`)
  is concrete, look up `rebind_alloc<…>::other` and instantiate. gcc precedent: TYPENAME_TYPE tsubst (§4).

### 6.4 Undefined-symbol-aware fallback — *Slice A (do FIRST)*
- **Blocks safety:** tsubst emits an `N_ID` to a symbol that won't be defined (the ADL operator, the
  rebind type); `cir_tree_has_error` does NOT catch it (only MIR does) ⇒ the compile BREAKS (exit 1)
  instead of falling back ⇒ **regresses map/vector** (clean baseline compiles them via re-parse).
- **Fix:** after building/substituting the tsubst body, verify every referenced symbol is emittable
  (in `referenced_funcs` with a body / `has_deferred_lazy_body` / `external_symbol_available` /
  `pending_funcs`) — analogous to the post-resolve body-availability check (964). If any ref is
  un-emittable → reject the pattern (error node / fallback). This makes the eligibility extension
  **SAFE** (clean fallback, no breakage) and de-risks B/C/D (each can be attempted incrementally,
  falling back until complete).

---

## 7. EXECUTION PLAN — ordered, each its own gated commit

> Recommended order: **A → B → (C, D)**. A makes the extension safe (no hits yet but no breakage);
> B/C/D convert specific bodies to hits. Each slice: build `make -j4 -C src`; gate per §8; commit
> only when green. The env-gate keeps flag-off byte-identical throughout.

**Slice A — SAFE eligibility widening (foundation).**
- Apply `tmp/construction-deferral-guard-WIP.patch` (eligibility ext +
  `tsubst_has_unqualified_call_pack_expansion` + the §6.1 guard).
- ADD the §6.4 completeness check (un-emittable symbol ref ⇒ fallback).
- **Expected outcome:** the cluster bodies become eligible and **fall back cleanly** (no SIGSEGV, no
  undefined-symbol MIR error). Some simple bodies (e.g. `_M_create_node`) MAY become hits.
- **Gate:** flag-on 670 + flag-off 670; reducers (`vec_recurse`/`rbtree_recurse`) exit 0; no new
  crashes; engagement ≥ baseline (22/13, 12/3); no `[why:]` shape regresses to a worse state.

**Slice B — construction deferral-to-hit.** Defer dependent-arg constructions (don't fall back);
re-lower at `copy_cir_subtree` with concrete types (reuse the placement-new deferral shape, §3.5).
**Gate:** the bodies whose only blocker was the construction become hits; flag-on/off 670; tsubst
output runtime-correct (the flag-on suite RUNS the code).

**Slice C — ADL free-operator** (`vector::_M_realloc_insert`). Route `__gnu_cxx::operator-` through
`resolve_copied_dependent_call` (model `1d69ee40`). **Gate:** `_M_realloc_insert` hit; 670/670.

**Slice D — rebind member-type** (`_Rb_tree` node family). Dependent member-type resolution in
`subst_datadef`. **Gate:** `_Rb_tree::_M_construct_node`/`_M_create_node`/… hits; 670/670.

(Out of this cluster, still on the worklist: `basic_string::_M_construct` — needs a **local class**
in a tsubst'd body, the `_Guard` RAII struct, a separate capability; `pair` piecewise ctors — need
**multi-pack + non-type index packs**.)

---

## 8. REPRODUCTION & VALIDATION

- **Reducers:** `tmp/vec_recurse.mad` (vector<string>, the crash), `tmp/rbtree_recurse.mad`
  (map<string,int>, the rebind undefined-symbol), `tmp/veci_recurse.mad` (vector<int>, no-crash
  control). With the WIP patch: vec_recurse SIGSEGVs (without the guard) / undefined-symbol (with it).
- **Build:** `make -j4 -C src` (serial = 10+ min). -O0 dev default. NEVER `CXX="ccache clang++"`
  (breaks `gen_sys_includes.sh`). NEVER pipe `make` through `tail` (masks errors + fakes exit 0).
- **gdb:** the binary has **no `-g`** (can't read locals) — either rebuild with
  `CXXFLAGS="-std=c++11 -Wall -O0 -g"`, or use targeted env-gated `fprintf` (the `MADC_TRACE_OAA`
  pattern: print at the boundary, `abort()` past a depth cap to get clean output).
- **Gates (per slice, correctness only — never perf-gate):**
  - flag-off `make -C src fulltest` → 670/0/0/18 + drift gates green (production unchanged).
  - flag-on `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` → 670/0/0/18 (runtime-correctness of
    newly-covered bodies — the suite RUNS the JIT'd code).
  - engagement: `MADC_XTEST_DEP_PARSE=1 bin/madc --show-stats <t>.mad | grep -E 'tsubst bodies|\[why:'`.
  - torture: byte-identical BY CONSTRUCTION (flag-on-only change). `--emit=c11` differs flag-on vs
    flag-off normally (different node provenance) — the gate is `--emit` byte-identical vs the PRIOR
    COMMIT, not flag-on-vs-flag-off.

---

## 9. SETTLED — do not re-litigate

- **Do NOT re-attempt eligibility widening before Slice A's completeness check** — it SIGSEGVs
  (without the guard) or emits undefined-symbol MIR errors (with the guard but no completeness
  check). Both proven this session.
- The construction guard (§6.1) is **correct but insufficient alone** — it converts crash→fallback;
  hits need B/C/D.
- The env-gate (`MADC_XTEST_DEP_PARSE`) makes every tsubst change production-safe; flag-off is
  byte-identical. The flag-on byte-identical harness is THE validator until re-parse is deleted.
- **No callee-name / shape hardcoding** (Rule #7) — re-resolve via the data-driven predicate path;
  the `[why:]` diagnostic's `== "<name>"`-free design is the tell.
- The whole remaining container tail is **uniformly deep** (this 4-capability stack + local-class +
  multi-pack). There is no bounded one-commit win left after `1d69ee40`. This is Codex-grade grind
  work; do it slice-by-slice with the per-slice gate, or hand to Codex with this doc.

---

## 10. APPENDIX — commits this session

`6570a849` diag worklist · `560cf165` retarget · `1d69ee40` **system-header free-call re-resolve
(landed hit)** · `3e950c69`/`54c6d85f`/`8deea8b6` keystone root-cause docs · `3692a172` wall-stack
doc · (this file). All experiments reverted; clean baseline preserved.
