# Plan — fix the `_Rb_tree` "incompatible argument type for pointer type parameter" warning

**Date:** 2026-06-23 · **Branch:** `feature/front-end-performance-claude` · **Status:** OPEN (own slice)

> ⛔ A WARNING IS A BUG. User directive (2026-06-23): "even if the output is correct, warnings
> are a real indication that something is not quite right" — **fix it, do not suppress it.**

## READ-CHECK (answer before acting)
1. Where is the warning emitted? → **c2mir** `c2mir.c:8508` (the `else` branch), on an `N_CALL`.
2. What does it mean? → madc handed c2mir a **call whose pointer argument has a pointee type
   incompatible with the parameter's pointer type**, and the arg IS a pointer (`right->mode ==
   TM_PTR`) → emitted as a **warning** (would be an error if not a pointer / under `-pedantic`).
3. So whose bug is it? → **madc's** (the CIR builder emits a mis-typed pointer argument). c2mir is
   correct to flag it. Fix at the deepest madc layer; gcc/clang are the oracle.
4. May I silence it (downgrade c2mir, `void*` cast, pragma)? → **NO.** That hides a real type
   mismatch. Insert the conversion / fix the node-pointer type that gcc/clang use.
5. Is it from the token-arena work? → **NO.** Pre-existing; the arena/`origin_id` commits don't
   touch type lowering. Likely the "residual cosmetic derived→base node-ptr warning" noted in the
   vector<T*>/map campaign — confirm and supersede that note.

## Symptom
`bin/madc --show-stats --no-embedded-headers --std=c++17 tests/testsubscript.mad` prints **20×**:
```
/usr/include/c++/13/bits/stl_tree.h:427:18: warning -- incompatible argument type for pointer type parameter
```
Output is correct; the warning is not. `stl_tree.h:427` is the `class _Rb_tree` template (the
std::map/std::set backbone) — so this is on the map/set instantiation path that `testsubscript`
exercises. 20× ≈ once per relevant instantiation.

## Mechanism (verified)
`c2mir.c:8489-8514` classifies a pointer-target assignment/arg/return. The three sub-cases:
- compatible pointer types → ok / sign-diff note;
- integer without cast → "using integer without cast…";
- **else (our case)** → "incompatible argument type for pointer type parameter". Because
  `right->mode == TM_PTR` (the arg is itself a pointer, just of the wrong pointee), it is a
  WARNING not an error. ⇒ madc passes a `T*` where the parameter wants a `U*` and `T*`↔`U*` are
  not compatible and madc did not insert the conversion gcc/clang would.

Most likely the `_Rb_tree` node-pointer family: `_Rb_tree_node_base*` vs `_Rb_tree_node<_Val>*`
(base↔derived node pointers), or an `_Alloc`/rebind pointer, passed to a member/helper without the
implicit base/derived pointer conversion. The reducer will localize the exact call.

## ROOT CAUSE — INVESTIGATED & CONFIRMED 2026-06-23 (HEAD a3dc969). NOT a bounded warning: TWO type-system bugs.

Reduced (`set<int>::insert` → 5 warnings) and compiled the emitted C with `c2m` to pin the exact
calls (`tmp/rb_warn_b.c` lines 1340/1347/1366 + 1345/1360). The 5 split into two distinct,
separable derived/base pointer-conversion defects. Minimal reducers in `tmp/`: `up_tmpl.mad`,
`up_ovl.mad` (run flagless — plain user classes; `up_val.mad`/`up_ref.mad` are the working controls).

### Bug A — struct→class promotion creates a DIVERGENT TWIN (3 of 5: lines 1340/1347/1366)
A `Derived*` argument bound to a `Base*` parameter where the derived→base upcast is NOT emitted, so
c2mir sees two unrelated struct pointers. `upcast_class_ptr` (cir_builder.cpp:924) is gated on
`pointee_user_class(param)` resolving to a `DataDefCLASS`. **Definitive diagnostic evidence** (a
temporary DBG in `upcast_class_ptr`, since reverted):
- working plain case (`up_val`): `take(Base*)` param → `pointee=Base bt=3 isCLASS=1` → upcast fires.
- failing template case (`up_tmpl`): SAME `take(Base*)` signature → `pointee=Base bt=1 isCLASS=0`
  (an un-promoted `DataDefSTRUCT`).
MECHANISM: madc keeps a struct a `DataDefSTRUCT` until it is *used as a base*, then
`promote_struct_base_to_class` (parser.cpp:33983) does `new DataDefCLASS`, copies the struct, and
repoints `struct_map`/`datatype_map` — **but existing `DataDefPTR::base_type` pointers to the old
struct are left dangling at the un-promoted twin.** A concrete `Derived : Base` (up_val) promotes
`Base` BEFORE `take`'s param resolves, so the param sees the class. But a **TEMPLATE** base clause
(`Node<T> : Base`, and `_Rb_tree_node<_Val> : _Rb_tree_node_base`) does NOT promote the concrete
base at definition — promotion is deferred to instantiation, AFTER earlier references (function
params) already bound to the struct. So `pointee_user_class(param)` → un-promoted struct → NULL →
no upcast. The instantiated derived class itself is fine (`nbases=1`, base recorded).
- Fix candidates (deepest layer — pick in the fix slice):
  - **F1 (eager):** when a template's base clause names a CONCRETE (non-dependent) struct, promote
    it at template-DEFINITION time (same as concrete inheritance). Simple; fixes the common
    template-before-param ordering, but NOT param-before-template (the param still twins later).
  - **F2 (identity, robust):** promotion must not leave stale references. Either promote in place
    (blocked — can't change a live object's C++ type) or add a `DataDefSTRUCT::promoted_to`
    forwarding link followed by the type-identity layer (`as_user_class` AND `is_or_derives_from`'s
    pointer compares, etc.). Handles all orderings; larger surface.
  - Do NOT fix at `as_user_class` alone (symptom layer) — pointer-identity compares elsewhere
    (`is_or_derives_from` `b.base == target`) would still see the twin.

### Bug B — overload scoring ignores derived/base pointer direction (2 of 5: lines 1345/1360)
A `Base*` argument bound to `_S_key`'s `_Link_type` (`Derived*`) parameter — only ONE `_S_key`
overload was emitted (the real header has `_S_key(_Const_Link_type)` AND `_S_key(_Const_Base_ptr)`).
`score_arg_to_param` (cir_builder.cpp:5062) pointer block (lines 5182-5193) returns `4` (VIABLE) for
ANY mismatched pointer-to-pointer pair regardless of derived/base direction, so a base-ptr arg
wrongly binds a derived-ptr param (and the correct base-ptr overload is never instantiated).
Reproduced with plain classes (`up_ovl`): emitted C binds `key(bp)` (bp=`Base*`) to `key(Derived*)`.
- Fix (deepest layer): in the pointer block, when both are class pointers and rawtypes differ, test
  `pointee_user_class` + `is_or_derives_from`: derived→base = standard conversion (score ~3, below
  exact); base→derived (and unrelated class pointers) = non-viable (-1) so the exact overload wins.
  RISK: changes overload viability the whole 669-test suite + torture depend on; memory records a
  prior naive attempt here ("upcast-temp made it WORSE") — gate hard, expect iteration.

### Why this is its own slice, not "slice 1"
Both are real correctness bugs (the warning IS a bug), but Bug A is a type-identity fix (regression
risk) and Bug B touches suite-wide overload viability (prior attempt regressed). Output is currently
CORRECT (cosmetic warning). Sequence this as a dedicated, hard-gated slice — do Bug B's reducer-first
hypothesis loop and Bug A's F1-vs-F2 decision deliberately; do not bundle with perf work.

## Method (3-oracle, fix at the deepest layer — `.claude/rules/gcc-methodology.md`)
1. **Reduce.** Build a minimal `.mad` (and equivalent `.cpp`) that instantiates the `_Rb_tree`
   member attributed to `stl_tree.h:427:18` and reproduces the single warning. Put it in `tmp/`
   with its flags (`--std=c++17 --no-embedded-headers`) per `feedback_reducers_need_flags`.
2. **Oracle.** Compile the `.cpp` with `gcc -S -fverbose-asm -O0`, `clang -S -O0`, and `c2m`; see
   the implicit pointer conversion (base↔derived adjust, or the exact node-pointer type) they use
   on that call. Compile the `.mad` with `bin/madc --emit=c11` and diff the emitted call against
   what gcc/clang expect — find the arg whose pointer type madc got wrong.
3. **Locate the deepest layer.** Trace where the CIR builder builds that call's argument node
   (the arg DataDef / pointer type / overload selection). Fix where the type is DETERMINED — the
   missing base/derived pointer conversion or the wrong node-pointer typedef resolution — NOT at
   the call site downstream, NOT by silencing c2mir.
4. **Fix + re-derive.** One reasoned change; rebuild; re-run the reducer (warning gone) then
   `testsubscript`.

## Verification (gate)
- The 20 warnings → **0** on `testsubscript`, AND on `testmap`/`testset`/`testcontainerdtor`
  (grep `bin/madc … 2>&1 | grep -c 'warning --'` == 0).
- `make -C src fulltest` → 669/0/0/18, drift gates green.
- gcc.c-torture failset byte-identical to the 51-name baseline.
- `--emit=c11` byte-identical except the corrected call's argument type.
- No new warnings anywhere; sweep the container tests for any other `warning --`.

## Non-goals (do NOT)
- Do NOT downgrade/silence the c2mir diagnostic, add `-Wno`, or a pragma.
- Do NOT `(void*)`/reinterpret-cast the argument to mute it.
- Do NOT make the parameter type weaker. The fix is the correct pointer conversion at the call's
  argument, where gcc/clang put it.
