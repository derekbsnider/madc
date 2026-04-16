# Changelog

## [Unreleased] — SMAUG Goal + Full POSIX Header Coverage (2026-04-16)

### Added — 31 Additional Embedded POSIX/libc Headers (c971eb1, ea06f5a)

38 embedded headers total (up from 3). All standard POSIX/libc headers madc programs
are likely to use are now covered. Each header provides constants via `#define` and
type aliases via `#define`; functions are available via the existing dlsym fallback.

**Batch 1** (c971eb1) — `<stdlib.h>`, `<string.h>`, `<limits.h>`, `<errno.h>`, `<fcntl.h>`,
`<signal.h>`, `<unistd.h>`, `<time.h>`, `<dirent.h>`, `<sys/wait.h>`, `<sys/stat.h>`

**Batch 2** (ea06f5a) — `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<netdb.h>`,
`<sys/un.h>`, `<poll.h>`, `<sys/select.h>`, `<sys/time.h>`, `<sys/mman.h>`, `<sys/ipc.h>`,
`<sys/shm.h>`, `<sys/resource.h>`, `<sys/types.h>`, `<pthread.h>`, `<termios.h>`,
`<syslog.h>`, `<dlfcn.h>`, `<ctype.h>`, `<stdint.h>`, `<locale.h>`, `<glob.h>`,
`<fnmatch.h>`, `<pwd.h>`, `<grp.h>`

- `gen_embedded_headers.sh` updated to use `find` + relative paths for subdirectory support
  (`sys/wait.h`, `sys/stat.h`, `netinet/in.h`, etc. all key correctly)
- Type aliases (`pid_t`, `time_t`, `size_t`, etc.) implemented via `#define` — substituted
  through the existing lexer pushback mechanism; no lazy registration needed

### Added — SMAUG 1.8 Compatibility Goal (bd9844c)

- **Long-term goal established:** run SMAUG 1.8 MUD (~158k LOC, C89) in madc without gcc
- **`smaug.tgz`** checked into repo root (official 1.8 tarball)
- **`docs/SMAUG_requirements.md`** — full gap analysis: system headers checklist (37/38
  embedded; only `<stdarg.h>` missing), language feature gap table with BLOCKER/HIGH/MEDIUM/LOW
  severity, 5-phase implementation roadmap (A: pointer/type system, B: macros, C: data
  structures, D: I/O infrastructure, E: full SMAUG boot)
- Top blockers identified: function-like macros, `char *` declarations, `->` operator,
  `(TYPE *)` casts, `&` address-of — all Phase A/B work

### Fixed — Integer Literal Storage (c971eb1)

- **`int` overflow in lexer decimal parser** — accumulator variable was `int`; values ≥ 2^31
  (e.g. `2147483648` from `#define INT_MIN -2147483648`) silently wrapped to negative.
  Fixed: widened accumulator to `int64_t`.

- **`_token` field overflow in `TokenBase`** — `int _token` truncated stored literal values
  for constants ≥ 2^31. Fixed: widened to `int64_t` throughout `TokenBase`, `TokenInt`,
  `TokenVar`; all `get()`/`set()` signatures updated to `int64_t`.

- **`safeneg` sign-extension truncation** — `cc.neg()` was followed by
  `cc.movsx(op, op.r8())`, which sign-extended from `al` (8 bits) back to 64 bits.
  This corrupted any negated value with absolute magnitude ≥ 128 (e.g. `-INT_MIN` → 1,
  `-200` → 56). Fixed: removed the `movsx` entirely — `cc.neg()` on the full-width GP
  register is correct and sufficient.

---

## [Unreleased] — Phase 4 Prep (2026-04-15 → 2026-04-16)

### Added — Standard C Infrastructure

- **Embedded header system** — `#include <name>` checks headers baked into the binary (via
  `scripts/gen_embedded_headers.sh` at build time) before filesystem. `include/madc/` contains
  the source headers. Three implemented: `<iostream>`, `<math.h>`, `<stdio.h>`.

- **`#include <iostream>`** — `cout`, `cin`, `cerr`, `endl` now require this include (matching
  C++ convention). Uses lazy registration — symbols created on first use, not at parse init.

- **`#include <math.h>`** — Auto-loads libm via `#load "libm.so.6"`. Defines `M_PI`, `M_E`,
  `M_SQRT2`, `M_SQRT1_2`, `INFINITY`, `HUGE_VAL`. Math functions (`sqrt`, `sin`, `cos`, `pow`,
  `floor`, `ceil`, `fabs`, `log`) available via dlsym fallback.

