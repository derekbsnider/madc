# TODO

## High Priority

### Language Completeness

- **Function-like macros** — `#define NAME(args) expr` (parameterized macros). Simple `#define NAME value`
  constant substitution, `#ifdef`/`#ifndef`/`#if`/`#else`/`#elif`/`#endif`, `#undef`, and
  `#if defined(X)` are all implemented. Function-like macros are deferred.

- **`printf` / `sprintf` / `snprintf` / `fprintf`** — C-style format strings are the idiomatic
  output mechanism for C-like scripts. `cout <<` covers basic cases but format strings are
  essential for anything numeric or padded. These are in libc (see below) but warrant explicit
  built-in treatment given their frequency of use.

### Standard Library Access

- **Fix dlsym namespace calling convention** — `ns::func()` calls resolved via dlsym have
  broken return values (always 0 for integers, code generation failure for doubles). The
  asmjit `cc.finalize()` reports `kErrorInvalidAssignment` (25) for double returns and
  `kErrorInvalidInstruction` (26) for integer returns. Root cause: likely a mismatch between
  the function signature registered by `addFunction()` (variadic with dtINT64 return) and
  the actual calling convention needed. Unqualified libc calls via the dlsym fallback
  (e.g. `getpid()`, `sleep(0)`) work correctly since they go through a different code path.

- **Embedded standard headers — additional headers** — The infrastructure is in place
  (`#include <math.h>` routes to embedded `include/madc/math.h` which auto-loads libm via
  `#load` and defines math constants). Additional headers needed once dlsym calling is fixed:
  - `<stdio.h>` — `EOF`, `SEEK_*` constants, `printf` family
  - `<stdlib.h>` — `EXIT_SUCCESS`, `EXIT_FAILURE`, `NULL`
  - `<fcntl.h>` — `O_RDONLY`, `O_WRONLY`, `O_CREAT`, etc.
  - `<signal.h>` — `SIGKILL`, `SIGTERM`, `SIGINT`, etc.
  - `<errno.h>` — `ENOENT`, `EACCES`, `EEXIST`, etc.

  Typedefs (`pid_t`, `size_t`, `off_t`, `time_t`, `mode_t`) map to existing madc integer types
  and can be defined with `#define` once that lands.

## Medium Priority

### POSIX Coverage

- **Time functions** — `time`, `clock`, `sleep`, `usleep`, `nanosleep`, `gettimeofday`,
  `localtime`, `gmtime`, `mktime`, `strftime`, `clock_gettime`. Available via `libc` namespace
  once that lands, but may need `struct tm` binding for `localtime`/`strftime` to be useful.

- **Process control** — `getpid`, `getppid`, `getuid`, `geteuid`, `fork`, `exec*`, `wait`,
  `waitpid`, `kill`, `signal`, `raise`, `abort`, `atexit`. `system()` and `getenv/setenv` are
  already built-in; this covers the rest of the process lifecycle.

- **Filesystem operations** — `stat`, `access`, `rename`, `remove`, `unlink`, `mkdir`, `rmdir`,
  `getcwd`, `chdir`, `opendir`, `readdir`, `closedir`. Currently madc has C++ `ifstream`/`ofstream`
  for file I/O; this covers directory traversal and metadata.

- **Memory functions** — `malloc`, `free`, `calloc`, `realloc`, `memcpy`, `memmove`, `memset`,
  `memcmp`. Needed for interoperating with C libraries that allocate/pass raw buffers.

- **Error reporting** — `errno` (global int), `strerror`, `perror`. Needed for any script that
  checks syscall return values.

### Language Completeness

- **String multi-return types** — Multi-return currently supports numeric (int64) slots only.
  String returns need pointer-passing via `__retbuf`. Significantly extends multi-return utility.

- **Right-shift operator `>>`** — `<<` is working; `>>` is tokenized but has no integration test.
  Verify it works in expression context (distinct from `>>` as template terminator in
  `vector<vector<int>>`).

- **Error diagnostics** — For the scripting use case, a raw JIT crash with no context is a bad
  experience. Audit what the user sees on type mismatches, missing symbols, bad casts. File and
  line number should always appear in error output.

## Low Priority

- **Multi-return in brace-less if** — `if (x) return a, b;` fails to parse because comma
  confuses the single-statement if body. Workaround: use braces `if (x) { return a, b; }`.

- **`(type, type)` multi-return declaration syntax** — Explicit `(int, string) func()` signature
  for documentation and type safety. Currently inferred from `return a, b;` at parse time.

- **`struct` interop for libc calls** — Some libc functions take/return `struct tm`, `struct stat`,
  `struct dirent`, etc. For full POSIX coverage these need madc struct definitions that match
  the C ABI layout so they can be passed to `libc::` functions.

## Deferred / Future

- **ARM64 support** — asmjit supports ARM64 backends. Currently x86-64 Linux only.

- **Phase 4: `libmadc.so` embedding API** — Decouple static globals, create public C API
  (`madc_create`, `madc_exec_file`, `madc_exec_string`, `madc_destroy`), build as shared library.
  Lower priority given the primary use case is running `.mad` scripts directly.

- **Networking** — `socket`, `bind`, `connect`, `listen`, `accept`, `send`, `recv`, `getaddrinfo`.
  Available via `libc` namespace once that lands, but useful enough to warrant first-class
  wrapper treatment eventually.

- **Operator overloading** — `<<`, `+`, `-`, `*`, `/` for user-defined types. High effort,
  niche benefit at this stage.

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
- ~~For-loop regdp clobber bug~~ — `TokenFOR::compile()` now resets `regdp` before each sub-compilation
- ~~Hex integer literals~~ — `0xFF`, `0xDEAD`, `0X1A` etc.
- ~~Postfix increment/decrement~~ — `x++`, `x--` with correct old-value-return semantics; parser uses `prevToken()` for prefix/postfix disambiguation
- ~~C preprocessor directives~~ — `#define`, `#undef`, `#ifdef`/`#ifndef`/`#if`/`#else`/`#elif`/`#endif`, `#if defined(X)`, `#if 0`/`#if 1`
- ~~Embedded header infrastructure~~ — `#include <name>` checks embedded headers first; `scripts/gen_embedded_headers.sh` bakes `include/madc/*.h` into binary; first header: `math.h`
- ~~dlsym fallback for libc~~ — unresolved function calls try `dlsym(RTLD_DEFAULT, name)` before erroring; `getpid()`, `sleep()`, `getuid()` etc. work without `#include` or `#load`
- ~~Variadic type-check bypass~~ — dlsym-resolved functions skip parameter type checks and infer return type from destination register
