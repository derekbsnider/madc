# Plan of attack: type-faithful template instantiation (the container-chain core)

**Status:** DESIGN PLAN — not yet approved for implementation. The deliverable is
this document. Grounded in source recon of clang (`/workspace/llvm-clang-src`),
gcc (`/workspace/gcc/gcc/cp/pt.cc`), and madc itself (2026-06-14).

**Campaign context:** `feature/retire-embedded-shims-claude` @ `77a7983`,
integration 589/15. The clean singleton failures are exhausted; **all 15 remaining
failures are deep multi-wall chains rooted in one architectural defect** —
template instantiation loses type fidelity. This plan is the considered approach
to that core, replacing the "pick the next singleton" tactic that has run out.

---

## 1. The problem, precisely

madc instantiates C++ templates by **textual token substitution + re-parse**
(Borland monomorphization): a template's saved token stream has its
type-parameter *tokens* replaced with the concrete type's tokens, and the result
is **re-parsed** as a concrete function/class. The re-parse re-derives every
type from the substituted *tokens*. Wherever that re-derivation is lossy, the
instantiated body is mis-typed. The shared symptom set:

| Wall | Tests | Root (file:line) |
|---|---|---|
| `cannot dereference non-pointer type` | `tv1`/testvector + container chain (~10) | `deref_type_for_variable` (`parser.cpp:1277`) is purely structural on `var->type`; a substituted `_Tp* first` re-parses to base `int` without the body-side `*` re-applied → `first` is `int`, `*first` throws (`parser.cpp:11892`) |
| `std::string` by-value corrupts | testforeach2 (and any value-class through a template body) | `__retbuf` ABI chosen from the re-parsed return/param `DataDef` (`func_def` `cir_builder.cpp:10448`, `class_return_via_retbuf` `cir_builder.cpp:4191`); if `std::string` re-parses to an incomplete/`is_dependent_placeholder` class (no visible dtor) the ABI silently falls back to by-value struct return → caller/callee ABI mismatch |
| bare-array call-site `import of undefined item` | testforeach2/transform with `a, a+N` | deduction never runs: `fn_template_deduce_param` peels `*` via `dynamic_cast<DataDefPTR*>` (`parser.cpp:26263`); a `DataDefCArray` isn't a `DataDefPTR` → returns −1. `operand_value_datadef` (`parser.cpp:26191`) does no array decay |
| `Reference data members (T&)` | testset/testcontainerdtor (set/map `_Rb_tree`) | rejected outright at `parser.cpp:21932` |
| `_S_destroy` undeclared, `Missing operand` | testvector/map/refreturn/template* | downstream container-chain instantiation gaps (same engine) |
| fn-returning-fn-ptr return → `long` | for_each functor return (warning) | `func_def` (`cir_builder.cpp:10520`) renders an anonymous `DataDefFPTR` return via `type_list` (no fn-returning-fn-ptr declarator); only a typedef'd fn-ptr return renders |

The instantiation web is **~86 functions across 3 structurally separate paths**
(class templates: `instantiate_template_use` `parser.cpp:2645`; namespace/free fn:
`try_instantiate_namespace_fn_template` `parser.cpp:26386` →
`instantiate_fn_template_binding` `parser.cpp:26584`; member fn:
`instantiate_member_fn_template_for_call` `parser.cpp:27508`). They share ONE
primitive — build `std::map<string,TokenDataType*> subst`, clone tokens, splice,
push to the parse deque, re-parse — re-implemented per path. There is no shared
"substitute a type into a parse subtree" function.

---

## 2. How clang and gcc do it (the canonical model)

Both compilers do **semantic substitution over a typed tree**, never a re-parse.
The two-step shape is identical:

1. **Substitute types into DECLARATIONS first.** Each instantiated parameter/local
   is rebuilt with its concrete type and registered in a per-instantiation scope.
   - clang: `addInstantiatedParametersToScope` + `SubstType`
     (`SemaTemplateInstantiateDecl.cpp:4577`/`4596`); locals via
     `LocalInstantiationScope` + `InstantiatedLocal`.
   - gcc: `tsubst_decl` re-runs `tsubst` on the DECL's `TREE_TYPE`
     (`pt.cc:15922`) then `register_local_specialization` (`pt.cc:16410`); body
     locals via `case DECL_EXPR` → `tsubst(decl,...)` + `cp_finish_decl`
     (`pt.cc:19745`/`19821`).

