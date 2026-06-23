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
