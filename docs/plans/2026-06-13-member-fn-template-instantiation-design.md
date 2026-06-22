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

### Sub-gap 5 — INSTANCE member-template instantiation (DONE, see commit; gated)
tv1 advanced past the construct body to **`import of undefined item
__new_allocator_int32_t__construct`**. The allocator_traits `construct` body calls
`__a.construct(__p, std::forward<_Args>(__args)...)` — an INSTANCE member-template call (`__a`
is an OBJECT, `__new_allocator<int>`), not the STATIC `Owner::m(args)` form the B-feature
handled. `__new_allocator` is a madc-LOCAL monomorphized class, so its `construct` member
template must instantiate on ODR-use.

**Clang/gcc research (recon: `/workspace/llvm-clang-src`, `/workspace/gcc`, recon-only).**
Both canon compilers instantiate a member function template **as a method of the (specialized)
class** — static vs instance is only a storage-class flag, and a non-static method's implicit
`this` is intrinsic to its type; it is NEVER lowered to a free function with an explicit
receiver. Clang (`SemaTemplateInstantiateDecl.cpp`): `VisitCXXMethodDecl` (~2588) builds
`CXXMethodDecl::Create(…, Record, …, SC = D->isStatic()?SC_Static:SC_None, …)` — a method of
`Record` (the class); `SubstFunctionType` (4451-4460) sets `ThisContext = the class` +
`ThisTypeQuals` from the object parameter, so the body's `this` substitutes to `Class<Args>*`;
the body is instantiated under `CXXThisScopeRAII ThisScope(SemaRef, ThisContext, …)` (3645,
3725, 688) so `this`/member access resolve. GCC (`gcc/cp/pt.cc`): `instantiate_decl` does
`push_nested_class(DECL_CONTEXT(t))` and the spec keeps `DECL_CONTEXT = the class` (1666) —
same model.

**Implication for madc + the fix.** madc's STATIC path (`instantiate_member_fn_template_for_call`)
takes a shortcut — drop `static`, rename, re-parse as a FREE function (no `this`) — which is
behaviorally fine for static (no object parameter) but doesn't generalize. The clang-faithful
fix for instance members is to instantiate them **as a method of the owner** (hidden `__this`,
class-scope body), not a free function with a fake receiver param (that only works for
`this`-independent bodies and silently diverges otherwise — rejected). As-built:
- `FnTemplateDef::instance_method` flag (owner_class alone can't distinguish: the static path
  also sets owner_class for class-scope member resolution).
- `register_skipped_class_template_function` retains the body for body-bearing member templates
  regardless of static-ness (was `is_static`-gated).
- `instantiate_member_fn_template_for_call` sets `ft.instance_method = !is_static`.
- `instantiate_fn_template_binding`: when `instance_method && owner_class`, after building the
  substituted `inj` tokens, split `RET name ( params ) { body }` at the params and call
  `parseFunction(retdd, inst_name, owner_class)` (the same method-parse entry as
  `parse_deferred_lazy_body` / out-of-line ctor defs) — hidden `__this`, member resolution —
  instead of `parseStatement` (free function). Frees the unused head tokens.
- `parseCallMethod` calls `instantiate_member_fn_template_for_call(tc)` after args (mirrors
  parseCallFunc; a TokenCallMethod IS-A TokenCallFunc).
- cir_builder Pass 0.75 (extern prototypes) SKIPS member-template placeholders: an instance
  placeholder's varargs+`this` signature `(struct C *, ...)` otherwise conflicts with the
  instantiated definition `(struct C *, int*, int)` ("incompatible types of … declarations").
  The instantiated definition carries its own prototype; an exported member template is called
  through its Itanium-mangled symbol, not the placeholder name.

Tests `testmemtmplinstance` (member access via `this` proves the method model, x=35),
reducers `tmp/im1` (this-independent body, x=5), `tmp/im2` (member access, x=35). tv1 now
advances to **sub-gap 6** (placement-new in the construct body).

