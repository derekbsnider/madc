# Changelog

## [Unreleased] — SMAUG Phase F Continues (2026-04-18)

### Added

- **Function pointer typedefs** — `typedef void DO_FUN(CHAR_DATA *ch, char
  *argument);` and `typedef int (*UNOP)(int);` (SMAUG-style and classic C
  forms). Both produce a `DataDefFPTR` registered in `datatype_map`.
  Declarations like `DO_FUN *cmd;` and `UNOP u;` yield function-pointer
  variables; call through with `cmd(args)`. The `*` decorator on Form 1
  (`DO_FUN *cmd`) is accepted as a no-op since the typedef already names a
  function-pointer storage. New helper `Program::parseFnPtrParams` reads a
  parameter list (types only, optional names discarded) and builds a
  `FuncDef` to wrap in the `DataDefFPTR`. `tests/testfnptrtypedef.mad` covers
  both forms, reassignment, and invocation with `cout <<`.

- **SMAUG command-table pattern** — `struct cmd { char *name; DO_FUN *fn; };`
  followed by `struct cmd c = { "who", do_who };` now compiles and runs.
  Both dispatch forms work: intermediate variable (`DO_FUN *fp = c.fn;
  fp(args);`) and direct invocation (`c.fn(args);`).
  `tests/testfnptrstruct.mad` covers the intermediate pattern;
  `tests/testfnptrmember.mad` covers direct invocation.

- **Direct struct-member function-pointer invocation** — `c.fn(args)` and
  `o.fn(a, b)` now parse and run. The parser detects the `(` following a
  `TokenMember` whose datadef is `DataDefFPTR` and builds a `TokenCallFunc`
  whose new `src_node` field points to the member. At compile time the
  fptr-call path compiles `src_node` to materialise the function-pointer
  value, instead of looking it up from a variable. Also fixes struct-body
  parsing so `DO_FUN *fn;` inside a struct stays a `DataDefFPTR` member
  (previously got wrapped in `DataDefPTR(DataDefFPTR)`, which defeated
  fptr dispatch — the parseDeclaration skip for fnptr-base needed the
  same treatment at the struct-body level).

### Fixed

- **Function-to-pointer decay for value contexts** — `fptr = func_name;`,
  `struct X x = { "name", func_name };`, `call(a, func_name, b);`, `cond ?
  f1 : f2` — anywhere a bare function identifier appears as a value — now
  pushes the function's address instead of mis-compiling as a no-arg call.
  Decay triggers when the function identifier isn't followed by `(` and
  either (a) top of opStack is an assignment op (`=`, `+=`, `-=`, `*=`,
  `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`) or (b) the follower token
  is a value-end (`,`, `}`, `)`, `]`, `:`). `cout << endl;` keeps the
  pre-existing behavior because `endl;` has neither, so BSL's special
  handling of ostream-consuming no-arg functions still applies.

- **`char*` coercion in function-pointer indirect calls** —
  `TokenCallFunc::compile`'s fptr path (the `is_function() && is_numeric()`
  branch) now runs the same `dtSTRING -> dtCHARptr` coercion via
  `string_cstr` that the direct-call path uses. Previously, passing a
  string literal to a typedef-declared `void (*)(char *)` function pointer
  would pass the `std::string*` pointer verbatim, so the callee received
  the string object header instead of the null-terminated bytes.

## [Unreleased] — SMAUG Phase F Continues (2026-04-17)

MadSMAUG's `hashstr.mad` now compiles AND runs correctly end-to-end
(`bin/madc MadSMAUG/src/SMAUG.mad` → expected link-count hash stats with
no runtime errors). Every language gap exposed while running the file
was fixed in madc proper. 81 integration + 25 unit tests pass.

### Added

