# Plan — complete, version-gated, C++-only reserved-keyword registry

**Status: INFRASTRUCTURE LANDED + GREEN; reservation STAGED (validated, not yet
activated) 2026-06-15 (SESSION 16).** Branch `feature/retire-embedded-shims-claude`
(LOCAL-ONLY). Goal (user directive): every RESERVED C++ keyword through C++26
accounted for, registered as a version-gated, C++-only token — contextual keywords
(`override`/`final`/`module`/`import`) stay identifiers (NOT reserved).

## 0. PLAN OF CONTINUANCE (next session — READ FIRST)

**State after SESSION 17 (2026-06-16):** tree GREEN (613/12/0/18, zero regression).
Branch LOCAL-ONLY; develop untouched. Committed this session:
- **Slice 1 (asm)** — `94c018c`. Reserved at CPP98; statement-level GNU-asm skip
  extracted into the shared `Program::skip_gnu_asm_statement`, called from both the
  ttIdentifier and ttKeyword parseStatement arms.
- **Slice 2 (decl keywords)** — `f2c9656`. explicit/mutable/virtual/export/public/
  private/protected reserved at CPP98; all sites already de-shimmed (contextual helpers).
- **Slice 3 (typename)** — `5a1c9c5`. INVESTIGATED + DEFERRED (staged, NOT reserved):
  regresses the late free-fn-template instantiation of std::move/forward
  (`int&& y=std::move(x)` drops the `(x)` call → undeclared `__ns_std_move`; repro
  tmp/fwd.mad). Bug is in the move/forward return-type instantiation/reparse path, not
  a plain de-shim. Fix that first (testmemtmplpackexpand, testlateinstproto), then reserve.
- **Slice 4 (expression keywords)** — landed incrementally, all zero-regression: 4a
  casts/typeid/decltype/alignof (`a687da9`), 4b true/false/nullptr (`de35b56`), 4c sizeof
  (`7d6b514`), 4d this (`23fe82e`). The §4 regressions were typename + interactions, not these.
- **Slice 6 (consteval/constinit)** — `0d69c79`. Reserved at CPP20; zero new de-shim
  (slice-5 ignored-specifier skip already covered their spellings). Smoke tmp/ceval.mad.
- **Slice 7 (alt-token operators)** — `a391880`. and/or/not/bitand/bitor/xor/compl/not_eq/
  and_eq/or_eq/xor_eq mapped to the existing operator tokens via new C++-gated
  `cpp_operator_map`. C-mode safe (`int and=5;` valid in C; <iso646.h> macros if included).
- **char8_t + char-type gating fix** — `653d007`. char8_t added (CPP20 datatype); fixed the
  pre-existing UNGATED char16_t/char32_t/wchar_t — now C++ built-in / C header typedef,
  matching gcc/clang (user-spotted).
- **Slice 5 (constexpr + if-constexpr)** — `5d64f7b`. THE value slice: constexpr is now
  a real tkCPPKEYWORD (CPP11), un-erased, which ACTIVATES the if-constexpr discard
  machinery (proven by tmp/ifcxd.mad — the dead branch is not compiled). Token-aware
  ignored-decl-specifier skip added centrally: `is_ignored_cpp_specifier_token` +
  `TokenCppKeyword::parse()` (covers leading `constexpr int g` AND storage-delegated
  `static constexpr` via parseKeyword) + the member-specifier loop. Real <string>
  parses (tmp/capi.mad exit 0). **This moved the vector wall PAST push_back**: testvector
  now fails at `stl_vector.h:428:54` (the `_Vector_base<_Tp,_Alloc>` base instantiation —
  incomplete return type + int↔pointer confusion, 5 c2mir check errors), not the old
  `__uninitialized_move_if_noexcept_a` import. The container tests stay red on this and
  the other §6b walls (`__is_constructible`, empty-`_Rb_tree` dtor, `_Tp2` dup).

**Registry status: substantially COMPLETE.** All reserved keywords that appear in real
code are now tokens (asm, decl-specifiers, expr keywords, constexpr/consteval/constinit,
char8_t, alt-token operators). **Only TWO items remain, both BLOCKED on separate work:**
- **typename** (slice 3) — blocked on the std::move/forward late-instantiation reparse bug
  (NOTE: tmp/fwd.mad is NOT a clean isolator — it fails even baseline on `int&& y=move(x)`
  rvalue-ref-to-int lowering; the clean repro is testlateinstproto with typename reserved).
