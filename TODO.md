# TODO

## High Priority

### Language Completeness

- **`goto label` + forward labels** — needed for several SMAUG sources
  (mud_prog.c, magic.c, build.c, tables.c, ban.c, services.c,
  mud_comm.c). `tkGOTO` exists as a lexer token but no parse/compile
  support. Currently routed around by stubbing the calling symbols.

- **`IRBuilder::coerce() invalid src`** inside fight.c / skills.c
  compile — surfaces after `class`-as-identifier lands and the
  umbrella reaches deep into those TUs. Likely a typed-pointer-return
  mis-wiring where a compile path lets `regdp.second` fall to NULL
  before calling a shared helper that then feeds an invalid IRValue
  into `ir.coerce`. Reproducer pending — current MadSMAUG umbrella
  front edge.

- **Real `<` / `<=` comparison with Mem-backed literal RHS** —
  `1.5 < 2.0` returns 0. `>` works. Narrow comparison bug distinct
  from the storage truncation just fixed; surfaced while verifying
  the fix via `==` / `<` / `>` checks.

- **Function-like macros shadowing later definitions** — SMAUG.mad does
  `#define bug(...) ((void)0)` to stub out calls, then `#include`s
  `db.c` which contains `void bug(const char *str, ...) { … }`. The
  lexer expands `bug(` at the definition, turning `void bug(...)` into
  `void ((void)0)` and killing the parse. The MadSMAUG umbrella now
  wraps the db.c include with `#undef bug` / re-`#define bug(...)` as
  the option-(b) workaround. A proper fix would teach the lexer to
  suppress function-like macro expansion when the identifier is in a
  definition/declaration head position (option-(a)).


## Medium Priority

### Language Completeness

- **Struct-member double / mixed float+double printf** —
  `printf("%.2f", v.x)` where v.x is a struct-member double
  (or more generally, mixing local floats with struct-member
  reals across multiple printf calls in one function) produces
  non-deterministic wrong output — the result depends on the
  JIT source filename length and unrelated earlier float
  activity. The generated asm looks correct on inspection
  (each call cvtss2sd's its own load and movdqa's into xmm0
  before the call), but asmjit's compiler pass reorders or
  elides something under specific register-pressure shapes.
  Single-local float varargs `printf("%f", local_float)` is
  reliable after the cvtss2sd-promotion fix. Broader cluster
  (struct-member double varargs, multi-float interleaved with
  printf) is a known asmjit Compiler interaction that the
  typed-register IR work is expected to supersede. Workaround:
  copy the real member into a local double before printf.

- **String multi-return types** — Multi-return currently supports numeric (int64) slots only.

- **Error diagnostics** — parser-side diagnostics already carry source context,
  and compiler top-level raw-string failures now anchor to the current
  statement token. Remaining work is to convert deeper raw-string throws to
  token-aware `Throw(...)` paths where a more precise inner location exists.

- **Operator overloading (user-defined)** — `operator+`, `operator<<`, etc. on user types.
  Currently the compiler has hard-coded special cases for std::string assign, stream `<<`,
  subscript; there's no way for user code to define its own.

## Low Priority

- **Multi-return in brace-less if** — `if (x) return a, b;` doesn't parse. Use braces.

- **`(type, type)` multi-return declaration syntax** — Explicit return type signatures.

## Known Runtime Bugs (surfaced but pre-existing)

- **`ruby::chars` crashes in MadValue destructor** — `ruby::chars(arr, str);`
  crashes at runtime inside `a.data.clear()` in `ruby_chars`. Likely an
  ABI/layout issue with how MadArray is passed through a direct call whose
  registered return type is `dtARRAY`. The call-in-testlang.mad was commented
  out because the widened function-to-pointer decay (which is correct C
  semantics) now lets it compile, exposing the crash. Other MadArray ops
  (php::array_push, php::count) work fine. Needs separate investigation.

## Deferred / Future


- **ARM64 support** — asmjit supports ARM64 backends. Currently x86-64 Linux only.

- **Phase 4: `libmadc.so` embedding API** — Decouple static globals, create public C API.

- **Full C23 standard coverage** — After SMAUG 1.8 compatibility is reached, the next
  long-term goal for the C-dialect side is full C23 (including C99/C11/C17 features along
  the way: designated initializers, compound literals, variadic macros with `__VA_ARGS__`,
  `_Static_assert`, `_Generic`, `_Alignas`/`_Alignof`, VLAs, digit separators,
  `#embed`, `constexpr`, `nullptr`, typeof, etc.). `.mad` stays as the
  naming convention for files using madc's beyond-C23 extensions (multi-language namespaces,
  etc.); bare `.c` / `.h` files get the C-compatible subset.

