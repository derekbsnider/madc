# Research + design — reference types as template arguments (w2a's last 2)

**Date:** 2026-06-13 (session 7). **Status:** research complete, fix designed, NOT
yet implemented. **Branch:** `feature/retire-embedded-shims-claude` @ `97bdb9d`.
**Recon material:** clang-18.1.3 binary (oracle) + `/workspace/llvm-clang-src`
(sparse clang frontend source, Apache-2.0 — recon ONLY, not vendored into madc).

## TL;DR

w2a (`std::vector<int> v;`) has 2 remaining c2m errors, BOTH in
`std::move_iterator<__normal_iterator<int*>>::operator*` returning
`reference = __conditional_t<is_reference<__base_ref>::value,
remove_reference<__base_ref>::type&&, __base_ref>` (= `int&&`), left as an opaque
placeholder struct. The ENTIRE blocker reduces to ONE missing feature:
**a reference-qualified TYPE as a template argument** (`Tmpl<int&>`, `Tmpl<T&&>`).
Everything else this construct needs already works in madc.

## What already works (verified, do NOT re-investigate)

- Member alias templates `X<C>::template type<A,B>` — reducer `tmp/mat1.mad` ✓
- gcc-13 internal `__conditional_t` form (`__conditional<C>::template type<If,Else>`,
  member-alias-template on a bool partial-spec) with a trait condition + non-ref
  args — `tmp/ic1.mad` ✓
- Public `std::conditional_t<…>` alias with a non-type param — fixed part-13
  (`4a50ae0`), `tests/testconditionalt.mad` ✓
- `conditional<true,int,long>::type` direct (partial-spec selection) — `tmp/ct4/ct5` ✓
- Reference RETURN types (`int& f(){…}`, `getref()=9`) — `tmp/ref2.mad` ✓
- Direct reference LOCAL binding (`int& r = n; r = 7;` → n==7) — `tmp/ref1.mad` ✓
- Member alias resolving to a reference in instantiation context
  (`using R=T&; conditional_t<is_reference<R>::value,…>`) — `tmp/mi3.mad` ✓

## The single blocker: reference-qualified type as a template argument

Reducers (all FAIL today with "Expecting ',' or '>' in <tname><...>"):
- `tmp/rt1.mad` `conditional_t<true, int&&, long>`
- `tmp/rt2.mad` `conditional_t<true, int&, long>`
- `tmp/rr1.mad` `remove_reference<int&>::type`
- `tmp/mi5.mad` EXACT move_iterator mirror (internal __conditional_t + member-alias
  base_ref + `remove_reference<base_ref>::type&&` true branch) → opaque placeholder.

### Root cause — duplicated, incomplete declarator-suffix folds

The template-argument parsers fold a trailing declarator suffix into the arg type.
The DEFAULT-argument loop (parser.cpp ~2709) already folds the full set
`*` / `&` / `&&` (via `getPointerType` / `getReferenceType`). But the EXPLICIT-arg
loops fold ONLY `*` (pointer), and THROW on `&`/`&&`. These `*`-only folds are
COPY-PASTED across several sites:

| site (approx) | function | current | reached by |
|---|---|---|---|
| ~2642 | `instantiate_template_use` explicit type arg | `*` only | rt1/rt2/rr1 (class template-id) |
| ~2709 | `instantiate_template_use` DEFAULT arg | `*`/`&`/`&&` ✓ | (model to copy) |
| ~3056 / ~3129 | `instantiate_template_alias_use` type-only path | `*` only | mi5's `type<…>` member alias template |
| ~4755 | builtin trait (`__is_pointer(int*)`) | `*` only | builtin trait args |

(My one-loop WIP `tmp/reftemplatearg_wip.patch` added `&`/`&&` to ~2642 only →
rt1/rt2/rr1 PASS, but mi5/w2a still fail because the `type<…>` member-alias-template
args go through the ALIAS type-only loop. REVERTED — see "lean design".)

### Lean design (per the "no duplication / OOP / reused" steer)

Do NOT copy the `&`/`&&` arm into N loops. EXTRACT ONE helper and replace the
duplicated `*`-only folds with calls to it — this REMOVES existing duplication AND
adds reference support uniformly (net: less code):

```cpp
// Consume a trailing template-arg declarator suffix (`*`, `&`, `&&`) and wrap the
// arg type accordingly, with reference collapsing. ONE owner for the rule.
TokenDataType *Program::fold_template_type_arg_suffix(TokenDataType *adt, TokenBase *origin);
```

Reference collapsing per clang `Sema::BuildReferenceType`
(`/workspace/llvm-clang-src/clang/lib/Sema/SemaType.cpp:2250`, C++ [dcl.ref]p6):
`LValueRef = spelledAsLValue || base is already an lvalue reference` — i.e.
`T& &`/`T& &&`/`T&& &` collapse to `T&`; only `T&&` (of a non-ref) stays `T&&`.
`getReferenceType` (parser.cpp ~10205) must collapse when `base` is already a
`DataDefREF` (verify/extend it). For w2a no collapsing actually occurs
(`remove_reference<int&>::type` is already `int`, then `int&&`).

Call the helper from ~2642, ~3056/~3129, ~4755 (and let ~2709 reuse it too).

## Separate PRE-EXISTING gap (do NOT conflate; NOT needed for w2a)

Reference-typedef / using-alias LOCAL binding is broken INDEPENDENTLY of templates:
`typedef int& RT; RT r = n; r = 7;` prints **5, not 7** ("assigning integer without
cast to pointer") — `tmp/ref3.mad` / `tmp/ref4.mad`. A reference type reached via a
typedef/alias isn't given reference-binding semantics for a LOCAL (works for a
DIRECT `int& r` and for RETURN types). w2a uses `reference` only as a typedef + a
RETURN type, so this gap does NOT block w2a — but it means a future
`conditional_t<…,T&,…> x = …;` bound local would silently miscompile. Track it as
its own slice (reference-aliased-local binding semantics); fix the parse feature
first.

## Open question for implementation (verify, don't assume)

The one-loop WIP did not clear w2a because the failing parse was in a DIFFERENT loop
(the alias `type<…>` path), not because resolution failed. Once the helper is wired
into ALL the loops above, RE-CHECK whether the move_iterator `__conditional_t` then
resolves to `int&&` and `operator*` emits valid C (it returns by reference — the
working ref2 shape — and is never CALLED for `vector<int> v;`, so it only needs to
compile). If a deeper resolution layer surfaces, attribute it with mi5 first.

## Method

Per `.claude/rules` §6: reduce (tmp/, default mode) → attribute (clang oracle +
`/workspace/mir/c2m FILE -ei`) → extract helper, wire all sites → re-probe
rt1/rt2/rr1/mi5/w2a → full gate (suite/unit/torture-51-failset/SMAUG) → commit.
Add a regression test using the WORKING shapes (reference RETURN / typedef /
template arg), NOT a bound reference local (the separate gap).
