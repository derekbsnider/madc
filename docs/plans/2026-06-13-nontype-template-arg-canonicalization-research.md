# Research — non-type template-argument canonicalization (and why the first attempt regressed 202 tests)

**Date:** 2026-06-13 (session 8, after p18). **Branch:** `feature/retire-embedded-shims-claude` @ `20e4bb3`.
**Motivation:** the retire-embedded-shims w2a wall (`std::vector<int>`) reached
`_Destroy_aux<__has_trivial_destructor(_Value_type)>` — a class template instantiated with a
**non-type argument that is a constant expression**. madc selects the PRIMARY `_Destroy_aux<bool>`
instead of the explicit `_Destroy_aux<true>` specialization, because it keys/matches non-type
arguments by their **raw spelling**, never by their evaluated **value**. g++/clang select `<true>`.

This doc records the clang model, madc's architecture, the root cause of the failed first attempt,
and the recommended design. **No code change is in the tree** — the WIP that regressed is saved at
`tmp/nontype_fold_v2_wip.patch` (do NOT reapply as-is).

---

## 1. What clang does (the canonical model)

`Sema::CheckTemplateArgument(NonTypeTemplateParmDecl *Param, ...)`
(`clang/lib/Sema/SemaTemplate.cpp:7210`), for an integral/enum parameter:

```cpp
llvm::APSInt Value;
ExprResult ArgResult = CheckConvertedConstantExpression(Arg, ParamType, Value, CCEK_TemplateArg);
if (ArgResult.get()->isValueDependent()) {          // dependent -> keep as Expression, unevaluated
    SugaredConverted = TemplateArgument(ArgResult.get());
    CanonicalConverted = Context.getCanonicalTemplateArgument(SugaredConverted);
    return ArgResult;
}
// Widen the value to sizeof(parameter type). Almost always a no-op, EXCEPT bool:
// extends 1 bit -> 8 bits.   (SemaTemplate.cpp:7527-7535)
Value = Value.extOrTrunc(Context.getTypeSize(IntegerType));
SugaredConverted   = TemplateArgument(Context, Value, ParamType);
CanonicalConverted = TemplateArgument(Context, Value, Context.getCanonicalType(ParamType));
```

And `ASTContext::getCanonicalTemplateArgument` (`ASTContext.cpp:6759`):
```cpp
case TemplateArgument::Integral:
    return TemplateArgument(Arg, getCanonicalType(Arg.getIntegralType()));
```

**Takeaways (the model madc must match in EFFECT, not mechanism):**
1. A non-type arg is evaluated to an integer **ONCE**, at semantic-analysis time, on a stable AST
   `Expr` node — never by re-parsing tokens.
2. The canonical identity is **(value, canonical parameter-type)**. So `<true>`, `<1>`,
   `<__has_trivial_destructor(int)>` ALL canonicalize to `Integral(1, bool)` and unify.
3. The **parameter type matters**: the value is widened/normalized to it (bool → 0/1). Two args with
   the same value but different param types are different arguments.
4. **Value-dependent** args are NOT evaluated — they stay as `Expression`. Only non-dependent
   (manifest-constant) args become `Integral`.

## 2. madc's architecture (where it diverges)

madc has **no SEMA/AST layer**: it monomorphizes templates by **token re-substitution + re-parse**
(Borland model), and a concrete instantiation's identity is a **sanitized arg-spelling string**:

- `instantiate_template_id` (parser.cpp ~2604) collects each arg. Type args resolve to a
  `TokenDataType`; **non-type args are collected as RAW tokens + a raw spelling**
  (`collect_template_argument_spelling`, ~2654) — never evaluated.
- The instantiation key is `mangled += "_" + sanitize_template_fragment(arg_spellings[i])`
  (parser.cpp ~2752). So `<true>` → key `..._true`, `<1>` → `..._1`,
  `<__has_trivial_destructor(int)>` → `..._has_trivial_destructor_int_`. **Three different keys for
  the same argument.**
