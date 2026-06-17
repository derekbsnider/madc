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

### Phase 3 — Value category on expressions (the *real* forwarding-ref rule)
- Add `ExprValueKind` to `TokenBase`; set it at producers; read it in the
  forwarding-ref branch of `fn_template_deduce_param` so an lvalue arg deduces `_Args=A&`
  and an rvalue deduces `_Args=A`, per [temp.deduct.call]p3 — instead of today's
  unconditional strip.
- NOTE: in madc's collapsed-render model lvalue-ref and value emit identically, so this
  is mostly *correctness/robustness* (right deduced type for traits like
  `is_constructible`, `__invoke_result`, perfect-forwarding chains), not a new visible
  behavior for the current suite. **May be staged/deferred** if Phases 1–2 already green
  the goal and no remaining test needs it — record the gap, don't fake it.

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

So the next work item is the **`move_iterator<ptr>::reference` / `iter_rvalue_reference_t`
type chain for a pointer iterator** (a trait/typedef-resolution fix), independent of the
Phase 2–4 reference cleanup. It converges with the SFINAE-engine walls (#22/#26) and the
`__and_` `decltype(__and_fn(0))` resolution. Reducers: `tmp/wb1.mad` (full),
`tmp/triv6/7.cpp` (is_trivial OK), `tmp/ac1/ac2.cpp` (construct OK in isolation).

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
