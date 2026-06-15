# Plan — faithful `__is_assignable` / `__is_constructible` intrinsics (vector cluster)

**Status: RECON DONE 2026-06-15 (SESSION 15) — ready for review, then implement
incrementally (Slice 1 `__is_assignable`, Slice 2 `__is_constructible`).** Branch
`feature/retire-embedded-shims-claude` (LOCAL-ONLY). Goal set by user: "Compiler
work: Group 1 — vector cluster." Companion handoff
`docs/plans/2026-06-12-retire-embedded-shims-HANDOFF.md` §1b.

## 1. Problem / what this unblocks

The vector cluster — **testvector, testforeachref, teststringref,
testsubscriptmember, testsubscriptarrow, testvectorptr** (and the valid 3-arg
`std::for_each` path) — fails at `stl_vector.h:428` (a clone-artifact stamp; the
real site is `bits/stl_uninitialized.h`). Minimal reducer (3-oracle: g++/clang
accept, madc fails):

```cpp
#include <vector>
using namespace std;
int main(){ vector<int> v; v.push_back(7); return 0; }
// madc: "use of undeclared identifier '_ValueType2'" @ stl_vector.h:428
//       + MIR: import of undefined item ..._M_realloc_insert
```

`push_back` → `_M_realloc_insert` → `std::uninitialized_copy`
(stl_uninitialized.h:161-186). The body:

```cpp
typedef typename iterator_traits<_ForwardIterator>::value_type _ValueType2;  // 168
using _From = decltype(*__first);                                            // 180
const bool __assignable = _GLIBCXX_USE_ASSIGN_FOR_INIT(_ValueType2, _From);  // 182
```

with (C++11/17, stl_uninitialized.h:99-101):

```cpp
#define _GLIBCXX_USE_ASSIGN_FOR_INIT(T, U) \
    __is_trivial(T) && __is_assignable(T&, U) && std::__check_constructible<T, U>()
```

## 2. Root cause (two unhandled intrinsics)

madc's type-trait support is a **correctness-first whitelist** (`type_trait_arity`,
parser.cpp ~4905): only traits madc can answer FAITHFULLY are recognized; the rest
fall through to the undeclared-identifier path (a clear error, never a WRONG bool
that would corrupt SFINAE). Currently: `__is_same`/`__is_base_of` (arity 2);
`__is_class`/`__is_union`/`__is_enum`/`__has_trivial_destructor`/`__is_trivial`
(arity 1). **`__is_assignable` and `__is_constructible` are both ABSENT:**

- `__is_assignable(_ValueType2&, _From)` → `__is_assignable` arity 0 → not a trait
  → parsed as a plain call; the type `_ValueType2&` in value context → "use of
  undeclared identifier '_ValueType2'". (The CURRENT error — it dies here, on the
  2nd `&&` term, before reaching the 3rd.)
- Once `__is_assignable` is added, parse continues to
  `std::__check_constructible<T,U>()`, whose body is
  `static_assert(is_constructible<T,U>::value, ...)` (stl_uninitialized.h:83-90).
  `is_constructible` = `__bool_constant<__is_constructible(_Tp,_Args...)>`.
  **VERIFIED madc cannot fold it:** `static_assert(__is_constructible(int,int))` →
  "Expecting integer constant expression"; `cout << is_constructible<int,int>::value`
  prints `0` for ALL inputs (latent: silently false, not just unhandled). So the
  static_assert fires → compile error. **Hence both intrinsics are required.**

## 3. Canon model (g++ AND clang — verified identical, `-fsyntax-only`)

Both are 2+arg compiler **intrinsics** (libstdc++'s `std::is_assignable` /
`std::is_constructible` are thin `__bool_constant<__is_xxx(...)>` wrappers; the
compiler owns the semantics). Truth tables PASS on both canons:

`__is_assignable(To, From)` ≡ "is `declval<To>() = declval<From>()` well-formed":
| expr | result | rule |
|------|--------|------|
| `__is_assignable(int&, int)` | true | lvalue-ref to non-const scalar, From convertible |
| `__is_assignable(const int&, int)` | false | const referent |
| `__is_assignable(int, int)` | false | prvalue non-class — can't assign |
| `__is_assignable(OK&, const OK&)` | true | class copy-assign available |
| `__is_assignable(NoAssign&, const NoAssign&)` | false | operator= deleted |

`__is_constructible(T, Args...)` ≡ "is `T obj(declval<Args>()...)` well-formed":
| expr | result |
|------|--------|
| `__is_constructible(int, int)` | true |
| `__is_constructible(int)` | true (value-init) |
| `__is_constructible(int, int*)` | false |
| `__is_constructible(S /*S(int)*/ , int)` | true |
| `__is_constructible(S)` | false (no default ctor) |

**REF- AND CV-QUALIFICATION ON THE ARGS IS LOAD-BEARING** (`int&`≠`int`,
`const int&`≠`int&`) — unlike `__is_same`/`__is_trivial`, which are ref-insensitive.

Clang specifics (user-requested): clang implements both in
`clang/lib/Sema/SemaExprCXX.cpp` `EvaluateUnaryTypeTrait`/`TypeTrait` via
`Sema::IsAssignable` / `Sema::isConstructible`, which run REAL overload resolution
on `operator=` / constructors in an unevaluated context (no SFINAE recursion limit
issues; pure well-formedness). gcc's `cp/semantics.cc` `is_xible` does the same via
`build_new_op`/`build_special_member_call`. Both reduce the trait to "does name
lookup + overload resolution succeed for this assignment/construction." That is
exactly the model madc must mirror — and madc ALREADY has the resolution
machinery (see §5).

## 4. madc architecture to touch

