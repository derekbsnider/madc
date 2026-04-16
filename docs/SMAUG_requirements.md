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
<stdarg.h>      ❌  <limits.h>      ✅  <errno.h>       ✅
<ctype.h>       ✅  <signal.h>      ✅  <time.h>        ✅
<fcntl.h>       ✅  <unistd.h>      ✅  <sys/types.h>   ✅
<sys/stat.h>    ✅  <sys/time.h>    ✅  <sys/socket.h>  ✅
<netinet/in.h>  ✅  <arpa/inet.h>   ✅  <netdb.h>       ✅
<sys/ioctl.h>   ✅
```

Still needed:
```
<stdarg.h>      ❌  va_list, va_start, va_end, va_arg  (BLOCKER — used heavily)
<sys/file.h>    ❌  flock() and LOCK_* constants (minor)
```

`<stdarg.h>` is the only missing standard header that is actually used in the source.
Functions like `vsprintf(buf, fmt, args)` depend on it.

---

## Language Features Needed

### ✅ Already Working
- Structs, nested structs, struct pointers
- Function pointers (`DO_FUN*`, `SPEC_FUN*`, `SPELL_FUN*`)
- Typedefs (`sh_int`, `bool`, `ch_ret`, etc.)
- Pointer arithmetic and casts
- `sizeof()`
- `#define`, `#ifdef`/`#ifndef`, `#if`, `#else`, `#endif`
- `#define IS_SET(flag, bit) ((flag) & (bit))` — simple expression macros
- Bitwise ops: `&`, `|`, `~`, `^`, `<<`, `>>`
- All arithmetic and comparison operators
- `if`/`else`, `for`, `while`, `do`/`while`, `switch`/`case`
- `break`, `continue`, `return`
- `calloc`/`free` (via dlsym)
- `socket`/`bind`/`listen`/`accept`/`send`/`recv`/`select` (via dlsym)
- `signal`/`gettimeofday`/`perror` (via dlsym)
- String functions: `sprintf`/`vsprintf`/`strlen`/`strcpy`/`strcmp` (via dlsym)
- Fixed-size arrays: `char buf[MAX_INPUT_LENGTH]`
- Array indexing and pointer subscript

### ❌ Not Yet Implemented — Blockers

**1. `va_list` / varargs** — SMAUG uses `va_start`/`vsprintf` in ~6 files
for `ch_printf`, `log_string`, `send_to_char` etc. Without this, all formatted
output functions fail.
- Need: `<stdarg.h>` embedded header, and compiler support for va_list passing
- Implementation path: inject `va_list` as a struct type alias, emit asmjit code
  to build argument list, call `vsprintf` via dlsym. OR: translate vararg function
  bodies to call `vsprintf` directly.

**2. Function-like macros** — SMAUG uses `#define KEY(literal, field, value) \` style
multi-line macros with `if`, `break`, assignments. Also:
```c
#define CREATE(result, type, number)  \
do { if (!((result) = (type *) calloc(...))) { ... } } while(0)
#define DISPOSE(point)  free(point); (point) = NULL;
```
These are in the critical hot path for all memory allocation. Without function-like
macros, every CREATE/DISPOSE call fails to parse.
- This is HIGH PRIORITY — it's on the existing TODO and would unlock many MUDs.

**3. `typedef struct` and forward declarations** — SMAUG does:
```c
typedef struct char_data CHAR_DATA;
// ... later ...
struct char_data { ... };
```
Forward declarations of typedef'd structs. Parser needs to handle this.

**4. `char *` pointer declarations** — Currently `char *str = "hello"` doesn't parse
because `char` + `*` conflicts with multiply. This is on the known issues list.
SMAUG uses `char *` extensively for all string fields.

**5. Multi-dimensional arrays** — `int board[8][8]`, `char inbuf[MAX_INBUF_SIZE]`,
`int skills[MAX_SKILL]`. The 2D array case needs parser support.

**6. `void *` generic pointer casting** — SMAUG casts `void *` to struct pointers
everywhere: `(CHAR_DATA *) malloc(...)`. Need explicit cast expression support.

**7. `NULL` as pointer** — Used everywhere. Already `#define NULL 0` in stdio.h,
but cast to pointer types must work: `ch->next = NULL`.

**8. Nested struct member chains** — `ch->desc->character->name` style multi-level
pointer dereferences. Need `->` operator on pointers to structs.

**9. Initializer lists / struct initialization** — `struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));`
and also: `static struct { ... } table[] = { { "name", func }, ... };`
Static initializer tables used heavily in const.c and tables.c.

**10. `perror` / `fprintf(stderr, ...)` / `exit()` calls** — All via dlsym, should work.

**11. `select()` with `fd_set`** — `fd_set` is a struct with a fixed array of longs.
The FD_SET/FD_ZERO/FD_ISSET operations are macros that manipulate bit arrays.
Either need function-like macros, or built-in fd_set support.

**12. String literals as array initializers** — `char buf[] = "hello"` style.

---

## C Language Gap Summary

| Gap | Severity | Files Affected |
|-----|----------|----------------|
| Function-like macros (`#define F(x) ...`) | **BLOCKER** | ALL (CREATE, DISPOSE, KEY...) |
| `char *` pointer declarations | **BLOCKER** | ALL |
| `va_list` / `<stdarg.h>` support | **BLOCKER** | comm.c, db.c, misc.c, imc-util.c |
| `->` struct pointer member access | **BLOCKER** | ALL |
| Forward struct typedef declarations | HIGH | mud.h + all .c files |
| Explicit cast syntax `(TYPE *)expr` | HIGH | ALL |
| 2D arrays `int x[N][M]` | HIGH | chess.c, save.c, etc. |
| Static initializer tables `= { {}, {} }` | HIGH | const.c, tables.c |
| `&` address-of in expressions | HIGH | ALL (malloc/calloc returns) |
| `fd_set` struct + FD_SET macros | HIGH | comm.c |
| `sizeof(struct name)` | MEDIUM | handler.c, db.c |
| `do { ... } while(0)` macros | MEDIUM | CREATE, DISPOSE |
| Ternary in struct member context | MEDIUM | various |
| Multi-line `#define` with `\` continuation | MEDIUM | mud.h macros |
| `unsigned char` / `unsigned int` | MEDIUM | bool typedef |
| `long int` / `short int` | LOW | pc_data, various |
| Negative array index `RUSAGE_CHILDREN = -1` | LOW | already fixed |

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