- **Inc/dec on struct members** (e8c3f0b) — `++ptr->links`, `ptr->field--`
  etc. via `TokenInc` / `TokenDec` now support `ttMember` (via `->` and
  `.`) and `*deref` targets, sharing the load-op-store pattern with the
  compound-assignment operators through `resolveCompoundLHS`. Previously
  threw "Increment on a non-variable rval" on anything other than a
  plain variable.

- **For-loop compound-comma increment with postfix inc** (e8c3f0b) — the
  classic SMAUG `for (ptr = head, c = 0; ptr; ptr = ptr->next, c++)`
  pattern now compiles and runs; the postfix-inc-in-incrementer path
  was the same underlying gap as the member-inc one.

- **Post-declaration `char *p; p = "literal";`** (acfc8b1) — and `ptr->
  name = "alice";` — TokenAssign detects char* ← dtSTRING and routes
  through `string_cstr` to pull the literal's data pointer. Before, the
  RHS's `std::string` operand was written verbatim into p, so reads
  dereferenced the string object header and printed garbage.

- **Unsigned comparison operators** (e8c3f0b) — `TokenLT` / `TokenLE` /
  `TokenGT` / `TokenGE` pick `setb` / `setbe` / `seta` / `setae` when
  either operand is unsigned, vs the signed `setl` / `setle` / `setg` /
  `setge` previously used always. Without this, `if (ptr->links <
  65535) ++ptr->links;` with `unsigned short int` jumped over the
  increment because 65535 sign-interprets as -1. New `safesetb` /
  `safesetbe` / `safeseta` / `safesetae` helpers in `typesafe.cpp`.

### Fixed

- **Global pointer variable read/write** (e8c3f0b) — `DataDefPTR` now
  overrides `movrval2mptr` / `movrval2rptr` / `movint2rptr` /
  `movmptr2rval` with explicit qword semantics. The base-class switch
  on `DataType` fell through to the unhandled default for `rtPtr()`
  values (>= 10000), so `global_ptr = x;` silently dropped the store.
  Every global pointer variable read would return the slot's stale
  initial memory rather than the stored value.

