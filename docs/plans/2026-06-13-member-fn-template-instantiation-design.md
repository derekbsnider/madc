# Design — member function-template instantiation on ODR-use (the "B" feature)

**Date:** 2026-06-13 (session 8, after part 19). **Status:** IMPLEMENTED (part 20) — static
member templates of madc-LOCAL classes instantiate on ODR-use; `tmp/w2a.mad`
(`std::vector<int> v;`) now COMPILES AND RUNS end-to-end (exit 0). Reducers `tmp/mft1.mad`
(runs both branches), `tmp/mft3.mad` (param-using body, pointer args → `sum=100`).
**Branch:** `feature/retire-embedded-shims-claude`.

---

## SESSION 9 (2026-06-14) — container cluster: testvector member-overload chain (parts 21-23)

`testvector` (the NEXT wall after w2a) reduced to `std::vector<int>::push_back` →
`__gnu_cxx::__alloc_traits<allocator<int>>::construct(impl, p, x)`. madc bound the WRONG
construct overload and emitted an undefined extern. Root-caused to FOUR stacked overload /
instantiation gaps; the first three are FIXED + gated this session, two sub-gaps remain.

- **Part 21 `39cb5d2` — [temp.func.order] for STATIC member-template calls.** A qualified
  static call (`Owner::m(args)`) resolved by name+arity BEFORE args were parsed and never
  reselected, so the first-registered overload won a tie (registration-order luck, not
  partial ordering). Fixes: (a) `findMethodOverload` gains a partial-ordering TIEBREAK
  (`member_tmpl_more_specialized`, reusing part-18 `po_pattern_match` over
  `template_param_spellings`/`template_param_names`) — `take(U*)` beats `take(P)` for a
  pointer arg; (b) `reselect_static_member_overload` (static analogue of
  `reselect_method_overload`): after `parseCallFunc` parses the args, a qualified static
  member-TEMPLATE call reselects + rebuilds the `TokenCallFunc` (tc->var is a non-rebindable
  reference) + re-runs the instantiation hooks. Wired at both qualified-call builders
  (parseExpr_dataTypeArm + parseExpr_identifierArm's `ns_resolved` join via owner/member
  out-params on `resolve_class_qualified_expression`). Test `testmemtmplorder`. Reducers
  `tmp/u4`/`tmp/u5`.
- **Part 22 `746aaa7` — class-scope `using Base::member;` imports base overloads.** It was
  SKIPPED (imported nothing), so name hiding dropped the base overloads when the derived
  declared its own. `try_import_using_base_member` (class-body parser) resolves the scope to
  a base `DataDefCLASS` (`resolve_class_type_alias`) and pushes its same-name overload(s)
  into the derived `methods`/`method_map` ([namespace.udecl]). Single-name-scope +
  single-member shape; else falls back to skip. Now overload resolution + part-21 partial
  ordering select the BASE `allocator_traits<allocator<int>>::construct` (`_Up*`) over
  `__alloc_traits`'s own SFINAE `construct` (`_Ptr`). Test `testusingbasemember`. Reducer
  `tmp/u3`.
- **Part 23 `505fd1f` — preserve parameter-pack-ness for member-template instantiation.**
  The B-feature hardcoded `ft.typeparam_is_pack = false`, so a variadic typeparam
  (`typename... _Args`) was deduced as one non-pack type → instantiation rejected. Threaded
  pack-ness: `FuncDef::template_param_is_pack` ← `Program::last_skipped_template_typeparam_is_pack`
  (set with `last_skipped_template_typeparams`) ← copied in
  `register_skipped_class_template_function` → used by `instantiate_member_fn_template_for_call`.
  A by-value variadic static member template now instantiates+runs (test
  `testvariadicmembertmpl`, `tmp/vmt5`).

**w2a/testvector arc this session:** `_S_destroy` face → (selection now correct: the base
construct is chosen) → `allocator_traits_..._construct` undefined import (pack arity now OK)
→ blocked on the TWO remaining sub-gaps below.

### Part 24 (`5a15685`) — reference params, fwd-ref packs, class-scope typedef params (sub-gaps 1-3 CLEARED)

The three sub-gaps below are FIXED + gated (integration 567/27 byte-identical +3 tests,
unit, torture 1571/51, SMAUG). tv1 advances through all four construct faces (selection,
typedef params, ref-args, pack arity) to the NEW blocker = sub-gap 4 (pack EXPANSION in the
body). Reducers tmp/refm1, tmp/vmt6, tmp/vmt7 → x=77; tests testmemtmplrefparam /
testmemtmplfwdrefpack / testmemtmpltypedefparam.