### Sub-gap 6 (`7202523`) — global-qualified new/delete `::new` / `::delete` (DONE, gated)
tv1's `__new_allocator<int>::construct` body is `::new((void*)__p) _Up(std::forward<_Args>(__args)...)`
and failed **`use of undeclared identifier 'new'`**. Determined GENERAL, not instantiation-specific:
unqualified `new(p) T(v)` works (`tmp/pn2`); `::new(p) T(v)` fails (`tmp/pn1`). Root: the leading-`::`
(global scope) branch of the expression parser read the token after `::` via
`contextual_identifier_name`, which maps the `new`/`delete` keywords to "new"/"delete", then looked
them up as identifiers. Fix (parser.cpp leading-`::` block): intercept a following `new`/`delete`
keyword and delegate to `TokenNEW`/`TokenDELETE::parse` (the same dispatch the unqualified form
uses); the `::` selects global operator new/delete, otherwise identical. +testplacementnewglobal.
PRE-EXISTING shared gaps (off-fix, fail for the UNqualified form too): scalar HEAP `new int(7)`
(`tmp/pn4` → "new without a class type") and CLASS placement `new(p) T()` (`tmp/pn5` → "incompatible
types in assignment to struct/union").

### Sub-gap 7 (`2e8abc1`) — placement-new through a void* address (DONE, gated)
With `::new` parsing, tv1's construct body lowered scalar placement new as `*(void*)__p = value`
(the placement expr `(void*)__p`'s own type) → c2mir **"assignment of incompatible value"** (can't
deref `void*`). Fix (cir_builder.cpp scalar placement-new branch, ~7649): cast the placement address
to the CONSTRUCTED type's pointer `(T*)addr` before the deref-store — placement new constructs a `T`
at the address regardless of the pointer's static type; a no-op when addr is already `T*` (e.g.
`new(&buf) int`). Handles `alloc_type`'s own pointer depth (levels+1). +testplacementnewvoidp
(`::new((void*)&buf) int(42)` → v=42). tv1 advances to sub-gap 8.

### sub-gap 8 — out-of-line `.tcc` member definitions — DONE (commit pending)
A member of a class template DEFINED outside the class body
(`template<class T> RET S<T>::member(...) { body }` — the `bits/*.tcc` shape) was never captured:
`instantiate_template_use` re-parses only the class BODY, and the out-of-line def is a separate
top-level template, so `TokenTEMPLATE::parse`'s non-class branch mis-registered it as a NAMESPACE
free function (`std::_M_realloc_insert`) — never called, leaving the real member an undefined import.

**Fix (deepest layer, reuses the existing lazy-body machinery):**
- **Capture** (`skipped_template_outofline_member` + `TokenTEMPLATE::parse` non-class branch): detect a
  skipped non-class template decl whose declarator name is preceded by `::` after a class-template-id
  `Class<...>` where `Class` is a registered template; store `{member_name, typeparams, decl}` keyed by
  `<defining-ns>::<class-name>` in `Program::out_of_line_member_defs`. Do NOT register it as a free fn.
- **Instantiate** (`register_outofline_member_instantiations`, called from `instantiate_template_use`
  after the class is monomorphized): substitute the def's type-params (positional to the use-site args)
  → concretes and the class-id `Class<...>` → the mangled tag, then register a `full_definition`
  `deferred_lazy_bodies[member->name]` (the SAME lazy path `parse_deferred_lazy_body` uses for
  defaulted member-template ctors). The body materializes only on ODR-use ([temp.inst]p2) — unused
  out-of-line members never instantiate. CLEAR the member's `emit_symbol`/`declaration_only` so the
  call falls through to the LOCAL emit name (== the deferred key) instead of the mangled-direct extern.
- **Return-type fix** (`parse_deferred_lazy_body` full_definition): re-parse with the member's REAL
  return type, not hardcoded `ddVOID` — `parseFunction` refreshes an already-declared funcdef's return
  from the passed type, so a non-void out-of-line member (`iterator insert(...)`) was silently rewritten
  to void. A ctor's funcdef returns ddVOID, so the defaulted-ctor caller is unchanged.