## Completed

### Session 2026-04-24 (SMAUG Phase F front-edge resumption)

- ~~**`class` as a plain C identifier**~~ — parseStatement routes
  `class` through parseExpression when it's not followed by an
  identifier or `{` (real class-declaration head); parsePostfixChain
  accepts `class` after `.` / `->` via `is_contextual_identifier_token`
  (which was already set up to accept tkCLASS and the STL keywords —
  just needed to be plugged into the postfix-chain member-name parse).
  Targeted regression: `tests/testclassident.mad`. Advances MadSMAUG
  through skills.c.

- ~~**`safemov(Operand, double, ...)` truncating to int for Mem
  destinations**~~ — when TokenOperator::optimize constant-folded
  a real expression (`double d = 1.0 + 0.5;`) and the caller's
  destination was a Mem, the double-valued fallback called
  `imm((int)d)` and silently dropped the fractional part. Now takes
  the Xmm-through-constpool path when `d1->is_real()`. Regression:
  `tests/testrealconstfold.mad`.

- ~~**Struct-array subscript element stride + base addressing**~~ —
  TokenSubscriptExpr and the TokenAssign write branch now fold
  non-power-of-2 element sizes into the index via `imul`, and both
  sites use `base_expr->operand()` rather than `compile()` to avoid
  the emit_ir_value load-first-element path for numeric-typed
  TokenMember bases. `struct { int bits[N]; }` reads / writes work,
  nested struct arrays like `o.arr[i].member` work with correct
  per-element strides. Targeted regression: `tests/teststructarrsub.mad`.

- ~~**`expr[i].member` parses + compiles for struct element types**~~
  — dot handler's ttSubscript branch now detects TokenSubscriptExpr
  explicitly (the NULL dynamic_cast to TokenSubscript used to
  segfault). TokenSubscriptExpr::compile returns raw element Mem for
  struct/class element types so the TokenMember parent-expr path can
  add the member offset without emit_ir_value corrupting the Mem.
  Advances MadSMAUG from handler.c:4789 to an undefined-function
  front edge in fight.c (`learn_from_failure`). Targeted regression:
  `tests/testsubscriptexprmember.mad`.

- ~~**Leading-dot float literal `.4`**~~ — lexer's single-`.` case now
  peeks for a digit and parses the fractional expansion (consuming
  the optional `f/F/l/L` suffix) before falling back to TokenDot.
  Advances MadSMAUG from handler.c:4683 to handler.c:4789. Targeted
  regression: `tests/testleadingdotfloat.mad`.

- ~~**`(*p).member` now parses as `p->member`**~~ — the dot handler in
  parseExpression cast `lhs_dot` to `TokenMember *` unconditionally,
  but `TokenDeref` and `TokenDerefExpr` both report `ttMember` without
  actually deriving from `TokenMember`. Added explicit branches:
  TokenDeref routes tv_var through the underlying pointer variable
  (no parent_expr — TokenMember's voperand pointer-in-Gp path compiles
  it as `[ptr+offset]`); TokenDerefExpr synthesizes a struct-typed
  object variable and passes the TokenDerefExpr as parent_expr (the
  struct-value "dot chain" branch then handles `[ptr+offset]`).
  Exposed by SMAUG's `xIS_SET((var), bit)` macro after `*vector`
  substitution — `vector` is also a madc keyword (STL container
  reserve), so the unary-`*` handler hands a keyword rather than an
  identifier and falls into the TokenDerefExpr path. Advances the
  MadSMAUG umbrella from handler.c:2989 to handler.c:4683. Targeted
  regression: `tests/testparenderefmember.mad`.

- ~~**Struct-copy initialization and assignment**~~ — `struct S a = other;`
  (decl init) and `a = other;` (plain assign) now emit
  `memcpy(&a, &other, sizeof(S))`. Parser pushes `=` back and falls
  through to the normal init path when the struct RHS isn't `{`;
  `TokenAssign::compile` gained a btStruct/btClass branch that LEAs
  both sides and invokes libc `memcpy`. Handles both `struct S b = a;`
  and `struct Inner extra = p->member;` (where TokenMember::operand
  LEAs the member into a Gp for non-numeric members). Targeted
  regression: `tests/teststructcopy.mad`. Advances MadSMAUG from
  handler.c:1284 to handler.c:2990 (next front edge is the parser
  crash on `(*p).member`).