- **Subscript → member assign** (e8c3f0b) — `p->next = arr[i];` now
  writes. `TokenSubscript::compile` respects a caller-supplied `Mem`
  destination (the struct member's Mem) by loading into a temp and
  storing; previously it overwrote `regdp.first` with its own fresh Gp
  and abandoned the member's Mem, making the assignment a no-op.

- **Sub-qword sign/zero-extension in `resolveCompoundLHS`** (e8c3f0b) —
  loading a word-sized member into a Gp64 for arithmetic now uses
  `movzx` (unsigned) / `movsx` (signed) / `movsxd` / `mov r32,m32`.
  Plain `cc.mov(gpq, word_ptr)` is not a valid x86 encoding — asmjit
  silently emitted a truncated op or dropped it, leaving the upper bits
  dirty for subsequent arithmetic.

- **`safemov(Mem, Gp)` size mismatch** (e8c3f0b) — now picks the `r8` /
  `r16` / `r32` / `r64` view based on the Mem's size, not the Gp's.
  Without this, writing a Gp64 (the widened member_lhs register) to a
  word-sized member emitted `mov word ptr, r64` which asmjit rejects,
  silently dropping the store.

- **Parser: comma peek-stop in conditional mode** (e8c3f0b) — a nested
  `parseExpression` called from the cast-body handler used to consume
  the `,` that terminates the outer function-call argument. So
  `strcpy((char *)h + 8, "x")` parsed as a one-arg call with `"x"`
  silently merged into the first arg's expression tree. Fixed by adding
  comma to the peek-stop set alongside `;`.

- **`TokenIF::compile` regdp reset** (e8c3f0b) — now zeros regdp before
  the condition, then branch, and else branch, matching what
  `TokenFOR` / `TokenWHILE` / `TokenDO` already do (per
  `.claude/rules/regdp-reset.md`).

### Tests

- `testincmember.mad` — prefix/postfix inc/dec on struct members
- `testunsignedcmp.mad` — unsigned comparisons inside if
- `testglobalptr.mad` — global pointer var read/assign
- `testsubtomember.mad` — `p->next = arr[i]` for NULL and non-NULL
- `testcastargcomma.mad` — cast+arith as call arg with commas
- `testcommaincrement.mad` — SMAUG's `for (...; ptr = ptr->next, c++)`
- `testpostdeclstr.mad` — `char *p; p = "literal";` and via struct member

## [v0.8.0] — 2026-04-17 — SMAUG Phase E Complete + Phase F Start

SMAUG Phase E finishes (C arrays, brace initializers, struct interop with `struct tm`/`timeval`/`fd_set`, end-to-end `select()`) and Phase F begins with the first `.c → .mad` port (`MadSMAUG/src/hashstr.mad`). Every language gap surfaced during the port has been fixed in madc proper: self-referencing structs, three-word compound types, multi-var decls, global fixed arrays, `stdin`/`stdout`/`stderr`, for-loop comma expressions, forward decl + definition, `__FILE__`/`__LINE__`, raw-pointer `ptr[i]` subscript, and more.

### Added — SMAUG 1.8 Source Port Begins (2026-04-17)

First .c → .mad port: `MadSMAUG/src/hashstr.mad` (copied from SMAUG 1.8's
`hashstr.c`). The bootstrap convention is an app-named top-level file
(`SMAUG.mad`) that `#include`s the ported sources in dependency order with
`main()` last; `bin/madc SMAUG.mad` compiles the whole tree.

Each language gap surfaced during the port was fixed in madc proper:

- **Self-referencing structs** (54087c2) — `struct X { struct X *next; ... };`
  works. `TokenSTRUCT::parse` pre-registers the tag in `struct_map` with an
  incomplete placeholder before entering the body-parsing loop, so field types
  like `struct X *` resolve the in-progress struct.

- **Three-word compound types** (54087c2) — `unsigned short int`, `signed long
  int`, `long long int`, `unsigned long long`, `signed long long` all
  produce the correct DataDef. Lexer reads up to two lookahead words and
  picks the longest match.

- **`void` as sole parameter** (54087c2) — `int f(void)` parses as zero-arg.

- **Global fixed-size arrays** (35c6bf0) — `struct X *arr[N]` at file scope
  now works. parseDeclaration allocates a `calloc`'d buffer when the decl is
  global/static; voperand loads the absolute address into the base-pointer Gp
  instead of stack-allocating. The cache-hit path skips `movreg` for fixed
  arrays (the pointer is a compile-time constant).

- **Multi-variable declarations** (35c6bf0) — `int a, b, c;` / `char *p, *q;` /
  `int x = 1, y = 2;`. After the first decl the parser pushes back a clone of
  the base type token so parseCompound naturally iterates and parseDeclaration
  recurses. Base type is preserved alongside the decorated decl_type so each
  var in `char *p, *q` gets its own independent pointer depth.

- **stdin / stdout / stderr** (35c6bf0) — lazy-registered in `<stdio.h>` as
  int64 globals whose backing slot holds `*dlsym("stderr")` etc. Reading
  `stderr` in madc loads libc's current FILE\* value.

- **`register` before struct / typedef** (35c6bf0) — `register struct X *p;`
  now compiles. Previously only primitive types after register were allowed.

- **Unary minus after a keyword** (35c6bf0) — `return -1;`, `if (-x > 0)` etc.
  `isPostfixPosition` / `isUnaryPosition` now treat keywords as unary-opening
  contexts instead of value-producing ones; the Neg→Sub conversion no longer
  misfires.

- **TokenRETURN multi-return detection tightened** (35c6bf0) — previously any
  non-`;` / non-symbol peek after parseExpression triggered the multi-return
  path, so `return X;` followed by `if (...)` / `<ident>()` / etc. misfired
  as multi. Keywords and type names are now also excluded.

- **For-loop comma expressions** (be6c359) — `for (a=0, b=1; cond; i++, j--)`
  parses and runs. `TokenFOR` gains `init_extras` / `incr_extras` vectors;
  the parser uses conditional `parseExpression` for init/cond/incr so `;`
  stays in the stream to gate the extras loops. Compile runs all extras in
  order after the main init and before the jmp-back-to-top.

- **Forward decl + definition param mismatch** (be6c359) — the definition
  pass was re-pushing every DataDef onto `func->parameters` (because
  `FuncDef::findParameter` compares against the DataDef name, not the param
  name), causing the ids[]-vs-parameters[] binding loop to overshoot. Track
  `func_already_declared` and skip the re-push on the 2nd pass.

- **Compile-time error handler robustness** (be6c359) — the
  `catch(const char*)` block crashed on NULL/dangling pointers in the
  exception value (ostream `<<` of NULL), masking the real compile error.
  Guarded.

### Added — Phase E Finish (2026-04-17)

- **`__FILE__` / `__LINE__`** (9e2d5ad) — lexer injects a quoted filename
  and current `source.line()` as a decimal integer. Works correctly inside
  `#define` bodies — each invocation captures the call site.

- **`cc.newXxx(name)` format-safety sweep** (c50acbe) — 32 direct call
  sites in compiler.cpp that passed user- or literal-derived names
  verbatim to asmjit's variadic register-naming API are now routed through
  a `"%s"` format. Variable names containing `%` (our `__literal__tab[%ld]
  = (%s, %ld)` style) previously crashed on the unmatched format spec.
  `DataDef::newreg` was fixed in an earlier commit; this extends the same
  pattern to the rest.

- **Struct interop for libc types + fd_set / select()** (2f08efd) —
  `struct tm` (56 bytes), `struct timeval` (16 bytes), `struct fd_set`
  (128 bytes) with glibc-x86-64-matching layouts. `FD_ZERO` / `FD_SET` /
  `FD_CLR` / `FD_ISSET` forward to `__madc_fd_*` helpers bundled into the
  madc binary (reachable via dlsym thanks to -rdynamic). `select()` works
  end-to-end with a real pipe.
  - Fixed latent `safemov(Gp, Mem)` bug: it used `cc.mov(r1, r2)` for all
    sizes, and asmjit resolves that by reading a full qword even when the
    Mem is 4/2/1 bytes. Reading an int32 struct member (e.g. `tm->
    tm_hour`) pulled 8 bytes starting at the field's offset, returning the
    adjacent field packed into the upper half. Now picks `movsxd` / `mov
    r32,mem` (implicit zero-extend) / `movsx` / `movzx` based on sizes.
  - Extended unary `&` to accept `&(name)` in addition to `&name`, so the
    macro-expanded `__madc_fd_set(fd, &(set))` parses.

- **Raw-pointer subscript `ptr[i]`** (189f4ae) — for `int *`, `char *`,
  `int32_t *`, etc. Computes `[ptr + i*sizeof(base)]` with SIB scaling by
  the pointed-to type's size. Unblocks C interop like `pipe(pfd); int rfd
  = pfd[0];` / `FD_SET(rfd, rfds);` / `select(rfd+1, &rfds, ...)`. Also
  fixed `safemov(Operand, Operand)` Gp←Mem path to forward to the size-
  aware typed overload instead of calling `cc.mov` directly.
  - Added `scripts/psed.sh` — python-backed literal-text patcher for
    multi-line edits where tab-sensitive Edit calls are brittle.

- **Multi-file project convention** (ac0cf4f) — README documents the
  app-named bootstrap file (e.g. `smaug.mad`) that `#include`s its sources
  in dependency order with `main()` last. No new tooling — the existing
  `#include` already resolves relative paths and handles nested includes.

### Added — C Arrays and Structs (SMAUG Phase E)

- **Chained `->` and `.` member access** (b3a9d9a) — `a->b->c`, `a->b.c`, `a.b.c` all
  compile. TokenMember's parent_expr path resolves intermediate pointers and struct
  addresses at codegen time; the dot handler now accepts TokenMember LHS in addition
  to TokenVar.

- **C fixed-size arrays — 1D** (fd98935) — `int arr[N]` allocates a stack slot and
  stores the LEA'd base pointer as the variable's operand. Subscript emits
  `[base + idx*elem_size]` via SIB. `TokenSubscript::compile` honors `regdp.first`
  when the caller supplies a destination register, so `int x = arr[i]` works as RHS.
  Supports int, int32_t, int16_t, char element types, plus char-array decay to
  `char *` for `printf "%s"`.

- **Multi-dimensional arrays** (26dca0e) — `int m[N][M]`, `int cube[N][M][K]`.
  `TokenSubscript` gains an `extra_indices` vector; chained `[i][j][k]` folds into
  a single linear offset `((i0*d1)+i1)*d2 + i2 + ...`.

- **Brace initializer lists for arrays** (a1774d0) — `int a[N] = { v0, v1, ... };`
  with explicit size, inferred size (`int a[] = {1,2,3}`), partial (rest zero-filled),
  and arbitrary expressions as values. Includes a parseExpression fix: the outer
  loop was consuming a trailing `}` past the final element; now treats `}` like `]`
  and breaks without consuming.

- **String-literal char-array init** (1bae4f4) — `char msg[] = "hello"` expands to
  a per-byte initializer list plus a null terminator (length = strlen + 1 for the
  inferred form; zero-padded for oversized explicit `char buf[20] = "hi"`).

- **`char *msg = "literal"`** (13bbc35) — routed through the same path as
  `char msg[] = "literal"` so the two forms produce identical internal storage
  (`ddCHAR` + `vfFIXEDARRAY` + inferred `dims`).

- **Struct initializer lists** (8df2dca) — `struct Foo x = { ... };` with scalars,
  pointers, char* (string_cstr coerces `std::string` literal → `const char *`), and
  `std::string` members (`string_assign` after the auto-construct in voperand).
  Partial inits zero-fill remaining numeric/pointer members.

- **Array-of-structs initializer** (e62d2e7) — `struct Entry tab[] = { {"a", 1},
  {"b", 2}, ... };`. New `TokenStructLit` AST node carries nested brace lists as
  array elements; `emit_struct_init` factored into a shared helper invoked at
  `base + i*struct_size`. `TokenSubscript` now returns a Gp pointer (LEA) when
  indexing into a struct-element array — no load — so `tab[i].name` reaches the
  member via `TokenMember`'s Gp-base dot-chain path (parser also accepts
  `TokenSubscript` as the LHS of `.`).

