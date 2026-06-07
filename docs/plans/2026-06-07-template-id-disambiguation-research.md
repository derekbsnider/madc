# Template-id vs type-alias disambiguation — how Clang does it, and the madc fix

**Date:** 2026-06-07. **Branch:** `feature/realhdr-parse-gaps2-claude` (HEAD `e63e639`).
**Why:** the real-header track derails instantiating `std::basic_string_view<char>`
with `parser.cpp:2644` "Expecting a type argument to **iterator**<>". This doc
captures a clang-internals research pass on the root mechanism and states the
verified madc bug site + minimal fix. Companion to
`2026-06-07-parser-pp-architecture-research.md` (which covered the broader
front-end; this one is the focused follow-up on the `<` ambiguity).

Sources: clang 18 source (`llvm/llvm-project`, `clang/lib/Parse/Parser.cpp`,
`clang/lib/Sema/SemaTemplate.cpp`), the official Clang Internals Manual, and
empirical `clang++ 18.1.3` / `g++` runs. GCC `cp_parser` names are from memory.

---

## The bug, in one sentence
madc assumes **`identifier <` = template-id** and dispatches on the **spelling**;
when `iterator` is a member type alias (`using iterator = const_iterator;`) but
`std::iterator` (the deprecated base template) is also registered, madc abandons
the already-resolved alias and tries to instantiate the same-spelled template,
then fails demanding a type argument.

## How Clang decides (name-lookup FIRST, not spelling)
1. **The lexer is oblivious** — it emits plain `identifier` + `less`. The
   Parser+Sema classify and replace them with an *annotation token*
   (`tok::annot_typename` for a resolved type, `tok::annot_template_id` for a
   template-id). [VERIFIED: `-Xclang -dump-tokens`, Internals Manual]
2. **`Parser::TryAnnotateName` → `Sema::ClassifyName`**, then switch on
   `NameClassificationKind`: a typedef/alias → `Type` → `annot_typename`,
   which **never enters the template path**. Only `TypeTemplate`/`VarTemplate`/
   `FunctionTemplate`/`Concept`/`UndeclaredTemplate` reach
   `Parser::AnnotateTemplateIdToken` — and only after confirming the next token
   is `<`. [VERIFIED: Parser.cpp ~1729–1835, 2050–2070]
3. **`Sema::isTemplateName` → `Sema::getAsTemplateNameDecl`** returns non-null
   **only** for `isa<TemplateDecl>(D)`. So `using iterator = const_iterator;`
   (a `TypeAliasDecl`, NOT a `TemplateDecl`) → `TNK_Non_template` (== 0) → `<`
   is parsed as less-than / the name stays a plain type. An alias *template*
   `template<class T> using X = …;` is a `TypeAliasTemplateDecl` (IS a
   `TemplateDecl`) → `TNK_Type_template` → template-id. [VERIFIED:
   SemaTemplate.cpp ~106, 213–232, 309–317; TemplateKinds.h]
4. **Lookup is scope-sensitive, not spelling-keyed.** Inside `basic_string_view`,
   unqualified `iterator` finds the member `TypeAliasDecl` (it *shadows* the
   namespace `std::iterator` template); qualified `std::iterator` finds the
   `ClassTemplateDecl`. They never collide because lookup respects scope.
   [VERIFIED: `-ast-dump`]
5. **Parse once, substitute at instantiation.** Clang parses a class-template
   body exactly once into a *dependent* AST and substitutes args at instantiation
   (`SemaTemplateInstantiate*`); it does NOT re-parse/re-disambiguate per use.
   The `iterator`-vs-`std::iterator` decision is made once, at parse time, on the
   bound declaration. [VERIFIED: `-ast-dump`]

## Minimal reproducer (clang++ and g++ both accept; madc fails) [VERIFIED]
```cpp
#include <iterator>                 // std::reverse_iterator + deprecated std::iterator (both templates)
template<class CharT>
struct sv {
  using value_type     = CharT;
  using const_iterator = const value_type*;
  using iterator       = const_iterator;                       // plain alias, NOT a template
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;
  using reverse_iterator       = const_reverse_iterator;
};
int main(){ sv<char> s; (void)s; return 0; }
```
Promote to `tests/` (with `.expect`) once it parses. Comparison-flavored target
(proves `<` must stay less-than when the name is a value):
```cpp
template<class T> struct Outer { static const int iterator = 3;
  int f(int a,int b){ return (a < iterator) > b; } };
```

## The madc bug site (VERIFIED in source)
`src/parser.cpp:2935–2942`, in `Program::resolve_declared_type_token`:
```cpp
if ( tb->type() == TokenType::ttDataType )            // 'iterator' ALREADY resolved to a TYPE
{
    if ( peekToken() && peekToken()->id() == TokenID::tkLT )
    {
        std::string tname = ((TokenDataType *)tb)->str;   // ← keeps spelling, discards the binding
        if ( TokenDataType *inst = instantiate_template_id(tname, tb) )
            return inst;
    }
    ...
```
`instantiate_template_id` → `instantiate_template_use` → **`find_template(tname)`**
is a **flat spelling lookup** into `template_map`. So an already-resolved member
alias whose spelling matches a registered template gets hijacked into template-id
parsing → the `2341`/`2644` "Expecting a type argument" error.