- **Reference args were passed by VALUE** (SIGSEGV): the call bound to the varargs
  declaration-only PLACEHOLDER, which has no `ref_params`. Fix: `reselect_static_member_overload`
  binds the call to the instantiated DEFINITION (`<placeholder>__mti`, real params + ref_params,
  non-varargs) for EVERY member-template static call. Selection and rebind are SEPARATED —
  findMethodOverload disambiguates multi-overload sets, but a single resolved overload still
  gets the rebind (findMethodOverload can't score a typedef-ref param, so don't gate the
  rebind on it). **Rejected alternative:** mutating the placeholder's signature corrupts
  findMethodOverload's arity gate (return-in-parameters[0] + is_varargs=false) and FLIPPED
  overload selection (caught by u3/u4 regressing) — do NOT reintroduce it.
- **Forwarding-ref packs** `Args&&...`: `fn_template_param_is_pack` accepts a `&`/`&&`-qualified
  pack core; the one-element pack substitution copies the ref-qualifier through before dropping
  the `...` (previously only dropped `...` immediately after the type ident → stray `...`).
- **Class-scope typedef params** `allocator_type&`/`Alloc&`: instantiate with the owner class on
  `class_scope_stack` (`FnTemplateDef::owner_class`, re-pushed inside
  `instantiate_fn_template_binding` after it swaps the stack to fresh) + a
  `resolve_current_class_type_alias` fallback in parseFunction's param-type resolution.

### Sub-gap 4 (`58d6c7f`) — PATTERN pack expansion in a body expression (DONE, gated)
tv1 failed at the construct body `__a.construct(__p, std::forward<_Args>(__args)...)` with
**"Missing operand"**. CHARACTERIZED (2026-06-14):
- `tmp/vmt9` `sink(args...)` (plain VALUE-pack expansion in a call arg) → **WORKS** (sink 42).
- `tmp/vmt10` `sink(std::forward<Args>(args)...)` (a PATTERN expansion: the run before `...`
  contains BOTH the TYPE pack `Args` as a template-arg `<Args>` AND the value pack `args`) →
  **"Missing operand"** (`import of undefined item C__fwd__mti`).
`instantiate_fn_template_binding`'s pack substitution handled a bare value-name `args...`
but NOT a one-element PATTERN expansion `<...Args...>(...args...)...`. The pattern's tokens
ARE substituted correctly inline by the main loop (type pack `Args`→bound type via `subst`,
value pack `args`→the bound arg), so for the single bound element the only missing step is
DROPPING the trailing `...` (which follows `)`, not an identifier, so the identifier-anchored
drop never fired and the three `tkDot` survived → stray ellipsis). **Fix:** before the
identifier handling in the substitution loop, drop a three-`tkDot` run that does NOT follow an
emitted comma — a pack-expansion ellipsis (a genuine C varargs `, ...` follows a comma). Gated
on a pack template (`!pack_param.empty()`). Test `testmemtmplpackexpand`; reducer `tmp/vmt10`.
Pre-existing off-path: multi-element packs (`tmp/vmt2`, deduction binds only one element).

### REMAINING sub-gap 5 — INSTANCE member-template instantiation (NEXT slice)
tv1 now advances past the construct body to a NEW, distinct wall:
**`import of undefined item __new_allocator_int32_t__construct`**. The allocator_traits
`construct` body calls `__a.construct(__p, std::forward<_Args>(__args)...)` — an INSTANCE
member-template call (`__a` is an object, `__new_allocator<int>`), not the STATIC
`Owner::m(args)` form the B-feature (`instantiate_member_fn_template_for_call`) +
`reselect_static_member_overload` handle. `__new_allocator` is a madc-LOCAL monomorphized
class, so its `construct` member template must instantiate on ODR-use the way the static one
does. Likely: extend the B-feature hook (or add an instance sibling) to the
member-access/`->`-call path so a declaration-only LOCAL instance member template instantiates
+ rebinds. Reduce an instance-member-template call from scratch (`obj.m<...>(args)` /
`obj.m(args)` where `m` is `template<...> void m(...)`), attribute via `--dump-cir`, fix
deepest layer, gate hard. Exported instance member templates already route through
`member_template_method_call` — confirm whether that path can be reused for LOCAL owners.

### EARLIER sub-gaps (superseded — kept for history)
1. **Forwarding-reference parameter packs** (`_Args&&... __args`): `try_instantiate_namespace_fn_template`
   deduction fails for an rvalue-ref pack (`tmp/vmt6` → undefined import; `tmp/vmt5` by-value
   pack works). Likely in the pack-binding branch (`fn_template_param_is_pack` / the `&&`
   collapse on a pack spelling).