+test `testoutoflinemember` (void + non-void + this-access). Reducers `tmp/ool1` (void → 7),
`tmp/ool2` (non-void → 41), `tmp/ool_nt` (non-template control → 9).

### sub-gap 9 — out-of-line MEMBER TEMPLATE definitions — DONE (commit pending)
An out-of-line member **TEMPLATE** with a TWO-level head
(`template<class _Tp,_Alloc> template<typename... _Args> void vector<_Tp,_Alloc>::_M_realloc_insert(
iterator, _Args&&...)`, bits/vector.tcc:441) — sub-gap 8 excluded it. The fix combines sub-gap 8's
out-of-line capture with the sub-gap-5 per-call member-template instantiation path:
- **Capture** (`skipped_template_outofline_member` now REPORTS `is_member_template` instead of
  rejecting a `template`-first decl — the class-id walk-back already locates `Class<...>::member`
  correctly, the inner head has no top-level `(` and sits left of the class-id). The capture also
  records the inner (member) type-params + pack-ness via `extract_inner_template_typeparams`.
- **Attach** (`register_outofline_member_instantiations`, member-template branch): substitute the
  CLASS params (the inner params pass through) + rename the class-id → mangled tag, then transform the
  substituted `template<U...> RET <mangled>::name(params){body}` into the in-class member-decl form
  `RET name(params){body}` the sub-gap-5 consumer expects (strip the inner `template<...>` head + the
  `<mangled>::` declarator qualifier), and set it as the monomorphized member's `member_template_decl`
  (+ `member_template_owner`, `template_param_names`/`is_pack` from the inner head). The in-class
  member-template DECLARATION (re-parsed during monomorphization) already created the member as an
  `is_member_template` placeholder with an EMPTY `member_template_decl`; this fills it. On the call,
  `instantiate_member_fn_template_for_call` (sub-gap 5) instantiates the body per inner-arg binding as
  a method of the monomorphized class — no new instantiation machinery.

+test `testoutoflinemembertemplate` (void + non-void + this-access). Reducer `tmp/oot1` (→ 42). tv1
(`vector<int>::push_back`) advances PAST `_M_realloc_insert` to sub-gap 10.