- **Full/explicit specializations** (`template<> struct X<true>`) are registered by the SAME
  spelling-keyed mangled name (parser.cpp ~27580-27607) into `datatype_map`/`struct_map` — NOT into
  `partial_spec_map`. So a full spec is selected by **key (spelling) lookup**, not by value.
- **Partial specializations** go into `partial_spec_map` and ARE matched semi-by-value:
  `match_partial_specialization` → `non_type_partial_spec_arg_matches`
  (parser.cpp ~12808) compares via `parse_simple_template_non_type_value`, which only understands
  integer literals, `true`/`false`, and the `is_void<T>::value` special case — NOT a trait call.
- **`TemplateDef` does not record the non-type parameter's TYPE** (`include/madc.h` ~1154: it has
  `typeparam_is_type` (bool: type vs non-type) but no per-param type). So madc currently cannot do
  clang's "widen to param type / bool-normalize" step.

So the divergence is structural: **clang keys by evaluated (value, type); madc keys by raw spelling.**

## 3. Why the first attempt regressed 202 tests (DO NOT repeat)

The attempt (`tmp/nontype_fold_v2_wip.patch`) added `Program::try_fold_nontype_template_arg` — an
isolated-token-stream evaluator (save `tokens` / swap a fresh deque of the collected arg tokens + `;`
/ `parse_constant_integer_expression` / restore) — and called it at TWO sites: the use-site
collection (parser.cpp 2604) and the full-spec registration (27493), replacing the arg spelling with
the folded integer.

Result: integration **386 passed / 202 failed** (was 560/27) — incl. tests with no templates at all
(`testif`, `testcast`, `testclass`). Root cause (two compounding faults):

1. **Inconsistent keying across the many spelling-keyed sites.** Instantiation keys are built from
   `sanitize_template_fragment(spelling)` in MANY places — `instantiate_template_id` (2604/2752), the
   `Tmpl<Args>::member` member-chain path (3135/3263), alias-template instantiation, base-clause
   resolution, AND full-spec registration (27600). Folding at only TWO of them made the SAME
   logical instantiation key differ between where it was registered and where it was referenced
   elsewhere → mass key mismatches → libstdc++'s `type_traits`/`char_traits`/`integral_constant`
   machinery (pervasive in every header) silently split into non-matching instantiations → every
   header-using test broke. Canonicalizing at two sites is strictly worse than at zero.
