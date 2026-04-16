# TODO

## High Priority

### SMAUG Phase A — C Pointer and Type System

These are the top blockers for SMAUG 1.8 compatibility (see `docs/SMAUG_requirements.md`)
and also the most impactful gaps for general C compatibility. Each item fixes a fundamental
parse/compile failure that affects virtually every real C program.

- **`char *` pointer declarations** — `char *str = "test";` doesn't parse. Parser sees `char`
  as a type token and `*` as multiply. Fix `parseDeclaration` to handle pointer declarator
  syntax. Affects every string field in every C struct.

- **`->` struct pointer member access** — `node->next`, `ch->name` style access. Required for
  any linked-list traversal. Parse as `deref + member` or as a dedicated binary operator.

- **`(TYPE *)` explicit cast expressions** — `(CHAR_DATA *) calloc(...)`, `(int *) ptr`. Cast
  syntax with a type name inside parentheses. Currently ambiguous with grouped expressions.

- **`&` address-of operator in expressions** — `&sa`, `&buf`, `sizeof(&x)`. Required for any
  call to socket functions, `stat`, `memset`, etc. that take pointer arguments.

- **Forward typedef struct declarations** — `typedef struct char_data CHAR_DATA;` followed later
  by `struct char_data { ... };`. Parser must accept forward references to undefined structs.

### SMAUG Phase B — Macros

- **Function-like macros** — `#define NAME(args) body` with parameter substitution.
  SMAUG's `CREATE`/`DISPOSE`/`KEY` macros cover every allocation and file-read. Also needed
  for `FD_SET`, `FD_ZERO`, `FD_ISSET` (from `<sys/select.h>`). High leverage for all C code.

- **Multi-line `#define` with `\` continuation** — `#define MACRO(x) \` spanning multiple
  source lines. Required by SMAUG's macro definitions in `mud.h`.

### SMAUG Phase D — I/O Infrastructure

- **`<stdarg.h>` / `va_list`** — `va_list`, `va_start`, `va_end`, `va_arg`. Used in
  `comm.c`, `db.c`, `misc.c` for `ch_printf`, `log_string`, `send_to_char`. Without this,
  all formatted output functions fail.

### Language Completeness

- **`cout << [const char*]` in chained expressions** — `cout << func_returning_cstr() << endl`
  crashes when the cstr function is inside a convergent BSL chain. Single `cout << cstr` works.
  Root cause likely in BSL convergence re-entering compile with stale regdp.

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

- **`unsigned` type keywords** — `unsigned char`, `unsigned int`, `unsigned short`, `unsigned long`.
  Also `long int`, `short int` explicit type combos. Used in `sh_int`, `bool` typedef contexts.

### Language Completeness

- **String multi-return types** — Multi-return currently supports numeric (int64) slots only.

- **Right-shift operator `>>`** — `<<` is working; `>>` is tokenized but needs integration test.

- **Error diagnostics** — File and line number should always appear in error output.

## Low Priority

- **Multi-return in brace-less if** — `if (x) return a, b;` doesn't parse. Use braces.

- **`(type, type)` multi-return declaration syntax** — Explicit return type signatures.

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
- ~~31 additional embedded POSIX/libc headers~~ — stdlib.h, string.h, limits.h, errno.h, fcntl.h, signal.h, unistd.h, time.h, dirent.h, ctype.h, stdint.h, dlfcn.h, glob.h, fnmatch.h, pwd.h, grp.h, locale.h, poll.h, syslog.h, termios.h, pthread.h, netdb.h, sys/socket.h, netinet/in.h, arpa/inet.h, sys/types.h, sys/stat.h, sys/wait.h, sys/mman.h, sys/select.h, sys/time.h, sys/ipc.h, sys/shm.h, sys/resource.h, sys/un.h (c971eb1, ea06f5a)
- ~~int64_t integer literal storage~~ — lexer decimal accumulator widened to int64_t; TokenBase._token widened to int64_t; no overflow for values ≥ 2^31 (e.g. INT_MIN, UINT32_MAX) (c971eb1)
- ~~safeneg truncation fix~~ — removed spurious `movsx(op, op.r8())` after `cc.neg()`; negated values ≥ 128 now correct (c971eb1)
- ~~SMAUG 1.8 requirements analysis~~ — gap analysis, severity table, 5-phase roadmap in docs/SMAUG_requirements.md (bd9844c)