2. **Class-scope typedef params in the instantiation context** (`allocator_type& __a`, a
   class-scope `typedef`/`using`): the B-feature routes the body through
   `try_instantiate_namespace_fn_template` with `ns=""`, so the owner class scope is lost and
   `allocator_type` doesn't resolve (`tmp/vmt7` → "Failed to find type 'Alloc' when parsing
   function parameters"). Needs the owner class pushed on `class_scope_stack` (or
   member-aware resolution) during the member-template instantiation parse.
   Also pre-existing: **multi-element packs** (`tmp/vmt2`, 2+ elements) — the deduction binds
   only zero/one pack element (single-element comment at parser.cpp ~25876); construct's pack
   is 1-element so it's not on the critical path, but `_S_construct`/emplace will need it.

Reducers (tmp/, gitignored): `u3` (using-import+order), `u4`/`u5` (member partial order),
`vmt2` (multi-elem pack), `vmt5` (by-value pack OK), `vmt6` (fwd-ref pack FAILS), `vmt7`
(typedef param FAILS), `tv1` (`vector<int>; push_back` — the live wall).

## As-built (part 20)

- **Capture:** `register_skipped_class_template_function` retains a STATIC body-bearing member
  template's decl tokens + owner on the FuncDef (`member_template_decl`, `member_template_owner`).
- **Hook:** `instantiate_member_fn_template_for_call(tc)` (parser.cpp, called at 10880 right after
  the namespace one): for a declaration-only static member-template callee with a LOCAL owner
  (`!is_externally_defined()` — exported ones keep `member_template_method_call`), build a one-shot
  `FnTemplateDef` from the retained body (strip `static`, rename the declarator to a DISTINCT
  `<call-sym>__mti` so it keeps its real params instead of colliding with the varargs placeholder),
  and instantiate via the shared `try_instantiate_namespace_fn_template` → `instantiate_fn_template_binding`.
