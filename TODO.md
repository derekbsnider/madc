# TODO

## High Priority

### Embedded POSIX Headers

Infrastructure is complete: embedded headers baked into binary, `#include <name>` checks
embedded map first, lazy symbol registration defers `addGlobal`/`addFunction` until first use,
dlsym fallback resolves unqualified libc/libm calls automatically, `RTLD_GLOBAL` on `#load`
makes loaded library symbols globally visible.

**Implemented headers:**
- `<iostream>` — `cout`, `cin`, `cerr`, `endl` (lazy registration)
- `<math.h>` — `M_PI`, `M_E`, `M_SQRT2`, `INFINITY`, auto-loads libm; `sqrt()`, `sin()`,
  `cos()`, `pow()`, `floor()`, `ceil()`, `fabs()`, `log()` etc. via dlsym
- `<stdio.h>` — `EOF`, `SEEK_SET`/`CUR`/`END`, `BUFSIZ`, `NULL`; `printf()`, `sprintf()`,
  `snprintf()`, `fprintf()` via dlsym

**Next headers (constants only — functions via dlsym):**
- `<stdlib.h>` — `EXIT_SUCCESS`, `EXIT_FAILURE`, `RAND_MAX`
- `<string.h>` — (functions only: `strlen`, `strcmp`, `strncmp`, `strcpy`, `strcat`, `memcpy`,
  `memset`, `memcmp` — all via dlsym)
- `<unistd.h>` — `STDIN_FILENO`, `STDOUT_FILENO`, `STDERR_FILENO`, `R_OK`, `W_OK`, `X_OK`,
  `F_OK`; typedefs `pid_t`→int, `uid_t`→int, `gid_t`→int, `off_t`→int64, `size_t`→uint64,
  `ssize_t`→int64 (via `#define`); functions `read`, `write`, `close`, `lseek`, `fork`,
  `execvp`, `pipe`, `dup2`, `getcwd`, `chdir`, `unlink`, `rmdir`, `access` via dlsym
- `<fcntl.h>` — `O_RDONLY` (0), `O_WRONLY` (1), `O_RDWR` (2), `O_CREAT` (64), `O_EXCL` (128),
  `O_TRUNC` (512), `O_APPEND` (1024), `O_NONBLOCK` (2048); `open` via dlsym
- `<signal.h>` — `SIGHUP` (1), `SIGINT` (2), `SIGQUIT` (3), `SIGKILL` (9), `SIGTERM` (15),
  `SIGCHLD` (17), `SIGCONT` (18), `SIGSTOP` (19), `SIGUSR1` (10), `SIGUSR2` (12),
  `SIG_DFL` (0), `SIG_IGN` (1); `kill`, `signal`, `raise` via dlsym
- `<errno.h>` — `EPERM` (1), `ENOENT` (2), `ESRCH` (3), `EINTR` (4), `EIO` (5),
  `EACCES` (13), `EEXIST` (17), `ENOTDIR` (20), `EISDIR` (21), `EINVAL` (22),
  `ENOMEM` (12), `ENOSPC` (28), `EPIPE` (32); `strerror`, `perror` via dlsym
- `<time.h>` — `CLOCKS_PER_SEC`, `CLOCK_REALTIME` (0), `CLOCK_MONOTONIC` (1); typedefs
  `time_t`→int64, `clock_t`→int64; `time`, `clock`, `difftime`, `strftime` via dlsym
- `<limits.h>` — `INT_MAX`, `INT_MIN`, `LONG_MAX`, `LONG_MIN`, `PATH_MAX` (4096)
- `<dirent.h>` — `DT_REG` (8), `DT_DIR` (4), `DT_LNK` (10); `opendir`, `readdir`,
  `closedir` via dlsym (struct access deferred)
- `<sys/wait.h>` — `WNOHANG` (1), `WUNTRACED` (2); `wait`, `waitpid` via dlsym
- `<sys/stat.h>` — `S_IRUSR` (0400), `S_IWUSR` (0200), `S_IXUSR` (0100), `S_IRGRP` (040),
  `S_IROTH` (04), `S_ISUID` (04000), `S_ISGID` (02000); `stat`, `fstat`, `chmod`, `mkdir`
  via dlsym (struct access deferred)

**Struct-dependent headers (require struct interop — deferred):**
- `struct tm` for `localtime`/`gmtime`/`mktime`/`strftime` (time.h)
- `struct stat` for `stat`/`fstat` result access (sys/stat.h)
- `struct dirent` for `readdir` result access (dirent.h)
- `struct sockaddr_in`/`struct in_addr` for networking (netinet/in.h)

### Language Completeness

- **Function-like macros** — `#define NAME(args) expr` (parameterized macros)

- **`cout << [const char*]` in chained expressions** — `cout << func_returning_cstr() << endl`
  crashes when the cstr function is inside a convergent BSL chain. Single `cout << cstr` works.
  Root cause likely in BSL convergence re-entering compile with stale regdp.

- **`char *` variable declarations** — `char *str = "test";` doesn't parse. The parser sees
  `char` as a type token and `*` as multiply. Need to handle pointer declaration syntax in
  `parseDeclaration`.

### printf improvements

- **printf with `%f`/`%e`/`%g` for doubles** — Verify doubles pass correctly through the
  variadic dlsym path for printf format strings. The x86-64 ABI requires `al` to hold the
  number of SSE registers used for variadic functions — this may need special handling.

## Medium Priority

### Language Completeness

- **String multi-return types** — Multi-return currently supports numeric (int64) slots only.

- **Right-shift operator `>>`** — `<<` is working; `>>` is tokenized but needs integration test.

- **Error diagnostics** — File and line number should always appear in error output.

## Low Priority

- **Multi-return in brace-less if** — `if (x) return a, b;` doesn't parse. Use braces.

- **`(type, type)` multi-return declaration syntax** — Explicit return type signatures.

## Deferred / Future

- **ARM64 support** — asmjit supports ARM64 backends. Currently x86-64 Linux only.

- **Phase 4: `libmadc.so` embedding API** — Decouple static globals, create public C API.

- **Networking** — `socket`, `bind`, `connect`, `listen`, `accept`, `send`, `recv`.

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
- ~~Embedded header infrastructure~~ — `#include <name>` routes to embedded headers baked into binary
- ~~dlsym fallback for libc~~ — unresolved function calls try `dlsym(RTLD_DEFAULT)` before erroring
- ~~Variadic dlsym calling convention~~ — dedicated compile path handles int, double, string args
- ~~`#include <iostream>`~~ — cout/cin/cerr/endl gated behind include (lazy registration)
- ~~`#include <math.h>`~~ — M_PI/M_E/M_SQRT2/INFINITY + libm auto-load
- ~~`#include <stdio.h>`~~ — EOF/SEEK_*/NULL + printf/sprintf/snprintf via dlsym
- ~~Lazy symbol registration~~ — `lazy_map` defers addGlobal/addFunction to first use; supports variables, functions, types, structs
- ~~Command line arguments~~ — `int main(int argc, char **argv)` with `get_argv(argv, i)`
- ~~`cout << func()` crash fix~~ — BSL only injects ostream for ostream-consuming functions
- ~~`RTLD_GLOBAL` for `#load`~~ — loaded library symbols visible via dlsym without namespace prefix
