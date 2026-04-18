# TODO

## High Priority

### Language Completeness

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

- **Typed for-init with comma** — `for (int i = 0, j = 10; ...)` declares only i. The non-
  typed form (`for (a = 0, b = 10; ...)`) works.

## Known Runtime Bugs (surfaced but pre-existing)

- **`ruby::chars` crashes in MadValue destructor** — `ruby::chars(arr, str);`
  crashes at runtime inside `a.data.clear()` in `ruby_chars`. Likely an
  ABI/layout issue with how MadArray is passed through a direct call whose
  registered return type is `dtARRAY`. The call-in-testlang.mad was commented
  out because the widened function-to-pointer decay (which is correct C
  semantics) now lets it compile, exposing the crash. Other MadArray ops
  (php::array_push, php::count) work fine. Needs separate investigation.

## Deferred / Future

- **struct stat / sockaddr_in / dirent layouts** — `struct tm` and `struct timeval` done;
  still need these for `stat()`, socket functions, `readdir()`. Same glibc-layout-match
  approach as tm/timeval.

- **ARM64 support** — asmjit supports ARM64 backends. Currently x86-64 Linux only.

- **Phase 4: `libmadc.so` embedding API** — Decouple static globals, create public C API.

- **Full C23 standard coverage** — After SMAUG 1.8 compatibility is reached, the next
  long-term goal for the C-dialect side is full C23 (including C99/C11/C17 features along
  the way: `_Bool`, designated initializers, compound literals, variadic macros with `__VA_ARGS__`,
  `_Static_assert`, `_Generic`, `_Alignas`/`_Alignof`, VLAs, `restrict`, digit separators,
  `#embed`, `constexpr`, `nullptr`, binary literals `0b…`, typeof, etc.). `.mad` stays as the
  naming convention for files using madc's beyond-C23 extensions (multi-language namespaces,
  etc.); bare `.c` / `.h` files get the C-compatible subset.

## Completed

### Session 2026-04-18

- ~~**Function pointer typedefs**~~ — `typedef void DO_FUN(CHAR_DATA *ch, char
  *argument);` (Form 1, SMAUG idiom) and `typedef int (*UNOP)(int);` (Form 2,
  classic C) both register a `DataDefFPTR` in `datatype_map`. Declarations like
  `DO_FUN *cmd;` and `UNOP u;` create function-pointer variables; the `*` on
  Form 1 uses is a no-op because the typedef already names a pointer-like
  storage. Assignment uses C's function-to-pointer decay: `fn = func_name;`
  takes the function's address (previously this mis-parsed as a no-arg call to
  `func_name`). Decay widens to any value-context follower — assignment
  operators (`=`, `+=`, `<<=` etc.), and struct/array-init / call-arg / ternary
  separators (`,`, `}`, `)`, `]`, `:`) — so SMAUG's command-table pattern
  `struct cmd c = { "who", do_who };` works. `cout << endl;` still parses as
  BSL-consumed call because `endl;` has neither an assignment operator on top
  of opStack nor a value-end follower. `tests/testfnptrtypedef.mad` covers
  typedefs + reassignment + invocation. `tests/testfnptrstruct.mad` covers
  the SMAUG command-table pattern.
- ~~**char* coercion in function-pointer indirect calls**~~ — `TokenCallFunc`'s
  fptr path now runs the same `string_cstr` coercion the direct-call path
  does, so `fp("world")` where `fp` is declared `void STRFN(char *)` passes
  the string-literal's `.c_str()` instead of the `std::string*` pointer.
- ~~**Direct invocation through struct-member function pointer**~~ — `c.fn(args)`
  now parses and runs. When the parser sees `(` after a `TokenMember` whose
  datadef is `DataDefFPTR`, it builds a `TokenCallFunc` whose new `src_node`
  field points at the member. At compile time the fptr-call path compiles
  `src_node` to materialise the function-pointer value, instead of looking
  it up from a variable. Also fixes struct-body parsing so `DO_FUN *fn;`
  inside a struct stays a `DataDefFPTR` member (previously got wrapped in
  an extra `DataDefPTR`, which defeated fptr dispatch). `tests/testfnptrmember.mad`
  covers direct invocation.
