# The libc fallback signature — an undeclared C library call gets the wrong return type

**Status:** open (this session's task #2)
**Severity:** silent wrong answer. Exit 0, wrong value, legal C89 source.
**Layer:** `Program::dynamic_symbol_fallback_return_type()` (src/parser.cpp) — the
single owner of "what does an undeclared, dlsym-resolved symbol return".

## The defect, measured

Reducers in `tmp/libc/`, run on the v0.85.0 JIT (`bin/madc 0.85.0`), oracle `gcc -O0`.
Both `.c` and `.mad` spellings behave identically — the extension is not the variable.

### Class 1 — a function that really returns `int`, read as 64 bits

```c
int printf(const char *, ...);
int main(void) { printf("%d\n", strcmp("abc","abd") < 0 ? 1 : 0); return 0; }
```

| expression                    | gcc  | madc         |
|-------------------------------|------|--------------|
| `strcmp("abc","abd") < 0`     | 1    | **0**        |
| `strncmp("abc","abd",3) < 0`  | 1    | **0**        |
| `memcmp("abc","abd",3) < 0`   | 1    | **0**        |
| `(long long)strcmp("abc","abd")` | -1 | **4294967295** |

The branch is simply not taken. `strcmp` returns `int` in `eax`; a `long long`
prototype reads all of `rax`, so `-1` reads as `0x00000000ffffffff`.

### Class 2 — a function that really returns `double`, read from the wrong register

```c
double f = floor(2.7), c = ceil(2.1), e = exp(0.0), l = log(1.0);
```

| expression   | gcc | madc                     |
|--------------|-----|--------------------------|
| `floor(2.7)` | 2.0 | **1.0**                  |
| `ceil(2.1)`  | 3.0 | **1.0**                  |
| `exp(0.0)`   | 1.0 | **4294966327.0**         |
| `log(1.0)`   | 0.0 | **4607182418800017408.0** |

`log(1.0)`'s 4607182418800017408 is the IEEE bit pattern of `1.0` read as an
integer — the value arrived in `xmm0` and the `long long` prototype read `rax`.

**The mechanism, isolated.** Six libm calls in one program, no `<math.h>`:

| call         | registered in the builtin registry? | gcc   | madc      |
|--------------|-------------------------------------|-------|-----------|
| `sqrt(2.0)`  | yes                                 | 1.414 | 1.414 ✓   |
| `fabs(-2.5)` | yes                                 | 2.500 | 2.500 ✓   |
| `sin(0.0)`   | yes                                 | 0.000 | 0.000 ✓   |
| `tan(0.0)`   | no                                  | 0.000 | **1.414** |
| `atan(0.0)`  | no                                  | 0.000 | **2.500** |
| `log10(100.0)`| no                                 | 2.000 | **0.000** |

The three unregistered ones print the *previous* call's `xmm0`. Registration is
the whole difference; nothing else about the three pairs differs.

### What is NOT affected (why 1084 green tests never saw it)

- With the real header in scope (`#include <string.h>`, `<math.h>`) the
  declaration is correct and every case above passes. The embedded system-library
  shims are retired, so this is real glibc's prototype doing the work.
- `std::string`'s `<`, `==` and `.compare()` are correct: libstdc++ funnels
  `__builtin_memcmp` through an `int` of its own, and that truncation preserves
  the sign.

## Root cause

`dynamic_symbol_fallback_return_type()` returns `DataType::dtINT64` for every
name except two small sets (`asctime`/`ctime` → `char *`, and the
`abort`/`exit`/`free` family → `void`). Everything else — `int`, `double`,
`float`, `long double`, `size_t`, `char *`, `void *` — is declared `long long`.

Ten lines below it in the same fallback chain, the C89 implicit-declaration path
builds `FuncDef(ddINT32)` — **`int`, which is what C actually says**. The two
guesses contradict each other, and the dlsym guess wins whenever libc exports
the name (i.e. always, for a libc name).

### Correcting an earlier claim

The previous session's note said "all 68 `__builtin_` registrations are dead".
That overstated it. Only three registrations name a spelling the lexer rewrites:
`__builtin_memcpy`, `__builtin_memset` (→ `memcpy`/`memset`, so both dead) and
`__builtin_frame_address` (already repaired by registering the rewritten name).
The other 65 aliases never had a registration to be dead. The real gap is the
one above: the fallback's default, not a dead lookup key.

Alias inventory in `src/lexer.cpp`: 69 explicit `__builtin_X → Y` singles, plus
57 C99 math roots × {*, f, l} = 171 more. Of those 57 roots the builtin registry
covers 6 (`copysign`, `fabs`, `sqrt`, `sin`, `cos`, `pow`) — the six that were
each found and fixed individually, one bug report at a time.

## The fix

**Adopt C's own answer for the default, and make the exceptions a table.**

1. `dynamic_symbol_fallback_return_type()` defaults to `dtINT32` — C's
   implicit-declaration rule, and what the sibling C89 path already does. That
   one change makes the entire `int`-returning C library correct
   (`str*cmp`, `mem*cmp`, the `printf`/`scanf` family, `<ctype.h>`, `atoi`, …)
   with no per-name knowledge at all.
2. Everything whose return is **not** `int` comes from one authored table keyed
   by libc name: `void`, `char *`, `void *`, `long`, `unsigned long`/`size_t`,
   `long long`, `double`, `float`, `long double`. This is gcc's model
   (`builtins.def`) — a compiler is *supposed* to know libc's signatures.
3. The 57 math roots and their `f`/`l` suffixes are **not retyped**. The root
   list moves out of `src/lexer.cpp` into the shared owner, so the alias map and
   the signature table expand the same list. Two copies of that list is the
   divergence this repo gates against — five of the six hand-registered roots
   exist because the sixth copy was missing an entry.

### Why flipping the default is safe to attempt

A missed **pointer**-returner is loud, not silent: `int f()` assigned to a
pointer is exactly the c2mir "using integer without cast for pointer type"
warning, and the zero-warnings law fails the build on it. A missed
**long/size_t** returner is the residual silent risk, so those classes are
enumerated deliberately rather than left to the default.

### Gates

- `tests/testlibcnoheader.mad` — one representative per class called with **no
  header in scope**, values checked against the gcc oracle. This is the gate
  that measures behaviour rather than the table's spelling.
- `scripts/check-libc-alias-signatures.sh` (fulltest) — every name the lexer
  rewrites *to* must have an explicit entry in the signature table. Adding an
  alias without a signature fails the build. Carries a negative control.

## Deliberately not in scope

`is_implicit_complex_builtin_name()` / `make_implicit_complex_builtin_func()`
(src/parser.cpp:306-360) is the same concern — name → return type for an
undeclared libc function — solved separately for the 9 `conj`/`creal`/`cimag`
spellings by chained string compares. It also supplies an *argument* type and a
non-varargs arity, which the dlsym path does not, so it is a different mechanism
rather than a duplicate of this table. Folding the two is a follow-up; the
complex names stay excluded from this table so the existing path keeps
precedence.

---

## Part 2 — the ARGUMENT convention for the f / l math families

Found while testing Part 1, and a separate mechanism: Part 1 fixes what an
undeclared call *returns*; this is about what it *passes*.

### Measured (`tmp/libc/p8.c`, same JIT, gcc oracle)

| call                | gcc   | madc after Part 1 |
|---------------------|-------|-------------------|
| `floorf(3.9f)`      | 3.000 | **2.000**         |
| `floorf(x)`, x float| 3.000 | **2.000**         |
| `fmodf(7.0f,4.0f)`  | 3.000 | **-nan**          |
| `sqrtf(9.0f)`       | 3.000 | 3.000 ✓           |
| `sqrtl(2.0)`        | 1.414214 | 1.414214 ✓     |
| `ldexp(1.0,3)`      | 8.000 | 8.000 ✓           |

`sqrtf` is the tell: it is one of the six roots registered by hand, **with a
declared `float` parameter**. Every other root reaches the fallback, which
declares zero parameters — the variadic convention — so C's default argument
promotion turns the `float` into a `double` and the real `floorf` reads a
`float` out of the low half of a `double`. madc is obeying the variadic rule
correctly; the function simply is not variadic.

`sqrtl` passing a `double` where `long double` is expected happens to work and
is not understood. It is not evidence of anything and must not be read as
coverage.

### Fix

The math roots take nine shapes, so the shared table gains an argument-shape
column alongside the return class:

| shape        | roots                                                        |
|--------------|--------------------------------------------------------------|
| `(T)->T`     | the 1-argument majority                                      |
| `(T,T)->T`   | atan2 copysign fdim fmax fmin fmod hypot nextafter pow remainder |
| `(T,T,T)->T` | fma                                                          |
| `(T,int)->T` | ldexp scalbn                                                 |
| `(T,long)->T`| scalbln                                                      |
| `(T,T*)->T`  | modf                                                         |
| `(T,int*)->T`| frexp                                                        |
| `(T,T,int*)->T` | remquo                                                    |
| `(T,ldouble)->T` | nexttoward                                               |

`T` is the family's own real type — `double` for the bare root, `float` for the
`f` suffix, `long double` for the `l` suffix — which is the same suffix rule the
return class already uses, so the two cannot disagree about a family.
`ilogb`/`lrint`/`lround`/`llrint`/`llround` keep shape `(T)->T`'s *argument* and
their own integer return.

Gate: `tests/testlibcnoheaderargs.mad`, one call per shape at all three suffixes,
oracled against gcc.

## Noted, not changed

`populate_builtin_registry()` declares `puts` as returning **`void`**
(src/parser.cpp) where C says `int`. That is a *declared* signature, not a
fallback guess, so nothing in this arc reaches it — but `if (puts(s) < 0)` will
not compile. Changing a registered builtin's return type has its own blast
radius (every `puts(...)`-as-a-statement call site), so it is recorded here
rather than folded in.

The `locale_t` family (`newlocale`, `duplocale`, `uselocale`, and their
glibc `__`-twins) is in the table as of Part 2, but that did NOT silence the
pre-existing `c++locale.h` pack-drain line "using integer without cast for
pointer type parameter" (12 occurrences before and after — measured, not
assumed). That warning is about the ARGUMENT: `__convert_from_v` passes a
`__c_locale` (typedef `__locale_t`, a pointer) that is typed as an integer, so
the root is typedef resolution for `extern "C" __typeof(uselocale) __uselocale;`
(madc has no `typeof`), not the fallback signature. Separate defect, separate
layer; the table entries stay because they are true and cover the direct-call
case.
