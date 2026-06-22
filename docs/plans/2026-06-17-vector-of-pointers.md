# Plan — get `vector<T*>` fully working

**Status: COMPLETE (2026-06-18).** `vector<A*>` and `vector<int*>` compile and RUN;
`testvectorptr` and `testsubscriptarrow` pass. fulltest 635/5/18 (was 633/7) — +2,
zero regressions. Wall 1 fix: `eb578af` (real-instantiate concrete-arg variadic class
templates + the iterator-invalidation SIGSEGV it exposed). Wall 2 fixes: `5a84c83`
(`new T()` prvalue spill) + `fcf8adc` (`&x` address-of prvalue spill). Branch
`feature/retire-embedded-shims-claude`, all pushed.

NOTE on the original `testsubscript` criterion below: `testsubscript` contains NO pointer
element types — it exercises `map<string,int>`/`map<string,string>`/`vector<string>`. Its
remaining failure is the **map** wall (`stl_map.h:102 incomplete struct or union`), which
this plan explicitly scopes to the separate map/set track (see "Relationship to other
plans"). So it is NOT a vector<T*> item; the "pointer part of testsubscript" was a
misread of the test's content.

Residual (cosmetic, not a regression): c2mir warns "incompatible pointer type" on a
derived→base pointer arg through the spilled temp (`vector<A*>` with `new B()`). c2mir is
strict about ALL struct-pointer conversions; gcc/clang do the implicit upcast silently and
the program runs correctly. An attempt to upcast the temp to the param's referent type
ADDED warnings (c2mir flags the explicit upcast-assign too), so the simpler form stands.

This was the SECOND clause of the /goal ("complete first-class-references THEN
vector<T*>"); first-class-references (`docs/plans/2026-06-17-first-class-references.md`,
Phases 1/2/4 @30236ee) was the prerequisite.

## Goal (ACHIEVED)

`std::vector<T*>` compiles and runs for both class-pointer (`vector<A*>`) and
primitive-pointer (`vector<int*>`) element types — `testvectorptr`, `testsubscriptarrow`
go green, with no regression to the rest of the suite (baseline 633/7/18 → 635/5/18).

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

### ROOT CAUSE CONFIRMED (2026-06-17, hypothesis 1) — on-demand nested member-alias-template expansion

Pinpointed via gated `MADC_DIAG_RET` in `skipped_template_function_return_type`
(`parser.cpp` ~29667) and a warm/ordering reducer (`tmp/probe_order.mad`):

- `_S_construct`'s return type `_Require<__has_construct<A*,A*>>` resolves (via
  `skipped_template_function_return_type` → `resolve_type_token_range` →
  `resolve_declared_type_token`, owner=`allocator_traits<allocator<A*>>` IN scope,
  ns_depth=1) to the OPAQUE incomplete struct
  `__enable_if_t___and____has_construct_A__A________value_void`. The tell: in this
  name `__has_construct<A*,A*>` is left **UNEXPANDED** — the member alias-template
  never expanded to `__construct_helper<A*,A*>::type` → `true_type` → fold to `void`.
- ALL isolated reducers FOLD correctly (0 opaque): direct `allocator_traits::construct`
  (`tmp/ac1.cpp`), hand-rolled member-alias `at<>` (`tmp/probe_member.cpp`), and even
  nested-via-`relay` (`tmp/probe_nested2.cpp`). So the trait machinery is fine.
- **The differentiator is INSTANTIATION ORDER.** `tmp/probe_order.mad` — a direct
  `allocator_traits<allocator<A*>>::construct(al, slot, q)` call placed BEFORE the
  `vector<A*>` ops — makes the "function return type is incomplete" error VANISH (0
  c2mir check errors; only a downstream MIR `undeclared func reg fp` remains, a
  SEPARATE wall). Warming materializes/memoizes `__construct_helper<A*,A*>::type`, so
  the later realloc-path `_S_construct` return folds. Plain `wb1.mad` (no warm) → 1
  check error.

So the FIX is on-demand expansion: when resolving the member alias-template-id
`__has_construct<A*,A*>` (member of the concrete `allocator_traits<allocator<A*>>`),
instantiate the nested member template FROM THE PRIMARY on demand instead of relying
on a prior direct use having materialized it. The warm-call is a DIAGNOSTIC only (can't
inject into user code) — do NOT ship it.

### FULL ROOT CAUSE (2026-06-17, traced end-to-end) — variadic class templates never body-instantiate

The opaque chain bottoms out at ONE cause:
`__has_construct<A*,A*>` = `typename __construct_helper<A*,A*>::type`. Resolving
`__construct_helper<A*,A*>` calls `instantiate_template_id` →
`instantiate_template_use` (`parser.cpp` ~2860). At line **2874**,
`template_has_parameter_pack(td.typeparam_is_pack)` is TRUE (`__construct_helper<_Tp,
_Args...>` has the `_Args...` pack), so it short-circuits to
`instantiate_opaque_template_use` (~2609) — which creates a `is_dependent_placeholder`
shell with **NO body parse** (line 2683-2691). So `using type = decltype(__test<_Alloc>
(0))` is never evaluated; `type` is never registered as a `type_aliases` entry. Back in
`resolve_typename_type_token` (~3963), `resolve_class_type_alias(owner,"type")` returns
NULL, `class_allows_opaque_member_type` is TRUE (system-header template-id), so line 3965
`materialize_dependent_member_type` makes `type` the opaque
`__construct_helper_A__A______type` → the whole `enable_if_t` stays opaque → incomplete.