2. **Rebuild each body expression by re-invoking the SAME semantic builders the
   parser uses**, so the result type is *recomputed* from now-concrete operands.
   - clang: `RebuildUnaryOperator` literally calls `Sema::BuildUnaryOp`
     (`TreeTransform.h:2688`); `*first` on an `int*` operand yields `int`.
   - gcc: `case INDIRECT_REF` does `RECUR(operand)` then `build_x_indirect_ref`
     (`pt.cc:21540-21560`); `build_x_binary_op` for `first != last`
     (`pt.cc:21768`); `finish_call_expr` for `f(*first)` (`pt.cc:22158`).

3. **Argument decay** ([temp.deduct.call]) is applied during deduction for
   by-value params: clang `AdjustFunctionParmAndArgTypesForDeduction`
   (`SemaTemplateDeduction.cpp:4014-4024`, `getDecayedType`); gcc
   `maybe_adjust_types_for_deduction` (`pt.cc:24444-24447`, array→ptr / fn→ptr).

4. **Lazy bodies:** instantiate the signature at the call site, defer the body
   (clang `PendingInstantiations`; gcc `add_pending_template` /
   `instantiate_pending_templates`). madc already has lazy bodies
   (`deferred_lazy_bodies` / `parse_deferred_lazy_body`).

**The essential property:** types are *recomputed by sema from concrete operands*,
never re-derived from text. madc's re-parse fails because after token
substitution the body's identifiers are bound by a *fresh, context-free parse*
that never re-establishes that `first` now means an `int*`-typed local.

---

## 3. The key realization

madc's re-parse is **not inherently wrong** — re-parsing is acceptable *iff the
re-parse re-runs full type resolution against a freshly-populated concrete scope*.
madc already owns the second half of the canonical model: **operator type
self-determination** (`.claude/rules/gcc-methodology.md` — operators compute
their result type from their operands). The missing half is the first: **the
re-parsed instantiated body must seat parameter/local declarations whose
`var->type` is the concrete, fully-formed substituted `DataDef`** (pointer-ness,
completeness, dtor all intact). The `subst` map already *holds* the right
`DataDef`; the loss happens when it is flattened to a token and the re-parse
reconstructs the type lossily.

This gives a **two-tier strategy**: an incremental tier that makes the re-parse
carry substituted types faithfully (clears every named wall, smallest blast
radius), and a durable tier that substitutes over the retained parse *subtree*
(`cir_node` already keeps it) and re-invokes madc's builders — the clang/gcc
model proper.

---

## 4. Tier 1 — make the re-parse type-faithful (incremental, do this first)

Each stage is independently shippable behind the full 4-gate (integration +
unit + gcc-torture 51-name failset byte-identical + SMAUG soak). Reduce every
case in `tmp/` DEFAULT-mode, 3-oracle (g++ / clang++ / stock c2m) before fixing.

### Stage 0 — array→pointer argument decay in deduction — ✅ DONE (`f03cfb4`)
- **What landed:** a bare C-array argument used as a VALUE decays to a
  pointer-to-element ([conv.array]/[temp.deduct.call]). A fixed array is a
  Variable with the `vfFIXEDARRAY` flag (`var.type` = ELEMENT type), so it types
  as `int` rather than a `DataDefCArray`; the decay is applied via
  `Program::array_decay_pointer(operand)` (handles the flag, fixed-array members
  and `DataDefCArray` uniformly) at THREE sites — by-value deduction
  (`parser.cpp` ~26557, beside the `77a7983` function→fn-ptr decay), the
  parse-time re-rank `resolved_call_funcdef` (`parser.cpp` ~26178), and the
  emit-time non-template overload ranker `call_target_funcdef`
  (`cir_builder.cpp` ~659). NOT applied inside `operand_value_datadef` itself so
  reference-to-array deduction keeps the array type.
- **Unblocked:** `std::transform(a, a+N, b, fn)` / `for_each` over bare arrays.
- **Reducer:** `tmp/tr2.mad` → "2 4 6 8"; +testtransform.mad bare-array coverage.
- **Gates:** integration 589/15 (zero regr), unit, torture 51-name byte-identical,
  SMAUG soak. **No headline flip** (as predicted) — Stage 1 is the cascade.

### Stage 1 — reference-to-pointer-param method return type (THE core) — ROOT RE-DIAGNOSED 2026-06-14
- **The original hypothesis (declarator-suffix re-application) is DISPROVEN by
  reducers.** Pointer template arguments and `T*` declarators in instantiated
  bodies ARE preserved correctly: `tmp/ptarg1.mad` (`Iter<int*>` with `It cur;
  It base(){return cur;}`) and `tmp/base1.mad` (`It{ T* cur; T* base(){...} }`)
  both emit correct C (`int *cur`, `base` returns `int*`) and run correctly.
  (NB: madc's process exit code is RUN-SUCCESS, not `main()`'s return — verify
  reducer correctness via `cout`/`--emit=c11`, never `$?`. Cost ~an hour here.)