- **Call→def binding (the crux, resolved):** `tc->var` is a `Variable &` (NOT rebindable), and the
  late-materialized (lib_funcs) definition emits its raw var name (not `func_emit_name`), so aliasing
  the *definition* fails. Instead alias the *call*: set the PLACEHOLDER FuncDef's `local_emit_name =
  inst_name` (the placeholder is `tc->var.type`; `call_emit_symbol` emits `local_emit_name`). Call and
  unique-named definition now emit the same symbol; the placeholder's extern + the real definition
  link.

## Known limitations (separate gaps, NOT B bugs — all gate-clean)

- **Array→pointer decay in deduction**: an ARRAY arg (`a`, `a+4`) deduces `It=int` not `int*`
  (`tmp/mft2.mad` → "cannot dereference non-pointer"). Pointer args work (`tmp/mft3.mad`). Separate
  pre-existing deduction-decay gap.
- **Multi-type instantiation of ONE static member template**: a single `local_emit_name` alias slot
  per placeholder (first instantiation wins). A second DISTINCT type would need per-type call
  overload re-selection (like free fns). Rare in the cleared corpus.
- **Non-static member templates** (this-taking): out of scope; the hook is gated to `vfSTATIC`.
- **Trait canonicalization of template-id type args** (`__has_trivial_destructor(Aux<false>)`): the
  Phase-1 evaluator's `read_local_type_operand` doesn't resolve template-id types (`tmp/mft1.mad`
  prints GENERIC vs g++ TRIVIAL). Separate Phase-1 follow-up.

---

**Original design (below) — superseded by As-built; the rebind path it proposed was infeasible
(`tc->var` is a reference), corrected to the call-alias above.**

## The gap

A class's **static member FUNCTION template** is not instantiated on ODR-use when the owner is a
madc-LOCAL monomorphized class. After part-19 canonicalization, w2a selects
`_Destroy_aux<true>` (= local `struct _Destroy_aux_1`) and calls its
`__destroy<int*>` member template — but madc emits a bare undefined extern
`void _Destroy_aux_1____destroy();` → `MIR error: import of undefined item _Destroy_aux_1____destroy`.

Real shape (`/usr/include/c++/13/bits/stl_construct.h:168`):
```cpp
template<> struct _Destroy_aux<true> {
    template<typename _ForwardIterator>
    static void __destroy(_ForwardIterator, _ForwardIterator) { }   // EMPTY (trivial case)
};
```
Pure-user reducer `tmp/mft1.mad`: `template<bool> struct Aux { template<typename It> static void
destroy(It,It){...} }; template<> struct Aux<true>{ template<typename It> static void destroy(It,It){...} };
Aux<__has_trivial_destructor(int)>::destroy(a,a+3);` → madc: `import of undefined item Aux_1__destroy`;
g++: runs the spec body.

## Current machinery (traced)

- **`register_skipped_class_template_function`** (parser.cpp ~26771): a member template is parsed
  declaration-only — records a varargs placeholder `FuncDef` (`is_member_template=true`,
  `declaration_only=true`, `is_varargs=true`, `template_param_names`, `template_param_spellings`,
  `template_return_spelling`) under `parse_id = owner->name + "__" + name` (e.g. `Aux_1__destroy`),
  and pushes it into `owner->methods`/`method_map`. **The BODY tokens are NOT retained.**
- **`member_template_method_call`** (cir_builder.cpp:5859, called at 3701): the ONLY existing
  instantiation path — but it is gated `if (!owner->is_externally_defined() &&
  !owner->is_extern_template_instantiated) return NULL;` (line 5870). It binds to an **Itanium
  mangled external symbol** (libstdc++ export). For a LOCAL owner it returns NULL → the call falls
  through to the default emission = the bare-extern call to `Aux_1__destroy`. **This is the bug:** a
  local class's member template needs LOCAL body instantiation, not an external symbol.
- **Reuse target — the free-fn-template path** (the correct model): bodies are retained into
  `fn_template_map[ns::name]` by `retain_namespace_fn_template_body` (parser.cpp ~25345);
  `instantiate_namespace_fn_template_for_call(tc)` (called at parser.cpp:10880, parse-time) →
  `try_instantiate_namespace_fn_template` (deduces via `fn_template_deduce_param`, DataDef-based) →
  `instantiate_fn_template_binding` (~25949: substitutes bound types into the body decl tokens,
  injects, `parseStatement` re-parses the concrete definition, which registers itself; memoized in
  `fn_template_instantiated`). The instantiated FuncDef is emitted by the cir fixpoint on ODR-use
  (the p17 `materialize_and_lower` drain).

## Design (parse-time, maximal reuse)

1. **Capture the body.** In `register_skipped_class_template_function`, when the decl has a body AND
   the owner is (or can become) a local instantiation, retain an `FnTemplateDef`-shaped record
   (decl tokens + typeparams + is_static + the owner) in a NEW `member_fn_template_map` keyed by
   `owner-identity + "::" + name`. Mirror `retain_namespace_fn_template_body`. (Low-risk; no behavior
   change until step 2 reads it.)
2. **Instantiate at parse time.** Add `instantiate_member_fn_template_for_call(tc)` right after the
   namespace one (parser.cpp:10880). When `tc->var` is a declaration-only member-template FuncDef
   whose owner is LOCAL (`!is_externally_defined()`) and a retained body exists:
   - deduce the template params from `tc->parameters` (need a **parser-side** deducer — the existing
     `bind_member_template_param` is `static` in cir_builder's anon namespace, unreachable; either
     reuse `fn_template_deduce_param`/`extract_free_signature` against the body, or lift a shared
     deducer);
   - substitute + inject + `parseStatement` the concrete definition (reuse the
     `instantiate_fn_template_binding` body-substitution core);
   - **THE CRUX — naming/binding.** The call is already bound to the varargs placeholder var
     `Aux_1__destroy`; the concrete instantiation has real params, so it will NOT "complete" the
     placeholder (param mismatch). Cleanest: parse the instantiated body as a concrete static method
     **with the owner pushed on `class_scope_stack`** so it registers as a real method of the owner,
     then **rebind `tc->var`** to the instantiated concrete var (so emission calls the defined symbol).
     Memoize per (owner, name, deduced-args) to instantiate once.
3. **Gate HARD** (this is the 202-regression area): reduce → 3-oracle attribute → byte-identical
   integration FAIL list vs the 27-baseline + torture 51-name failset + SMAUG soak, after each substep.

## Risks / open questions

- **Re-entrancy:** `instantiate_fn_template_binding` injects onto the global `tokens` stream and
  re-parses — during header parsing this is the exact pattern that SIGSEGV'd in parts 12-13 when the
  surrounding parse was suspended. The free-fn path is safe because it fires at a stable parse point
  (after the call's args are fully parsed, at 10880). The member hook fires at the SAME point, so it
  should inherit that safety — VERIFY with the gate.
- **Non-static member templates** (with a `this`): out of scope for w2a (`__destroy` is static).
  Restrict step 2 to `vfSTATIC` member templates first; non-static is a follow-up.
- **Deducer availability:** prefer reusing `extract_free_signature` + `fn_template_deduce_param`
  (parser-side, DataDef-based) over the cir-side string deducer, to avoid a cross-TU move.

## Bottom line

Not a one-liner: body capture + a parse-time member-instantiation hook + a deducer + the
naming/rebind crux, all in the most regression-sensitive area. The reuse path
(`instantiate_fn_template_binding`) is identified; the crux is registering/rebinding the instantiated
method under a name the already-bound call resolves to. Implement behind the full gate, static-only
first, w2a as the proving wall.
