# TODO

## High Priority

### Language Completeness

- **`cout << [const char*]` in chained expressions** — `cout << func_returning_cstr() << endl`
  crashes when the cstr function is inside a convergent BSL chain. Single `cout << cstr` works.
  Root cause likely in BSL convergence re-entering compile with stale regdp.

- **`*ptr` dereference on stack variables** — `int x = 42; int *p = &x; *p = 99;` crashes
  at runtime because `&x` LEAs a virtual register that may not have a stack address.
  Need to force address-taken variables to the stack via `cc.newStack()`.

- **`->` member access in certain printf arg combinations** — Direct `->` in variadic args
  works for simple cases but crashes in specific multi-struct patterns. Workaround: use temp
  variables.

- **`char *p; p = "literal";`** — post-decl string-literal assignment misbehaves (the sugar
  that turns `char *p = "literal"` into char-array + LEA only fires at declaration time).
  SMAUG code uses this pattern after-decl often. Fix: detect the pattern in TokenAssign
  and route through the same backing-storage mechanism.

- **`c++` / `--x` in compound-comma increment contexts** — blocks SMAUG's `for (…; …; ptr =
  ptr->next, c++)` pattern. Error: "Increment on a non-variable rval". Needs investigation
  in the postfix-inc/dec AST path when the preceding expression consumed unusually.

### printf improvements

- **printf with `%f`/`%e`/`%g` for doubles** — Verify doubles pass correctly through the
  variadic dlsym path for printf format strings. The x86-64 ABI requires `al` to hold the
  number of SSE registers used for variadic functions — this may need special handling.

## Medium Priority

### Language Completeness

- **String multi-return types** — Multi-return currently supports numeric (int64) slots only.

- **Right-shift operator `>>`** — `<<` is working; `>>` is tokenized but needs integration test.

- **Error diagnostics** — File and line number should always appear in error output.

- **Operator overloading (user-defined)** — `operator+`, `operator<<`, etc. on user types.
  Currently the compiler has hard-coded special cases for std::string assign, stream `<<`,
  subscript; there's no way for user code to define its own.

- **Sweep remaining `cc.newXxx(name)` calls for `%` safety** — `DataDef::newreg` and 32
  compiler.cpp sites routed through `"%s"` format. typesafe.cpp and some lambda paths still
  use raw names; audit those too.

## Low Priority

- **Multi-return in brace-less if** — `if (x) return a, b;` doesn't parse. Use braces.

- **`(type, type)` multi-return declaration syntax** — Explicit return type signatures.

- **Function pointer typedefs** — `typedef void DO_FUN(CHAR_DATA *ch, char *argument);`

- **Typed for-init with comma** — `for (int i = 0, j = 10; ...)` declares only i. The non-
  typed form (`for (a = 0, b = 10; ...)`) works.

## Deferred / Future

- **struct stat / sockaddr_in / dirent layouts** — `struct tm` and `struct timeval` done;
  still need these for `stat()`, socket functions, `readdir()`. Same glibc-layout-match
  approach as tm/timeval.

- **ARM64 support** — asmjit supports ARM64 backends. Currently x86-64 Linux only.

- **Phase 4: `libmadc.so` embedding API** — Decouple static globals, create public C API.

## Completed

### Session 2026-04-17 (Phase E finish + Phase F start)

- ~~**`__FILE__` and `__LINE__` macros**~~ — lexer expands to quoted filename and
  `source.line()`; works correctly inside `#define` bodies (each invocation captures the
  call site) (9e2d5ad)
- ~~**`cc.newXxx(name)` format-safety sweep**~~ — 32 direct call sites in compiler.cpp
  routed through `"%s"` format; `DataDef::newreg` already done earlier (c50acbe)
- ~~**Struct interop for libc types**~~ — `struct tm` (56 bytes), `struct timeval`
  (16 bytes), `struct fd_set` (128 bytes) with glibc-x86-64-matching layouts. FD_ZERO /
  FD_SET / FD_CLR / FD_ISSET macros forward to `__madc_fd_*` helpers. `select()` works
  end-to-end with a real pipe. Fixed `safemov(Gp, Mem)` to sign/zero-extend sub-qword
  loads (int32 struct members previously read 8 bytes) (2f08efd)
- ~~**Raw-pointer subscript `ptr[i]`**~~ — for `int *`, `char *`, etc., emit `[ptr + i *
  sizeof(base)]` with SIB scaling. Also fixed `safemov(Operand, Operand)` Gp←Mem path to
  forward to the size-aware variant (189f4ae)
- ~~**App-named bootstrap convention**~~ — documented in README: use `smaug.mad` /
  `myapp.mad` at project root, `#include`ing sources in dependency order with main() last
  (ac0cf4f)
- ~~**Self-referencing structs**~~ — `struct X { struct X *next; ... };` works by pre-
  registering the tag in struct_map before body parse (54087c2)
- ~~**Three-word compound types**~~ — `unsigned short int`, `signed long int`, `long long
  int`, `unsigned long long`, etc. (54087c2)