- **The VERIFIED root** (traced on `tmp/tv1.mad` with `MADC_DEBUG_NS_RESOLVE` +
  `MADC_DEBUG_FNTPL`): a class-template method whose return type is a **reference
  to a pointer-typed template parameter** — `It& base()` / `const It& base()
  const` with `It = int*`, i.e. exactly `__gnu_cxx::__normal_iterator::base()`
  (returns `const _Iterator&`) — has its **declaration-side** return type
  recorded as the SCALAR `int32_t`, even though the lazy DEFINITION emits the
  correct `int**` (reference-to-`int*`). So at a template-argument-DEDUCTION site,
  `operand_value_datadef(__position.base())` reads the bound (declaration) method's
  `returns` = `int32_t`, and `_InIt`/`T*` deduction against a scalar fails →
  the param is left unbound → falls back to the `int64_t` default → that default
  cascades through `__uninitialized_move_if_noexcept_a` →
  `__uninitialized_copy_a` → `uninitialized_copy`, whose body
  `using _From = decltype(*__first);` then throws **`cannot dereference
  non-pointer type`** (the reported `stl_uninitialized.h:179`; the tv1 error
  line/col is the monomorphization origin, not the real site).
- **Reducers (in `tmp/`, DEFAULT mode):** `tmp/ref2.mad` / `tmp/ref5.mad` — a
  namespace class template `W<It>{ It cur; const It& base() const {...} }` (or
  non-const `It&`) + a namespace fn template `sink(T* first, T* last)`; calling
  `ns::sink(w.base(), w.base())` with `W<int*>` reproduces `import of undefined
  __ns_ns_sink` and `FNTPL deduce ns::sink param[0] sp='T*' arg_dd=int32_t`.
  `tmp/ref3.mad` (`int* p = w.base();`) proves the SAME call types correctly as
  `int*` in direct-assignment context — so the type IS recoverable; the bug is
  the declaration-side return-type recording read by the deduction path. The
  `const` is NOT the trigger (`It&` fails identically). A free (non-namespace)
  `sink` hits the SEPARATE global-free-fn-template gap (`use of undeclared
  identifier`), so reduce inside a namespace.
- **Where the fix likely belongs:** the class-template method-instantiation path
  that records a method's return-type `DataDef` on its DECLARATION/placeholder
  FuncDef when the class is monomorphized — a `T&`/`const T&` return where `T`
  binds to a POINTER must record `getReferenceType(int*)` (a `DataDefREF` over
  `int*`, which `operand_value_datadef` already unwraps to `int*`), not the
  scalar base. `getReferenceType` itself is correct (`parser.cpp:10540`); the
  loss is upstream, in how `It&` is resolved/substituted for the declaration's
  return at instantiation time (the definition resolves it correctly later, so
  the decl and def diverge). Confirm with the emit-C-vs-bound-FuncDef comparison.
- **Unblocks:** the `cannot dereference non-pointer type` class → the
  `std::vector` container chain (`tv1` sub-gap 13 and the ~10 dependent tests).
- **Risk:** medium — touches class-template method instantiation; gate hard.
  Highest-leverage single change in the plan. **NOT YET IMPLEMENTED** — root
  re-diagnosed; the original Stage-2 (iterator-proxy deref-time safety net) may
  become unnecessary or change shape once the decl-side return type is correct.

### Stage 2 — deref-time safety net for iterator-proxy classes
- **The fix:** in `deref_type_for_variable` (`parser.cpp:1277`) and
  `effective_pointer_type_for_member_access` (`parser.cpp:1232`), when `var->type`
  is a class with an `operator*` / `pointer` member typedef (an iterator), recover
  the pointee type semantically instead of throwing. Catches `*it` where `it` is a
  class iterator (not a raw pointer) — the libstdc++ `__normal_iterator` case.
- **Risk:** low-medium; additive (only fires where it currently throws).

