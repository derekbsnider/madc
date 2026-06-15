# Plan — alias-template non-type bool arg must use the full constexpr fold (`enable_if_t<C,T>`)

**Status: RECON + 3-ORACLE DONE 2026-06-15 (SESSION 15) — ready to IMPLEMENT in a
fresh cycle (this is the handoff).** Branch `feature/retire-embedded-shims-claude`
(LOCAL-ONLY). This is the live vector-cluster wall after `__is_assignable` (`a2f453f`)
and the catch-all fix (`79141eb`). Companion: handoff
`docs/plans/2026-06-12-retire-embedded-shims-HANDOFF.md` §0d, and
`docs/plans/2026-06-15-is-assignable-constructible-intrinsics-plan.md` §0.

## 1. Problem

`vector<int>::push_back` (reducer `tmp/v1.mad`) now peels down to `__relocate_a_1`'s
RETURN TYPE: `__enable_if_t<std::__is_bitwise_relocatable<int>::value, _Tp*>` is left
an OPAQUE/incomplete type instead of resolving to `int*`
(`__is_bitwise_relocatable<_Tp> : is_trivial<_Tp>`, stl_uninitialized.h:1084;
`__enable_if_t<bool,T>` = `typename enable_if<bool,T>::type`). Symptom at c2mir:
"incompatible return-expr type in function returning a struct/union" / "function
return type is incomplete".

## 2. ROOT (3-oracle isolated — the gap is narrow and the fix is well-defined)

**The alias-template-id non-type BOOL argument is folded only when spelled as a
LITERAL `true`/`false`. A non-literal constant-expression bool (a parenthesized
expression, OR a trait `::value`) is NOT routed through the full constant-expression
evaluator** that `static_assert` / `fold_nontype_template_arg` / `eval_local_type_trait`
already use. So `enable_if_t<C, T>` selects the wrong/opaque enable_if branch unless
`C` is a literal.

3-oracle reducers (all `--std=c++17 --no-embedded-headers`; g++ AND clang accept ALL;
regenerate in `tmp/`):

| reducer | construct | g++/clang | madc | shows |
|---|---|---|---|---|
| `blit2` | `typedef eif<true,int> R;` | ok | **ok** | literal bool folds (control) |
| `bns`   | `template<class T> eif<true,T*> pick(T*)` | ok | **ok** | literal bool in fn-return folds (control) |
| `blit`  | `typedef eif<(1==1),int> R;` | ok | **FAIL** | a trivial bool *expr* does NOT fold |
| `bdir`  | `typedef eif<is_trivial<int>::value,int> R;` | ok | **FAIL** | a DIRECT trait `::value` does NOT fold |
| `bcon`  | `typedef eif<bwr<int>::value,int> R;` (`bwr:is_trivial`) | ok | **FAIL** | inherited trait `::value` does NOT fold |
| `bnst`  | `template<class T> eif<bwr<T>::value,T*> pick(T*)` | ok | **FAIL** | the real `__relocate_a` shape |
| `fa`    | `static_assert(bwr<int>::value)` + `!bwr<NT>::value` | ok | **ok** | the trait-`::value` fold EXISTS (constexpr path) |