- ~~**`#include` canonicalization via realpath**~~ — `should_tokenize_include`
  now canonicalizes each resolved quoted-include path through `realpath()`
  before the include-once check. The MadSMAUG umbrella was tripping on
  `mud.h` being pulled in both as `upstream_src/mud.h` (from SMAUG.mad)
  and bare `mud.h` (from ident.c / interp.c / ibuild.c): two distinct
  keys in the seen-files map even though both resolve — via symlinks —
  to the same underlying file. Embedded-header keys starting with `<`
  bypass realpath entirely.

- ~~**`*p++ = rhs` / `*p-- = rhs` as a write target**~~ — the read side
  (`c = *p++`) already compiled through TokenDerefStep, but the write
  side threw `Assignment on a non-variable lval` because
  TokenAssign::compile only dispatched TokenVar / TokenDeref /
  TokenDerefExpr / TokenMember / TokenSubscript* LHS kinds. Added a
  TokenDerefStep LHS branch that mirrors the read: capture `old_ptr = ptr`,
  step the pointer variable, then expose `[old_ptr]` as the Mem lval
  the numeric-assignment path writes into. Closes the MadSMAUG umbrella
  front edge in `act_move.c:182` (`grab_word`). Targeted regression:
  `tests/testderefpostincstore.mad`.

### Session 2026-04-23

- ~~**`->` after expression parents (call / subscript / cast / deref-expr)**~~ —
  `TokenMember::operand()`'s chained-arrow path previously called
  `parent_expr->operand()` unconditionally, which works for `TokenMember`
  parents (they re-materialize their own address each call) but silently
  failed for `TokenCallFunc` / `TokenCallMethod` / `TokenSubscript` /
  `TokenDerefExpr`, whose `operand()` returns a fresh register without
  emitting the computation. Now compiles non-`ttMember` parent expressions,
  so `get_slot(i)->value` and the chained call-ptr dispatch patterns emit
  the producing computation before dereferencing. Targeted regression:
  `tests/testglobalptrarrayarrow.mad`.

- ~~**Mem-backed arithmetic expression codegen**~~ — plain arithmetic /
  bitwise operators now compute through a temp register when the caller's
  destination is Mem, then mirror the result back; compound assignments now
  do the same for stack-backed variables. This closes SMAUG front edges in
  `bet.h` / `hashstr.c` such as `number = (number * 10) + ...`,
  `number *= (multiplier = 1000)`, and `hash = len % STR_HASH_SIZE`.
  Targeted regressions: `tests/testassignexprmem.mad`,
  `tests/testcompoundassignmem.mad`.

- ~~**Unary `*` on fixed arrays (`*arg`, `!*buf`)**~~ — the parser now
  accepts fixed arrays in unary dereference contexts via normal C
  array-to-pointer decay. This advances the MadSMAUG umbrella past
  `interp.c`'s `if ( !*arg )` in `do_timecmd`. Targeted regression:
  `tests/testderefarray.mad`.

- ~~**`struct servent` interop in embedded `<netdb.h>`**~~ — 32-byte
  glibc-matching layout (`char *s_name; char **s_aliases; int s_port;
  char *s_proto;`). Closes the MadSMAUG umbrella bootstrap's
  `sock.sin_port = serv->s_port;` front edge in upstream `ident.c`.
  `tests/testservent.mad` drives `getservbyname("ftp","tcp")` and
  `getservbyname("http","tcp")` end-to-end and asserts
  `sizeof(struct servent) == 32`.

- ~~**Extended `<errno.h>` socket/network constant coverage**~~ —
  added `EWOULDBLOCK` (= `EAGAIN`), `EINPROGRESS`, `EALREADY`,
  `ENOTSOCK`, `EDESTADDRREQ`, `EMSGSIZE`, `EPROTOTYPE`, `ENOPROTOOPT`,
  `EPROTONOSUPPORT`, `ESOCKTNOSUPPORT`, `EOPNOTSUPP`, `EPFNOSUPPORT`,
  `EAFNOSUPPORT`, `EADDRINUSE`, `EADDRNOTAVAIL`, `ENETDOWN`,
  `ENETUNREACH`, `ENETRESET`, `ECONNABORTED`, `ECONNRESET`, `ENOBUFS`,
  `EISCONN`, `ENOTCONN`, `ESHUTDOWN`, `ETIMEDOUT`, `ECONNREFUSED`,
  `EHOSTDOWN`, `EHOSTUNREACH`, plus the System V / extended POSIX
  errors (`EDEADLK`, `ENAMETOOLONG`, `ENOLCK`, `ENOSYS`, `ENOTEMPTY`,
  `ELOOP`, `EDOM`, `EILSEQ`, `EOVERFLOW`, `ENODATA`, `ETXTBSY`,
  `EUSERS`, `EDQUOT`, `ESTALE`, `ENOMSG`). Closes the MadSMAUG umbrella
  bootstrap's `errno != EINPROGRESS` front edge in upstream `ident.c`.

