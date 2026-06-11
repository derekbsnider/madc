# `===` / `!==` Strict Equality — Design (STD_MADC dialect)

Date: 2026-06-11. Status: approved by user (brainstorming session).
Queue item 1 of the v0.29 handoff NEXT QUEUE — generalized per user
direction: `===` works on **any** operands, not just `madc::value`.

## Motivating example (user-supplied)

```c
uint32_t a = 5;
int32_t  b = 5;
uint8_t  c = 5;

a == b    // true  — value equality after usual conversions
a == c    // true
a === b   // FALSE — types differ (signedness)
a === c   // FALSE — types differ (size)
```

`===` is type-domain identity AND value equality. `!==` is its negation.

## 1. Surface & gating

- `===` is `Token3Eq` (already exists, `tokens.h`); `!==` is a **new**
  `Token3NotEq` (`TokenID::tk3NotEq`, string `"!=="`).
- Precedence 7 (the `==`/`!=` tier), left-associative. Result type `int`
  (0/1), same as `==`.
- Both operands are always evaluated, once, left-to-right — identical
  evaluation rules to `==`.
- **STD_MADC-only**, gated in the lexer exactly like `<=>`
  (`lexer.cpp:2905` precedent): when `language_std != STD_MADC`, `===`
  lexes as `==` then `=`, and `!==` lexes as `!=` then `=` — a syntax
  error in normal contexts, matching g++/clang conformance.
- This **fixes a latent conformance bug**: `Token3Eq` is currently
  emitted unconditionally (`lexer.cpp:2055`), so `===` lexes even in
  `--std=c` / `--std=c++` modes today.
- `!==` requires a new peek in the lexer's `'!'` case with the same gate.
- `pch.cpp` token-id reconstruction gains the `tk3NotEq` case (the
  `tk3Eq` case exists at `pch.cpp:150`).

## 2. Semantics

`a === b` ⇔ type-domain identity of the operands AND value equality.
`a !== b` ⇔ `!(a === b)` — it **never** dispatches `operator!=`.

### 2.1 Scalar vs scalar — representation identity

Both operand types are resolved through typedefs first (typedefs are
pure aliases: `uint32_t` ≡ `unsigned int`). Then:

- **Kind must match.** Kinds: bool, integer, float, enum, pointer.
  Mixed kinds → statically false (`int === double` → false).
- **Integer:** same size AND same signedness. So `long === long long`
  → true (both 64-bit signed); `uint32_t === int32_t` → false
  (signedness); `uint32_t === uint8_t` → false (size); `char === int8_t`
  → true on this target (char is signed); `char === uint8_t` → false.
- **Float:** same size. `double === float` → false; `long double`
  distinct from `double`.
- **bool is its own kind:** `bool === uint8_t` / `bool === char` →
  false. Its domain is true/false, not 0..255.
- **Enums are their own domain:** only the *same* enum type matches
  (then values compare). `enum Color === int` → false;
  `enum Color === enum Fruit` → false.
- **Qualifiers are ignored.** `const`/`volatile` don't change a value's
  representation: `const int === int` → true (the comparison reads
  rvalues).
- **Pointers:** pointee types must match by this same predicate,
  recursively; `void*` is distinct from every `T*`. On a match, compare
  addresses (ordinary `==` pointer semantics). `int* === int32_t*` →
  true; `int* === uint32_t*` → false. Function pointers match only on
  identical signature (return + parameter types by this predicate).
- **Literals keep their C type.** `5` is `int`, `5u` `unsigned int`,
  `5L` `long`, `5.0` `double`, `'x'` `char`. So `uint32_t a === 5` →
  false, `a === 5u` → true. Suffixes give exact control; `===` never
  converts.

### 2.2 Class involved — user overload first, then the domain rule

When at least one operand is a class object (including references,
which resolve as the referenced class — existing 2d machinery):

- **User `operator===` wins.** If a viable user-declared `operator===`
  (member or free/friend — see §2.5) exists for the operand pair,
  ordinary overload resolution dispatches it, exactly like any other
  operator. Only when none exists does the domain rule below apply.
- Otherwise defer to the **same `operator==` overload resolution `==` uses**
  (member/free operator==, implicit ctor conversions, the existing
  candidate scoring). A scalar/literal that implicitly converts into
  the class's domain is compared *inside* that domain.
  - `madc::value v = 5; v === 5` → **true** iff v's runtime tag is
    integer 5. No special case: `value::operator==`
    (`src/madc_value.cpp:253`) is already tag-strict — kind mismatch
    returns false before any value compare. `v === 5.0` → false at
    runtime (integer tag vs real).
  - `std::string s("x"); s === "x"` → **true** (the literal enters the
    string domain; matches PHP, where a string literal IS a string).
- **Same class, no viable `operator===` and no viable `operator==`** →
  compile error, identical to `==` ("no match"). Same-type strict
  comparison genuinely needs the class to say how values compare.
- **No viable candidate across different types** (where `==` would be
  a compile error) → **statically false** instead. Unrelated domains
  are simply not strictly equal.

### 2.3 Statically-false lowering

A statically-false `===` lowers to the comma expression
`(left, right, 0)` (and `!==` to `(left, right, 1)`): operand side
effects are preserved, evaluation order is kept, and the result is the
constant. No warning is emitted — static falseness is the operator's
semantics, not a suspected mistake.

### 2.4 `!==` dispatch with user overloads

`a !== b` first tries a viable user `operator!==`; if none exists it is
`!(a === b)` (which itself may dispatch a user `operator===`). It never
falls back to `operator!=`.

### 2.5 User-defined `operator===` / `operator!==` (dialect extension)

- Declarable like any other overloaded operator: member, free, or
  hidden friend. The parser's operator-name grammar accepts the `===` /
  `!==` tokens after the `operator` keyword.
