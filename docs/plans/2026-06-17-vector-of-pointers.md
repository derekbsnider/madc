# Plan — get `vector<T*>` fully working

**Status:** active goal (set by user 2026-06-17). Branch
`feature/retire-embedded-shims-claude`. Companion: handoff §0a-ter in
`docs/plans/2026-06-12-retire-embedded-shims-HANDOFF.md`; task #34.

## Goal

`std::vector<T*>` compiles and runs for both class-pointer (`vector<A*>`) and
primitive-pointer (`vector<int*>`) element types — `testvectorptr`,
`testsubscriptarrow`, and the pointer cases in `testsubscript` go green, with no
regression to the rest of the suite.

## Current state (verified live, 2026-06-17)

- WORKS: `vector<int>` (`2 10 20`), `vector<string>` (`2 hi yo`). Vector is fine
  for non-pointer and object element types.
- NOT WORKING: `vector<T*>` — fails at c2mir codegen, NOT at parse. It is a
  partially-built feature, not a regression.
- Already landed toward it: `50775a8` (render pointer-to-pointer `A**` data
  members at full depth) and `d9c0154` (this session — emit deduced
  dependent-member-probe args as `TokenDataType` not `TokenIdent(name)`, fixing
  the `rebind<A*>` instantiation + `w[0]->v` operator-`->`).

## The blocker chain (drilled to one root)

```
vector<T*>
  └─ allocator_traits::_S_construct return type
       enable_if_t<__and_<__has_construct<alloc, A*, A*...>>::value, void>
       → stays UNRESOLVED → emitted as an undefined/incomplete struct
       → c2mir: "function return type is incomplete" / type mismatch
  └─ enable_if_t folds for a literal/`is_same<>::value` condition (OK)
     but NOT for `__and_<...>::value`
  └─ even 1-arg `__and_<true_type>::value` == 0 (want 1)
       __and_<_B1> is literally:  struct __and_<_B1> : public _B1 {};
  └─ ★ ROOT: a class template that INHERITS FROM A TYPE PARAMETER
       (`template<typename B> struct X : B {}`) does not link the concrete base
       on instantiation, so inherited static `::value` resolves to 0.
```

3-oracle reducer `tmp/inh2.cpp` (g++/clang vs madc):
- `struct C : tt {}` (concrete base) → `C::value` = 8  ✓ works
- `template<typename B> struct X : B {}` → `X<tt>::value` → madc 0, g++/clang 8 ✗

## Root analysis

- `resolve_class_static_member_const_value` (`parser.cpp:1924`) **does** walk
  `cls->bases` / `base_class`. The bases are simply **not populated** for a
  param-base instantiation.
- The base-clause parser (`parser.cpp:22355`, `if (tn->id()==tkColon)`) is **not
  reached** for a param base during instantiation — `MADC_DEBUG_BASE_CLAUSE`
  prints nothing. Two non-exclusive causes to confirm:
  1. `X<tt>` is being instantiated **opaquely** (a type-parameter base reads as a
     "dependent surface", routing through `instantiate_opaque_template_use`
     instead of a full body parse), so the base clause is never parsed.
  2. Even if the body IS parsed, the substituted base token is a
     `TokenDataType(tt)` (from `clone_template_tokens_with_type_subst`, which
     replaces the identifier `B` with the bound type token), and the base-clause
     loop rejects it at `parser.cpp:22376` — `is_contextual_identifier_token` is
     true only for `ttIdentifier`/`ddARRAY`, not a class `ttDataType`.

## Implementation steps

1. **Confirm the instantiation path.** Add a temporary gated trace (behind a
   `#ifdef`) at `instantiate_template_use` vs `instantiate_opaque_template_use`
   for `X<tt>` (reducer `tmp/inh2.cpp`). Determine whether a type-parameter base
   forces the opaque path. (`template_has_parameter_pack` /
   `has_dependent_surface` gating near `parser.cpp:2874` are the suspects.)
2. **Fix cause (2) regardless** (it is a latent bug): the base-clause loop must
   accept a **substituted `TokenDataType` base**. At `parser.cpp:22375`, when
   `bn` is a `ttDataType` resolving to a `DataDefCLASS`, take it as the base
   directly instead of requiring `is_contextual_identifier_token`. (A class
   `ttDataType` is exactly what the type-param substitution produces.)
3. **Fix cause (1) if confirmed:** a type-parameter base alone must NOT force
   opaque instantiation once its argument is concrete — the body must be parsed
   so the substituted base is linked into `ddc->bases`. Narrow the
   dependent-surface test so a fully-substituted param base is treated as
   concrete.
4. **Verify the cascade** with the trait reducers, in TYPE position only:
   `tmp/inh.cpp`, `tmp/inh2.cpp` (root), then `tmp/en7.mad` / `tmp/en9.mad`
   (`__and_`/`enable_if_t`), then `tmp/wb1.mad` (`vector<A*>`).
5. **Separate downstream bug (after the root):** the forwarding-ref `_Args&&` of
   a POINTER element renders `A***` in `_S_construct` vs `A**` in `construct`
   (`tmp/wb1.c` lines ~3255 vs ~3354) → "incompatible argument type for pointer
   type parameter". Fix the reference-over-pointer level for a forwarding-ref
   whose deduced `_Args` is itself a pointer.
6. **Green the tests:** `testvectorptr`, `testsubscriptarrow`, the pointer part
   of `testsubscript`. `make -C src fulltest`, zero regressions.

## Verification protocol

- 3-oracle (g++ AND clang AND madc) on every reducer before and after.
- Probe traits in **type position** (`using X = ...; sizeof(X)`) — inline
  `::value` / bare `true_type` in a `printf` arg hits an unrelated
  expression-position name-resolution quirk that pollutes probes.
- Reducers run with their real flags (`--std=c++17 --no-embedded-headers`);
  the `.mad` tests run in default mode (no flags) — match the mode being
  diagnosed.
- `make -C src fulltest` must stay green (baseline 633/7) save for the container
  tests this is intended to flip.

## Risks

- The base-clause + instantiation paths are load-bearing (every class/template).
  Changes here can regress inheritance broadly — lean on fulltest + the MI/vtable
  tests, and keep the `TokenDataType`-base acceptance narrow (class types only).
- Narrowing the dependent-surface test could re-admit genuinely-dependent bases
  to the body-parse path; gate strictly on "argument fully substituted/concrete".

## Done =

`vector<A*>` and `vector<int*>` run correctly; the three container tests above
pass; fulltest green with zero regressions; reducers match g++/clang.
