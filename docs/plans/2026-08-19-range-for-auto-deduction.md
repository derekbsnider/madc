# `for (auto &kv : m)` — deduce the range-for element type at parse time

**Status:** IMPLEMENTED (this session; see the commit carrying this doc)

## Measured at live HEAD (v0.85.0 + the libc-fallback commits)

| case | result |
|------|--------|
| `auto it = m.begin(); ++it; it->first` | ✓ works (an earlier session note claiming this failed to parse was WRONG — re-measured) |
| `for (std::pair<const std::string,int> &kv : m)` | ✓ works |
| `for (auto &kv : m)` over `std::map` | ✗ "member reference is not a structure or union" on `kv.first` |
| `for (auto &x : v)` / `for (auto x : v)` over `std::vector<int>` | ✗ c2mir check errors ("incompatible argument type for arithmetic type parameter") |

One defect, one place: `auto` **as the range-for element type**, both container
shapes. Everything else about `auto` and about range-for already works.

## Root cause — an ordering bug in `TokenFOR::parse()` (src/parser.cpp ~45216)

The parser sets `fe->elemtype = &dt->definition` and declares `fe->elemvar`
**before** `fe->container = pgm.parseExpression(...)` runs. With `auto`,
`dt->definition` is `ddAUTO` (size 0, dtVOID), so:

- the body parses with `kv` typed as the placeholder → `kv.first` has no class
  to resolve against → the map case's error;
- the CIR receives an element of size 0 → the vector case's check errors.

The container's type is fully known one statement later. Nothing about the
element can be deduced before the container parses; everything can be after.

## Fix (parse layer — where the type originates, Rule #2)

1. **Reorder:** parse the container expression FIRST, then declare the element
   variable. This is also what C++ says — [stmt.ranged] evaluates the range
   outside the loop-variable's scope, so `for (auto x : x)` must bind the
   RANGE `x` to the outer variable. The current order is a latent shadowing
   bug even without `auto`; the reorder fixes both. (Ship a shadowing case in
   the test so the scope claim is measured, g++/clang++ oracled.)
2. **Deduce when `elemtype == &ddAUTO`,** from the container expression's type:
   - raw array `T[n]` → `T`;
   - madc `array` (ddARRAY) → **`string`** — DECIDED DEFAULT: the element model
     follows task #91 R0's subscript ruling (string-first, the Python/PHP
     element model behind `sys.argv[i]`), so `auto` answers what `a[i]` answers.
     NOT `value`: `for (value v : a)` does not compile today (measured,
     tmp/auto/fv.mad — three c2mir check errors; a LOUD feature gap, recorded
     here as adjacent work, not chased in this arc), and a deduction that
     produces a broken loop would be worse than none;
   - class container → `class_index_iteration_protocol` (element =
     `operator[]`'s return) else `class_iterator_iteration_protocol`
     (element = `ip.elem`);
   - deduction failure names the container type, same refusal style as the
     dumper's.
   Gate the deduction on the SAME `--std=` predicate the declaration-`auto`
   path uses (parser.cpp ~62023: `!is_c_mode() || language_std == STD_C23`),
   extracted into a named helper (`auto_deduction_allowed()`) instead of a
   second copy of the expression (helper-methods rule).
3. **`const`/`&` composition:** `for (auto &kv : m)` binds a reference to
   `std::pair<const K, V>` — the deduced type must keep the recognizer's
   answer EXACTLY (no stripping the pair's const member), since the working
   explicit-type case proves the downstream machinery handles it.

## Prerequisite refactor — make the iterator recognizer parser-callable

`class_iterator_iteration_protocol` is a non-static CirBuilder member for
exactly ONE line: the `class_return_via_retbuf(...)` viability check (memoized
`class_needs_dtor` — builder state, so the method cannot simply be made
static). The split that keeps one owner:

- The recognizer becomes `static`, taking an optional `CirBuilder *abi` last
  parameter. When non-NULL (every existing call site) it performs the retbuf
  viability check exactly as today, same decline spelling. When NULL (the
  parser's deduction call) the STRUCTURAL answer is returned without the ABI
  check.
- Worst case for the parser's NULL: deduction succeeds on a container whose
  iterator codegen later refuses — and that refusal already prints its named
  decline, so there is no silent path. The alternative (hoisting the check to
  call sites) would put one decline spelling in two places.

## Oracle (already captured)

`tmp/auto/fa.cpp` — g++ and clang++ -O0 agree byte-for-byte (vector by value,
vector mutated through `auto &`, map `.first`/`.second` through `auto &` and
`const auto &`, raw array, and the shadowing case `for (auto x : x)` summing 6).

## What implementation added beyond this plan

- The reorder exposed a SECOND shadowing defect one layer down: the raw-array
  LOWERING referenced the container by emitted name inside the wrap block that
  declares the element, so `for (auto x : x)` subscripted the just-declared
  element (loud for an int element; a POINTER element compiled and read
  garbage — the silent shape). Fixed where the names are emitted:
  `translate_foreach_loop` gained a `prelude` channel and the carray arm
  captures the range into a unique `T *__fe_a_N` BEFORE the element
  declaration, typed by the ARRAY's element (never the declared loop element —
  `for (long e : int_arr)` must not stride 8 over 4-byte slots).
- The recognizers moved into a PUBLIC window of CirBuilder — the parser is
  their third consumer, and that is the whole point of having shared ones.
- The gate predicate became `Program::auto_deduction_allowed()`, replacing the
  inline expression at the declaration-`auto` site (one owner).

## Tests

`tests/testforeachauto.mad`, oracled against g++ AND clang++ -O0:
`for (auto x : vector)`, `for (auto &x : vector)` (mutating through the ref),
`for (auto &kv : map)` reading `.first`/`.second`, `for (const auto &kv : map)`,
`for (auto e : int_array)`, `for (auto s : madc_array)` (deduces string), and the shadowing
case `int x[3]; for (auto x : x)`. The failing shapes above become the expect
lines; `tests/testforeachiter.mad` (explicit types) stays as the
no-regression control.
