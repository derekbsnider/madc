# _Rb_tree incompatible-pointer warnings — findings (2026-06-29)

c2mir warns "incompatible argument type for pointer type parameter" (c2mir.c:8509) on
container code. **Production-wide, pre-existing** (testsubscript flag-off 20, flag-on 24 before
any fix). Source: madc emits `N_CALL` args whose pointer pointee type mismatches the parameter's.

## Landed
**(a) `08642457` — derived→base ctor-arg upcast.** Constructor-arg coercion (`class_ctor_call_addr`,
`class_ctor_call`) didn't apply `upcast_class_ptr` like the general call path (3608). Fixed; verified
670/670 both flags + torture by construction. testsubscript 20→18, mapmin 10→9. Clears the
derived→base **ctor-arg** upcast class.

## Remaining cluster (NOT yet fixed) — all in `_Rb_tree`
Categorized via `gcc -fsyntax-only -Wincompatible-pointer-types` on `madc --emit=c11` output of a
minimal `std::map<int,int>` (`tmp/mapmin.mad`): of 9 remaining,
- **7×** `_S_key` call: expected `_Rb_tree_node<pair>*` (derived), got `_Rb_tree_node_base*` (base).
- **2×** `pair`/iterator ctor: `Derived**` vs `Base**` pointer-to-pointer.

## The 7× `_S_key` case — TWO hypotheses DISPROVEN (do not repeat)
1. **DISPROVEN: overload-resolution picks the derived-param `_S_key` for a base arg (downcast
   viability).** Instrumented `find_method_by_callable_arity` (arity-only chooser) AND
   `score_arg_to_param`. For the REAL `_S_key`, `findMethodOverload` scores the **derived** candidate
   **5** and base **4** — because **the argument is typed `_Link_type` (derived) at selection time**,
   so selecting the derived overload is *correct for that static type*. The downcast pairing
   (arg=base, param=derived) **never reaches** `score_arg_to_param`. So this is not a selection bug.
   - Side-finding (real but NOT the cause): `score_arg_to_param`'s `adc->rawtype()==pdc->rawtype()`
     shortcut returns 5 for ANY two pointers (rawtype = storage type, equal for all pointers) — it
     cannot distinguish `Base*` from `Derived*`. A class-pointer inheritance discrimination (exact 5 /
     upcast 4 / downcast −1, checked BEFORE the rawtype shortcut) fixes synthetic reducers
     `tmp/overload_static.mad` / `tmp/overload_const.mad`. Patches saved
     (`tmp/b-part1-score_arg_to_param.patch`, `tmp/b-part2-reselect-static.patch`) but **DO NOT fix
     the real warnings and Part-2 (reselect_static for non-templates) REGRESSED a correct binding**
     (rebound base→derived when score wasn't discriminating). Not applied. Revisit only as a
     standalone correctness cleanup WITH a full torture gate — not for these warnings.
2. **DISPROVEN: `static_cast<Derived*>(basePtr)` downcast not emitting the cast.** Reducer
   `tmp/static_cast_down.mad` emits cleanly (r=7, no warning), matching gcc.

## Current best hypothesis (UNCONFIRMED — start here next)
The arg to `_S_key` has **static type derived (`_Link_type`) but is emitted as a base pointer**. The
mismatch lives inside a specific instantiated `_Rb_tree` accessor — likely `_M_begin()`/`_M_end()`
(declared returning `_Link_type` via `static_cast<_Link_type>(_M_impl._M_header._M_parent)`, a
`static_cast` of a **member-access** pointer) or the iterator `_M_node` typing. Either the member
`static_cast<_Link_type>` doesn't emit the base→derived cast, or the accessor's return value is
emitted as base where its declared return type is derived. NEXT: trace `_M_begin`'s emitted body in
`tmp/mapmin_emit.c`; reduce to a member-`static_cast`-return reducer; check return-type fidelity.

## (c) 2× `Derived**` vs `Base**`
Pointer-to-pointer at a `pair`/reference-to-node-pointer parameter — a node-pointer variable typed
`_Link_type` where the source expects `_Base_ptr` (`&var` → `Derived**`). Likely the same
node-pointer-typing root as the `_S_key` hypothesis. Address together.

## Reducers / artifacts
- `tmp/mapmin.mad` (+ `--emit=c11` → gcc `-Wincompatible-pointer-types` = the categorizer).
- `tmp/overload_static.mad`, `tmp/overload_const.mad` — synthetic overload-scoring repro (MISLEADING:
  they reproduce a *different* synthetic manifestation, not the real `_S_key` cause).
- `tmp/static_cast_down.mad` — clean (downcast emit works for a simple local).
- Saved (reverted, do-not-apply) patches: `tmp/b-part1-score_arg_to_param.patch`,
  `tmp/b-part2-reselect-static.patch`.
- Separate bug found: free-function overloads collide to one symbol ("Repeated item declaration") —
  member overloads get `__oN`, free ones don't.