### sub-gap 10 — auto-return functions with a trailing return type — DONE (commit pending)
The `'auto' requires an initializer` face was NOT a stray `auto` decl — it was an `auto`-RETURN
FUNCTION with a trailing return type: libstdc++'s cross-type `__normal_iterator operator-` returning
`decltype(__lhs.base() - __rhs.base())` (reached by `_M_realloc_insert`'s `__position - begin()`).
`parseDeclaration` treated `auto` only as a deduced VARIABLE. Fix:
- parseDeclaration: the auto-var block is guarded `&& nt->id() != tkOpBrk` — a `(` after the
  declarator (`auto f(`) means a function, so control falls through to the function path (decl_type
  stays ddAUTO).
- parseFunction: the trailing-qualifier loop CAPTURES a `-> T` (`tkDeRef`) as raw balanced tokens, and
  RESOLVES it below — after the param VARIABLES enter scope (`code->method` set), since
  `-> decltype(__a - __b)` names the parameters (not findable at the qualifier loop). `FuncDef::returns`
  is a reference (cannot be reseated), so the resolved type rebuilds the FuncDef via the new
  `clone_funcdef_with_return` and re-points the Variable / `funcdef_map` (handles trailing `*`/`&`).
- Deferred (in-class method) bodies carry the captured tokens on
  `DeferredFunctionBody::trailing_ret_tokens` and resolve at materialization
  (`parse_deferred_function_body`, params back in scope) — `enqueue_deferred_function_body` gained an
  optional `trailing_ret` param.

+test `testautotrailingreturn` (free + member, concrete + `decltype` + `decltype(this->x)`). Reducers
`tmp/au1` (`-> int`→2), `tmp/au3` (`-> decltype(a-b)`→5), `tmp/au4` (member→21 42), `tmp/au5`
(`-> decltype(this->x)`→7). Deduced-return `auto f(){return e;}` (no `-> T`) is a follow-up (`tmp/au2`
fails "returning void"). tv1 advances PAST the auto-return wall to sub-gap 11.

### sub-gap 11 — direct-initialization of a non-class variable — DONE (commit pending)
tv1 was at **`Failed to find type 'this' when parsing function parameters`** from inside
`_M_realloc_insert`'s instantiated body: **`pointer __new_start(this->_M_allocate(__len));`** —
DIRECT-INITIALIZATION of a NON-class (pointer) variable `T name(expr)`, mis-parsed as a FUNCTION
declaration `pointer __new_start(<params>)` (first "param" token `this` is not a type). madc already
does ctor-style direct-init for CLASS types (the ctor-call branch in parseDeclaration); a
pointer/scalar `T` was the gap — `T name(...)` always fell through to the function-declaration path.

**As-built:** a non-class `T name(X)` is direct-init (== `T name = X`, [dcl.ambig.res] the
"most vexing parse") when `X` is an expression rather than a parameter-declaration-clause. Fix
(parser.cpp, deepest layer, reuses the existing `= expr` initializer machinery — no new init path):
- New `Program::paren_group_is_nonclass_direct_init()` — a non-consuming peek at the token after the
  `(`. Returns true (→ direct-init) ONLY when that token CANNOT begin a parameter-declaration: a
  literal (ttInteger/ttReal/ttString/ttChar), `this`, or a ttIdentifier that names an in-scope value
  variable (`findVariable`). A type name tokenizes as ttDataType, so a ttIdentifier here is a
  non-type name. This is a SOUND under-approximation: every diverted form is unambiguously direct-init;
  anything that could begin a param-clause (a type/keyword, `void`, empty `()`, `*`/`&`) keeps the
  most-vexing-parse default (function declaration).
- A new branch in parseDeclaration (right after the class ctor-call branch, before the brace-init
  block) injects a synthetic `=` before the `(` when the type is non-class, non-`auto`, non-array,
  non-reference AND `paren_group_is_nonclass_direct_init()` — so `(X)` parses as a parenthesized
  primary expression through the existing scalar/pointer `= expr` path (mirrors the `T x{...}`
  brace-init synthetic-`=` reuse).
- **C++/madc only** (`!is_c_mode()`): `T name(expr)` direct-init is not C syntax — in C `int x(5);`
  is a function declaration. The gate is load-bearing: a K&R definition `void f(x, v) union u *x, v;`
  whose param NAMES shadow file-scope globals (gcc-torture `921112-1.c`) otherwise diverted to
  direct-init (the regression that the gate fixes — caught by the torture failset diff).

Reducers (tmp/, default mode): `di1` (`typedef int* P; P p(q)` → `*p`=7), `di2` (`int *p(q)` → 7),
`di3` (`int x(5)` → 5), `di8` (`enum E e(raw)` → 2); MVP defaults preserved: `di9` (`int f(P)` typedef
param stays a function decl), `di10` (`int g()` stays a function decl). Promoted to
`tests/testdirectinit`. tv1 advances PAST the `this`-param wall to **`use of undeclared identifier
'_ValueType1'`** (the next face — see sub-gap 12).

### sub-gap 12 — `_ValueType1` undeclared (the NEXT slice; tv1's new face)
`tmp/tv1` now fails at **`use of undeclared identifier '_ValueType1'`** then the MIR import of
`vector_int32_t_std__allocator_int32_t____M_realloc_insert`. `_ValueType1` is a libstdc++-internal
template parameter name (likely from the `__relocate`/`__uninitialized` or iterator-traits machinery
reached inside `_M_realloc_insert`'s instantiated body). Reduce from tv1, attribute via the 3 oracles
+ `--emit=c11`/`--dump-cir`, fix the deepest layer. Also the deduced-return follow-up from sub-gap 10
(`auto f(){ return e; }`, no `-> T`) remains open and off the critical path.

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