- ~~**Extended `<fcntl.h>` constant coverage for `fcntl()`**~~ — embedded
  `<fcntl.h>` now defines the `F_DUPFD`/`F_GETFD`/`F_SETFD`/`F_GETFL`/
  `F_SETFL`/`F_GETLK`/`F_SETLK`/`F_SETLKW`/`F_SETOWN`/`F_GETOWN`/
  `F_DUPFD_CLOEXEC` command constants and the adjacent `FD_CLOEXEC`,
  `O_NDELAY`, `O_ASYNC`, `O_DIRECTORY`, `O_NOFOLLOW` flags at their
  Linux x86-64 values. This closes the MadSMAUG umbrella `F_SETFL` front
  edge in upstream `ident.c`'s `fcntl(..., F_SETFL, FNDELAY)` path.
  `tests/testfcntl.mad` runs an end-to-end `F_GETFL` / `F_SETFL` round
  trip against a real file descriptor to prove nonblocking mode is
  actually set.

- ~~**Control-flow paren helper + assignment-expression regressions**~~ —
  control-flow condition parsing now goes through a reusable parenthesized
  expression helper instead of per-keyword ad hoc handling, which fixed
  `_Bool` single-statement `if (...) stmt; else stmt;` parsing/branching.
  Function-call RHS assignment now also returns into Mem-backed lvalues
  correctly, so assignment-in-condition loops work again with stack-backed
  locals. Direct regressions now pass: `tests/testc23_bool.mad`,
  `tests/testpostfix.mad`, and `tests/testassigninexpr.mad`.

- ~~**Pointer-return typing for dereferenced call results**~~ —
  `addFunction()` now resolves generic `rtPtr(...)` signatures through a
  helper instead of a few hard-coded pointer cases, so builtin/external
  functions like `__errno_location()` retain typed pointer returns during
  parse. The unary `*` path also now treats identifier-followed-by-`(` as a
  full call expression, which fixes `*get_msg()`, `*(version.c_str())`, and
  `errno` / `*(__errno_location())` parsing. Targeted regressions:
  `tests/test_ptr_fn_deref.mad`, `tests/test_get_argv_deref.mad`,
  `tests/test_errno_deref.mad`.

- **Next MadSMAUG front edge** — full `/workspace/MadSMAUG/src/SMAUG.mad`
  now advances to undeclared `timerisset` at `interp.c` / `do_timecmd`
  (`/workspace/MadSMAUG/src/SMAUG.mad:1257:18`). Likely next work is
  embedded timeval helper macro coverage.

### Session 2026-04-18

- ~~**Assignment as expression inside declaration init**~~ — declaration
  initializers like `int y = (x = 42);` now preserve the outer assignment to
  the declared variable instead of collapsing to the inner assignment only.
  The parser keeps the original assignment-context parse for normal
  initializers, then wraps the result only when the parsed initializer does
  not already assign to the declared variable. Brace initializers stay on the
  existing `init_list` path. `tests/testdeclassignexpr.mad` covers nested
  assignment in declaration init, expression use, and comma declarations.
- ~~**Right-shift operator `>>` integration coverage**~~ — `tests/testbsl.mad`
  now exercises plain arithmetic right shift in addition to the existing
  left-shift cases, and `tests/testbsl.expect` asserts the concrete outputs.
  This closes the lingering gap where `>>=` and `cin >>` were covered but
  plain bitwise `>>` had no integration assertion.
- ~~**`cc.newXxx(name)` percent-safety sweep**~~ — verified that the
  user-derived register names now flow through `"%s"` call sites or
  `DataDef::newreg()`, which already applies the safe formatting wrapper in
  `include/datadef.h`. The remaining named temporaries in `typesafe.cpp` and
  lambda-related paths are fixed literals, not user-controlled format
  strings, so the old TODO entry was stale rather than unfinished.