### Added — Ergonomics

- **Crash handler with backtrace** (308b622) — `SIGSEGV`, `SIGABRT`, `SIGFPE`,
  `SIGBUS`, `SIGILL` caught at startup. Writes signal name, fault address (for
  SEGV/BUS), and a `backtrace_symbols_fd` trace to stderr, then restores the
  default handler and re-raises so the shell sees the real exit status and core
  dumps still drop.

- **`str.length()` / `str.size()` methods** (f04b7b6) — `std::string` exposes its
  length via instance methods that wrap `madc_string_length`.

### Changed

- **Removed builtin `strlen`** (f04b7b6) — the pre-registered madc wrapper
  expected a `std::string *` argument and misfired on char arrays/pointers.
  `strlen(char *)` now resolves via dlsym fallback to libc, which is the natural
  type fit. A `madc::`-namespaced type-aware alternative could be added later if
  useful.

### Fixed

- **`DataDef::newreg` format-string safety** (e62d2e7) — asmjit's
  `newGpq/newXmm/newIntPtr` are variadic printf-style: they interpret the first
  `const char *` argument as a format. Variable names containing `%` (e.g. our
  `__literal__tab[%ld] = (%s, %ld)` string-literal variable names) crashed on the
  unmatched format spec, dereferencing garbage as a pointer. Fixed by passing
  `"%s"` as the format and the name as the argument. (Other direct callers of
  `cc.newIntPtr(name)` likely share the same latent bug; sweep is on TODO.)