- **`#include <stdio.h>`** — Defines `EOF`, `SEEK_SET`/`CUR`/`END`, `BUFSIZ`, `NULL`. `printf`,
  `sprintf`, `snprintf` available via dlsym fallback.

- **dlsym fallback** — Unresolved function calls followed by `(` try `dlsym(RTLD_DEFAULT, name)`
  before throwing "undeclared identifier". Works for all libc functions: `getpid()`, `sleep()`,
  `abs()`, `strlen()`, etc. No `#include` or `#load` needed for basic libc.

- **Variadic dlsym call path** — Dedicated compile path for dlsym-resolved functions. Builds
  `FuncSignature` from actual argument types (int, double, string→cstr). Infers double return
  type from destination register or argument types. Supports `sqrt(4.0)`, `pow(2.0, 10.0)`.

- **C preprocessor directives** — `#define NAME value` (constant substitution via pushback
  re-tokenization), `#undef NAME`, `#ifdef`/`#ifndef`/`#if`/`#else`/`#elif`/`#endif`,
  `#if defined(X)`, `#if !defined(X)`, `#if 0`/`#if 1`. Nested conditionals handled correctly.

- **`#pragma pack(push, N)` / `#pragma pack(pop)`** — Controls struct field alignment. Maintains
  a stack of pack values in the lexer.

- **C ABI struct alignment** — Structs now use natural x86-64 alignment by default: fields
  placed at `align_up(offset, min(field_size, 8))`. Total size rounded to max member alignment.
  `DataDefSTRUCT.pack`: 0=natural (default), 1=packed, N=max alignment N.

- **`struct __attribute__((packed))`** — Packed structs with no padding between fields.
  Attribute parsed before or after the struct tag name.

- **`sizeof()` operator** — Resolves to integer constant at parse time. Supports `sizeof(int)`,
  `sizeof(struct name)`, `sizeof(int32_t)`. Works in expressions: `sizeof(int) * 10`.

- **Compound assignment operators** — `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`,
  `>>=`. All 10 operators with int and double support.

- **Postfix increment/decrement** — `x++` and `x--` with correct old-value-return semantics.
  Parser uses `prevToken()` for prefix/postfix disambiguation.

- **Hex integer literals** — `0xFF`, `0xDEAD`, `0X1A`, mixed-case digits.

- **Command line arguments** — `int main(int argc, char **argv)`. Script args passed from
  the command line. `get_argv(argv, i)` built-in returns `const char*` for the i-th argument.

- **Lazy symbol registration** — `lazy_map<name, {header, kind}>` defers `addGlobal`/
  `addFunction` until the parser first encounters the symbol. Supports variables, functions,
  types, and structs. Extensible for future `#include` headers.

- **`RTLD_GLOBAL` for `#load`** — Loaded library symbols are globally visible via
  `dlsym(RTLD_DEFAULT)`. No namespace prefix needed after `#include <math.h>`.

### Fixed — Phase 4 Prep

- **For-loop `regdp` clobber** — `TokenFOR::compile()` now resets `regdp` before condition,
  statement, and increment sub-compilations. Prevents comparison results from overwriting
  loop counter variables.

- **`cout << func()` crash** — BSL was injecting the ostream as a hidden first parameter to
  ALL function calls on the right side of `<<`. Fixed: only inject for ostream-consuming
  functions (checked via `has_ostream()` on return type).

- **dlsym function name** — dlsym fallback now registers functions under their original name
  (was `__dl_` prefixed, which broke repeated calls to the same function).

- **`streamout_cstr` null safety** — Added null pointer check for `const char*` output.

---

## [Phase 3.5+] — 2026-04-15

### Added — Post Phase 3.5

- **`switch`/`case`/`default` statement** — C-style switch with fall-through semantics. Case values are literal constants. `break` exits via loopstack. Tests: `testswitch.mad`.

- **`cin` / `>>` input operator** — Read from stdin. `cin >> name >> age;` for string, int, double. Chained input via BSR convergence (mirrors `<<` for cout). `DataDefISTREAM` added. Tests: `testcin.mad`.

- **Class methods** — `class Counter { int count; void inc() { count = count + 1; } };` Methods receive hidden `__this` parameter (void*). Member access resolves through `[__this + offset]`. Method names mangled as `ClassName__methodName`. Tests: `testmethod.mad`.

- **Regex support** — `madc::regex_match()`, `madc::regex_search()`, `madc::regex_replace()` via `std::regex`. `perl::grep` and `perl::split` upgraded to use regex (fallback to substring on invalid patterns). Tests: `testregex.mad`.