`eif` = `template<bool C, class T> using eif = typename std::enable_if<C,T>::type;`
(namespaced to dodge the orthogonal §6 gap). The contrast `fa` (fold exists) vs
`bdir`/`blit` (alias arg doesn't use it) IS the diagnosis: two fold paths diverge.
Dependence is NOT the issue — `bcon` (concrete `int`) fails identically to `bnst`
(dependent `T`); the alias-arg fold is literal-only regardless.

## 3. Likely code sites (for the implementer — VERIFY with a gated diag)

- `instantiate_template_alias_use` (parser.cpp ~3128; the non-type-arg substitution
  path ~3214 "Substitute the collected args — type AND non-type"): this is where
  `eif<C,T>`'s args are collected (`collect_template_argument_spelling`) and the alias
  body `typename enable_if<C,T>::type` is resolved in an isolated stream. The bool `C`
  arg is evidently matched literally rather than constant-evaluated. Trace how `C` is
  turned into a value here and why only `true`/`false` spellings select the right
  `enable_if` partial spec.
- The constexpr fold that DOES work (wire the alias-arg path into it):
  `fold_nontype_template_arg` (parser.cpp ~13540+) and `eval_local_type_trait`
  (~13628) and `parse_simple_template_non_type_value` — the path `static_assert` and
  non-type CLASS-template args use (which folds `(1==1)`, `is_trivial<int>::value`, and
  inherited `bwr<int>::value` — proven by `fa` and the existing `__is_trivial`/
  `is_constructible`-style tests). The fix is to route the alias's non-type bool arg
  through this SAME fold before selecting the `enable_if` branch.
- `enable_if<C,T>::type` itself is a partial-spec/`::type` member selection on `C`;
  confirm the alias body resolution picks `enable_if<true,T>` (has `type`) vs
  `enable_if<false,T>` (no `type`) based on the FOLDED `C`, not the spelling.

## 4. How clang / g++ do it (recon)

Both substitute template args then **constant-evaluate** every non-type argument
uniformly — there is no "literal-only" fast path:
- clang: `CheckTemplateArgument` → `CheckTemplateArgumentIsConvertedConstantExpression`
  → `EvaluateConvertedConstantExpression` (SemaTemplate.cpp / SemaOverload.cpp). A
  `bool` non-type arg is a converted-constant-expression; `bwr<int>::value` evaluates
  via the same constant evaluator as `static_assert`.
- g++: `convert_nontype_argument` (pt.cc) → `fold_non_dependent_expr` /
  `cxx_constant_value`; `maybe_constant_value` folds `Trait<int>::value` after any
  substitution. Alias templates are transparently substituted (`tsubst`) — the alias
  is not a separate evaluation regime.
The lesson: madc's alias-arg path must reuse its ONE constant-expression fold (the
`static_assert` one), exactly as §3 proposes — no parallel literal matcher.

## 5. Approach + validation

- **Single fix, deepest layer:** make the alias-template-id non-type-arg path
  (`instantiate_template_alias_use`) evaluate a bool (and ideally any integral) arg
  through the existing constexpr fold (`fold_nontype_template_arg` &al.) instead of a
  literal-spelling match. Keep the LITERAL path's result identical (it already works).
  Do NOT add a parallel evaluator — REUSE the one `static_assert` uses (3R credo).
- **Verify-first:** gated diag in the alias non-type-arg handling printing the arg
  spelling + whether it folded; confirm `(1==1)` / `is_trivial<int>::value` reach it
  unfolded today.
- **3-oracle FIRST** (the §2 table): blit/bdir/bcon/bnst must flip to madc-ok and stay
  g++/clang-ok; blit2/bns must stay ok (no literal regression). Commit a guard
  (`tests/testaliasnontypebool` — the §2 table, default mode where it parses).
- **`gcc-on-emitted-C`** on `tmp/v1.mad` after: the `__enable_if_t…__relocate_a_1`
  opaque-return error must clear; record the NEXT push_back wall (likely
  `__is_constructible` — still pending — or deeper `_M_realloc_insert` codegen).
- **`make -C src fulltest`** — zero regression. The alias-arg path is shared by EVERY
  `enable_if_t`/`conditional_t`/`__detected_or_t` use across libstdc++; a wrong fold
  here corrupts SFINAE broadly. Watch the whole suite, not just the cluster. (This is
  why it is its own slice, gated hard.)

## 6. Orthogonal finding (SEPARATE follow-up — do NOT fold into this slice)

A free function template declared at **global (`::`) scope** and called (qualified or
not) is "use of undeclared identifier" — `template<class T> T idf(T x){return x;}` then
`idf(5)` / `idf<int>(5)` FAILS, while the SAME in a `namespace mz { }` works
(`mz::idf(5)` ok). Reducers `tmp/fc.cpp` (global, fails) vs `tmp/ns1.cpp` (namespaced,
ok). This did NOT block push_back (its templates are `std::`-namespaced and DO
instantiate — `__relocate_a_1__o2` is emitted), so it is a distinct bug. Worth its own
plan; likely the global-scope free-fn-template registration/lookup path. (Also relates
to the SESSION-15 g1/g2 reducers: variadic free-fn-template call/emission gaps.)

## 7. Honest assessment
Narrow, well-isolated root (one fold path), but the fix touches a hot, SFINAE-wide
shared path → validate suite-wide. After it clears, `__is_constructible` (the original
"Slice 2" of the is-assignable plan) is the likely next push_back wall, then deeper
`_M_realloc_insert` codegen. The vector cluster remains multi-wall; this is one slice.
