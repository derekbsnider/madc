# Test Status

Test results as of May 2, 2026 (post-v0.13.0 plus first-wave C23 compatibility and ongoing Phase 4.2 libmadc API work: `madc::value`, `madc::error`, logging lifecycle fixes, exploratory storage/federation API contracts, working `dsv://` + `flr://` + `vlr://` + `qdbm://` + `gdbm://` + `bdb://` backend slices, FLR tombstone sidecars with pre-reap restore, and registration-based `infer_mapper()` for host C++ storage types).

Run with: `bin/madc tests/<name>.mad` or `make -C src fulltest`

## Current Batch Status — 254 passed, 0 failed, 0 timed out, 0 skipped

Latest `scripts/run_tests.sh` result:

- Passing: 254 integration tests
- Failing: none
- Timed out: none
- Unit tests: all passing (doctest) — 80 datadef + 23 IR + 1 libmadc_bdb + 2 libmadc_dsv + 3 libmadc_flr + 1 libmadc_gdbm + 1 libmadc_qdbm + 5 libmadc_error + 19 libmadc_value + 1 libmadc_vlr + 10 libmadc_storage_contract

The latest IR-focused validation batch passes directly, including:

- `testassignexprmem.mad`
- `testcompoundassignmem.mad`
- `testderefarray.mad`
- `testassign.mad`
- `testassigninexpr.mad`
- `testc23_bool.mad`
- `testcin.mad`
- `testfnptrtypedef.mad`
- `testint.mad`
- `testpostfix.mad`
- `test_ptr_fn_deref.mad`
- `test_get_argv_deref.mad`
- `test_errno_deref.mad`
- `testfnptrmemberarrow.mad`
- `testglobalptrarrayarrow.mad`
- `testmapidentifier.mad`
- `testderefparenarrow.mad`
- `testfnptrcast.mad`
- `testcaseconstexpr.mad`
- `testneginit.mad`
- `testdupliteral.mad`
- `testderefmember.mad`
- `testdirtype.mad`
- `testternaryvalue.mad`
- `testternarystring.mad`
- `testsizeofexpr.mad`
- `testarrayc.mad`
- `testcompoundnarrow.mad`
- `teststringcast.mad`
- `teststrcmpret.mad`
- `teststrcharptrarr.mad`
- `testptrarith.mad`
- `testdoublestore.mad`
- `testdoublecompound.mad`
- `teststrarrinit.mad`
- `testsigneddiv.mad`
- `teststructptrsub.mad`
- `testfloat.mad`
- `testintsuffix.mad`
- `testdoubleptr.mad`
- `testderefeq.mad`
- `testderefcmp.mad`
- `teststructdoublecompound.mad`
- `testdoubleptrwrite.mad`
- `testfloatvarargs.mad`
- `testderefpostincstore.mad`
- `teststructcopy.mad`
- `testparenderefmember.mad`
- `testleadingdotfloat.mad`
- `testsubscriptexprmember.mad`
- `teststructarrsub.mad`
- `testrealconstfold.mad`
- `testclassident.mad`
- `testreturnnextident.mad`
- `testcompoundsubexpr.mad`
- `testnegbraceInit.mad`
- `testcharnoterm.mad`
- `testgoto.mad`

## Passing Tests — 185 integration (latest batch)

`scripts/run_tests.sh` drives `testcin.mad` with piped stdin (`Alice 42
hello world`) and `testargv.mad` with argv (`hello world`), asserting
on their output instead of skipping. The runner now also reports
`TIMEOUT: tests/...` explicitly when `timeout 5` kills a spinning test,
instead of collapsing that case into a generic `FAIL`.

### New post-v0.8.0 (SMAUG Phase F regressions — hashstr.mad runs)