- ~~**`void` as sole parameter**~~ — `int f(void)` now parses as zero-arg (54087c2)
- ~~**Global fixed-size arrays**~~ — `struct X *arr[N]` at file/static-local scope;
  heap-allocated calloc'd backing storage, voperand loads absolute address (35c6bf0)
- ~~**Multi-variable declarations**~~ — `int a, b, c;`, `char *p, *q;`, `int x = 1, y =
  2;`. Push-back synthetic base-type token so parseCompound iterates naturally. Base type
  preserved for per-var `*` decorators (35c6bf0)
- ~~**stdin / stdout / stderr**~~ — lazy-registered in `<stdio.h>` as int64 globals
  initialized from `*dlsym("stderr")` etc.; returns the FILE* for fprintf (35c6bf0)
- ~~**`register struct X *p;`**~~ — register now accepts struct/typedef tokens via the
  same keyword-delegation path as static (35c6bf0)
- ~~**Unary minus after a keyword**~~ — `return -1;`, `if (-x > 0)` no longer misfires as
  "Missing operand". isUnaryPosition treats keywords as unary positions; isPostfixPosition
  excludes them (35c6bf0)
- ~~**TokenRETURN multi-return detection tightened**~~ — keywords and types excluded from
  the detection peek so `return X;` followed by `if(...)` / `ident()` doesn't misfire
  (35c6bf0)
- ~~**For-loop comma expressions**~~ — `for (a=0, b=1; cond; i++, j--)` — TokenFOR has
  init_extras / incr_extras vectors parsed via conditional parseExpression, compiled in
  order (be6c359)
- ~~**Forward decl + definition param mismatch**~~ — `FuncDef::findParameter` compares
  against DataDef name, so every definition-pass re-pushed all params. Track
  func_already_declared; skip re-push on the 2nd pass (be6c359)
- ~~**Compile-time error handler robustness**~~ — catch(const char*) block no longer
  crashes on NULL err_msg or dangling tb (be6c359)

### Session 2026-04-16 (Phase E underway)

- ~~**Chained `->` and `.` member access**~~ — `a->b->c`, `a->b.c`, `a.b.c`; LHS can be
  TokenVar, TokenMember, or TokenSubscript (b3a9d9a)
- ~~**C fixed-size arrays (1D)**~~ — `int arr[N]`, stack allocation + subscript
  `[base+idx*elem_size]` via SIB; honors regdp.first destination so it works as RHS of
  assignment (fd98935)
- ~~**Multi-dimensional fixed arrays**~~ — `int m[N][M]`, `int cube[N][M][K]`; extra_indices
  vector folded into linear offset (26dca0e)
- ~~**Brace initializer lists for arrays**~~ — `int a[] = {1,2,3}`, size inference, partial
  fill (rest zero), expressions as values (a1774d0)
- ~~**String-literal char-array init**~~ — `char msg[] = "hello"` expands to per-byte init
  list plus null terminator (1bae4f4)
- ~~**`char *msg = "literal"` sugar**~~ — routed through same path as `char msg[]` so the
  two forms produce identical storage (13bbc35)
- ~~**Struct initializer lists**~~ — `struct X v = { s0, s1 }` with scalars, pointers,
  char* (string_cstr coercion), std::string (string_assign); partial fill zero-fills
  remaining numeric/pointer members (8df2dca)
- ~~**Array-of-structs initializer**~~ — `struct Entry tab[] = {{"a",1}, {"b",2}}`; new
  TokenStructLit node for nested braces; TokenSubscript returns a Gp pointer for struct
  elements so `tab[i].name` reaches the member via TokenMember's Gp-base dot-chain path
  (e62d2e7)
- ~~**Crash handler with backtrace**~~ — SIGSEGV/SIGABRT/SIGFPE/SIGBUS/SIGILL caught at
  startup, prints signal name + fault address + backtrace to stderr, then re-raises so
  the shell still sees the real exit status (308b622)
- ~~**Remove mis-firing strlen builtin; add `.length()`/`.size()`**~~ — strlen resolves
  via dlsym to libc (correct for char*/char[]); std::string exposes length/size as
  instance methods (f04b7b6)
- ~~**DataDef::newreg format safety**~~ — variable names with `%` (our `__literal__foo`
  literal variable names embedded printf format specs) crashed asmjit's variadic
  `newGpq(name)`; fixed by passing `"%s"` as the format (e62d2e7)
- ~~**emit_struct_init no longer mutates shared Operand**~~ — the coercion of a
  std::string literal to char* was reassigning through the reference returned by
  compile(), which aliased the cached Operand in operand_map for global literals;
  subsequent uses of the same literal in another struct init re-coerced the already
  char-pointer, causing "alice" SSO bytes to be interpreted as a pointer
  (0x6563696c61) and crash in printf. Now uses a local Operand. (e62d2e7)

### Prior

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
- ~~**For-loop increment parsing bug**~~ — `i++`/`i--`/`++i`/`--i` in for-loop third position now works. Root cause: conditional `parseExpression` left `;` in stream; fixed with one extra `nextToken()` in `TokenFOR::parse()`
