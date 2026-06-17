# Plan — get `vector<T*>` fully working

**Status:** active goal (set by user 2026-06-17). Branch
`feature/retire-embedded-shims-claude`. Task #34. This is the SECOND clause of the /goal
("complete first-class-references THEN vector<T*>"). First-class-references is now COMPLETE
(`docs/plans/2026-06-17-first-class-references.md`, Phases 1/2/4 @30236ee) — it was a
prerequisite but is not sufficient; this plan owns the rest.

## Goal

`std::vector<T*>` compiles and runs for both class-pointer (`vector<A*>`) and
primitive-pointer (`vector<int*>`) element types — `testvectorptr`, `testsubscriptarrow`,
and the pointer cases in `testsubscript` go green, with no regression to the rest of the
suite (baseline 633/7/18).

## Current state (re-verified live, 2026-06-17 @30236ee)

- WORKS: `vector<int>` (`tmp/wbint.mad` → 11), `vector<string>`, `vector<object>`.
- NOT WORKING: `vector<T*>` — fails at c2mir codegen, NOT at parse. A partially-built
  feature, not a regression. Two INDEPENDENT remaining walls (below).
- `bin/madc tests/testvectorptr.mad` → 4 c2mir check errors:
  - `testvectorptr.mad:32-34` "lvalue required as unary & operand" (×3) — **Wall 2**, the
    prvalue `push_back(new B())` section (lines 31-35).
  - `stl_vector.h:428:54` "function return type is incomplete" — **Wall 1**, shared by the
    lvalue `push_back(p)` section (lines 20-28) too.
  - `stl_iterator.h:1449` "incompatible return-expr type" (move_iterator) — SECONDARY, not
    the root (see below).

## What is already DONE toward this (do NOT re-investigate)

- `50775a8` render pointer-to-pointer `A**` data members at full depth.
- `d9c0154` emit deduced dependent-member-probe args as `TokenDataType` not `TokenIdent`
  (fixed `rebind<A*>` instantiation + `w[0]->v` operator-`->`).
- **`39160c0` (this session) — the OLD ROOT of this plan, now FIXED + VERIFIED.** The old
  blocker chain bottomed out at "a class template that INHERITS FROM A TYPE PARAMETER
  (`template<typename B> struct X : B {}`) does not link the concrete base on instantiation,
  so inherited static `::value` resolves to 0" → `__and_<_B1>::value`==0 →
  `enable_if_t<__and_<…>>` never folds. `39160c0` made `TokenCLASS::parse`'s base-clause loop
  ACCEPT a substituted `TokenDataType` base (a class/struct `ttDataType` is exactly what
  type-param substitution produces; it was previously rejected by
  `is_contextual_identifier_token`). VERIFIED at HEAD: `tmp/inh2.cpp` 3-oracle now prints
  `8 8` (was `8 0`) — `ParamInh<tt>::value` folds like the concrete base. So `__and_<…>::value`
  resolves now.
- **Phase 1 (first-class refs)** fixed the forwarding-ref `_Args&&` over a pointer element:
  it renders `A**` (was `A***`), so the old "step 5" downstream `A***`-vs-`A**` mismatch is
  gone.

## Wall 1 — SFINAE trait-fold is CONTEXT-DEPENDENT (PRIMARY, shared)

The param-base `::value` now resolves (above), but `_S_construct`'s return type
`enable_if_t<__and_<__has_construct<_Alloc, _Tp*, _Args...>>::value, void>` STILL stays an
opaque undefined struct *in the full vector context* — even though the SAME `_S_construct`
folds to a concrete type in isolation:
- `tmp/ac1.cpp` (WORKS): isolated `allocator_traits<allocator<A*>>::construct` — `_S_construct`
  returns concrete `struct A *`; the enable_if struct never appears
  (`grep __enable_if_t___and tmp/ac1.c` = 0).
- `tmp/wb1.mad` (FAILS): full `vector<A*>` — `_S_construct` returns
  `struct __enable_if_t___and____has_construct_..._void`, USED but never DEFINED (0
  definitions) → "function return type is incomplete".

So the trait folds in one instantiation context and stays opaque in another. NOT references,
NOT `operator->`, NOT `move_iterator` (all isolate-PASS: `tmp/arr1..3.cpp`, `tmp/mi1.cpp`;
the `stl_iterator.h:1449` warning is real but SECONDARY). Converges with campaign walls #22
(`if constexpr` fold) / #26 (`__is_invocable`) / GOAL task #34.

Trait chain to make fold reliably:
- `__has_construct<_Alloc, _Pointer, _Args...>` = `decltype(__construct_helper<…>::__test<…>(0))`
  (a decltype-overload-SFINAE probe).
- `__and_<_Bn...>` = `decltype(__detail::__and_fn<_Bn...>(0))`.
- `enable_if_t<bool, T>` = `enable_if<bool,T>::type`.