2. **Re-entrancy.** `instantiate_template_id` is invoked RECURSIVELY while parsing system headers
   (a template arg can itself be a template-id; a trait arg's type resolves via
   `resolve_declared_type_token` → more instantiation). Evaluating during collection — even with a
   deque save/restore — runs the constant evaluator nested inside a suspended parse; the evaluator
   re-enters instantiation and registration with a swapped stream. This is the SAME failure class the
   part-13 banner warned about ("push/drain on the shared `tokens` stream desynced the suspended outer
   template parse"). A deque swap restores `tokens` but not the registrations/namespace effects of
   nested evaluation.

**Lesson:** non-type canonicalization is NOT a local fold at the collection site. madc keys by
spelling in many places; changing the spelling for some breaks identity. The fix must make the
canonical form consistent at the ONE place keys are built, and must not run a re-entrant evaluator
mid-parse.

## 4. Recommended design (surgical, consistent, non-re-entrant)

Two independent problems; w2a needs BOTH (this doc is about A; B is tracked separately):

### A. Non-type argument canonicalization

Make the canonical key for a non-type arg its **(value, param-type)**, computed at the SINGLE
key-construction chokepoint so every site agrees. Concretely:

- **Record the non-type parameter TYPE in `TemplateDef`** (a `std::vector<DataDef*> typeparam_types`
  or similar), captured when the primary template's parameter list is parsed. Needed for bool
  normalization (clang's widen-to-param-type) and to know which args are integral/enum (foldable).
- **Canonicalize at key construction, not at collection.** Where `sanitize_template_fragment` builds
  the per-arg key fragment, for a NON-TYPE slot whose param type is integral/enum AND whose arg is a
  manifest constant, render the fragment from the **evaluated value normalized to the param type**
  (bool → `0`/`1`). Dependent / non-constant args keep their spelling. Doing it at the one chokepoint
  guarantees use-sites, full-spec registration, partial-spec patterns, member-chain path, and alias
  instantiation all produce identical keys. (Audit every `sanitize_template_fragment(arg_spelling)`
  call — §3 fault 1 lists them — and route them through one `nontype_arg_key(spelling, tokens,
  paramType)` helper.)
- **Evaluate WITHOUT re-entrancy.** Do NOT swap the global `tokens` stream mid-parse. Either (a)
  evaluate the already-collected `arg_tokens` with a constant evaluator that walks a LOCAL cursor over
  a token vector (no global `tokens` mutation, no nested instantiation side effects), or (b) defer:
  collect raw, and canonicalize lazily at the moment a key is first built for an already-fully-parsed
  arg-token list (still local-cursor). The evaluator must reject value-dependent forms (the
  `constant_initializer_has_runtime_access`-style guard) so only manifest constants fold.
- **`parse_simple_template_non_type_value`** can then stay as-is (it already unifies `true`==`1`); or,
  once keys canonicalize, partial-spec value-matching is mostly redundant.

The cheapest *correct* first step may be (b)+chokepoint: a single `nontype_arg_canonical_fragment()`
used by ALL key builders, with a local-cursor constant evaluator. Prove it with the reducer
`tmp/nt1.mad` (`Aux<__has_trivial_destructor(int)>::f()` → must print `TRIVIAL`) AND
`tmp/nt2.mad` (direct decl picks the `<true>` spec), THEN the FULL gate (the 202-regression shows how
sensitive header parsing is — gate before trusting anything).

### B. Member-template instantiation (w2a's OTHER blocker — separate slice)

Even with A, w2a's emitted C still has `_Destroy_aux<true>::__destroy<int*>` as an undefined extern:
the selected spec's `__destroy` is itself a **member function template**, and madc registers member
templates declaration-only with the body SKIPPED (`register_skipped_class_template_function`,
parser.cpp ~26434). Instantiating it on ODR-use is the member-template-instantiation feature (capture
the body + monomorphize per call, reusing the free-fn-template `instantiate_fn_template_binding`
machinery). This is the substantial feature reframed-away in p18 and is required for w2a regardless of A.

## 5. Reducers (recreate in tmp/ — gitignored)

- `tmp/nt1.mad` — `namespace ns { template<bool> struct Aux{static void f(){puts("NONTRIVIAL");}};
  template<> struct Aux<true>{static void f(){puts("TRIVIAL");}}; } int main(){
  ns::Aux<__has_trivial_destructor(int)>::f(); }` — g++/clang: `TRIVIAL`; madc: `NONTRIVIAL`.
  Member-access form (goes through the type instantiation + member-chain).
- `tmp/nt2.mad` — direct decl: `template<bool> struct S{int prim;}; template<> struct S<true>{int
  spec;}; int main(){ S<__has_trivial_destructor(int)> s; s.spec=7; return s.spec; }` — g++: exit 7;
  madc: "Unidentified member 'spec' in 'S_1'" (primary selected).
- `tmp/w2a.mad` — the real wall.

## 6. Status / next

- Tree clean at `20e4bb3`; the canonicalization attempt is reverted (saved patch only).
- `__has_trivial_destructor` builtin, partial ordering, and template-id deduction (p18, `a64fd00`)
  are committed and stand — they correctly route w2a to this non-type-arg face.
- Next implementer: do **A** at the key chokepoint with a local-cursor evaluator + `TemplateDef`
  param types, gate hard; then **B** (member-template instantiation). Neither is a one-liner.