- ~~**Global function-pointer initialization**~~ — `DO_FUN *g = do_who;` at
  file scope, and `struct cmd tab[] = { {"who", do_who}, ... };` (SMAUG-style
  command tables), now compile and run correctly. Root cause was two-fold:
  (a) `Variable` constructor skipped heap allocation for any `btFunct` type,
  including `DataDefFPTR` which IS storage; (b) user functions compiled AFTER
  `TokenProgram` processed globals, so LEA of the target function's label
  emitted nothing (funcnode was still NULL). Fixes: allocate storage for
  `DataDefFPTR` globals (size 8), and add a pre-pass in `Program::compile`
  that creates a FuncNode label for every user function before globals
  compile, via `TokenFunc::prepareFuncNode` (factored out of
  `TokenFunc::compile`). A new `pending_funcs` vector on `Program` tracks
  user functions + lambdas in source order. Also excludes `DataDefFPTR`
  variables from the existing function-global-x86code backfill loop to
  avoid mistreating their 8-byte data as a `Method*`.
  `tests/testfnptrglobal.mad` covers global fn-ptr + SMAUG command-table
  dispatch.
- ~~**Reassigning a struct's function-pointer member**~~ — `c.fn = other_fn;`
  now works. `TokenVar::compile` for a function identifier assumed the
  assignment destination was a Gp register; for struct-member LHS the
  destination is a Mem. Now LEAs the function address into a tmp Gp and
  stores to the Mem when `regdp.first->isMem()`. Also returns the tmp
  unchanged for no-dest callers. `tests/testfnptrreassign.mad` covers
  member reassignment and reassignment through a local fn-ptr variable.
- ~~**`sizeof(object)` for variables and fixed arrays**~~ — the parser now
  resolves `sizeof(identifier)` through normal variable lookup before falling
  back to type lookup, so local scalars and fixed arrays like `char buf[32];`
  return the right compile-time size. `tests/testsizeof.mad` now covers
  `sizeof(scalar)` and `sizeof(buf)`.
- ~~**First real SMAUG source compatibility test (`requests.c`)**~~ — added
  `tests/testsmaug_requests.mad`, which exercises the upstream
  `requests.c` body against a minimal SMAUG shim header. This flushed out the
  `sizeof(buf)` parser gap and the missing opaque `FILE` alias in embedded
  `<stdio.h>`, both now fixed enough for the test to compile and run.
- ~~**printf with `%f`/`%e`/`%g` for doubles**~~ — verified the existing
  variadic call path handles double arguments correctly for both direct libc
  `printf` and a user-defined `...` wrapper around `vsprintf`. Mixed
  string/int/double calls and repeated double arguments all format correctly;
  no compiler change was required. `tests/testprintfdouble.mad` covers `%f`,
  `%e`, `%g`, mixed scalar calls, and multiple doubles per call.
- ~~**`->` member access in certain printf arg combinations**~~ — the parser's
  early comma-count guard in `parseCallFunc()` / `parseCallMethod()` now skips
  "too many parameters" rejection for declared varargs targets, so wrapper
  calls like `wrapper("%s %ld", ch->name, ch->in_room->vnum, ...)` compile
  correctly even when the extra arguments are `->` member expressions or
  macro-expanded member expressions. `tests/testprintfmember.mad` covers plain
  libc `printf`, macro-expanded nested members, and a user-declared `...`
  wrapper around `vsprintf`.
