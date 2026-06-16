# Plan — `if constexpr` branch discarding (the `__uninitialized_move_if_noexcept_a` wall)

**Status: RECON + 3-ORACLE DONE 2026-06-15 (SESSION 16) — ready to IMPLEMENT.**
Branch `feature/retire-embedded-shims-claude` (LOCAL-ONLY). Live wall after the
enable_if_t non-type-bool fold (`a67cc72`). Companion: handoff
`docs/plans/2026-06-12-retire-embedded-shims-HANDOFF.md` §0d.

## 1. Problem
`vector<int>::push_back` (reducer `tmp/v1.mad`) fails with `MIR error: import of
undefined item __ns_std___uninitialized_move_if_noexcept_a`. That free fn template
is called ONLY in the `else` branch of vector.tcc `_M_realloc_insert`:

```cpp
if _GLIBCXX17_CONSTEXPR (_S_use_relocate())   // == `if constexpr (...)` in C++17
  { ... _S_relocate(...) ... }                // -> std::__relocate_a  (TAKEN for vector<int>)
else
  { ... std::__uninitialized_move_if_noexcept_a(...) ... }   // DISCARDED for vector<int>
```

`_S_use_relocate()` is `true` for `vector<int>` (int is trivially relocatable), so g++
and clang **discard** the `else` branch and never instantiate
`__uninitialized_move_if_noexcept_a`. madc emits BOTH branches → the discarded branch
references a free fn template madc does not instantiate → emitted as an undefined
`extern long __ns_std___uninitialized_move_if_noexcept_a();` → MIR import error.

## 2. ROOT (3-oracle isolated)
**madc has no `if constexpr` discarding.** The lexer DEFINES `constexpr` as an empty
macro (`src/lexer.cpp:1045 define_map["constexpr"]=""`), so `if constexpr (cond)` is
preprocessed to a plain runtime `if (cond)` and `TokenIF` emits BOTH branches; madc
never knows the `if` was `constexpr`.

3-oracle (`--std=c++17 --no-embedded-headers`; regenerate in `tmp/`):
| reducer | construct | g++/clang | madc |
|---|---|---|---|
| `ifcx2` | `template<class T> W::f(){ if constexpr(sizeof(T)>=4){taken();}else{discarded();} }`, `W<int>` | runs `taken` only; emits taken only | runs `taken` BUT emits **both** `taken()`+`discarded()` calls |
| `ifcx`  | same but else calls `nonexistent_undeclared_fn(this)` | OK (else discarded, never instantiated) | **FAILS** (tries to emit/parse the dead branch) |
| `cxfold`| `static_assert(W::use())` where `use()` is `static constexpr bool { sizeof(T)>=4 }` | OK | **OK** — madc already folds a constexpr static-member-fn call |

`cxfold` de-risks the crux: madc's constant-expression evaluator already folds a
`constexpr` member-fn call, so folding `_S_use_relocate()` (a chain of constexpr
fns/traits) is feasible.

## 3. How clang / g++ do it
C++17 [stmt.if]/2: for `if constexpr`, the condition is a contextually-converted
constant expression; the non-taken substatement is a *discarded statement*. In a
TEMPLATE instantiation the discarded branch is **not instantiated** (templated
entities in it are not required to be valid for the actual args). Both compilers fold
the condition, pick the live branch, and never instantiate/codegen the dead one.

## 4. Approach (deepest layer; general — fixes EVERY `if constexpr` in libstdc++)
NOT a per-symbol fix. Implement real `if constexpr`:

- **(a) Lexer — preserve `if constexpr`.** Stop silently erasing `constexpr` when it
  immediately follows `if`. Options to evaluate: (i) lexer lookahead — when emitting
  the `if` keyword, if the next source token is `constexpr`, consume it and flag the
  `if` (set `TokenIF::is_constexpr`); keep the global `constexpr`->"" erase for the
  declaration-specifier contexts. (ii) make `constexpr` a real reserved token and have
  the existing specifier lists (parser.cpp ~26199/26385/26452/28557, already list the
  word "constexpr") consume-and-ignore it, while `if` reads it. Prefer (i) — minimal
  blast radius, leaves the decl path untouched.
- **(b) Parser `TokenIF::parse` (parser.cpp ~23120).** Add `bool is_constexpr`. When
  set: fold the condition via the constant-expression evaluator
  (`parse_constant_integer_expression` / the `static_assert` path — proven to fold
  constexpr-fn calls). Then parse the TAKEN branch normally and **skip the DISCARDED
  branch as raw tokens WITHOUT instantiating** (token-skip the balanced `{...}` /
  single statement; do NOT call the live `parseStatement` on it, since that triggers
  template instantiation + symbol emission). Mirror an existing balanced-skip helper.
  Leave plain (non-constexpr) `if` exactly as today.
- **(c) Fallback.** If `is_constexpr` but the condition does NOT fold to a constant
  (genuinely dependent / unfoldable), fall back to emitting both branches as today
  (no worse than current) — but log it; for the libstdc++ cases the condition folds.

## 5. Validation
- **3-oracle FIRST** (the §2 table): `ifcx`/`ifcx2` discard the dead branch (emit only
  the taken one); add a guard `tests/testifconstexpr` (both-branches-valid +
  discarded-branch-references-undeclared, default mode, prints the taken branch).
- **`gcc-on-emitted-C`** on `tmp/v1.mad`: the `__uninitialized_move_if_noexcept_a`
  undefined-import must clear; record the next push_back wall.
- **`make -C src fulltest`** — zero regression. `if constexpr` is pervasive in
  libstdc++; a wrong discard corrupts control flow broadly. Validate suite-wide.

## 6. Honest assessment
Meaty, multi-layer (lexer + parser + condition-fold reuse + dead-branch token-skip),
but the crux (folding the condition) is de-risked (§2 `cxfold`) and the feature is
general + standard. The token-skip-without-instantiation (b) is the subtle part: must
skip a balanced block without running `parseStatement`. After it clears, v1.mad peels
to its next push_back layer. Pre-existing separate follow-ups unchanged (§6b false-
mask enable_if, is_constructible, empty-_Rb_tree dtor SIGSEGV, _Tp2 dup, STALE-API).
