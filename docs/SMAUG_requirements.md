# SMAUG 1.8 Compatibility Requirements

> **Note:** This document records the C-language gap analysis that
> drove madc's Phase A–F work. The actual SMAUG 1.8 port now lives in
> a separate repository — **[MadSMAUG](https://github.com/derekbsnider/MadSMAUG)**
> — because the SMAUG / Merc / DikuMUD license stack is distinct from
> madc's own MPL 2.0 licence and should not be mingled.
>
> madc continues to treat SMAUG as a motivating use case for "a
> realistic C89 codebase that madc must be able to compile." When
> porting surfaces a compiler gap, the fix lands here along with a
> *minimal* test case (pure madc, no SMAUG-derived code). The full
> port progress and per-file status are tracked in MadSMAUG's
> `docs/port-progress.md`.

**Goal:** Run SMAUG 1.8 MUD source (smaug.tgz from ftp.smaug.org) in madc without needing gcc.

SMAUG 1.8 is ~158,000 lines of C across ~70 .c files. It is clean C89/C90 — no C99, no
GCC extensions (one `__attribute__((format))` that is `#define`'d away). This is a realistic
target.

---

## System Headers Required

All already embedded in madc:
```
<stdio.h>       ✅  <stdlib.h>      ✅  <string.h>      ✅
<stdarg.h>      ✅  <limits.h>      ✅  <errno.h>       ✅
<ctype.h>       ✅  <signal.h>      ✅  <time.h>        ✅
<fcntl.h>       ✅  <unistd.h>      ✅  <sys/types.h>   ✅
<sys/stat.h>    ✅  <sys/time.h>    ✅  <sys/socket.h>  ✅
<netinet/in.h>  ✅  <arpa/inet.h>   ✅  <netdb.h>       ✅
<sys/ioctl.h>   ✅  <sys/file.h>    ✅
```

All standard headers used by SMAUG are now embedded. `<stdarg.h>` was added in Phase D.

---

## Language Features Needed

### ✅ Already Working (as of v0.6.0 / v0.7.0 / Phase E in progress)
- `char *` pointer declarations in variables, struct members, function parameters
- `->` struct pointer member access — single-level AND chained (`ch->in_room->name`)
- `.` member access on struct values and via dot-chain (`a.b.c`, `a->b.c`)
- `.` member access through array subscripts (`tab[i].name`)
- `(TYPE *)` explicit cast expressions: `(CHAR_DATA *) calloc(...)`
- `&` address-of operator for struct variables
- `*ptr` dereference for heap pointers
- Forward typedef struct declarations: `typedef struct char_data CHAR_DATA;`
- Function-like macros: `#define CREATE(result, type, number)` with `\` continuation
- `do { ... } while(0)` macro bodies (CREATE/DISPOSE pattern)
- `enum` keyword with auto-incrementing constants
- `static`, `const`, `extern` keywords
- `typedef` for primitive types: `typedef short int sh_int; typedef unsigned char bool;`
- `unsigned char`, `unsigned int`, `unsigned short`, `unsigned long`, `long int`, `short int`
- Compound assignments on struct members: `SET_BIT(ch->act, BV00)` via `|=` on `->` members
- Structs, nested structs, struct pointers
- Function pointers (`DO_FUN*`, `SPEC_FUN*`, `SPELL_FUN*`)
- `sizeof()` including `sizeof(type *)` and `sizeof(struct name)`
- `#define`, `#ifdef`/`#ifndef`/`#if`/`#else`/`#endif`, `#if defined(X)`
- `#define IS_SET(flag, bit)`, `SET_BIT`, `REMOVE_BIT`, `MAX`, `MIN` macros
- Bitwise ops: `&`, `|`, `~`, `^`, `<<`, `>>`
- All arithmetic and comparison operators
- `if`/`else`, `for`, `while`, `do`/`while`, `switch`/`case`
- `break`, `continue`, `return`
- `calloc`/`free`/`malloc`/`realloc` (via dlsym)
- `socket`/`bind`/`listen`/`accept`/`send`/`recv` (via dlsym)
- `signal`/`gettimeofday`/`perror` (via dlsym)
- String functions: `sprintf`/`strlen`/`strcpy`/`strcmp`/`strdup`/`strcat` (via dlsym)
- File I/O: `fopen`/`fclose`/`fprintf`/`fgets` (via dlsym)
- C fixed-size arrays (1D and multi-dim): `int arr[N]`, `int board[8][8]`,
  `char inbuf[MAX_INBUF_SIZE]`, `int cube[N][M][K]`
- Array indexing and pointer subscript
- Brace initializer lists: `int a[] = {1,2,3}`, explicit size, inferred size, partial
  with zero-fill, arbitrary expressions
- String-literal init for char arrays: `char msg[] = "hello"`, `char buf[N] = "hi"`
- `char *name = "literal"` — sugar for `char name[] = "literal"` (same storage)
- Struct initializer lists: `struct Foo v = { s0, "str", 42 }` (scalars, pointers,
  char*, std::string)
- Array-of-structs initializers: `struct Entry tab[] = {{"a", 1}, {"b", 2}, ...}`
- `NULL` as pointer assignment: `ch->next = NULL`
- `str.length()` / `str.size()` methods on std::string
- Crash handler with backtrace on fatal signals (dev ergonomics)
- `__FILE__` / `__LINE__` predefined macros
- Self-referencing structs: `struct X { struct X *next; ... };`
- Three-word compound types: `unsigned short int`, `long long int`, etc.
- `void` as sole parameter: `int f(void)`
- Global fixed-size arrays: `struct X *arr[N]` at file scope
- Multi-variable declarations: `int a, b, c;`, `char *p, *q;`, `int x = 1, y = 2;`
- `stdin`, `stdout`, `stderr` (via `#include <stdio.h>`, dlsym-resolved from libc)
- `register` before struct / typedef / primitive
- Unary minus after keywords: `return -1;`, `if (-x > 0)`
- For-loop comma expressions: `for (a=0, b=1; cond; i++, j--)`
- Forward function declaration followed by definition (same signature)
- Raw-pointer subscript `ptr[i]` for any base type
- `struct tm` (from `<time.h>`) with glibc-matching layout; `localtime`/`gmtime`/
  `strftime`/`timegm` interop works bidirectionally
- `struct timeval` (from `<sys/time.h>`); `gettimeofday` works
- `struct fd_set` (from `<sys/select.h>`) + `FD_ZERO`/`FD_SET`/`FD_CLR`/`FD_ISSET`
  macros, with `select()` working end-to-end against real pipes
- Prefix/postfix `++`/`--` on struct members (via `->` and `.`) and `*deref`
- `for (...; ...; ptr = ptr->next, c++)` — compound-comma increment
- `char *p; p = "literal";` — post-declaration string-literal assignment
  (and `ptr->name = "literal";` via struct member)
- Unsigned `<` / `<=` / `>` / `>=` comparisons use `setb`/`setbe`/`seta`/
  `setae` when either operand is unsigned (SMAUG's `if (ptr->links < 65535)
  ++ptr->links;` pattern)
- Global pointer variables (`struct X *gp = NULL;`) — read and assign via
  DataDefPTR qword overrides
- `p->next = arr[i];` — subscript result written into a struct member
- `fcntl(fd, F_SETFL, O_NONBLOCK)` — embedded `<fcntl.h>` now covers the
  full `F_*` command set (`F_DUPFD`, `F_GETFD`, `F_SETFD`, `F_GETFL`,
  `F_SETFL`, `F_GETLK`, `F_SETLK`, `F_SETLKW`, `F_SETOWN`, `F_GETOWN`,
  `F_DUPFD_CLOEXEC`) plus `FD_CLOEXEC`, `O_NDELAY`/`O_ASYNC`/
  `O_DIRECTORY`/`O_NOFOLLOW` at Linux x86-64 values
- `struct servent` from `<netdb.h>` — 32-byte glibc layout
  (`char *s_name; char **s_aliases; int s_port; char *s_proto;`) so
  `getservbyname()` / `getservbyport()` return values expose `->s_port`
  etc. (use `ntohs()` to convert to host-order integer)
- Socket/network errno values — `EINPROGRESS`, `ECONNREFUSED`,
  `EWOULDBLOCK`, `EADDRINUSE`, `ETIMEDOUT`, etc. (full glibc set, see
  `include/madc/errno.h`)

### ✅ Phase D Complete

- **`va_list` / varargs** — `va_start`/`va_arg`/`va_end`, packed `int64_t[]` buffer,
  `__madc_vsprintf`/`__madc_vsnprintf`/`__madc_vfprintf` helpers, `-rdynamic` flag

### 🚧 Remaining compiler gaps that affect SMAUG-class C code

**1. Error diagnostics with file:line** (MEDIUM) — compile-time errors
don't uniformly carry file:line, making large-file ports harder to
debug.

**2. String multi-return types** (MEDIUM) — `return s, n;` for mixed
string + int returns. Numeric-only works.

**3. `ruby::chars` runtime MadArray crash** (MEDIUM) — separate
runtime bug unrelated to compiler gaps, tracked in TODO.md under
Known Runtime Bugs. Does not block SMAUG porting since SMAUG does
not use the Ruby namespace.

**4. Per-file port progress** is tracked in the external MadSMAUG repo —
see `docs/port-progress.md` there. As the port advances, expect more gaps
to surface; each one lands here along with a minimal test.

---

## C Language Gap Summary

| Gap | Severity | Status |
|-----|----------|--------|
| Function-like macros (`#define F(x) ...`) | BLOCKER | **DONE** (v0.6.0) |
| `char *` pointer declarations | BLOCKER | **DONE** (v0.6.0) |
| `va_list` / `<stdarg.h>` support | **BLOCKER** | **DONE** (v0.7.0) |
| `->` struct pointer member access | BLOCKER | **DONE** (v0.6.0) |
| Chained `->` / `.` member access | HIGH | **DONE** (Phase E) |
| Forward struct typedef declarations | HIGH | **DONE** (v0.6.0) |
| Explicit cast syntax `(TYPE *)expr` | HIGH | **DONE** (v0.6.0) |
| 2D arrays `int x[N][M]` | HIGH | **DONE** (Phase E) |
| Static initializer tables `= { {}, {} }` | HIGH | **DONE** (Phase E) |
| String-literal char-array init `char s[] = "..."` | HIGH | **DONE** (Phase E) |
| Array-of-structs `tab[] = {{...}, {...}}` | HIGH | **DONE** (Phase E) |
| `&` address-of in expressions | HIGH | **DONE** (v0.6.0) |
| `fd_set` struct + FD_SET macros | HIGH | **DONE** (Phase E) |
| `struct tm` + `strftime` / `localtime` | HIGH | **DONE** (Phase E) |
| `struct timeval` + `gettimeofday` | HIGH | **DONE** (Phase E) |
| `select()` end-to-end | HIGH | **DONE** (Phase E) |
| Self-referencing structs | HIGH | **DONE** (Phase F) |
| Multi-variable declarations | HIGH | **DONE** (Phase F) |
| Global fixed-size arrays | HIGH | **DONE** (Phase F) |
| Raw-pointer subscript `ptr[i]` | HIGH | **DONE** (Phase E) |
| For-loop comma expressions | HIGH | **DONE** (Phase F) |
| `stdin` / `stdout` / `stderr` | HIGH | **DONE** (Phase F) |
| Forward function decl → definition | HIGH | **DONE** (Phase F) |
| `register` before non-primitive | MEDIUM | **DONE** (Phase F) |
| Unary `-` after keyword | MEDIUM | **DONE** (Phase F) |
| `__FILE__` / `__LINE__` | MEDIUM | **DONE** (Phase E) |
| `void` as sole parameter | MEDIUM | **DONE** (Phase F) |
| Three-word compound types | LOW | **DONE** (Phase F) |
| `char *p; p = "lit";` post-decl assign | HIGH | **DONE** (Phase F) |
| `c++` in comma-increment contexts | HIGH | **DONE** (Phase F) |
| Inc/dec on struct members (`++ptr->f`) | HIGH | **DONE** (Phase F) |
| Global pointer variable read/write | HIGH | **DONE** (Phase F) |
| Unsigned comparison ops (setb/seta) | HIGH | **DONE** (Phase F) |
| Subscript → member assign (`p->n = arr[i]`) | HIGH | **DONE** (Phase F) |
| Cast+arith as call arg (`f((char *)h+8, x)`) | HIGH | **DONE** (Phase F) |
| `struct stat` + `timespec` | MEDIUM | **DONE** (Phase F) |
| `struct sockaddr` + `sockaddr_in` + `in_addr` | MEDIUM | **DONE** (Phase F) |
| `struct dirent` + `d_name[256]` | MEDIUM | **DONE** (Phase F) |
| Fixed-size array members in struct bodies | MEDIUM | **DONE** (Phase F) |
| Unary `&` / `*` after a cast | MEDIUM | **DONE** (Phase F) |
| Function pointer typedefs | MEDIUM | **DONE** (Phase F) |
| Function-to-pointer decay (value contexts) | MEDIUM | **DONE** (Phase F) |
| SMAUG command-table pattern (struct + fptr init) | HIGH | **DONE** (Phase F) |
| Direct struct-member fn-pointer invocation | MEDIUM | **DONE** (Phase F) |
| Global fn-pointer init at file scope | MEDIUM | **DONE** (Phase F) |
| Struct-member fn-pointer reassignment | LOW | **DONE** (Phase F) |
| Address-taken stack locals (`int *p = &n;`) | MEDIUM | **DONE** (Phase F) |
| Assign-in-condition (`while ((p = f()) != NULL)`) | MEDIUM | **DONE** (Phase F) |
| `sizeof(object)` on variables / fixed arrays | LOW | **DONE** (Phase F) |
| printf `%f`/`%e`/`%g` doubles verified | LOW | **DONE** (Phase F) |
| `->` in varargs arg list | LOW | **DONE** (Phase F) |
| chained `cout << char*` | LOW | **DONE** (Phase F) |
| `fcntl` command constants (`F_SETFL` etc.) | HIGH | **DONE** (Phase F) |
| `struct servent` + `getservbyname` | HIGH | **DONE** (Phase F) |
| Socket/network errno constants (`EINPROGRESS` etc.) | HIGH | **DONE** (Phase F) |
| Parser SIGSEGV in SMAUG `ident.c:268` `connect(...) && errno != EINPROGRESS` | HIGH | **TODO** (Phase F front edge) |
| `sizeof(struct name)` | MEDIUM | **DONE** (v0.5.0) |
| `do { ... } while(0)` macros | MEDIUM | **DONE** (v0.6.0) |
| Ternary in struct member context | MEDIUM | **DONE** (v0.6.0) |
| Multi-line `#define` with `\` continuation | MEDIUM | **DONE** (v0.6.0) |
| `unsigned char` / `unsigned int` | MEDIUM | **DONE** (v0.6.0) |
| `long int` / `short int` | LOW | **DONE** (v0.6.0) |

---

## What SMAUG Does NOT Use

- C++ features (pure C89)
- `__attribute__` (commented out / `#define`'d away)
- `inline` keyword
- VLAs (variable-length arrays)
- Designated initializers (`{.field = value}`)
- `fork()` (single-process, event-driven)
- `setjmp`/`longjmp`
- threads/pthreads
- `mmap`

---

## Implementation Roadmap Toward SMAUG

### Phase A — C pointer and type system (enables most of SMAUG to parse)
1. `char *` declarations (fix `*` ambiguity in parseDeclaration)
2. `->` member-of-pointer operator
3. `(TYPE *)` explicit cast expressions
4. `&` address-of operator in expressions
5. Forward typedef struct declarations

### Phase B — Macros (unlocks CREATE/DISPOSE/KEY and all utility macros)
6. Function-like macros: `#define NAME(args) body`
7. Multi-line `\` continuation in `#define`
8. `do { ... } while(0)` macro bodies

### Phase C — Data structures
9. 2D arrays `int x[N][M]`
10. Static initializer tables `= { {...}, {...} }`
11. `unsigned char` / `unsigned int` / `unsigned short` type keywords
12. `long int` / `short int` explicit type combos

### Phase D — I/O infrastructure
13. `<stdarg.h>` — `va_list`, `va_start`, `va_end`, `vsprintf`/`vsnprintf`
14. `fd_set` as a built-in struct (or via struct interop)
15. `FD_ZERO`, `FD_SET`, `FD_ISSET` as function-like macros or built-ins

### Phase E — Full SMAUG boot
16. `select()` wired up end-to-end
17. `gettimeofday()` returning a `struct timeval` (needs struct interop)
18. File I/O: `fopen`/`fclose`/`fread`/`fwrite`/`fscanf`/`fgets` via dlsym

---

## Quick Wins First

The single highest-impact items for general C compatibility (not just SMAUG):

1. **`char *` declarations** — fixes a glaring gap, affects every C program
2. **`->` operator** — required for any linked-list / struct-pointer code
3. **Function-like macros** — already on TODO, unlocks enormous range of C code
4. **`(TYPE *)` casts** — needed for any malloc-based code
5. **`&` address-of** — needed for `&sa` style calls to socket functions

These five items would get madc from "toy language" to "can compile real C programs."