- **`emit_struct_init` aliasing corruption** (e62d2e7) — the `std::string`→
  `const char *` coercion was reassigning through the `Operand &` returned by
  `inits[i]->compile(...)`. For global literal variables that reference aliases
  the cached entry in `operand_map`; subsequent uses of the same literal saw the
  already-coerced `char *` and ran string_cstr over it again, interpreting the
  char pointer as a `std::string *` and reading SSO bytes as a new "pointer"
  (e.g. `"alice"` becoming `0x6563696c61` → SEGV in printf). Fixed by using a
  local `Operand` for the effective value.

## [v0.7.0] — 2026-04-16 — SMAUG Phase D: va_list + For-Loop Fix

### Added — SMAUG Phase D: Variadic Functions

- **`va_list` / `<stdarg.h>` support** — Variadic functions with `...` syntax. Hidden
  `__va_args` parameter carries a packed `int64_t[]` buffer from call site to callee.
  `va_start` macro sets the pointer, `va_arg` is a compiler intrinsic that reads and
  advances, `va_end` is a no-op macro. Design avoids the System V `va_list` struct
  (impossible in asmjit Compiler mode due to virtual registers).

- **`vsprintf`/`vsnprintf`/`vfprintf` helpers** — Format-string-aware C functions
  (`__madc_vsprintf` etc.) compiled into the binary. Parse `%d`/`%s`/`%f`/etc. from
  the format string and call `sprintf` per-specifier with args from the packed buffer.
  Redirected via `#define vsprintf __madc_vsprintf` in embedded `<stdarg.h>`.

