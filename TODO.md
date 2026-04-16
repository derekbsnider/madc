# TODO

## High Priority

### For-loop increment parsing bug (pre-existing)

- **`for ( i = 0; i < N; i++ )` fails** — The for-loop parser calls
  `parseExpression(tn, true)` for the condition part. In conditional mode, the expression
  parser consumes through the `;` separator, leaving the increment `i++` as a new statement.
  `parseStatement(;)` returns the empty semicolon, then the parser expects `)` but finds `i++`.
  Root cause: `TokenFOR::parse()` at parser.cpp:2616-2624 — the condition's `parseExpression`
  in conditional mode doesn't stop at `;` correctly. Fix: either parse the condition without
  conditional mode (stop at `;` explicitly), or add `;` as a stop token in conditional mode.

### Language Completeness

- **`cout << [const char*]` in chained expressions** — `cout << func_returning_cstr() << endl`
  crashes when the cstr function is inside a convergent BSL chain. Single `cout << cstr` works.
  Root cause likely in BSL convergence re-entering compile with stale regdp.

- **`*ptr` dereference on stack variables** — `int x = 42; int *p = &x; *p = 99;` crashes
  at runtime because `&x` LEAs a virtual register that may not have a stack address.
  Need to force address-taken variables to the stack via `cc.newStack()`.