- **concept/requires/co_await/co_return/co_yield** (slice 8) — madc lacks concept/coroutine
  PARSING; today it SKIPS them via string-gated paths. Reserving as tokens breaks those
  skip paths, so de-shim `skip_requires_clause` &al. first.

**Higher-value pivot:** the orthogonal vector wall at `stl_vector.h:428:54` (the
`_Vector_base<_Tp,_Alloc>` base instantiation — incomplete return type + int↔pointer, 5
c2mir errors), now the freshest container blocker after constexpr cleared push_back.

**Prior infrastructure (SESSION 16):** `622a13b` (gate, tkCPPKEYWORD, de-shim groundwork,
dormant if-constexpr — now active).

**The user's directive:** finish reserving ALL reserved C++ keywords (gated, C++-only).
This is a bounded, validated slice list — NOT a big-bang (a full activation regressed 9
tests, §4). Each slice: add table rows → build → `tmp/capi.mad` (the `<string>` closure
reducer) + `make -C src fulltest` → fix the surfaced direct-`ttIdentifier`/semantic
sites → **zero regression vs the 12-known baseline before the next slice**. Compare
fail-sets with `comm -23` against a saved baseline list (the §7 protocol).

**Recommended slice order (each its own commit):**
1. ✅ DONE (`94c018c`). **asm** — shared `Program::skip_gnu_asm_statement` called from
   both parseStatement arms.
2. ✅ DONE (`f2c9656`). **Declaration keywords** explicit/mutable/virtual/export/public/
   private/protected — all sites already de-shimmed; landed clean.
3. ⏸ DEFERRED (`5a1c9c5`). **typename** — needs the `std::move`/`forward` template-
   instantiation REPARSE path fixed first (it drops the call → `__ns_std_move` undeclared;
   repro tmp/fwd.mad; testmemtmplpackexpand, testlateinstproto). Find the reparse site
   that mis-handles `typename` in the move/forward return type.
4. ✅ DONE — **Expression keywords**, landed incrementally (per-keyword bisect, all
   zero-regression): 4a casts/typeid/decltype/alignof (`a687da9`), 4b true/false/nullptr
   (`de35b56`), 4c sizeof (`7d6b514`), 4d this (`23fe82e`). The SESSION-16 §4 full-
   activation regressions came from `typename` + multi-keyword interactions, NOT these —
   each reserved cleanly in isolation.
5. ✅ DONE (`5d64f7b`). **constexpr** — un-erased + reserved (CPP11); the if-constexpr
   discard machinery is now ACTIVE (proven tmp/ifcxd.mad). Token-aware ignored-specifier
   skip added (`is_ignored_cpp_specifier_token` + `TokenCppKeyword::parse` + member loop).
   CLEARED the `__uninitialized_move_if_noexcept_a` push_back wall — testvector now fails
   DEEPER at `stl_vector.h:428:54` (`_Vector_base<_Tp,_Alloc>` base instantiation).
6. ✅ DONE (`0d69c79`). **consteval / constinit** (CPP20) — un-erased + two table rows;
   the slice-5 ignored-specifier skip already handled their spellings (zero new de-shim).
7. ✅ DONE (`a391880`). **Alternative-token operators** and/or/not/bitand/... — mapped
   spelling→existing operator token via the new C++-gated `cpp_operator_map` (separate
   from keyword_map only because operator tokens aren't TokenKeyword*). C-mode safe.
8. **C++20 set** — PARTIAL. char8_t DONE (`653d007`, datatype, C++-gated — also fixed the
   pre-existing ungated char16_t/char32_t/wchar_t). REMAINING: concept/requires/co_await/
   co_return/co_yield — needs the concept/coroutine SKIP paths (`skip_requires_clause`
   &al.) de-shimmed to accept the tokens first (madc lacks concept/coroutine parsing). DEFER.

**Caution:** the SEMANTIC regressions (slice 4) are the real risk — budget investigation,
not just mechanical edits. If a slice's regressions aren't cleanly de-shimmable, leave
that keyword staged and move on; partial completion is fine (the table documents the rest).

**Bridge back to the campaign:** once slice 5 lands, resume the retire-embedded-shims
push_back onion (the original `tmp/v1.mad` wall, now cleared past enable_if_t by
`a67cc72`) — next wall after if-constexpr is whatever v1.mad surfaces.

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
