# Test Status

Test results as of April 14, 2026 (Phase 1 + string parameter fix).

Run with: `bin/madc tests/<name>.mad`

## Passing Tests — 24/24

| Test | What it tests | Output |
|------|--------------|--------|
| `test.mad` | String variable, puts() | Prints string |
| `test2.mad` | Large loop (100M iterations) | `100000000` |
| `test3.mad` | Basic program structure | Runs silently |
| `test4.mad` | Char literals, putchar(), user-defined string funcs | `Hello, World!`, `hi`, `test`, `Hello, World!`, `HEY`, `hey 123`, `v0.0.1` |
| `test5.mad` | String ops | `Hello, World!`, `hi` |
| `testassign.mad` | Variable assignment | `456` |
| `testbsl.mad` | Bit shift operators | `200`, `40` |
| `testcout.mad` | cout stream output | `This is a test, x = -1` |
| `testfor.mad` | For loop | `a == 5` |
| `testfunc.mad` | User-defined functions | `10`, `15` |
| `testif.mad` | If/else | `this is a test` |
| `testif2.mad` | If with integer condition | `1` |
| `testinc.mad` | Increment/decrement | `1`, `0` |
| `testint.mad` | Integer types, assignment | `123: 123`, `i: 456`, `j: 456` |
| `testlocal.mad` | Local string variable | `Hello, World!` |
| `testmath.mad` | Integer arithmetic | `0`, `-2` |
| `testmath2.mad` | More arithmetic | `15`, `5` |
| `testnot.mad` | Bitwise NOT | `-2`, `-1` |
| `testprint.mad` | String print | `Hello, World!` |
| `testreturn.mad` | Function return values | `100`, `101` |
| `testsstream.mad` | Stringstream | `456`, `123`, `5`, stream content, `This is a test to cout: 5` |
| `teststruct.mad` | Struct member access | `test.name: Joe Blow`, `test.id: 2`, `test.age: d` (uint8=char in stream) |
| `testversion.mad` | Version string | `v0.0.1` |
| `testwhile.mad` | While loop | `100000000` |

## Phase 1 Fixes Applied

| Fix | Status | Details |
|-----|--------|---------|
| `-v/--verbose` flag | ✓ Done | `DBG()` macro gated on `madc_verbose`; parse `-v`/`--verbose` in `main()` |
| Char literal compile | ✓ Done | Added `TokenChar::compile()` and `operand()`; `case ttChar:` in `TokenBase::compile()` |
| Error reporting | ✓ Done | `throwbuf::sync()` prints before throwing; catch block was correct |
| Struct member access | ✓ Done | Fixed `addOffset` vs `setOffset`; load numeric members into Gp; LEA for string members; construct/destruct string members in struct |
| `register` keyword | ✓ Done | Added `vfREGISTER` flag, `TokenREGISTER` token, parsed in `TokenREGISTER::parse()` |
| doctest framework | ✓ Done | `include/doctest.h`, `tests/unit/test_datadef.cpp` (25 tests), `make test` |

## Unit Tests

Run with: `make -C src test`

| Test File | Tests | Status |
|-----------|-------|--------|
| `tests/unit/test_datadef.cpp` | 25 | All pass |

## Phase 2 Fixes Applied

| Fix | Status | Details |
|-----|--------|---------|
| String parameter pass-by-ref | ✓ Done | `voperand()` creates bare Gp for `vfPARAM` non-numeric vars; `cleanup()` skips param destruction |
| `dtSTRING → dtCHARptr` coercion | ✓ Done | `string_cstr()` helper auto-converts string args to `const char*` when calling `puts()` etc. |

## Known Issues

- String pass-by-value is implemented as pass-by-reference (caller's string is shared, not copied)
- No true string copy semantics yet for function parameters