- ~~**Typed for-init with comma**~~ — `for (int i = 0, j = 10; ...)` now
  works by reusing `parseDeclaration()` for the typed `for` initializer and
  feeding any synthetic comma-continuation declarations into
  `TokenFOR::init_extras`. `tests/testfortypedcomma.mad` covers plain scalar
  declarations, three-variable init, and mixed pointer/scalar init in the
  same typed `for` header.
- **Error diagnostics narrowed** — `Program::compile()` now prints file/line
  context for raw-string compiler failures by anchoring them to the current
  statement token (and the current pre-pass token during FuncNode setup),
  instead of emitting location-less `: error:` lines. Full closure still
  requires replacing deeper raw `throw "..."` sites with token-aware
  diagnostics where available.

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
- ~~**Assignment as a value in enclosing expressions**~~ — `while ((entry =
  readdir(d)) != NULL)`, `if ((n = get()) > 0)`, `y = (x = 42)` now evaluate
  the assignment as an expression that returns the assigned value. Root
  cause: `TokenAssign::compile`'s numeric-assign path wrote the RHS into
  the LHS storage, restored the caller's pre-existing `regdp.first`, and
  returned it — but never mirrored the assigned value into that
  caller-provided destination. Now when the caller's `regdp.first` is a
  distinct Gp (the enclosing comparison / containing-assignment passed
  its own accumulator), copy `_operand` into it via `safemov` before
  returning. Unlocks the standard SMAUG readdir / accept / recv
  assign-in-condition idioms. `tests/testassigninexpr.mad` covers
  while-condition, if-condition, chained assign, and paired assign.
- ~~**`struct sockaddr` + `struct sockaddr_in` + `struct in_addr` interop**~~ —
  `sys/socket.h` adds the 16-byte generic `struct sockaddr`; `netinet/in.h`
  adds the 16-byte `struct sockaddr_in` and 4-byte `struct in_addr` with
  glibc-matching layouts. Also `sa_family_t`, `in_port_t`, `in_addr_t`,
  `socklen_t` type aliases. Real socket bind on loopback works via
  `bind(s, (struct sockaddr *)&addr, sizeof(addr))`. `tests/testsockaddr.mad`
  opens, binds, closes a TCP loopback socket.
- ~~**`struct dirent` interop**~~ — `dirent.h` carries the 280-byte glibc
  layout. `opendir()` / `readdir()` / `closedir()` via dlsym; user code
  iterates and inspects `entry->d_type` / `entry->d_ino`.
  `tests/testdirent.mad` scans `tests/` and counts DT_REG / DT_DIR entries.
- ~~**Fixed-size array members in struct bodies**~~ — `char buf[N]`,
  `int m[N][M]` inside a struct now parses. The parser collects the
  dimension(s) after the member identifier, multiplies them, and calls
  `addMember(name, type, count)` with the product. The member reserves
  `count * sizeof(base)` bytes inline; access the buffer's starting
  pointer via `&obj.buf`. Needed for `struct dirent::d_name[256]` and
  SMAUG's many fixed char buffers.
- ~~**Unary `&` / `*` after a cast**~~ — `(struct sockaddr *)&addr` and
  `(int *)*ptr` now parse. The cast block nulls `_prv_token` after
  consuming its closing `)` so `isUnaryPosition` sees the cast-body's
  head token in a unary context; otherwise the close-paren leaked
  through and `&` / `*` mis-parsed as binary operators.
- ~~**`struct stat` + `struct timespec` interop**~~ — `sys/stat.h` now embeds
  the glibc x86-64 `struct stat` layout (144 bytes) with natural C ABI
  alignment plus the `struct timespec` (16 bytes) used by its `st_atim` /
  `st_mtim` / `st_ctim` triplet. Type aliases (`mode_t`, `uid_t`, `off_t`,
  etc.) land as `#define`s that expand to the concrete madc primitive types.
  File-type predicate macros (`S_ISREG`, `S_ISDIR`, `S_ISLNK`, `S_ISBLK`,
  `S_ISCHR`, `S_ISFIFO`, `S_ISSOCK`) added as function-like macros.
  `st_atime` / `st_mtime` / `st_ctime` aliases resolve to
  `st_Xtim.tv_sec` via object-like macros, matching glibc. `stat()`,
  `fstat()`, `lstat()`, `chmod()`, `mkdir()`, `mkfifo()` resolve through
  the existing dlsym fallback. `tests/teststat.mad` drives real `stat()`
  on a regular file, a directory, a missing path, and checks
  `st_mtime > 0`.
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
