# Phase 6B: madc Features — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement try/catch/throw exception handling, MadArray lifecycle for namespace functions, computed object sizes, and stream/string quick fixes. Target: ~365/475 (77%).

**Architecture:** Exceptions use the existing SJLJ runtime (`exception_runtime.cpp`) — the emitter just generates C11 that calls `__madc_try_push`, `setjmp`, `__madc_throw_*`, etc. MadArray gets placement-new wrappers like strings/fstreams. Object size `#define`s are computed from `sizeof()` at build time. All subagent tasks use the opus model.

**Tech Stack:** C++11, setjmp/longjmp, Gecko GLR, c2mir/MIR

---

### Task 1: Computed object sizes (replaces hardcoded #defines)

**Files:**
- Modify: `src/madc_emit_c.cpp` (preamble generation)

Replace hardcoded `#define MADC_STRING_SIZE 32` etc. with values computed from C++ `sizeof()` at runtime. The emitter runs inside the madc binary (which is C++), so it can query real sizes.

- [ ] **Step 1: Find where size defines are emitted**

Search `src/madc_emit_c.cpp` for `MADC_STRING_SIZE` and `MADC_OFSTREAM_SIZE`. Find the preamble generation code (look for `header +=` near the bottom of the `emit()` method).

- [ ] **Step 2: Replace hardcoded values with computed sizes**

Change the preamble generation from:
```cpp
header += "#define MADC_STRING_SIZE 32\n";
header += "#define MADC_OFSTREAM_SIZE 512\n";
```
To:
```cpp
char __sz[256];
snprintf(__sz, sizeof(__sz),
    "#define MADC_STRING_SIZE %zu\n"
    "#define MADC_OFSTREAM_SIZE %zu\n"
    "#define MADC_IFSTREAM_SIZE %zu\n"
    "#define MADC_FSTREAM_SIZE %zu\n"
    "#define MADC_SSTREAM_SIZE %zu\n",
    sizeof(std::string),
    sizeof(std::ofstream),
    sizeof(std::ifstream),
    sizeof(std::fstream),
    sizeof(std::stringstream));
header += __sz;
```

Also add MadArray size for Task 3:
```cpp
snprintf(__sz, sizeof(__sz),
    "#define MADC_ARRAY_SIZE %zu\n",
    sizeof(MadArray));
header += __sz;
```

This requires `#include "datadef.h"` which is already included.

- [ ] **Step 3: Build and verify**

Run: `make -C src`
Run: `bin/madc --emit-c tests/testhello.mad 2>&1 | grep MADC_`
Expected: `#define MADC_STRING_SIZE 32` (or whatever the actual sizeof is)

Run: `make -C src fulltest`
Run: `bash scripts/run_tests.sh --backend=mir 2>&1 | tail -1`
Expected: no regressions.

- [ ] **Step 4: Commit**

```
feat: compute MADC_*_SIZE defines from sizeof() — portable across ABIs
```

---

### Task 2: try/catch/throw emission (10 tests)

**Files:**
- Modify: `src/madc_emit_c.cpp` (try/catch/throw statement handlers)

This is the largest task. The exception runtime already exists and is
linked — the emitter just needs to generate C11 that calls it.

**Exception runtime functions (already extern C, already linked):**
```
__madc_try_push(ctx)       — push try context
__madc_try_pop()           — pop after normal exit
__madc_throw_int(val)      — throw int
__madc_throw_cstr(str)     — throw string
__madc_rethrow()           — rethrow
__madc_exception_type()    — get type tag (1=int, 2=dbl, 3=cstr)
__madc_exception_int()     — get int value
__madc_exception_cstr()    — get string value
__madc_exception_clear()   — clear after catch
```

- [ ] **Step 1: Read how try/catch/throw are parsed by Gecko**

Search `src/madc_anode.h` for `AN_TRY`, `AN_CATCH`, `AN_THROW`.
Search `src/madc_emit_c.cpp` for `AN_TRY` to see if there's stub handling.
Read `tests/testexcept.mad` to understand the basic test case.

