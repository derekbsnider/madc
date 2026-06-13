# Design — member function-template instantiation on ODR-use (the "B" feature)

**Date:** 2026-06-13 (session 8, after part 19). **Status:** design, ready to implement.
**Branch:** `feature/retire-embedded-shims-claude`. **Reducer:** `tmp/mft1.mad` (library-independent),
real wall `tmp/w2a.mad`.

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
