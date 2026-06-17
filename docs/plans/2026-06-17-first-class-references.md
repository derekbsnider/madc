# Plan — First-class references (stop lowering `T&`/`T&&` to pointers in the type model)

**Status:** active design + execution (user-approved 2026-06-17 — "bite the bullet, do it
the proper way"). Branch `feature/retire-embedded-shims-claude`. Motivating bug:
`vector<T*>` (handoff §0a-ter; task #34). Supersedes the targeted "fix the one
forwarding-ref site" shim (option B) in favor of the deepest-layer fix (rule #2).

## Why (the problem this kills)

madc lowers a C++ reference to a pointer **in the type model itself**:
`DataDefREF : public DataDefPTR`, a reference renders its name as `T*`, and the two
parse-time lowering sites call `getPointerType()`. A reference and a pointer therefore
become the *same shape*; only the virtual `is_reference()` separates them, and that
**collides when the referent is itself a pointer** (`A*&` and `A**` are both name `"A**"`).

Because the distinction is erased early, every downstream site that needs it has grown
its own special case to reconstruct it from side-flags:
`operand_value_datadef` (3 branches), `fn_template_deduce_param` (amps-stripping),
`FuncDef::returns_ref`, `FuncDef::ref_params[]`, `vfREFERENCE`, `ret_is_ref`,
`returns_ref_override`, `template_return_ref`. ~90 distinct sites
(parser + cir_builder). Each new shape (forwarding-ref **over a pointer**) finds a gap
one of them didn't cover — the `A***` bug. It is **one missing abstraction patched N
times**, the textbook rule-#2 violation.

## Target (clang's model — the 6 load-bearing decisions)

From clang recon (`SemaTemplateDeduction.cpp`, `SemaType.cpp`, `Type.h`, `ASTContext.cpp`):

1. **First-class reference nodes**, distinct TypeClass — `LValueReferenceType` /
   `RValueReferenceType`, NOT a flag on the pointer type. Dispatch by kind.
2. **Expressions never have reference type** ([expr]p6). An expression's type is the
   *pointee*; reference-ness lives as a per-expression **value category**
   (`VK_LValue`/`VK_XValue`/`VK_PRValue`). The reference type lives on the *declaration*.
3. **Value category is per-expression and read during deduction.** The forwarding-ref
   rule keys on `Arg->Classify().isLValue()`, never on type shape.
4. **Reference collapsing is ONE operation in ONE place** (`BuildReferenceType`'s single
   `LValueRef = SpelledAsLValue || getAs<LValueReferenceType>()`): `& &`→`&`, `& &&`→`&`,
   `&& &`→`&`, `&& &&`→`&&`.
5. **One deduction-adjust function** (`AdjustFunctionParmAndArgTypesForDeduction`):
   strip P's top cv → if P is a ref, P = pointee (one strip) → if forwarding-ref and arg
   is lvalue, ArgType = lvalue-ref-to-A → else array/function decay. All cases, uniform.
6. **Lower references to pointers ONLY at codegen.** They survive all of Sema as
   reference nodes; `CodeGenTypes::ConvertType` maps them to `ptr` on exit.

## Madc embedding of the target

- `DataDef` gains a real reference identity distinct from `DataDefPTR`. Keep `DataDefREF`
  but make it carry an **lvalue/rvalue bit** (today `&`/`&&` are collapsed — see
  `fold_template_arg_declarator` comment at `parser.cpp:11491`). Minimal: add
  `bool rvalue` to `DataDefREF` + `is_rvalue_reference()`. (A two-subclass split
  `DataDefLREF`/`DataDefRREF` is the clang-purest form but heavier; the bit is enough
  for madc and keeps RTTI sites simple.)
- `TokenBase` gains a 3-state **value category** (`vcPRValue`/`vcLValue`/`vcXValue`),
  defaulting to a conservative value that reproduces today's behavior. Set it at the
  obvious producers (named var → lvalue; `*p` → lvalue; call → from callee return
  ref-kind; literal/arith → prvalue; `std::move`/xvalue → xvalue).
- References stay `DataDefREF` through deduction/overload/instantiation; the **single
  lowering to a pointer operand stays in cir_builder/emit** (already there: the
  `vfREFERENCE`→`N_DEREF`, ref-param→`N_ADDR`, ref-return→`N_ADDR` machinery). Those
  sites re-key from the side-channels onto `type->is_reference()`.

**Safety rail:** a `DataDefREF` renders its name as `"T*"` — identical to the pointer it
lowers to — so Phases 1–2 change in-memory *identity* without changing *emitted C*.
fulltest must stay byte-for-byte green through them; that is the regression gate.

## Phases (each ends green: `make -C src fulltest`, baseline 633/7/18)

### Phase 1 — References first-class in the type model (fixes `vector<T*>`)
- Param lowering `parser.cpp:~32268`: store `getReferenceType(&pb->definition)` (with the
  rvalue bit) instead of `getPointerType(...)`. Keep `ref_params.push_back(true)` as a
  derived mirror for now.
- Var-decl lowering `parser.cpp:~34244`: store `getReferenceType(decl_type)` instead of
  `getPointerType(decl_type)`. Keep `vfREFERENCE` as a mirror.
- Make `getReferenceType` preserve lvalue/rvalue and keep its existing collapsing.
- Confirm the deduction strip (`operand_value_datadef` / `fn_template_deduce_param`)
  now sees `is_reference()` true for `forward<A*>`'s return → `_Args=A*` → `A**`.
- **Expected:** `testvectorptr`, `testsubscriptarrow`, pointer part of `testsubscript`
  flip green; emitted C for existing tests unchanged; zero regressions.
- This is the phase that satisfies the standing goal. Commit + push.

### Phase 2 — Collapse the side-channels onto the type (the cleanup)
- Migrate the ~25 `ref_params[i]` reads + the `vfREFERENCE`/`returns_ref` reads in
  cir_builder to derive from `param_type->is_reference()` / `returns.is_reference()`.
- Make `returns_ref` a reference *return type* (`DataDefREF`) rather than a flag +
  bare-referent convention, OR keep the flag strictly as a cache derived from the type.
- Delete now-dead reconstruction branches in `operand_value_datadef`. Build with `-Wall`;
  treat `-Wunused` on the deleted web as the proof the cut was complete
  (no-parallel-implementations rule).
- Green; commit per sub-step (small, reversible).

### Phase 3 — Value category on expressions → EXTRACTED to its own plan
Moved to `docs/plans/2026-06-17-expr-value-category.md` (user decision 2026-06-17: complete
ALL of this plan, but Phase 3 is the one genuinely-additive/separable piece — a new
capability with a conservative default, NOT a half-migration — so it gets its own plan and
runs LAST, after Phases 2+4 here and after the SFINAE/`vector<T*>` work). It is NOT
"deferred to later/never": it has its own plan + a concrete start trigger. Execution order
of THIS plan is therefore **Phase 1 (done) → Phase 2 → Phase 4**.

### Phase 4 — One collapse, one deduction-adjust (final consolidation)
- Ensure all reference creation routes through `getReferenceType` (single collapse).
- Extract a single `adjust_param_and_arg_for_deduction()` mirroring clang's function;
  route `fn_template_deduce_param` / `unify_spec_pattern_arg` /
  partial-spec deduction through it. Delete scattered amps/ref logic.
- Green; this is where the "20 variations" become one.

## Phase 1 outcome + remaining vector<T*> blocker (recorded 2026-06-17)

Phase 1 landed (commit `474cc2b`), regression-free (633/7/18, emitted C unchanged).
Verified effects:
- `vector<A*>` (`tmp/wb1.mad`): c2mir errors 3 → 1. The forwarding-ref `_Args&&` param
  now renders `A**` (was `A***`) — the lost-reference-flag bug is gone.
- Isolated `allocator_traits<allocator<A*>>::construct` — copy (`tmp/ac1.cpp`) AND move
  (`tmp/ac2.cpp`) — compile and run. Hand-rolled `operator[]`/method returning `A*&`
  + `->` (`tmp/arr1..3.cpp`) all work. So the arrow path and the construct path are fine.

The SINGLE remaining `vector<T*>` blocker is NOT references and NOT the arrow — it is the
**realloc move chain for a pointer element**:
- `_M_realloc_insert` instantiates BOTH its relocate branch and its element-move branch
  (madc does not dead-branch-eliminate the `_S_use_relocate()` `if`), so the move branch
  is always compiled.
- The move branch uses `move_iterator<A**>`. `move_iterator::reference` =
  `iter_rvalue_reference_t<_Iterator>` (C++17: `conditional<is_reference<iterator_traits<
  A**>::reference>, remove_reference_t<...>&&, ...>` → `A*&&`). madc computes this
  reference-type chain WRONG for a pointer iterator → `move_iterator<A**>::operator*`
  has a return-expr/return-type mismatch (warning at `stl_iterator.h:1449`).
- That feeds the move-context `_S_construct(alloc, A**, A*&&)`, whose
  `enable_if_t<__and_<__has_construct<...>>::value, void>` then fails to fold → emitted as
  an incomplete struct → "function return type is incomplete" at `stl_vector.h:428`
  (the sole remaining c2mir error). NB: the SAME `_S_construct` folds fine in isolation
  (ac1/ac2) — only the move_iterator-sourced argument breaks it.

So the next work item is the **`move_iterator<ptr>::reference` type chain for a pointer
iterator** (a trait/typedef + return-reference fix). It CONVERGES with Phase 2's
return-reference leg (move_iterator::operator* IS a method returning a reference), and with
the SFINAE-engine walls (#22/#26) + `__and_`'s `decltype(__and_fn(0))`.

### ⚠ CORRECTION (2026-06-17, later): move_iterator is a RED HERRING — real blocker is the SFINAE fold

After saving the move_iterator recipe below, deeper reduction REFUTED it as the root:
- `tmp/mi1.cpp` — the move_iterator `reference` typedef chain (`iterator_traits<A**>::reference`
  → `conditional<is_reference<A*&>, remove_reference_t<A*&>&&, ...>` → `A*&&`) + `operator*`,
  in ISOLATION — **compiles and runs in madc** (prints 7). So the typedef chain is fine.
- The REAL difference is the `_S_construct` RETURN TYPE folding, context-dependently:
  - `tmp/ac1.cpp` (works): `_S_construct` returns a concrete `struct A *` — enable_if struct
    NEVER appears (`grep __enable_if_t___and` = 0).
  - `tmp/wb1.mad` (fails): `_S_construct` returns `struct __enable_if_t___and____has_construct
    _A__A________value_void` which is USED but **never defined** (0 definitions) → incomplete
    type → the lone c2mir error.
- i.e. the SAME `enable_if_t<__and_<__has_construct<...>>::value, void>` folds to a concrete
  type in one context and stays an OPAQUE undefined struct in another. The blocker is the
  **SFINAE trait-fold reliability** (`__and_` = `decltype(__detail::__and_fn<_Bn...>(0))`,
  `__has_construct` = `decltype(__construct_helper::__test(0))`, `enable_if_t`), NOT
  references and NOT move_iterator. Converges with walls #22/#26/#34.
- NEXT (for the vector<T*> phase, AFTER the plan): make `__and_`/`enable_if_t`/`__has_construct`
  fold reliably to `void` for the pointer-element construct in the full vector context. Find
  why ac1 folds (to A*, itself suspicious) but wb1 leaves it opaque — likely an
  instantiation-ORDER / caching issue in the trait engine, or the decltype-overload-SFINAE of
  `__and_fn`/`__construct_helper::__test` not evaluating in the move/realloc context.
- The move_iterator recipe below is RETAINED for reference only (the warning at
  stl_iterator.h:1449 is real but secondary; do not treat it as the root).

### MOVE_ITERATOR FIX RECIPE (SECONDARY — retained; see correction above)

libstdc++ 13, C++17 path (`/usr/include/c++/13/bits/stl_iterator.h`, class `move_iterator`
~line 1449):
```
using __base_ref = typename iterator_traits<_Iterator>::reference;          // 1458
using reference  = __conditional_t< is_reference<__base_ref>::value,        // 1509-1511
                                    typename remove_reference<__base_ref>::type&&,
                                    __base_ref >;
reference operator*() const { return static_cast<reference>(*_M_current); } // 1560-1565
pointer   operator->() const { return _M_current; }                        // 1569-1571
iterator_type base() const { return _M_current; }                          // 1544-1546
```
For `_Iterator = A**` (vector<A*> realloc move chain): value_type=`A*`, so
- `iterator_traits<A**>::reference` = `A*&`  (lvalue ref to the pointer element)
- `is_reference<A*&>` = true → `reference` = `remove_reference<A*&>::type&&` = **`A*&&`**
- `operator*` returns `A*&&`; `static_cast<A*&&>(*_M_current)` where `*_M_current` is `A*&`.

SYMPTOM in madc: `move_iterator<A**>::operator*` emits return type `A**` but the return-EXPR
`static_cast<reference>(*_M_current)` is typed at a different pointer level → warning
"incompatible return-expr type in function returning a pointer" (`stl_iterator.h:1449`).
That bad `A*&&` then flows as the arg to the move-context
`_S_construct(alloc, A**, A*&&)`, whose `enable_if_t<__and_<__has_construct<...>>::value,
void>` fails to fold → incomplete return struct → the lone "function return type is
incomplete" (`stl_vector.h:428`). The SAME `_S_construct` folds in isolation (tmp/ac1/ac2)
— ONLY the move_iterator-sourced argument breaks it.

LIKELY ROOT (verify with 3-oracle reducers, TYPE position):
1. `iterator_traits<A**>::reference` for a pointer iterator — must be `A*&` (T& of value_type
   A*), not `A**`. Check the pointer-specialization of iterator_traits.
2. `remove_reference<A*&>::type` must be `A*` (strip the ref level, NOT the pointer level) —
   with first-class refs (Phase 1) `A*&` is a DataDefREF(A*) so remove_reference = base_type
   = A*. Verify it doesn't over-strip to `A`.
3. `__conditional_t<true, A*&&, ...>` → the `A*&&` arm must build DataDefREF(A*) (rvalue),
   which is exactly Phase 2's return-reference work — so do Phase 2 first, then re-test.
4. `operator*` return-expr `static_cast<A*&&>(A*&)` must type as DataDefREF(A*), matching the
   declared `reference` return.

EXPECT: once function/method RETURN references are first-class (Phase 2) AND
remove_reference/iterator_traits resolve `A*&`/`A*` correctly for the pointer iterator,
move_iterator::operator* returns a clean DataDefREF(A*), the move-context _S_construct
enable_if folds to void, and `tmp/wb1.mad` / testvectorptr / testsubscriptarrow go green.

Reducers: `tmp/wb1.mad` (full vector<A*>), `tmp/triv6/7.cpp` (is_trivial OK),
`tmp/ac1/ac2.cpp` (construct copy+move OK in isolation), `tmp/arr1-3.cpp` (arrow OK).
Probe traits in TYPE position only; give reducer virtual methods a body.

## Risks & mitigations

- **`ref_params` is load-bearing** for ~25 cir_builder sites. Mitigation: Phase 1 keeps
  it as a mirror; Phase 2 migrates readers before deleting it.
- **Mangling** reads `&&`→`O`/`&`→`R` from `param_cpp_spellings` strings, not DataDefs —
  keep the spelling in sync when the rvalue bit moves onto the type.
- **Emitted-C drift** is the canary: any Phase 1–2 change that alters emitted C for an
  existing test is a bug in the migration, not an intended change. Diff emitted C on a
  spot set (`tmp/wbint`, a ref-heavy test) before/after.
- **Value-category (Phase 3)** is the largest new surface; isolate it, default
  conservative, and gate each producer with a test.

## Done =

References are a first-class type kept distinct from pointers through deduction, overload
resolution, and instantiation; the per-site `ref_params`/`vfREFERENCE`/`returns_ref`
reconstruction is gone (one type predicate replaces it); `vector<A*>` and `vector<int*>`
run; reference collapsing and deduction-adjust each live in one place; fulltest green;
the `A***`-class of bugs cannot recur because the reference is never confused with the
pointer it lowers to.