| Test | What it tests |
|------|--------------|
| `testincmember.mad` | Prefix/postfix inc/dec on struct members (`++ptr->links`, `obj.f--`), including if-guarded for the size-aware load/store path |
| `testunsignedcmp.mad` | Unsigned comparisons in if-conditions (setb/seta path) for short and int |
| `testglobalptr.mad` | Global pointer variable read/assign (DataDefPTR qword overrides) |
| `testsubtomember.mad` | `p->next = arr[i]` — subscript result into a struct member Mem |
| `testcastargcomma.mad` | Cast+arith as first call arg with a following comma, e.g. `strcpy((char *)h+8, "x")` |
| `testcommaincrement.mad` | `for (...; ptr = ptr->next, c++)` — SMAUG's comma-increment pattern |
| `testpostdeclstr.mad` | `char *p; p = "literal";` and `r->name = "literal";` |
| `testcoutcstr.mad` | Chained `cout << char*` output, including function-returned `char*` and mixed string-prefix chains |
| `testdeclassignexpr.mad` | Assignment as an expression inside declaration initializers (`int y = (x = 42)`) |
| `testprintfmember.mad` | Varargs wrapper calls with `->` member arguments, macro-expanded nested members, and plain `printf` mixes |
| `testprintfdouble.mad` | `%f` / `%e` / `%g` formatting through direct `printf` and `...` wrappers, including mixed args and multiple doubles |
| `testsmaug_requests.mad` | Upstream SMAUG `requests.c` compatibility test with a minimal `mud.h` shim and embedded POSIX/C headers |
| `testc23_bool.mad` | C `_Bool` keyword aliasing to madc's bool type, including scalar and fixed-array initialization |
| `teststaticassert.mad` | `_Static_assert` / `static_assert` with arithmetic, `sizeof`, and `alignof` constant expressions |
| `testalignof.mad` | `alignof` / `_Alignof` on primitive, pointer, struct, array, and member expressions |
| `testtypeof.mad` | `typeof(expr)` / `typeof(type)` driving global and local declarations |
| `testnullptr.mad` | Typed `nullptr` literal in pointer declarations and boolean tests |
| `testdigitsep.mad` | C23 digit separators in decimal, hex, binary, and floating literals |
| `testbinlit.mad` | C23-style binary integer literals (`0b...` / `0B...`) in assignments, expressions, and conditions |
| `testrestrict.mad` | `restrict` as a parsed no-op qualifier in pointer declarations and function parameters |
| `testflock.mad` | Embedded `<sys/file.h>` and `flock()`/`LOCK_*` constants via dlsym fallback |
| `testincludeonce.mad` | `#include` include-once behavior for repeated local includes within a single compile |
| `testassigninexpr.mad` | Assignment expressions used in `while` / `if` conditions and chained assignment value flow |
| `testassignexprmem.mad` | Stack-local Mem destinations on plain arithmetic / `%` expressions |
| `testcompoundassignmem.mad` | Stack-local compound assignment with Mem-backed LHS (`*=`, then `+=`) |
| `testderefarray.mad` | Unary `*` on fixed arrays (`!*buf`, `*word`) via array-to-pointer decay |
| `test_ptr_fn_deref.mad` | Dereference of a user-function `char *` return (`*get_msg()`) |
| `test_get_argv_deref.mad` | Dereference of a method-call `char *` return (`*(version.c_str())`) |
| `test_errno_deref.mad` | Dereference of builtin/external pointer-return path via `errno` / `__errno_location()` |

### New in Phase E / F session

| Test | What it tests |
|------|--------------|
| `testchain.mad` | Chained `->` and `.` member access (a->b->c, a->b.c, a.b.c) |
| `testfixedarr.mad` | C fixed-size arrays (1D + multi-dim), brace init, char* init, string-literal init |
| `teststructinit.mad` | Struct initializer lists and array-of-structs init |
| `teststructinterop.mad` | struct tm, struct timeval, struct fd_set + FD_* macros, select() |
| `testfileline.mad` | `__FILE__` / `__LINE__` predefined macros, including inside function-like macros |

### New tests added in this session

| Test | What it tests |
|------|--------------|
| `testcompoundassign.mad` | All 10 compound assignment operators (+=, -=, *=, etc.) |
| `testfortypedcomma.mad` | Typed `for` initializer with comma-separated declarations (`for (int i = 0, j = 10; ...)`) |
| `testhex.mad` | Hex integer literals (0xFF, 0xDEAD, 0X1A) |
| `testpostfix.mad` | Postfix x++/x-- with old-value-return semantics, including `for` and `while (x--)` |
| `testdefine.mad` | #define, #undef, #ifdef, #ifndef, #if, #elif, #else, #endif |
| `testlibc.mad` | dlsym fallback: getpid(), sleep(), getuid(), getppid() |
| `testmathh.mad` | #include <math.h>: M_PI, sqrt, floor, ceil, fabs, pow, sin, cos |
| `testargv.mad` | int main(int argc, char **argv) — requires cmd args (manual) |
| `teststruct3.mad` | C ABI alignment, __attribute__((packed)), mixed field sizes |
| `testsizeof.mad` | sizeof(type), sizeof(struct), sizeof in expressions |

### Notes

- `testcin.mad` is driven by `scripts/run_tests.sh` with piped stdin
- `testargv.mad` is driven by `scripts/run_tests.sh` with argv
- `include_helper.mad` is not standalone (included by testinclude.mad)
- `include_once_helper.mah` is not standalone (included by testincludeonce.mad)
- All tests that use `cout`/`cin`/`cerr`/`endl` now require `#include <iostream>`

## Previously Passing Tests — 54/54 integration + 25/25 unit

