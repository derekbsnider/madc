# SMAUG 1.8 Compatibility Requirements

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
<sys/ioctl.h>   ✅
```

Still needed:
```
<sys/file.h>    ❌  flock() and LOCK_* constants (minor)
```

All standard headers used by SMAUG are now embedded. `<stdarg.h>` was added in Phase D.

---

## Language Features Needed

### ✅ Already Working (as of v0.6.0 / v0.7.0)
- `char *` pointer declarations in variables, struct members, function parameters
- `->` struct pointer member access (single-level; chained via temp variable)
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
- Fixed-size arrays: `char buf[MAX_INPUT_LENGTH]`
- Array indexing and pointer subscript
- `NULL` as pointer assignment: `ch->next = NULL`

### ✅ Phase D Complete

- **`va_list` / varargs** — `va_start`/`va_arg`/`va_end`, packed `int64_t[]` buffer,
  `__madc_vsprintf`/`__madc_vsnprintf`/`__madc_vfprintf` helpers, `-rdynamic` flag

### ❌ Not Yet Implemented — Remaining Blockers

**1. Multi-dimensional arrays** — `int board[8][8]`, `char inbuf[MAX_INBUF_SIZE]`.
The 2D array case needs parser support.

**2. Static initializer tables** — `static struct { ... } table[] = { { "name", func }, ... };`
Used heavily in const.c and tables.c. Requires aggregate initialization syntax.

**3. `select()` with `fd_set`** — `fd_set` is a struct with a fixed array of longs.
The FD_SET/FD_ZERO/FD_ISSET operations are macros that manipulate bit arrays.

**4. Chained `->` without temp variable** — `ch->desc->character->name` works with
a temp variable but not as a single expression yet.

**5. String literals as array initializers** — `char buf[] = "hello"` style.

---

## C Language Gap Summary

| Gap | Severity | Status |
|-----|----------|--------|
| Function-like macros (`#define F(x) ...`) | BLOCKER | **DONE** (v0.6.0) |
| `char *` pointer declarations | BLOCKER | **DONE** (v0.6.0) |
| `va_list` / `<stdarg.h>` support | **BLOCKER** | **DONE** (v0.7.0) |
| `->` struct pointer member access | BLOCKER | **DONE** (v0.6.0) |
| Forward struct typedef declarations | HIGH | **DONE** (v0.6.0) |
| Explicit cast syntax `(TYPE *)expr` | HIGH | **DONE** (v0.6.0) |
| 2D arrays `int x[N][M]` | HIGH | Not started |
| Static initializer tables `= { {}, {} }` | HIGH | Not started |
| `&` address-of in expressions | HIGH | **DONE** (v0.6.0) |
| `fd_set` struct + FD_SET macros | HIGH | Not started |
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