- [ ] **Step 2: Add exception runtime declarations to preamble**

In the emitter's preamble generation, add:
```c
#include <setjmp.h>

struct __madc_try_ctx {
    jmp_buf jbuf;
    void *prev;
    void *cleanup_mark;
};

extern void *__madc_try_push(void *ctx);
extern void __madc_try_pop(void);
extern void __madc_throw_int(long val);
extern void __madc_throw_double(double val);
extern void __madc_throw_cstr(const char *val);
extern void __madc_rethrow(void);
extern int __madc_exception_type(void);
extern long __madc_exception_int(void);
extern double __madc_exception_double(void);
extern const char *__madc_exception_cstr(void);
extern void __madc_exception_clear(void);
```

- [ ] **Step 3: Implement AN_TRY emission**

When the emitter encounters `AN_TRY(try_body, catch_list)`:

```c
{
    struct __madc_try_ctx __try_ctx;
    __madc_try_push(&__try_ctx);
    if (setjmp(__try_ctx.jbuf) == 0) {
        // emit try body
        __madc_try_pop();
    } else {
        // emit catch dispatch
    }
}
```

The catch dispatch depends on how many catch clauses there are.
Check `AN_CATCH_LIST` / `AN_CATCH` / `AN_CATCH_ALL` anode structures.

For a single `catch (int e)`:
```c
} else {
    long e = __madc_exception_int();
    // catch body
    __madc_exception_clear();
}
```

For multiple catches, chain on `__madc_exception_type()`:
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
        // catch-all body
        __madc_exception_clear();
    }
}
```

Exception type tags: 1=int, 2=double, 3=string, 99=any.

- [ ] **Step 4: Implement AN_THROW / AN_THROW_EXPR emission**

For `throw expr`:
- If expr is integer type: `__madc_throw_int(expr)`
- If expr is string/cstr type: `__madc_throw_cstr(expr)`
- If expr is double type: `__madc_throw_double(expr)`

For `throw;` (rethrow, no expression): `__madc_rethrow()`

Determine the type from sema or from the AST node type. The simplest
approach: check if the throw expression is a string literal → cstr,
otherwise → int (most common case).

- [ ] **Step 5: Handle destructor cleanup in try blocks**

When objects with destructors are constructed inside a try block, the
emitter should emit destructor calls before `__madc_try_pop()` on the
normal exit path. The existing `scope_class_vars` tracking handles this.

For exception-path cleanup, the runtime's `__madc_cleanup_unwind_to()`
handles it automatically IF cleanup entries are pushed. For Phase 6B,
start without cleanup push (basic try/catch works), and add cleanup
integration as a follow-up if needed.

- [ ] **Step 6: Test**

Test each exception test individually:
```
testexcept testexcept_dtor testexcept_dtor_inherit
testexcept_dtor_nested testexcept_dtor_nothrow testexcept_dtor_order
testexcept_dtor_partial testexcept_dtor_rethrow testexcept_dtor_string
testrethrow
```

Run: `make -C src fulltest`
Run: `bash scripts/run_tests.sh --backend=mir 2>&1 | tail -1`

- [ ] **Step 7: Commit**

```
feat: try/catch/throw transpilation via SJLJ exception runtime
```

---

### Task 3: MadArray extern C wrappers + lifecycle (7 tests)

**Files:**
- Modify: `src/madc_mir_backend.cpp` (add MadArray wrappers)
- Modify: `src/madc_emit_c.cpp` (array variable declaration + destruction)

- [ ] **Step 1: Add MadArray wrappers to runtime**

In `src/madc_mir_backend.cpp`, inside the `extern "C"` block, add:

```cpp
// MadArray lifecycle — placement-new wrappers
void *madarray_construct(void *ptr)
    { return new(ptr) MadArray; }
void madarray_destruct(void *ptr)
    { ((MadArray *)ptr)->~MadArray(); }
