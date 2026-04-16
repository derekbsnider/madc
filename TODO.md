# TODO

## High Priority

### Language Completeness

- **Hex literals** — `0xFF`, `0x1A`, etc. are not recognized by the lexer. The lexer reads
  decimal integers only. Fix: in the lexer's number-reading path, detect `0x`/`0X` prefix
  and parse the remaining hex digits with `strtol(..., 16)`.

- **C preprocessor directives** — `#define NAME value` (constant substitution), `#define NAME(args) expr`
  (function-like macros), `#ifdef` / `#ifndef` / `#if` / `#else` / `#elif` / `#endif`, `#undef`.
  Implemented at lexer level (lexer already handles `#include` and `#load`). `#if defined(X)` and
  `#if (expr)` constant-expression evaluation needed for `#if`/`#elif`.

- **`printf` / `sprintf` / `snprintf` / `fprintf`** — C-style format strings are the idiomatic
  output mechanism for C-like scripts. `cout <<` covers basic cases but format strings are
  essential for anything numeric or padded. These are in libc (see below) but warrant explicit
  built-in treatment given their frequency of use.

### Standard Library Access

- **Built-in `libc` and `libm` namespaces** — `libc::getpid()`, `libc::sleep(1)`,
  `libm::sqrt(x)`, `libm::pow(x, y)` etc. work without any `#load` directive.
  Implementation uses a **two-phase approach**: (1) during parsing, `libc::`/`libm::` calls
  are recorded as symbol references but nothing is resolved yet; (2) after parsing completes,
  a single resolution pass `dlopen`s each library once and `dlsym`s only the symbols actually
  referenced in the source — nothing else. Libraries that aren't used don't get opened at all.
  `libc` → `dlopen(NULL, RTLD_LAZY)` (already in-process); `libm` → `dlopen("libm.so.6", RTLD_LAZY)`.
  Key `libm` functions: `sqrt`, `pow`, `exp`, `log`, `log2`, `log10`, `sin`, `cos`, `tan`,
  `asin`, `acos`, `atan`, `atan2`, `floor`, `ceil`, `round`, `fabs`, `fmod`, `hypot`.

- **Embedded standard headers** — `dlsym` gives function pointers but not the type information
  from C headers: struct layouts, constants, and typedefs. Solution: madc-native `.mad` header
  files that define these using madc's own type system (field offsets and constant values
  hardcoded for x86-64 Linux ABI), **compiled directly into the binary** so no external files
  are needed at runtime. This preserves the core "single binary, nothing else" promise.

  Build mechanism: source headers live in `include/madc/` as human-editable `.mad` files; the
  Makefile converts them to C++ byte arrays at build time (via `xxd -i` or a small gen script)
  and links them in. The lexer's `#include` handler checks the in-memory map first:
  - `#include <madc/libc/fcntl>` — angle-bracket form routes to embedded headers
  - `#include "local.mad"` — quoted form stays as filesystem lookup (existing behaviour)

  Priority headers (depends on `#define` and struct support being solid):
  - `<madc/libc/fcntl>` — `O_RDONLY`, `O_WRONLY`, `O_CREAT`, `O_APPEND`, `O_TRUNC` etc.
  - `<madc/libc/stat>` — `struct stat` with correct field offsets, `S_IRUSR`, `S_IWUSR` etc.
  - `<madc/libc/signal>` — `SIGKILL`, `SIGTERM`, `SIGINT`, `SIGHUP` etc.
  - `<madc/libc/errno>` — `ENOENT`, `EACCES`, `EEXIST`, `EINVAL`, `ENOMEM` etc.
  - `<madc/libc/time>` — `struct tm`, `struct timespec`, `struct timeval`, `CLOCK_MONOTONIC` etc.
  - `<madc/libc/dirent>` — `struct dirent` for `opendir`/`readdir` results
  - `<madc/net/socket>` — `AF_INET`, `SOCK_STREAM`, `struct sockaddr_in`, `struct in_addr`

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