| Test | What it tests | Output |
|------|--------------|--------|
| `test.mad` | String variable, puts() | Prints string |
| `test2.mad` | Large loop (100M iterations) | `100000000` |
| `test3.mad` | Basic program structure | Runs silently |
| `test4.mad` | Char literals, putchar(), user-defined string funcs | `Hello, World!`, `hi`, `test`, `Hello, World!`, `HEY`, `hey 123`, `v0.0.1` |
| `test5.mad` | String ops | `Hello, World!`, `hi` |
| `testassign.mad` | Variable assignment | `456` |
| `testbsl.mad` | Bit shift operators (`<<` and `>>`) | `200`, `40`, `16`, `15` |
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
| `testns.mad` | Namespace resolution (std::) | `Hello from std::cout!`, `x = 42`, stderr output, unqualified still works |
| `testphp.mad` | php:: namespace functions | trim/ltrim/rtrim, ucfirst/lcfirst, str_replace, str_repeat, explode/implode, sort, nested-array `array_column` |
| `teststruct2.mad` | User-defined structs | `p.x: 10`, `p.y: 20`, `bob.name: Bob Smith`, `bob.age: 42`, `bob.id: 1001` |
| `testclass.mad` | Class definitions with data members | `p.x: 100`, `p.y: 200`, `bob.name: Bob`, `bob.age: 30` |
| `testinclude.mad` | `#include` directive | `Hello, World!`, `Hello, Mad-C!`, `include works!` |
| `testusing.mad` | `using namespace std` | `using namespace std works!` |
| `testwhile.mad` | While loop | `100000000` |
| `testcapture.mad` | Lambda capture of outer variables | Captured values printed |
| `testcin.mad` | `cin >>` input from stdin | Reads and echoes user input (needs stdin) |
| `testcolon.mad` | `:=` short variable declaration (Go-style type inference) | Inferred-type variables |
| `testdefer.mad` | `defer` statement (Go-style deferred execution) | Deferred output at scope exit |
| `testdlcall.mad` | `dlcall()` through function pointer | Calls C library function via pointer |
| `testdlopen.mad` | `dlopen`/`dlsym`/`dlclose` | Loads and calls shared library symbols |
| `testescape.mad` | Escape sequences in string literals (`\n`, `\t`, etc.) | Formatted output with escapes |
| `testforeach.mad` | Range-based `for (type var : array)` | Iterates over MadArray elements |
| `testforeach2.mad` | Range-based for with STL containers | Iterates over vector/map/set |
| `testfstream.mad` | File I/O with ifstream/ofstream/fstream | Read/write file operations |
| `testfuncptr.mad` | Function pointers via `auto fn = func` | Calls through stored function pointer |
| `testlambda.mad` | Lambda expressions with `auto` and `[]` | Defines and calls inline lambdas |
| `testlang.mad` | Multi-language namespace usage in one program | php/perl/python/ruby/js functions together, including `ruby::chars` |
| `testloop.mad` | Loop constructs (for, while, do-while) | Various loop patterns |
| `testmadc_ns.mad` | `madc::` namespace (regex, array) | madc::regex_match, regex_search, regex_replace |
| `testmap.mad` | `map<K,V>` typed STL container | Insert, find, erase, iterate |
| `testmethod.mad` | Class methods with `this` pointer | Method call compiles and dispatches |
| `testmultiret.mad` | Multiple return values (Go-style) | Function returns multiple values via `__retbuf`; runtime output asserted via `.expect` |
| `testprefer.mad` | Namespace precedence directives | `prefer rust, c;` and `#pragma prefer rust, c` change bare identifier lookup order |
| `testrust.mad` | rust:: namespace helpers | trim/contains/replace, split/join, first/last/get, push/pop |
| `testrubycharsshadow.mad` | Namespace-call argument shadowing | `ruby::chars(chars, s)` resolves local arg, not namespace function |
| `testperl.mad` | perl:: namespace functions | chop, chomp, split, join, grep, glob |
| `testregex.mad` | Regex functions (match, search, replace) | Pattern matching and substitution |
| `testset.mad` | `set<T>` typed STL container | Insert, find, erase, iterate |
| `testsubscript.mad` | `[]` subscript operator on strings and containers | Indexed access |
| `testswitch.mad` | `switch`/`case`/`default` statement | Branch selection by value |
| `testternary.mad` | Ternary operator (`cond ? a : b`) | Conditional expression |
| `testvector.mad` | `vector<T>` typed STL container | push_back, size, at, iterate |

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
| `tests/unit/test_ir.cpp`      | 23 | All pass — IR Stage 0 scaffolding + Stage 1/2 coerce coverage |

## Phase 2 Fixes Applied

| Fix | Status | Details |
|-----|--------|---------|
| String parameter pass-by-ref | ✓ Done | `voperand()` creates bare Gp for `vfPARAM` non-numeric vars; `cleanup()` skips param destruction |
| `dtSTRING → dtCHARptr` coercion | ✓ Done | `string_cstr()` helper auto-converts string args to `const char*` when calling `puts()` etc. |
| User-defined structs (2.1) | ✓ Done | `TokenSTRUCT::parse()` parses `struct Name { type member; ... };`, builds `DataDefSTRUCT` dynamically, registers in `struct_map` |
| Namespace resolution (2.3+2.4) | ✓ Done | `namespace_map` registry, `::` resolution in `parseExpression()`, `std::` namespace with cout/cerr/endl |
| `#include` + `using` (2.5) | ✓ Done | Lexer handles `#include "file.mad"` with relative paths; parser handles `using namespace X;` and `using X::member;` |
| Class definitions (2.2) | ✓ Done | Data members, `class Name { ... };` syntax, type registered in `datatype_map` for prefix-free use |

## Known Issues

- String pass-by-value is implemented as pass-by-reference (caller's string is shared, not copied)
- No true string copy semantics yet for function parameters