- **Chained `->` without temp variable** — `ch->in_room->name` parses but the intermediate
  TokenMember isn't a real variable in operand_map. Works with: `ROOM_DATA *r = ch->in_room;
  r->name;`. Needs TokenMember-as-object support in codegen.

- **`->` member access in certain printf arg combinations** — Direct `->` in variadic args
  works for simple cases but crashes in specific multi-struct patterns. Workaround: use temp
  variables.

### printf improvements

- **printf with `%f`/`%e`/`%g` for doubles** — Verify doubles pass correctly through the
  variadic dlsym path for printf format strings. The x86-64 ABI requires `al` to hold the
  number of SSE registers used for variadic functions — this may need special handling.

## Medium Priority

### SMAUG Phase C — Data Structures

- **2D arrays** — `int board[8][8]`, `char inbuf[MAX_INBUF_SIZE]`. Parser needs to handle
  multi-dimensional array declaration and indexing `arr[i][j]`.

- **Static initializer tables** — `static struct { ... } table[] = { { "name", func }, ... };`
  used heavily in `const.c` and `tables.c`. Requires aggregate initialization syntax.

- **`char *str = "hello"` string literal to pointer** — Assign string literal directly to
  `char *` variable. Currently requires `strdup("hello")` workaround.

### Language Completeness

- **String multi-return types** — Multi-return currently supports numeric (int64) slots only.

- **Right-shift operator `>>`** — `<<` is working; `>>` is tokenized but needs integration test.

- **Error diagnostics** — File and line number should always appear in error output.

- **`__FILE__` and `__LINE__` macros** — Used in SMAUG's CREATE/DISPOSE error messages.

## Low Priority

- **Multi-return in brace-less if** — `if (x) return a, b;` doesn't parse. Use braces.

- **`(type, type)` multi-return declaration syntax** — Explicit return type signatures.

- **Function pointer typedefs** — `typedef void DO_FUN(CHAR_DATA *ch, char *argument);`

## Deferred / Future

- **Struct interop for C functions** — `struct tm` for `localtime`/`strftime`, `struct stat`
  for `stat`/`fstat`, `struct dirent` for `readdir`, `struct sockaddr_in` for networking.
  Headers are embedded with all constants; struct field access is the remaining gap.

- **`fd_set` + FD_SET/FD_ZERO/FD_ISSET** — `fd_set` is a fixed-size bit array struct.
  Either model as a struct with known layout, or implement FD_* as built-in functions.
  Needed for `select()` in SMAUG's main network loop.

- **`select()` end-to-end** — `select` is available via dlsym; needs `fd_set` struct interop
  and `struct timeval` to be usable (both in `sys/select.h` and `sys/time.h`).

- **`gettimeofday()` returning struct timeval** — Used in SMAUG's main loop. Needs struct interop.

- **ARM64 support** — asmjit supports ARM64 backends. Currently x86-64 Linux only.

- **Phase 4: `libmadc.so` embedding API** — Decouple static globals, create public C API.

- **Operator overloading** — `<<`, `+`, `-`, `*`, `/` for user-defined types.

## Completed

- ~~Escape sequences in string literals~~ (c90acff)
- ~~`[]` subscript operator~~ (c90acff)
- ~~`:=` short variable declaration~~ (7027d4c)
- ~~`[&]` lambda capture by reference~~ (4fa5126)
- ~~`madc::` namespace~~ — `madc::array` + regex functions
- ~~`std::` namespace scoping~~ — `std::vector<T>`, `std::map<K,V>`, `std::set<T>`, `std::list<T>`, `std::cin`
- ~~Register-only iterator~~ — numeric foreach element variables use `vfREGISTER`
- ~~Fix asmjit deprecation warnings~~ — ~70 call sites migrated
- ~~`switch` statement~~ — C-style with fall-through and break (deb3578)
- ~~`>>` input operator / `cin`~~ — reads string, int, double from stdin (1feb665)
- ~~Class methods~~ — hidden `__this` pointer, member access, name mangling (f375944)
- ~~Regex support~~ — `madc::regex_match/search/replace`, upgraded `perl::grep/split` (2aabea0)
- ~~Multiple return values~~ — `q, r := divide(17, 5)` with hidden `__retbuf` (b1c0e86, 8d07f44)
- ~~Ternary operator~~ — `condition ? true_expr : false_expr` with stack-slot merge (f64cf18)
- ~~Multi-return conditional crash~~ — skip cleanup for multi-return paths (8d07f44)
- ~~Compound assignment operators~~ — `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`
- ~~For-loop regdp clobber bug~~ — `TokenFOR::compile()` resets `regdp` before each sub-compilation
- ~~Hex integer literals~~ — `0xFF`, `0xDEAD`, `0X1A` etc.
- ~~Postfix increment/decrement~~ — `x++`, `x--` with correct old-value-return semantics
- ~~C preprocessor directives~~ — `#define`, `#undef`, `#ifdef`/`#ifndef`/`#if`/`#else`/`#elif`/`#endif`
- ~~Embedded header infrastructure~~ — `#include <name>` routes to embedded headers baked into binary; subdirectory support via `find` + relative-path keys
- ~~dlsym fallback for libc~~ — unresolved function calls try `dlsym(RTLD_DEFAULT)` before erroring
- ~~Variadic dlsym calling convention~~ — dedicated compile path handles int, double, string args
- ~~`#include <iostream>`~~ — cout/cin/cerr/endl gated behind include (lazy registration)
- ~~`#include <math.h>`~~ — M_PI/M_E/M_SQRT2/INFINITY + libm auto-load
- ~~`#include <stdio.h>`~~ — EOF/SEEK_*/NULL + printf/sprintf/snprintf via dlsym
- ~~Lazy symbol registration~~ — `lazy_map` defers addGlobal/addFunction to first use; supports variables, functions, types, structs
- ~~Command line arguments~~ — `int main(int argc, char **argv)` with `get_argv(argv, i)`
- ~~`cout << func()` crash fix~~ — BSL only injects ostream for ostream-consuming functions
- ~~`RTLD_GLOBAL` for `#load`~~ — loaded library symbols visible via dlsym without namespace prefix
- ~~31 additional embedded POSIX/libc headers~~ — 38 total (c971eb1, ea06f5a)
- ~~int64_t integer literal storage~~ — lexer/token widened to int64_t (c971eb1)
- ~~safeneg truncation fix~~ — removed spurious movsx after cc.neg() (c971eb1)
- ~~SMAUG 1.8 requirements analysis~~ — gap analysis in docs/SMAUG_requirements.md (bd9844c)
- ~~**`char *` pointer declarations**~~ — DataDefPTR, pointer parsing in decl/struct/params/sizeof (a3576bc)
- ~~**`->` struct pointer member access**~~ — reuses TokenMember Gp path (683f379)
- ~~**`&` address-of operator**~~ — TokenAddrOf with LEA, unary detection (8b0f8e2)
- ~~**`(TYPE *)` cast expressions**~~ — TokenCast, cast detection in parseExpression (31e5fbd)
- ~~**Forward typedef struct declarations**~~ — placeholder struct filled in-place (e8e6379)
- ~~**Function-like macros**~~ — #define NAME(params) body with substitution (680d982)
- ~~**Multi-line #define with \\ continuation**~~ — both function-like and object-like (680d982)
- ~~**do { } while(0) macro bodies**~~ — regdp reset + TokenInt fix (d15f01c)
- ~~**Ternary inside parentheses**~~ — brackets check before done=true (70425d7)
- ~~**unsigned/signed/long/short compound types**~~ — lexer compound specifiers (8bfab4f)
- ~~**Variadic arg promotion**~~ — sub-64-bit movsx/movzx for printf (299ccf5)
- ~~**C string function redirect**~~ — strlen(char*) → libc via dlsym (78949c0)
- ~~**dlsym return to Mem**~~ — temp Gp for setRet when regdp.first is Mem (41c977a)
- ~~**Typedef'd types in struct members**~~ — identifier resolved against datatype_map (41c977a)
- ~~**struct Type in function parameters**~~ — parseFunction handles struct/typedef names (8b0f8e2)
- ~~**`*ptr` dereference operator**~~ — TokenDeref for read/write through heap pointers (f481ea4)
- ~~**`static` keyword**~~ — persistent locals with vfSTATIC + heap allocation (ebcfd93)
- ~~**`const` and `extern` keywords**~~ — consumed/skipped for C compatibility (ebcfd93)
- ~~**`enum` keyword**~~ — global constant variables with auto-increment (b0f5df4)
- ~~**`typedef` for primitive types**~~ — typedef int sh_int; typedef unsigned char bool; (0047fd0)
- ~~**Compound assignments on struct members**~~ — resolveCompoundLHS helper (b61fdbe)
- ~~**make fulltest**~~ — unit + integration tests in one command (bbc2f04)
- ~~**isUnaryPosition()/isPostfixPosition() helpers**~~ — replaces duplicated checks (59805a6)
- ~~**`<stdarg.h>` / `va_list`**~~ — `...` in function decls, va_start/va_end macros, va_arg intrinsic, packed int64_t[] buffer, format-aware vsprintf helper (9afa644)