- **`-rdynamic` linker flag** — Exports binary symbols for `dlsym(RTLD_DEFAULT)`
  visibility, enabling JIT code to call built-in C helpers like `__madc_vsprintf`.

- **39 embedded headers** — Added `<stdarg.h>` (was 38).

### Fixed

- **For-loop increment parsing bug** — `for ( i = 0; i < N; i++ )` now works. All
  increment forms (`i++`, `i--`, `++i`, `--i`, `--c`) parse correctly. Root cause: the
  conditional peek-stop in `parseExpression` left the `;` separator in the token stream,
  so `TokenFOR::parse()` was passing `;` to `parseStatement` instead of the increment
  expression. Fixed by consuming the `;` separator explicitly before calling `parseStatement`.

## [v0.6.0] — 2026-04-16 — SMAUG Phase A/B/C: C Pointer System + Macros

### Added — C Pointer and Type System (Phase A — all 5 items complete)

- **`char *` pointer declarations** — `DataDefPTR` class tracks pointed-to type. Pointer
  handling in `parseDeclaration`, struct/class members, function parameters, and `sizeof()`.
  Supports `char *`, `int *`, `void *`, `struct *`, `char **` (double pointers).

- **`->` struct pointer member access** — `ptr->member` parsed in `parseExpression`,
  resolved via `DataDefPTR::base_type`. Reuses `TokenMember` Gp codegen path
  (`[gp + offset]`). Chained `->` supported (with temp variable for intermediate).

- **`(TYPE *)` cast expressions** — Detects casts by checking if `(` is followed by a
  type name. `TokenCast::compile()` passes through the value with target type annotation.
  Works with `(CHAR_DATA *)calloc(...)`, `(struct tag *)ptr`, `(int *)raw`.

- **`&` address-of operator** — `TokenAddrOf` emits LEA for stack variables. Unary
  detection via `isUnaryPosition()` helper. Works for struct variables passed to functions.