long madarray_size(void *ptr)
    { return (long)((MadArray *)ptr)->data.size(); }
```

This requires `#include "datadef.h"` which is already included.

- [ ] **Step 2: Add array variable declaration handling in emitter**

When the sema identifies a variable as type `array` or `MadArray`,
the emitter should declare it as a stack buffer and construct:

```c
char arr[MADC_ARRAY_SIZE];
madarray_construct(arr);
```

And track in `scope_class_vars` for LIFO destruction with
`madarray_destruct(arr)`.

- [ ] **Step 3: Add extern declarations to preamble**

```c
extern void *madarray_construct(void *ptr);
extern void madarray_destruct(void *ptr);
extern long madarray_size(void *ptr);
#define MADC_ARRAY_SIZE <computed>
```

- [ ] **Step 4: Test**

Test: testlang, testperl, testphp, testrubycharsshadow, testrust,
testforeach, testforeach2

These tests call namespace functions that internally create/return
MadArrays. The key question is whether the `void*` parameter passing
works correctly with placement-new'd MadArrays.

Run: `make -C src fulltest`
Run: `bash scripts/run_tests.sh --backend=mir 2>&1 | tail -1`

- [ ] **Step 5: Commit**

```
feat: MadArray extern C wrappers + lifecycle management in transpiler
```

---

### Task 4: Quick stream/string fixes (4 tests)

**Files:**
- Modify: `src/madc_emit_c.cpp` (cout string coercion, stream detection)
- Modify: `src/madc_mir_backend.cpp` (verify/add missing runtime symbols)

- [ ] **Step 1: Fix testctorstring — string member in cout**

Run `bin/madc --emit-c tests/testctorstring.mad` and find where a
string class member is printed. The emitter should emit
`streamout_cstr(os, string_cstr(&obj->name))` instead of
`streamout_int64(os, obj->name)`.

Fix the cout type inference for class members that are strings:
check `sema->class_info[class].field_types[member]` — if TC_CLASS
with class "string", use `streamout_string` not `streamout_int64`.

- [ ] **Step 2: Fix testns — cout << not lowered**

Run `bin/madc --emit-c tests/testns.mad` to see what's emitted.
The `<<` operator on `cout` is not being detected by `is_cout_chain()`.
Find why and fix — likely a scoping or variable resolution issue.

- [ ] **Step 3: Fix testfstream — missing stoi**

Run `bin/madc --backend=mir tests/testfstream.mad 2>&1` to see error.
If `stoi` is unresolved, verify `__std_stoi` is exported from the
runtime. If `printstream` or other helpers are missing, add them.

- [ ] **Step 4: Fix testsstream — missing printstream**

`printstream` is a madc convenience function. Check the legacy compiler
for its implementation. Add an extern C wrapper if needed.

- [ ] **Step 5: Test and commit**

Run: `make -C src fulltest`
Run: `bash scripts/run_tests.sh --backend=mir 2>&1 | tail -1`

```
fix: stream/string quick fixes — cout string coercion, missing runtime symbols
```

---

### Task 5: Verify baseline and update status

- [ ] **Step 1: Run full legacy test suite**

Run: `make -C src fulltest`

- [ ] **Step 2: Run transpiler test suite**

Run: `bash scripts/run_tests.sh --backend=mir 2>&1 | tee /tmp/mir_phase6b.txt`

- [ ] **Step 3: Update KG**

```bash
scripts/kg_query.sh "MERGE (p:Phase {name: 'Phase 6B'}) SET p.description = 'madc features — exceptions, MadArray, computed sizes, stream fixes', p.status = 'complete', p.date = '2026-05-26' RETURN p.name"
scripts/kg_query.sh "MATCH (s:Status {name: 'Transpiler Baseline'}) SET s.description = 'N/475 via --backend=mir', s.date = '2026-05-26' RETURN s.name"
```

- [ ] **Step 4: Commit**

```
docs: Phase 6B complete — update transpiler baseline
```