- `type_trait_arity` (parser.cpp ~4905): add the two names. `__is_assignable` → 2.
  `__is_constructible` → VARIADIC (≥1) — `type_trait_arity` returns a fixed int
  today; add a variadic sentinel (e.g. -1) + handle it in both parsers.
- **TWO eval paths, both need the new branches AND ref/cv-aware arg capture:**
  - `Program::evaluate_type_trait` (parser.cpp 5004): stream-based, returns a
    `TokenInt`. Arg loop (5012-5035) folds trailing `*` (→ pointer DataDef) and
    calls `consume_template_type_arg_qualifiers` capturing `cv_spelling` **which it
    then DISCARDS**; it does **NOT** handle `&`/`&&`. VERIFIED: `__is_same(int&,int&)`
    → "Expecting ',' or ')'".
  - the `fold` path (parser.cpp ~13497-13540): token-vector based, used by the
    constexpr/`static_assert`/non-type-template-arg evaluator. Same dispatch list,
    same arg-capture gap.
- Both currently model an arg as a bare `DataDef *`. For these two intrinsics each
  arg must carry **{DataDef*, ref-kind ∈ none|lvalue|rvalue, is_const}**. madc
  represents references as a FLAG (`vfREFERENCE`/`returns_ref`), not a distinct
  DataDef — so this is a small per-arg struct, not a new type.

## 5. Faithful algorithm (reuse existing resolution — no new resolver)

The handoff's "half-faithful is FORBIDDEN" bar is met by REUSING the same
resolution madc uses for real assignment/construction, as a PROBE:

- **`trait_is_assignable(To, From)`** (mirror the truth table):
  1. `To` not a reference AND `To` non-class → **false** (prvalue scalar).
  2. `To` referent `const` → **false**.
  3. `To` scalar/pointer/enum (lvalue-ref, non-const) → **true** iff `From` is
     convertible to the referent (same/arith/pointer-compatible). Mechanical.
  4. `To` class → resolve `operator=` for the class with argument `From`: check
     `method_map["operator="]` (user/overload) else the implicit copy/move-assign,
     recursing on bases+members for availability (mirror `trait_has_trivial_destructor`'s
     shape + the `class_copy_assign` lookup, cir_builder.cpp ~4373/4392). Deleted /
     no-viable → false.
- **`trait_is_constructible(T, Args...)`**: reuse ctor overload resolution
  (`class_ctor_call` machinery): 0 args → default-ctor availability; 1 arg → copy/
  convert/converting-ctor; N args → matching ctor. Scalar T: 0/1 arg with a
  convertible source → true (per table). const/ref T edge cases per canon.

Keep both EXACT for every input or fall through to error — never a guess. Where
madc genuinely cannot resolve a class's operator=/ctor faithfully yet, FALL THROUGH
(clear error) rather than emit a wrong bool — but the cluster's inputs (int, T*,
std::string copy/move assign+construct) are all resolvable.

## 6. Slices (incremental, each fully validated)

- **Slice 1 — `__is_assignable` + the ref/cv arg-capture infrastructure.** Add the
  per-arg ref/cv capture to BOTH parsers (regression-test the existing traits —
  `__is_same`/`__is_trivial` must be byte-identical for non-ref args), add the trait
  + `trait_is_assignable`. Reducer: the §3 `__is_assignable` truth table (already
  passes both canons as `tmp/isa.cpp`). Then re-run the vector reducer: EXPECT it to
  advance from "_ValueType2 undeclared" to the `__check_constructible` /
  is_constructible wall — confirming Slice 2 is needed and isolating it.
- **Slice 2 — `__is_constructible` (variadic).** Variadic arity in both parsers +
  `trait_is_constructible` reusing ctor resolution. Truth table (§3). Then the
  vector reducer should clear both trait walls. EXPECT a NEXT wall behind them
  (re-diagnose live — likely `_M_realloc_insert` body translation or a further
  trait); record it.

## 7. Validation (each slice)

- **3-oracle truth tables FIRST** (g++ AND clang `-fsyntax-only` + madc; the isa/isc
  static_assert tables — commit them as `tests/testisassignable.mad` /
  `tests/testisconstructible.mad`, default mode where possible).
- **Regression-guard the existing traits**: `__is_same`/`__is_trivial`/`__is_class`/
  `__is_base_of`/`__has_trivial_destructor` must not change for any current input —
  the arg-capture refactor touches their shared parser. Run the existing trait tests.
- **Vector reducer** + `gcc-on-emitted-C` after each slice.
- **`make -C src fulltest`** — zero regression. These intrinsics feed SFINAE
  broadly (is_assignable/is_constructible/is_copy_assignable/move detection across
  libstdc++) — a wrong bool corrupts trait selection suite-wide. Watch for NEW
  failures in tests that already pass, not just the vector cluster.
- Re-check the whole vector cluster (6 tests) — some may flip, some may reveal
  deeper walls (the `_M_realloc_insert` body, `rebind`, vector<T*>).

## 8. Open questions (resolve at implementation)
- Does the `fold` path (13527) get reached for these in `static_assert` /
  non-type-template-arg contexts, or only `evaluate_type_trait` (5004)? Instrument
  both; the `__check_constructible` static_assert uses the constexpr fold.
- `__is_constructible` variadic arg-capture: confirm the parsers can collect N
  ref/cv type-ids cleanly (the `*`-fold loop already iterates; extend for `&`/cv + N).
- Faithful class operator=/ctor "availability" — does madc track `= delete` on
  operator=/ctors precisely enough? If not, a conservative-but-WRONG answer is
  forbidden; verify against the cluster's real types (std::string, int, T*) and
  fall through (error) for any class madc can't resolve exactly.
- After both slices, what is the NEXT vector wall? (Almost certainly more — the
  cluster is deep. Record it for the next slice.)