- **Multiple return values** — Go-style `return q, r;` and `q, r := divide(17, 5);`. Hidden `__retbuf` parameter injected at compile time. Values written to `[retbuf+i*8]`. Works with conditional returns in braced if/else. Tests: `testmultiret.mad`.

- **Ternary operator** — `condition ? true_expr : false_expr`. Uses stack-slot merge to avoid asmjit register convergence issues. Colon acts as expression stop in non-bracketed context. Tests: `testternary.mad`.

- **`madc::` namespace** — `madc::array` works alongside bare `array` keyword. Also hosts regex functions. Backward compatible.

- **`std::` namespace scoping for containers** — `std::vector<int>`, `std::map<string, int>`, `std::set<string>`, `std::list<int>` all work alongside bare keywords. `std::cin` also available.

- **Register-only foreach iterator** — Numeric element variables in range-for loops use `vfREGISTER` for tighter loops.

- **`pushToken()` / deque-based token queue** — Token queue changed from `std::queue` to `std::deque` for speculative parsing support.

### Fixed — Post Phase 3.5

- **asmjit v1.14 deprecation warnings** — Migrated ~70 call sites:
  - `FuncSignatureT<...>(CallConvId::kCDecl)` → `FuncSignature::build<...>()`
  - `FuncSignatureBuilder` → `FuncSignature`
  - `Operand::size()` → `x86RmSize()` (on asmjit operands only)
  - `cc.setArg()` → `funcnode->setArg()`

- **Mem←Mem safemov** — Added temporary register path for `safemov(Mem, Mem)` operations (needed for class method member access).

- **Multi-return cleanup crash** — Skipping `cleanup()` on multi-return paths prevents double-destruct when multiple return statements exist in if/else branches.

### Added — Phase 3.5 (Modern Language Features)

- **Range-based for loops** — `for (type var : container) { ... }` C++ style iteration over `array` and `vector<T>`. Parser detects `:` in for-header, emits `TokenFOREACH` with index-based loop. Break/continue supported.

- **Function pointers** — `auto fn = my_function; fn(args);` Store function addresses in variables and call through them. `DataDefFPTR` wraps `FuncDef` for typed indirect calls via `cc.invoke(ptr_reg, funcsig)`.