- ~~**`cout << [const char*]` in chained expressions**~~ — `TokenBSL::compile()`
  now routes `dtCHARptr` through `streamout_cstr` before the generic numeric
  stream path, so chained forms like `cout << ident(msg) << endl` and
  `"prefix: " << ident(msg)` no longer fall into the unsupported numeric
  switch. `tests/testcoutcstr.mad` covers plain `char*`, function-returned
  `char*`, and mixed string-prefix chains.
- ~~**`*ptr` dereference on stack variables**~~ — address-taken numeric locals and
  parameters now spill to stable stack slots instead of living only in virtual
  registers. `TokenAddrOf` marks variables as address-taken during parse,
  `TokenCpnd::voperand()` allocates a typed `cc.newStack()` slot for those
  numerics, and `TokenFunc::compile()` spills incoming numeric parameters into
  that slot before the body runs. `tests/testptr.mad` now covers both
  `int n; int *p = &n; *p = ...;` and `int *p = &param;`.

### Session 2026-04-17 (Phase F continues — hashstr.mad runs)

- ~~**Inc/dec on struct members**~~ — `++ptr->links`, `ptr->field--` etc.
  TokenInc / TokenDec extended to `ttMember` (via `->` and `.`) and
  `*deref` via `resolveCompoundLHS`. Shares the load-op-store pattern
  with the compound-assignment operators (e8c3f0b).
- ~~**`c++` / `--x` in compound-comma increment contexts**~~ — same
  underlying fix; SMAUG's `for (ptr = head, c = 0; ptr; ptr = ptr->next,
  c++)` pattern now compiles and runs (e8c3f0b).
- ~~**`char *p; p = "literal";`**~~ — post-decl string-literal
  assignment. TokenAssign detects char* ← dtSTRING and routes through
  `string_cstr` to pull the literal's data pointer; previously wrote
  the std::string operand verbatim so reads printed garbage (acfc8b1).
- ~~**Unsigned comparison ops**~~ — `TokenLT`/`LE`/`GT`/`GE` pick
  setb/setbe/seta/setae when either operand is unsigned. Without this,
  `if (ptr->links < 65535) ++ptr->links;` with unsigned short skipped
  the then-branch because 65535 sign-interprets as -1 (e8c3f0b).
- ~~**Global pointer variable read/write**~~ — `DataDefPTR` overrides
  `movrval2mptr`/`movrval2rptr`/`movint2rptr`/`movmptr2rval` with
  qword semantics. Base-class switch on DataType fell through to the
  unhandled default for rtPtr() values (>= 10000), so `global_ptr = x;`
  was a silent no-op (e8c3f0b).
- ~~**Subscript → member assign**~~ — `p->next = arr[i];` now writes.
  `TokenSubscript::compile` respects a Mem destination by loading
  into a temp and storing; previously overwrote regdp.first with its
  own fresh Gp and abandoned the member's Mem (e8c3f0b).
- ~~**Sub-qword sign/zero-extension in `resolveCompoundLHS`**~~ —
  loading a word-sized member into a Gp64 for arithmetic now uses
  movzx/movsx/movsxd. Plain `cc.mov(gpq, word_ptr)` is invalid x86 and
  asmjit silently emitted truncated ops or dropped them (e8c3f0b).
- ~~**`safemov(Mem, Gp)` size mismatch**~~ — picks r8/r16/r32/r64
  register view based on Mem size, not Gp size. Without this, writing
  a Gp64 (widened member_lhs) to a word-sized member silently dropped
  (e8c3f0b).
- ~~**Parser: comma peek-stop in conditional mode**~~ — nested
  parseExpression (in the cast-body handler) used to consume the `,`
  that terminated the outer function-call arg, so `strcpy((char *)h +
  8, "x")` parsed as one arg (e8c3f0b).
- ~~**`TokenIF::compile` regdp reset**~~ — zeroes regdp before
  condition, then branch, and else branch, matching the regdp-reset
  rule already applied to TokenFOR/WHILE/DO (e8c3f0b).

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