WHY reducers can't repro: `instantiate_opaque_template_use` fires for EVERY pack
template, but my hand-rolled reducer classes are NOT `from_system_header`, so they take
the eager full-instantiation path elsewhere and register `type`. Real `allocator_traits`
IS a system header → opaque path. (The warm call forces a full direct instantiation that
registers `type`, hence it "fixes" wb1.)

### FIX DESIGN — real instantiation of CONCRETE-ARG variadic class templates

The body-substitution loop in `instantiate_template_use` (3196-3268) ALREADY expands a
`pack_subst[name]` (3212-3240). What's missing: (1) the 2874 short-circuit sends packs
to opaque BEFORE the real path; (2) the arg loop (2911-2968) THROWS at 2913 when args
exceed typeparams instead of absorbing a trailing pack into `pack_subst`.

Plan:
1. At 2874, only go opaque when the pack args are genuinely DEPENDENT (or body empty).
   Cleanest: let the real path handle packs and fall back to opaque if any resolved arg
   `datadef_has_unresolved_dependent_surface`. (Decide opaque AFTER arg parse.)
2. In the arg loop, when `pi` reaches the pack index (`first_template_pack_index`),
   absorb every REMAINING arg into `pack_subst[pack_name]` (as `TokenDataType*` elems)
   instead of throwing — the body loop already consumes `pack_subst`.
3. Keep non-type-pack and dependent-pack uses on the opaque path (regression safety:
   the 2874 short-circuit is load-bearing for dependent variadic uses across the suite).
4. Gate hard on `make -C src fulltest` (633/7/18) — this touches core template
   instantiation; lean on the MI/vtable/container tests.

Residual AFTER this lands (already seen in `probe_order` once Wall 1 cleared):
`stl_vector.h:428:54` "incompatible argument type for pointer type parameter" (warning)
+ MIR `undeclared func reg fp` — the next sub-wall (and Wall 2 prvalue is separate).

### IMPLEMENTATION STATUS (2026-06-17) — WIP in `git stash@{0}`, NOT committed

Implemented the fix design above and it **ELIMINATES Wall 1's incomplete-type error**
(`stl_vector.h:428 "function return type is incomplete"` is GONE) — verified on
`tmp/wb1.mad`. The change (in `git stash@{0}`, durable):
- `include/madc.h`: new `bool allow_variadic_real_inst` Program flag.
- `parser.cpp` `template_pack_real_instantiable(td)`: concrete trailing-TYPE-pack shape
  (tests `typeparam_is_type` directly — `has_non_type_params` is a MISNOMER, set true
  even for a pure type pack, line ~30736).
- `instantiate_template_use`: short-circuit to opaque is bypassed ONLY when
  `allow_variadic_real_inst && template_pack_real_instantiable`; flag consumed+cleared so
  nested body instantiations don't inherit it.
- arg loop absorbs a trailing type pack into `pack_subst` (clamp `bind_pi` to
  `pack_index`); default-fill loop treats a reached pack as a valid EMPTY pack;
  dependent-arg variadic uses still go opaque (pre-change behavior preserved).
- `resolve_typename_type_token`: scoped-sets the flag around the chain-head
  `instantiate_template_id` (member-TYPE context only — the constant-fold `__and_::value`
  path stays opaque, avoiding `__and_`/`tuple` recursive real-inst).

REMAINING (two sub-issues, why it is NOT yet committed — tree must stay green):
1. **`__construct_helper<A*,A*>::type` decltype not folding to `true_type`.** After
   real-inst, `__has_construct` resolves to NULL (was the opaque struct) — the
   `using type = decltype(__test<_Alloc>(0))` overload-SFINAE member parses but doesn't
   evaluate to `true_type`. So overload-1 `_S_construct` is SFINAE-dropped; need the
   decltype to actually fold (the `__test<allocator<A*>>(0)` overload pick).
2. **Stack-overflow recursion**: a deferred lazy body parsed during
   `CirBuilder::translate_module` real-instantiates a variadic whose alias args recurse
   unboundedly — `resolve_declared_type_token` ↔ `alias_use_args_all_concrete` ↔
   `instantiate_template_alias_use`. Needs a recursion-depth/in-progress guard before
   this is safe. (The `allow_variadic_real_inst` flag tamed the constant-fold `__and_`
   recursion, but the deferred-lazy-body member-type path still recurses.)

NEXT SESSION: `git stash apply stash@{0}`, then (a) make the decltype `::type` fold to
`true_type` in the real-instantiated `__construct_helper`, (b) add a recursion guard to
the alias/typename real-inst path. Reducers all FOLD (system-header-ness is the trigger)
— iterate on `tmp/wb1.mad` directly with `-DMADC_DIAG_RET` (the gated diags are in the
stash). Baseline to keep green: 633/7/18.

Residual AFTER Wall 1 (seen in `probe_order`): `stl_vector.h:428:54` "incompatible
argument type for pointer type parameter" (warning) + MIR `undeclared func reg fp` —
track as the next sub-wall once Wall 1's on-demand expansion lands.

**Older investigation hypotheses (now subsumed by the confirmed root above; reduce from `tmp/wb1.mad`, 3-oracle each):**
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
