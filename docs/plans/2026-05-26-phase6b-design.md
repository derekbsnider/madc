# Phase 6B Design: madc Features — Exceptions, Namespaces, STL, Streams

**Date:** 2026-05-26
**Scope:** try/catch/throw, MadArray lifecycle, STL containers, stream fixes
**Baseline:** 344/475 transpiler tests passing (72.4%)
**Target:** ~365/475 (~77%)

## 1. try/catch/throw (10 tests)

The legacy runtime (`exception_runtime.cpp`) provides a complete SJLJ
exception model with these extern C functions already linked:

```
__madc_try_push(ctx)      — push try context, return jbuf pointer
__madc_try_pop()          — pop after normal exit
__madc_throw_int(val)     — throw int, unwind cleanups, longjmp
__madc_throw_double(val)  — throw double
__madc_throw_cstr(str)    — throw string
__madc_rethrow()          — rethrow current exception
__madc_exception_type()   — get exception type tag (1=int, 2=dbl, 3=cstr)
__madc_exception_int()    — get int value
__madc_exception_double() — get double value
__madc_exception_cstr()   — get string value
__madc_exception_clear()  — clear exception state
__madc_cleanup_push(...)  — push destructor entry
__madc_cleanup_pop()      — pop without calling
```

**Emitted C pattern for `try { body } catch (int e) { handler }`:**

```c
#include <setjmp.h>

// Preamble struct (matches exception_runtime.cpp layout):
struct __madc_try_ctx {
    jmp_buf jbuf;
    void *prev;
    void *cleanup_mark;
};

// In function body:
{
    struct __madc_try_ctx __try_ctx;
    __madc_try_push(&__try_ctx);
    if (setjmp(__try_ctx.jbuf) == 0) {
        // try body
        __madc_try_pop();
    } else {
        long e = __madc_exception_int();
        // catch body
        __madc_exception_clear();
    }
}
```

For multiple catch clauses, chain `if/else if` on `__madc_exception_type()`:
```c
} else {
    int __exc_type = __madc_exception_type();
    if (__exc_type == 1) {
        long e = __madc_exception_int();
        // int catch body
        __madc_exception_clear();
    } else if (__exc_type == 3) {
        const char *s = __madc_exception_cstr();
        // string catch body
        __madc_exception_clear();
    } else {
        // catch(...) body
        __madc_exception_clear();
    }
}
```

**throw:** Emit `__madc_throw_int(expr)`, `__madc_throw_cstr(expr)`,
or `__madc_rethrow()` based on expression type.

**Destructor cleanup inside try:** When objects with destructors are
constructed inside a try block, push cleanup entries so unwinding calls
dtors in LIFO order. For the transpiler, this means emitting
`__madc_cleanup_push()` after each object construction inside try.

## 2. MadArray/MadValue lifecycle (7 tests)

Namespace functions that return arrays (php::explode, perl::split, etc.)
use MadArray/MadValue internally. The transpiler needs to:

1. Declare array variables as `char arr[MADC_ARRAY_SIZE]` (stack buffer)
2. Call `madarray_construct(arr)` — placement-new
3. Track for LIFO destruction via `madarray_destruct(arr)`

**New extern C wrappers needed in `madc_mir_backend.cpp`:**
```c
void *madarray_construct(void *ptr);    // placement-new MadArray
void madarray_destruct(void *ptr);      // destructor
long madarray_size(void *ptr);          // size()
```

Check `sizeof(MadArray)` for the size constant. The existing namespace
wrappers (`__php_explode`, `__perl_split`, etc.) already take/return
`void*` for arrays — they just need properly constructed objects.

## 3. STL containers (2 tests)

Same pattern as fstream — method calls lowered to extern C wrappers.
The legacy `ns_stl.cpp` has typed helpers. Add extern C wrappers:

```c
// map<string,int>
void *madc_map_construct(void *ptr);
void madc_map_destruct(void *ptr);
void madc_map_put(void *ptr, void *key, long val);
long madc_map_get(void *ptr, void *key);

// set<string>
void *madc_set_construct(void *ptr);
void madc_set_destruct(void *ptr);
void madc_set_insert(void *ptr, void *key);
long madc_set_has(void *ptr, void *key);
```

Emitter recognizes `map`/`set` typed variables and lowers `.put()`,
`.get()`, `.insert()`, `.has()` to wrapper calls.

## 4. Quick fixes (4 tests)

- **testfstream** — `stoi` not registered. Add `__std_stoi` to runtime
  (already added in Phase 4 — verify it's exported).
- **testsstream** — `printstream` not registered. This is a madc
  convenience function — add extern C wrapper.
- **testctorstring** — string member in cout prints pointer not value.
  Emit `string_cstr()` when cout-ing a class member that's a string.
- **testns** — `cout << expr` not lowered for this test. Extend
  stream detection to catch the missed pattern.

## Deferred (2 tests)

- **testmadc_ns** — namespace::function call syntax not in Gecko grammar
- **testrustmatch** — Rust-style match blocks not in Gecko grammar

## 5. Computed object sizes (all phases benefit)

Replace hardcoded `#define MADC_STRING_SIZE 32` etc. with sizes
computed at runtime from the madc binary (which is C++ and knows
`sizeof(std::string)`). The emitter calls `sizeof()` directly:

```cpp
// In madc_emit_c.cpp emit() method:
char buf[128];
snprintf(buf, sizeof(buf), "#define MADC_STRING_SIZE %zu\n", sizeof(std::string));
header += buf;
```

Same for MADC_OFSTREAM_SIZE, MADC_IFSTREAM_SIZE, MADC_FSTREAM_SIZE,
MADC_SSTREAM_SIZE, and any new container sizes. Add static_assert
guards in `madc_mir_backend.cpp` to catch ABI surprises.

This makes the transpiler portable across libstdc++ vs libc++,
GCC vs Clang, and different OS targets.

## Files modified

| File | Change |
|------|--------|
| `src/madc_emit_c.cpp` | try/catch/throw emission, MadArray/STL decl+method dispatch, cout fixes |
| `src/madc_mir_backend.cpp` | MadArray/STL extern C wrappers, printstream, verify stoi |
| `src/madc_grammar.cpp` | (minimal — only if try/catch grammar needs adjustment) |
