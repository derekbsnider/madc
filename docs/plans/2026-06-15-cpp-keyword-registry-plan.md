# Plan — complete, version-gated, C++-only reserved-keyword registry

**Status: INFRASTRUCTURE LANDED + GREEN; reservation STAGED (validated, not yet
activated) 2026-06-15 (SESSION 16).** Branch `feature/retire-embedded-shims-claude`
(LOCAL-ONLY). Goal (user directive): every RESERVED C++ keyword through C++26
accounted for, registered as a version-gated, C++-only token — contextual keywords
(`override`/`final`/`module`/`import`) stay identifiers (NOT reserved).

## 1. Why this is a multi-session de-shim (not a one-pass change)
madc deliberately handles most C++ keywords as **contextual identifiers** matched by
SPELLING (Cfront-style; KG lesson "Tokenizer remapping for contextual keywords" — a
grammar-level hard reservation once caused a 49-test regression). Hundreds of parse
sites do `tb->type() == ttIdentifier && str == "kw"`. A hard keyword token has
`type() == ttKeyword`, so each such DIRECT check must be de-shimmed to accept the
token (the pattern the `friend`→`tkFRIEND` commit `6351ef8` used, for ONE keyword).

A full activation of the C++98+C++11 set surfaced **9 regressions** (see §4) — some
SEMANTIC (scope loss, codegen), not mere lexing — confirming each keyword has a tail
of sites and must be activated in validated slices.

## 2. Mechanism (LANDED — infrastructure, inert until the table is populated)
- `Program::cpp_keyword_active(LanguageStd min_std)` (include/madc.h): true in the
  madc dialect OR a C++ mode `>= min_std`; NEVER in C. The version floor.
- `tkCPPKEYWORD` + `class TokenCppKeyword : TokenKeyword` (include/tokens.h): a generic
  reserved keyword with no dedicated dispatch token; `str` carries the spelling; it
  IS-A TokenIdent so `((TokenIdent*)tb)->str` works.
- `tkCPPKEYWORD` admitted to `is_contextual_identifier_token` /
  `contextual_identifier_name` (src/parser.cpp): every spelling-based site + the
  parseExpression ttKeyword→ttIdentifier fall-through (18020) then works transparently.
- Staged registration table in `add_keywords` (src/lexer.cpp), gated by
  `cpp_keyword_active` — currently EMPTY (the complete set is listed in-comment).

## 3. The complete reserved C++ keyword table (account-for-them-all)
Already dedicated tokens (keep; gate at CPP98): try catch throw class using namespace
template new delete friend (C++-gated today) + the shared C/C++ control/storage set.
Datatypes (add_datatypes): bool char int void float double short long signed unsigned
wchar_t (CPP98) char16_t char32_t (CPP11); **char8_t (CPP20) — MISSING, add as datatype.**
To REGISTER via the gated table (currently string-matched / erased / missing):
- **C++98:** asm explicit export mutable virtual this typename sizeof public private
  protected typeid true false static_cast const_cast reinterpret_cast dynamic_cast
- **C++11:** constexpr decltype alignof nullptr static_assert thread_local
  (alignas → kept as `__attribute__` map; noexcept → kept as lexer balanced-paren strip;
   consteval/constinit → ERASED today, need decl-specifier consume handling when reserved)
- **C++20:** char8_t concept requires co_await co_return co_yield  (DEFERRED — §5)
- **Alternative-token operators (CPP98):** and and_eq bitand bitor compl not not_eq or
  or_eq xor xor_eq → map spelling→operator token (or define-substitution), C++-gated.
- **C++23 / C++26:** no new reserved keywords.

## 4. De-shim sites (the actual work)
DONE this session (generalized `ttIdentifier`-direct → `is_contextual_identifier_token`/
`contextual_identifier_name`; inert under the empty table, forward-compatible):
- `is_template_type_param_intro` (typename in `template<typename T>`).
- base-spec `virtual`/access loop (~21603); access-label `public:/private:/protected:`
  (~22101); member-specifier `virtual`/`mutable`/`explicit` loop (~22177).
- `parse_constant_primary` (~6227: sizeof/alignof/nullptr/casts/true/false in constant
  context).
- typedef base-type gate (~24005: `typedef decltype(...) X;`).
- parseStatement ttKeyword case (~34732): route tkCPPKEYWORD → parse_static_assert_statement
  / parseExprStmt.
REMAINING (mapped by the 9-regression activation — fix per slice, fulltest-gated):
1. **asm** — statement dispatch is `ttIdentifier`-gated (`is_gnu_asm_identifier_token`
   reached only via the ttIdentifier statement arm). → testasmoutputonly, testnestedasmbarrier.
2. **typename/template reparse** — `std::move`/`forward` instantiation re-parse path
   loses the param when typename is a token (`__ns_std_forward` undefined). →
   testmemtmplpackexpand, testlateinstproto.
3. **`__x` scope loss** (testmadceval `__madc_runtime_eval`, testnoautoload) — a keyword
   token in a parameter/body context drops a binding; SEMANTIC, investigate before reserving.
4. **codegen** — testmathh (9 c2mir check errors), teststringplus / teststrplusbody_realhdr
   (1 each) — a cast/this lowering under reservation; SEMANTIC.

## 5. C++20 deferral (concept/requires/co_*)
madc presents as a C++20+ dialect to real headers, which use `concept`/`requires`
(active under `__cpp_lib_concepts`, e.g. <compare>/<concepts>) and the coroutine
keywords. madc lacks concept/coroutine PARSING; today it SKIPS those declarations via
string-gated paths (`skip_requires_clause` &al.). Reserving these as tokens breaks the
skip paths. So they stay contextual until the skip paths are de-shimmed to accept the
tokens. Listed in the table comment so the registry is complete.

## 6. constexpr + `if constexpr` (the original driver)
`constexpr` is one of the staged reservations. Once it is a token, the LANDED (dormant)
`if constexpr` machinery activates: `fold_if_constexpr_condition` + `skip_discarded_statement`
+ the `TokenIF::parse` is_constexpr branch (discards the non-taken branch without
instantiating — C++17 [stmt.if]/2). That clears the `__uninitialized_move_if_noexcept_a`
push_back wall (the discarded `if constexpr` else-branch). See
`docs/plans/2026-06-15-if-constexpr-discarding-plan.md`.

## 7. Activation strategy (slices, each fulltest-gated)
Per slice: add a few keyword rows to the table → build → run header-heavy reducers
(`tmp/capi.mad` = <string> closure) + `make -C src fulltest` → fix the surfaced
direct-`ttIdentifier`/semantic sites → zero-regression before the next slice. Suggested
order: (a) asm (1 site); (b) the access/specifier decl keywords (sites already done) —
explicit/mutable/virtual/public/private/protected/export; (c) typename (after the
move/forward reparse fix); (d) the expression operators sizeof/casts/typeid/decltype/
alignof/nullptr/true/false (after the __x + codegen investigations); (e) constexpr +
activate if-constexpr; (f) consteval/constinit (decl-specifier consume); (g) alt-token
operators; (h) C++20 set after the concept/coroutine skip-path de-shim.