- **Gating is automatic:** the tokens only lex under STD_MADC (§1), so
  `operator===` in a `--std=c++` TU is a syntax error with no extra
  machinery — same philosophy as the `<=>` floor.
- **Mangling (madc_mangle, the single symbol source):** Itanium has no
  operator code for `===`, so madc uses the vendor-extended operator
  encoding `v <arity> <source-name>`:
  `operator===` ⇒ `v23eq3` (binary, source-name `eq3`) and
  `operator!==` ⇒ `v23ne3` (binary, source-name `ne3`). Member forms
  mangle with arity 1 semantics handled by the normal member-operator
  path (the encoding still records the vendor name). Only
  madc-dialect classes can declare these, so every definition and call
  site is a madc-emitted symbol — no g++/libstdc++ interop arises by
  construction.
- `= default` synthesis for `operator===` is NOT part of this track
  (no C++ default-comparison analogue; see Out of scope).
- Rationale for inclusion: if `==` on `madc::value` is later loosened
  to PHP-style juggling, its strict compare moves to `operator===` —
  the overload gives that change a home without touching `===`'s core
  rules.

### 2.6 Relation to `==` on `madc::value`

`value::operator==` is currently strict in the C++ runtime, so `==`
and `===` coincide on two `madc::value` operands today. If `==` on
`madc::value` is later loosened to PHP-style juggling, that is a
separate change to the `==` path; `===` semantics here do not move.

## 3. Lowering (CirBuilder — Tier 1, ADR 0001 / lowering-vs-raising)

- New type-system helper **`DataDef::same_representation(const
  DataDef *other) const`** implementing §2.1 (kind + size + signedness,
  enum identity, bool kind, recursive pointee). Lives on `DataDef` per
  the helper-methods rule.
- In CirBuilder's binary-operator emission (the `tkEquals → N_EQ`
  switch region, `cir_builder.cpp:7834`):
  - `tk3Eq` / `tk3NotEq`, both operands scalar: `same_representation`
    → emit `N_EQ` / `N_NE`; mismatch → `N_COMMA(l, r, 0|1)`.
  - Class involved: first resolve a user overload under the operator
    names `"==="` / `"!=="` (the operator-name mapping at
    `cir_builder.cpp:4803` gains `tk3Eq → "==="`, `tk3NotEq → "!=="`,
    tried before the fallbacks); if none, `tk3Eq` routes through the
    **identical** operator-dispatch path `tkEquals` uses (`"=="`), and
    `tk3NotEq` wraps the resolved `===` compare in `N_NOT`. No viable
    candidate + different types → the comma-constant lowering; same
    type → propagate the same error `==` raises.
  - `madc_mangle` gains the vendor-extended operator names (§2.5):
    `operator===` ⇒ `v23eq3`, `operator!==` ⇒ `v23ne3`; `test_mangle`
    pins both.
- `Token3Eq` (and `Token3NotEq`) remain in the high-level tree — the
  MC11-IR both-views invariant. c2mir sees only ordinary C11 nodes.
  **Zero MIR-fork changes.**
- `Token3Eq::ioperate/foperate` constant-fold predicates currently use
  `datatype() == datatype()`; align them with `same_representation`
  (no live consumers found in the survey — verify during
  implementation; `Token3NotEq` mirrors whatever `Token3Eq` does).

## 4. Renderers & adjacent surfaces

- `--emit=c11` emits the lowered C (`==`, `!=`, `!`, comma-expr) —
  portable C11, no renderer work.
- High-level renderers (madc view) re-emit `===` / `!==` from the
  retained tokens.
- The **eval-DSL** (`madc_program.cpp` `rewrite_expression_string_compares`)
  keeps its documented value-compare rewrite for string operands
  (`===` on two DSL strings → `strcmp(a,b) == 0`); the DSL is a
  separate, spec'd surface and is unchanged by this design.

## 5. Error handling

- No new diagnostics beyond §2.2's "same class, no operator==" (reuses
  the `==` error path verbatim).
- Below the std floor there is no `===` token, so misuse surfaces as
  the natural C/C++ syntax error — same philosophy as `<=>` gating.

## 6. Testing

- **`tests/test3eq.mad` + `.expect`** (JIT integration): the motivating
  uint32/int32/uint8 example with `==` vs `===`; `long === long long`;
  `double === float` false / `double === double` ==; bool vs uint8_t;
  enum vs int and enum vs same/different enum; literal strictness
  (`a === 5` false, `a === 5u` true); pointer pointee rule + address
  compare; `madc::value === 5 / 5.0 / "5"`; `std::string === "x"` /
  different value; side-effect preservation of a statically-false
  compare (`f() === g()` with counters); the full `!==` mirror of each;
  a class declaring `operator===` (e.g. Money: `==` compares amount,
  `===` compares amount AND currency) exercising member and free
  forms, `!==` fallback negation, and an explicit `operator!==`.
- **`tests/test3eqgate.*`** (`.flags` with `--std=c++17`,
  `.expect_err`): `a === b` must fail to compile outside STD_MADC —
  test3waygate precedent.
- **doctest unit** for `DataDef::same_representation` in `tests/unit/`.
- Gates per the standing method: `make -C src fulltest` (cap every
  run), full gcc-torture failset diff (the lexer `=` / `!` paths are
  touched and the gating removes `===` from C modes), SMAUG soak,
  zero regressions.

## Out of scope

- Loose `==` juggling on `madc::value` (PHP-style `5 == "5"`). When it
  happens, `value`'s strict compare moves to `operator===` (§2.5).
- `= default` synthesis for `operator===` / `operator!==` (no C++
  default-comparison analogue to mirror).
- Warning on statically-false `===` (could be added later if wanted).