### Stage 3 — complete-class identity for value-class params/returns (string ABI)
- **The fix:** ensure the re-parsed instantiated body resolves `std::string` (and
  any value class) to the COMPLETE `DataDefCLASS` (with dtor/copy-ctor visible),
  not a forward `is_dependent_placeholder` (`parser.cpp:2868`). Then
  `class_return_via_retbuf` (`cir_builder.cpp:4191`) and the by-value param copy
  path (`cir_builder.cpp:4220-4250`) select the correct retbuf ABI. Likely an
  instantiation-ordering fix (resolve/complete the class before the body's ABI is
  emitted), or a re-resolution of placeholder member types at materialization.
- **Unblocks:** `std::string` by-value through a template body (fe14 corruption);
  a prerequisite for testforeach2 and much of the container chain with string
  elements.
- **Risk:** medium; ABI correctness — verify against g++ emitted ABI.

### Stage 4 — reference data members (`T& member;`)
- **The fix:** at `parser.cpp:21932`, instead of rejecting, store the member as a
  pointer (`getReferenceType`, refs already modeled as `DataDefPTR`), require
  constructor init-list binding (`m = &arg`; a reference member has no default
  init and cannot be reseated), and auto-deref on member access. Machinery exists
  piecewise (refs-as-pointers, ctor init-lists).
- **Unblocks:** `std::set`/`std::map` `_Rb_tree` (testset/testcontainerdtor)
  — the next wall after `alignas` (`9b7b944`).
- **Risk:** medium; new member kind.

### Stage 5 — residual container walls + fn-returning-fn-ptr return
- `_S_destroy` (`allocator_traits<...>::_S_destroy` static-member wall),
  `Missing operand` in instantiated bodies — drive from the container reducers
  as Stages 1-3 land (several likely fall out once typing is faithful).
- **Bounded standalone:** render an anonymous `DataDefFPTR` return in `func_def`
  (`cir_builder.cpp:10520`) via a fn-returning-fn-ptr declarator (the
  `R (*g(args))(p)` form) so `std::for_each`'s functor return stops rendering as
  `long` (removes the c2mir warning). Independent of the above; do when convenient.

---

## 5. Tier 2 — typed substitution over the parse subtree (durable, later)

The architecturally faithful endpoint, matching clang/gcc: substitute the
type-parameter *nodes* in the retained parse subtree (every `cir_node` already
carries its `TokenBase` parse subtree + tokens per `.claude/rules/mc11-ir.md`),
and re-invoke madc's semantic builders (`TokenX::compile`/`operand`,
`CirBuilder`) on the substituted operands so result types are recomputed at
concrete operand types — a `tsubst`/`TreeTransform` analogue. This replaces the
per-path clone-and-reparse loops with ONE typed-substitution pass, retiring the
~86-function web's duplication.

**Do NOT start Tier 2 until Tier 1 has cleared the named walls** — Tier 1 is the
unblock-the-campaign work; Tier 2 is a refactor that should be justified by the
recurring cost of Tier-1 patches, and scoped as its own multi-session effort
(it touches the instantiation core for every template kind). When undertaken,
build it as a single `substitute_parse_subtree(node, subst)` primitive that all
three paths adopt, and migrate one path at a time behind a guard, gating each.

---

## 6. Invariants & process
- Every stage: reduce in `tmp/` DEFAULT mode (no flags = real headers); 3-oracle
  (g++ AND clang++ AND stock `/workspace/mir/c2m`); fix the DEEPEST layer; full
  4-gate (`bash scripts/run_tests.sh`; `make -C src test`;
  `python3 scripts/run_gcc_testsuite.py` failset byte-identical to
  `docs/parity/torture-failset-current.txt`; SMAUG soak exit 124 + ready).
- No shims, no per-test special-casing, no hardcoded std:: names (the campaign
  contract: `scripts/check-no-std-hardcoding.sh`).
- Don't ignore compiler warnings — analyze every one (the fn-ptr-return warning
  is itself a Stage-5 item, not noise).
- Branch stays LOCAL (`-claude`); develop untouched; commit per stage with the
  4-gate evidence in the message.

## 7. Recommended order & expected payoff
~~Stage 0 (quick, transform/for_each-on-arrays)~~ ✅ DONE (`f03cfb4`) → **Stage 1
(the core — unblocks the ~10-test container chain) ← NEXT** → Stage 2 → Stage 3
(string ABI) → Stage 4 (set/map) → Stage 5 (residuals + fn-ptr-return warning).
Stage 1 is the single highest-leverage change; Stages 0/2/4 are contained;
Stage 3 is the riskiest (ABI). Realistic expectation: Tier 1 can flip most of the
15 remaining failures; the count did NOT move on Stage 0 alone (it advanced the
reducer, like the sub-gaps), but Stage 1 should cascade.