**Investigation hypotheses (reduce from `tmp/wb1.mad`, 3-oracle each):**
1. **Instantiation ORDER / caching.** The trait may be evaluated (+memoized) while
   `_Args`/`_Pointer` are still dependent, caching the opaque form; the later concrete use
   reads the cache. ac1 reaches `_S_construct` directly; wb1 reaches it via
   `_M_realloc_insert` → move/relocate branch — compare the two orders.
2. **decltype-SFINAE not evaluated in the move/realloc context.** `_M_realloc_insert`
   instantiates BOTH the relocate and the element-move branches (madc does not dead-branch
   `_S_use_relocate()`), so the move-context `_S_construct` (an `A*&&` arg from
   `move_iterator<A**>`) is compiled. If `__construct_helper::__test(0)` isn't evaluated for
   that arg form, `__has_construct` stays dependent → `__and_` dependent → `enable_if_t`
   opaque.
3. **Pointer-element thread-through.** Confirm `__has_construct<…, _Tp*, …>` for `_Tp*`=`A*`
   threads the pointer element as `A*` (not collapsed to integer — audit row 6b: `int*`
   collapsing through `__uninitialized_*`/`_S_relocate`).

Use `--emit=c11` + `grep __enable_if_t___and` and `--dump-cir` to see whether the trait
folded. Probe traits in TYPE position; reducer virtuals DEFINED.

## Wall 2 — prvalue `push_back(new B())` (overlaps Phase 3)

`testvectorptr:32-34` pass a PRVALUE (`new B()`, an `A*` rvalue) to `push_back`. madc binds
`push_back(const T&)` and emits `&(new B()…)` → "lvalue required as unary & operand".
Correct C++ either selects `push_back(T&&)` for a prvalue or materializes a temporary for the
const-ref bind. This is EXPRESSION VALUE-CATEGORY = **Phase 3**
(`docs/plans/2026-06-17-expr-value-category.md`), which the user ordered to run LAST. Options:
- **(A, preferred if cheap):** bounded "materialize a temporary when binding a reference to a
  prvalue" in `reference_bind_address_expr` (today it throws "must be an lvalue") — no full
  per-expression value-category, just spill a prvalue to an addressable temp.
- **(B):** pull Phase 3 forward so a prvalue faithfully selects `push_back(T&&)`.
- DECISION PENDING (user). The lvalue section (lines 20-28) does NOT need this — Wall 1 alone
  should make `push_back(p)` of a variable compile + run.

## Sequencing

1. **Wall 1 first** (shared, primary). Then verify a prvalue-free `vector<A*>` reducer
   (`A* p=new A(); v.push_back(p);`) stores + reads at stride 8.
2. **Wall 2** second, per the A/B decision.
3. `testsubscript`/`testsubscriptarrow`/`testvectorptr` converge (shared pointer-element
   threading).

## Implementation / verification protocol

- 3-oracle (g++ AND clang AND madc) on every reducer before and after.
- Probe traits in **type position** (`using X = ...; sizeof(X)`) — inline `::value` / bare
  `true_type` in a `printf` arg hits an unrelated expression-position name quirk.
- Reducers run with their real flags (`--std=c++17 --no-embedded-headers`); `.mad` tests run
  in default mode (no flags) — match the mode being diagnosed.
- `make -C src fulltest` must stay green (633/7/18) save for the container tests this flips.
- Commit with `git commit -F -` heredoc; stage files explicitly (never `git add -A` — it
  grabs the non-ours `mir-debug-support.md`).

## Relationship to other plans

- DEPENDS ON: `docs/plans/2026-06-17-first-class-references.md` (COMPLETE).
- Wall 2 overlaps: `docs/plans/2026-06-17-expr-value-category.md` (Phase 3).
- DEEP-ENGINE CONTEXT: `docs/plans/2026-06-14-template-instantiation-core-plan.md` (the
  container template-instantiation core — tsubst over the cir_node parse subtree). Wall 1's
  context-dependent fold is one face of that engine.
- map/set sibling walls (`_M_valptr`, C++20 `.contains()`) tracked in the campaign memory
  `project_retire_embedded_shims.md`, NOT here.

## Risks

- The base-clause + instantiation paths are load-bearing (every class/template). Keep changes
  narrow; lean on fulltest + the MI/vtable tests.

## Done =

`vector<A*>` and `vector<int*>` run correctly; `testvectorptr`, `testsubscriptarrow`, the
pointer part of `testsubscript` pass; fulltest green with zero regressions; reducers match
g++/clang.

## Reducers (in `tmp/`, recreate if pruned)

- `tmp/wbint.mad` — `vector<int>` control (WORKS → 11).
- `tmp/wb1.mad` — `vector<A*>` (Wall 1 reproduction; reduce from here).
- `tmp/ac1.cpp` — isolated `allocator_traits<allocator<A*>>::construct` (FOLDS — localizes
  Wall 1 to instantiation context).
- `tmp/inh2.cpp` — the OLD param-base root (now `8 8`, FIXED @39160c0 — a regression sentinel).
- `tmp/arr1..3.cpp`, `tmp/mi1.cpp` — arrow / move_iterator isolations (PASS; ruled out).
- `tests/testvectorptr.mad`, `tests/testsubscriptarrow.mad` — integration targets.