- **Lambda expressions** — `[](params) { body }` and `[type](params) { body }` for typed returns. Anonymous functions hoisted to AST as top-level `TokenFunc` entries (asmjit can't nest addFunc/endFunc). Auto-named `__lambda_0`, `__lambda_1`, etc.

- **`auto` keyword** — Type inference for function pointer and lambda assignments. `auto fn = greet;` or `auto add = [int](int a, int b) { return a + b; };`

- **`defer` statement** — Go-style deferred execution. `defer statement;` registers code to run at scope exit in LIFO order, before destructors. Stored on `TokenCpnd::deferred` vector, compiled in reverse during `cleanup()`.

- **`std::for_each()`** — Iterates a MadArray calling a function pointer per element. Works with named function pointers and inline lambdas.

- **Typed STL containers** — C++ template syntax with lazy DataDef instantiation:
  - `vector<int>`, `vector<string>` — push_back, pop_back, at, size, clear, empty + range-for
  - `map<string, int>`, `map<string, string>` — put, get, contains, erase, size, clear
  - `set<string>`, `set<int>` — insert, contains, erase, size, clear
  - `list<int>`, `list<string>` — push_back, push_front, size, clear

- **New source file** `src/ns_stl.cpp` — C++ helper functions for all STL container operations.

- **Documentation** — `docs/language/modern/` for range-for, function pointers, lambdas, defer. `docs/rules/` for branching and feature guard rationale.

- **Infrastructure** — `make debug` target, `scripts/run_tests.sh` helper, feature branch workflow with `#ifdef` guards.

### Added — Phase 2 (Core Language Features)

- **User-defined structs** — `struct Name { type member; ... };` parsed dynamically, registered in `struct_map`. All forms: named, anonymous, typedef, inline variable declaration.

- **Class definitions** — `class Name { int x; string name; };` with data members. Classes registered in `datatype_map` for prefix-free usage (`Point p;` instead of `class Point p;`). Method parsing infrastructure in place.

- **Namespace resolution (`::`)** — `namespace_map` registry on Program. `parseExpression()` resolves `ns::member` syntax. Unknown namespaces with `#load` handles fall back to `dlsym`.

- **`std::` namespace** — `std::cout`, `std::cerr`, `std::endl` mapped to existing globals.

- **`#include` directive** — `#include "file.mad"` tokenizes included file inline at lexer level. Saves/restores Source via move semantics. Recursive includes work. Relative path resolution.

- **`using` statement** — `using namespace std;` imports all namespace members. `using std::cout;` imports single member.

- **Identifier-as-type resolution** — `parseStatement()` checks `datatype_map` for user-defined types, enabling `ClassName var;` syntax without keywords.

### Added — Phase 3 (Multi-Language Namespaces & Dynamic Loading)

- **`php::` namespace** (36 functions) — String: trim, ltrim, rtrim, chop, ucfirst, lcfirst, str_repeat, str_replace, str_pad, str_word_count, nl2br, str_rot13, chunk_split, number_format, wordwrap. Array: explode, implode, count, array_push, array_push_int, array_pop, array_get, array_get_int, array_shift, array_unshift, array_reverse, array_unique, array_search, array_slice, array_merge, in_array, sort, rsort.

- **`perl::` namespace** (21 functions) — chop, chomp, grep, glob, split, join, push, pop, shift, unshift, scalar, reverse, lc, uc, ucfirst, lcfirst, index, rindex, length, substr.

- **`python::` namespace** (16 functions) — title, swapcase, center, ljust, rjust, zfill, count, startswith, endswith, isdigit, isalpha, isalnum, isspace, replace, format.

- **`ruby::` namespace** (12 functions) — squeeze, tr, chars, capitalize, delete, count, include, gsub, sub, rotate, compact, flatten.

- **`js::` namespace** (6 functions) — btoa, atob (base64), encodeURIComponent, decodeURIComponent, parseInt (with radix), stringify (JSON).

- **MadValue / MadArray** — Tagged union (`int64/double/string`) and container type for PHP-style mixed-type arrays. `array` keyword as a first-class data type with JIT construct/destruct.

- **`#load` directive** — `#load "libfoo.so" as ns;` opens shared libraries via dlopen, creates namespace with lazy dlsym resolution.

- **`dlopen`/`dlsym`/`dlclose`/`dlcall`** — First-class dynamic linking functions. `dlcall(funcptr, args...)` invokes through a function pointer with automatic string-to-cstr coercion.

- **Variadic function calling** — Functions with 0 declared parameters accept any argument count/types at compile time. Used by dlopen-resolved functions and `dlcall`.

- **ifstream/ofstream/fstream** — File I/O as first-class data types. Methods: open, close, eof, good, is_open. `<<` operator for writing, `getline()` for reading.

- **Type conversion functions** — `to_string(result, int)`, `stoi(str)`, `stod(str)`, `strlen(str)`.

- **C library globals** — `system(cmd)`, `getenv(result, name)`, `setenv(name, value)`, `unsetenv(name)`.

### Fixed — Phase 2/3

- **String pass-by-value** — `voperand()` was constructing an empty string for function parameters, losing the caller's pointer. Fixed: parameter variables get a bare Gp register; `cleanup()` skips parameter destruction.

- **`dtSTRING -> dtCHARptr` coercion** — Added `string_cstr()` helper. `TokenCallFunc::compile()` auto-converts string arguments to `const char*` when calling functions like `puts()`.

- **Stream good()/eof() crash** — `ifstream`/`ofstream` inherit `std::ios` via virtual inheritance. Casting `void*` directly to `std::ios*` skipped the vtable pointer adjustment (256 bytes on x86-64). Fixed with type-specific wrapper functions.

- **Struct definition returns** — `TokenSTRUCT::parse()` returned `this` for pure definitions, causing `TokenKeyword::compile()` errors. Fixed to return NULL.

---

## [Phase 1] — 2026-04-14

### Added

- **`-v` / `--verbose` flag** — `DBG()` output gated behind `madc_verbose`.
- **`register` keyword** — `vfREGISTER` flag for register-only variables.
- **doctest unit test framework** — `include/doctest.h`, `tests/unit/`, `make test`.
- **Documentation:** usage.md, testing.md, test-status.md, revival-plan.md, rules.

### Fixed

- Char literal compilation (`putchar('h')` now works).
- Struct member access (`addOffset` vs `setOffset`).
- Struct string member lifecycle (construct/destruct).
- `DBG()` dangling-else (do-while idiom).
- asmjit v1.14 API migration.

---

## Prior to Changelog

- Project originally written ~2019.
- Dormant for ~7 years due to asmjit API changes breaking the build.
- April 2026: asmjit v1.14 migration completed; binary builds and runs.