## Minimal fix (mirror clang's gate) — for the next session
1. **An already-resolved type token is a type, not a template-by-spelling.** In
   the `ttDataType` branch (2935–2942), do NOT call `instantiate_template_id`
   when the token's bound declaration is a non-template alias/typedef. (Clang:
   `Type` classification → `annot_typename`, never the template path.) The
   spelling-only template dispatch should fire only on the *unresolved*
   identifier path (`is_contextual_identifier_token`, parser.cpp ~2999), never on
   a `ttDataType`.
2. **Make the template gate binding/scope-aware, not spelling-aware.**
   `find_template` must respect shadowing: if the active scope binds the spelling
   to a non-template type, don't return a same-spelled namespace template.
3. **Distinguish alias-template (params → `TNK_Type_template`) from plain type
   alias (`TNK_Non_template`)** when deciding whether `<` opens template args.
4. Resolve the disambiguation once on the bound decl (parse-time), not per-use by
   spelling.

Highest-leverage edit: #1 (+ #2). Gate the `ttDataType`+`<` → instantiate path on
"the binding is actually a template," not the bare spelling. This is the
deepest-layer fix for the whole `identifier <` ambiguity class, not just
`iterator`. Cross-reference function names — clang: `TryAnnotateName` /
`ClassifyName` / `isTemplateName` / `getAsTemplateNameDecl` / `AnnotateTemplateIdToken`;
gcc: `cp_parser_template_name` / `lookup_template_name` gating `cp_parser_template_id`.

CAUTION: this touches core type-token resolution — go test-gated (fulltest +
gcc.c-torture failset-diff + SMAUG soak), and re-run the real-header probe to
confirm `basic_string_view<char>` advances. Related: [[project_template_instantiation]].

---

## ⚠ CORRECTION (2026-06-07, evidence-based via instrumentation) — the live root cause is PARTIAL SPECIALIZATION, not 2935-2942

The disambiguation hypothesis above was a **static misread**. Instrumenting the
actual `<string_view>` derail (live `bin/madc`, DBG at the throw sites) proves the
failure never reaches `parser.cpp:2935-2942`. The chain is:

`using string_view = basic_string_view<char>` (pp line 14060) → instantiate
`basic_string_view<char>` → `using const_reverse_iterator =
std::reverse_iterator<const_iterator>` → instantiate `reverse_iterator<const char*>`
→ its base clause `: public iterator<typename iterator_traits<_Iterator>::iterator_category, …>`
→ resolve arg `typename iterator_traits<const char*>::iterator_category`
→ `iterator_traits<const char*>` resolves to the **empty PRIMARY** template
(instrumented: `owner=iterator_traits_char_ member=iterator_category alias=NULL
incomplete=0 depsurface=0`) → no `iterator_category` member → `resolve_typename_type_token`
returns NULL → the arg to `iterator<>` is NULL → **throw at parser.cpp:2341**
(NOT 2644, NOT 2935). The failing arg token is literally `typename`.

**Root cause (confirmed with minimal reducers + g++ oracle):** madc parses
partial specializations but **silently discards them and always instantiates the
PRIMARY**.
- `template<class T> struct tr{int v(){return 0;}}; template<class T> struct tr<T*>{int v(){return 1;}};`
  `tr<char*>().v()` → **g++ returns 1, madc returns 0** (primary used for the
  pointer too — a silent correctness bug, not just a parse error).
- Explicit specialization (`template<> struct box<int>`) DOES work; only PARTIAL
  specialization is unimplemented.

**Where it's dropped:** `TokenTEMPLATE::parse` (parser.cpp). A partial spec has
`specialized_template_id == true` AND `typeparams` **non-empty**. The explicit-spec
branch fires only for `specialized_template_id && typeparams.empty()`
(parser.cpp:20181); the primary branch fires only for `!specialized_template_id`
(20263). A partial spec matches NEITHER → its captured body returns at 20272
**without registering anything**.

**The deepest-layer fix = implement partial specialization** (no shortcut;
special-casing `iterator_traits` would be forbidden hardcoding):
1. **Represent:** add `bool is_partial_specialization` + `std::vector<std::string>
   spec_pattern` (the per-arg spellings, e.g. `["T*"]`) to `TemplateDef`
   (include/madc.h:1082). Store partial specs in a NEW `partial_spec_map[class_name]
   -> vector<TemplateDef>` — NOT `template_map`, because `register_template` merges
   same-namespace variants and would clobber the primary.
2. **Register:** in `TokenTEMPLATE::parse`, the partial-spec case
   (`specialized_template_id && !typeparams.empty()`) captures body + own typeparams
   + `spec_pattern = specialization_arg_spellings`; push into `partial_spec_map`.
3. **Match at instantiation:** in `instantiate_template_use` (parser.cpp:2279),
   after the concrete args are resolved, scan `partial_spec_map[tname]` for a spec
   whose `spec_pattern` UNIFIES with the concrete args (deduce the spec's typeparams:
   bare `T`→arg; `T*` vs `C*`→T=pointee; `const T`→recurse; literal must equal).
   Pick the most specialized (most non-typeparam structure). If matched, instantiate
   THAT body with the deduced substitution; else fall back to the primary.

This is a real, high-blast-radius feature (the template engine all of std:: rides
on). Implement as ONE focused, test-gated pass: fulltest 528/4/0/26 + FULL
gcc.c-torture failset-diff (ZERO regressions) + SMAUG soak. Minimal regression
tests: the `tr<T*>` reducer (expect 1), `tr3<char*>::cat` sizeof (expect 4), then
the `<string_view>` probe advancing past line 14060. Related:
[[project_template_instantiation]].