- **Forward typedef struct declarations** — `typedef struct tag_name ALIAS;` before the
  struct body exists. Creates placeholder `DataDefSTRUCT` (size 0), filled in-place when
  the full definition is encountered. The typedef alias automatically sees the completed type.

### Added — Macros (Phase B — all 3 items complete)

- **Function-like macros** — `#define NAME(params) body` with parameter substitution.
  Whole-word matching prevents substring replacement. Handles nested parens, string literals
  in arguments, and zero-parameter macros.

- **Multi-line `#define` with `\` continuation** — Both function-like and object-like macros
  support backslash line continuation.

- **`do { } while(0)` macro bodies** — Fixed do-while crash when multiple loops in sequence
  (regdp reset + TokenInt with NULL regdp.first). SMAUG's CREATE/DISPOSE macros now work.

### Added — Data Structures and Types (Phase C partial)

- **`unsigned`/`signed`/`long`/`short` compound type keywords** — Lexer handles compound
  specifiers: `unsigned char` → dtUINT8, `unsigned int` → dtUINT32, `long int` → dtINT64,
  `short int` → dtINT16, bare `unsigned` → dtUINT32, etc.

- **`enum` keyword** — `enum { NAME, NAME = val, ... }` with auto-incrementing values and
  explicit `= N` assignments. Each enumerator registered as a global constant variable.

- **`typedef` for primitive types** — `typedef int sh_int;`, `typedef unsigned char bool;`,
  `typedef char *LPSTR;`. Alias names can redefine existing type names.

- **`static` keyword** — Static local variables persist across function calls. Heap-allocated
  with `vfSTATIC` flag. Runtime initialization skipped (pre-initialized via allocation).

- **`const` keyword** — Consumed and passed through to type declaration.

- **`extern` keyword** — Consumed and skipped to semicolon.

- **`*ptr` dereference operator** — Read and write through pointer. `TokenDeref` returns
  Mem operand `[ptr_gp]` for numeric types. Works with heap pointers (calloc/malloc).

### Added — Infrastructure

- **`struct Type` in function parameters** — `parseFunction()` handles `struct Name` and
  typedef'd identifiers as parameter types (was only accepting `ttDataType` tokens).

- **Typedef'd types in struct member definitions** — `ROOM_DATA *in_room;` inside a struct
  body now works (identifier resolved against `datatype_map`).

- **`isUnaryPosition()` / `isPostfixPosition()` helpers** — Replaces duplicated prevToken
  checks for unary `&`, `*`, prefix/postfix `++`/`--`, and Neg→Sub conversion.

- **`resolveCompoundLHS()` helper** — All 10 compound assignment operators (`+=`, `|=`, etc.)
  now work on struct members via `->`, not just plain variables. Load-op-store pattern for
  Mem operands.

- **`make fulltest` target** — Runs unit tests + all integration tests in one command.

### Fixed

- **Ternary inside parentheses** — `int m = (a > b ? a : b)` now works. Was setting
  `done=true` unconditionally after ternary, preventing closing `)` from being consumed.
  Fix: only set `done` when `brackets == 0`.

- **Variadic argument promotion** — Sub-64-bit integer types (char, short, int32) now
  sign/zero-extended to 64-bit before passing to variadic functions (printf, etc.).

- **C string function redirect** — When a registered built-in (e.g. `strlen`) expects
  `std::string` but receives a `char *` pointer argument, redirects the call to the C
  library version via dlsym.

- **dlsym return to Mem operand** — Function returns assigned directly to `->` members
  no longer crash. Uses temp Gp register for `setRet` then writes to Mem.

- **Mem-to-Gp for variadic args** — Struct member access via `->` in variadic function
  arguments (printf) now loads Mem into temp Gp before passing.

- **Do-while regdp reset** — `TokenDO` and `TokenWHILE` compile() now reset regdp before
  body and condition sub-compilations. Also fixed `TokenInt::compile()` with NULL regdp.first.

---

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
