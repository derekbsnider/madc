# Phase 6A: Core C Correctness — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix ~30 transpiler failures in varargs, struct pointer types, references, function pointers, scoping, and misc emitter bugs. Target: ~340/475 (72%).

**Architecture:** Mostly individual emitter bug fixes. One grammar addition for `va_arg(ap, type)`. No new files — all changes in existing emitter, grammar, tokenizer, and runtime.

**Tech Stack:** C++11, Gecko GLR grammar, c2mir/MIR

---

### Task 1: va_arg grammar rule + emission (6 tests)

**Files:**
- Modify: `src/madc_grammar.cpp` (add va_arg production)
- Modify: `src/madc_emit_c.cpp` (emit va_arg correctly)

`va_arg(ap, int)` passes a type as the second argument. Gecko rejects it because argument lists only accept expressions.

- [ ] **Step 1: Study how sizeof(type) is handled in the grammar**

Read `src/madc_grammar.cpp` and find the `sizeof` production — it accepts a type_name in parens. `va_arg` needs the same pattern: `va_arg(expr, type_name)`.

- [ ] **Step 2: Add va_arg as a primary expression**

Add to the grammar in `primary_expression` or as a new production:
```
va_arg_expr : VA_ARG '(' assignment_expression ',' type_name ')'  # va_arg_expr (2 4)
```

Check if `VA_ARG` is already a terminal code. If not, add it. The tokenizer maps madc's `va_arg` identifier to this code.

- [ ] **Step 3: Handle va_arg in the emitter**

When the emitter encounters the new `va_arg_expr` anode, emit:
```c
va_arg(ap_expr, type)
```
Map madc types to C11 types (int → int64_t, double → double, etc.).

- [ ] **Step 4: Ensure stdarg.h is in preamble**

The preamble should already have `#include <stdarg.h>` from Phase 4 va_list work. Verify.

- [ ] **Step 5: Test**

Test each: teststdarg2, testvarargs, testnestedvarargs, testvarargsstructcomplex, testsmallstructarraycall, testvarargsstructruntime

Run: `make -C src fulltest`

- [ ] **Step 6: Commit**

```
feat: va_arg(ap, type) grammar rule + emission in transpiler
```

---

### Task 2: Fix char* struct members emitted as int64_t (4 tests)

**Files:**
- Modify: `src/madc_emit_c.cpp` (struct field emission, type tracking)

Struct members of type `char*` are emitted as `int64_t`, causing printf to receive integers instead of pointers.

- [ ] **Step 1: Find where struct fields are emitted**

Search `src/madc_emit_c.cpp` for `emit_class_field_item` and the struct emission path. Understand how field types are determined.

- [ ] **Step 2: Fix pointer type emission**

When emitting struct fields, if the type contains `*` (pointer), preserve it in the output. The issue is likely that `emit_type()` is mapping `char*` to `int64_t` because madc internally uses int64 for pointers.

Trace the type emission path and ensure pointer types are preserved as `char *`, `int *`, `struct Foo *`, etc.

- [ ] **Step 3: Test**

Test: teststructarrayofptr, teststructfwdtypedefarr, teststructinitnested, testnestedpackedmember

Run: `make -C src fulltest`

- [ ] **Step 4: Commit**

```
fix: preserve char* pointer types in struct member emission
```

---

### Task 3: Fix reference semantics (2 tests)

**Files:**
- Modify: `src/madc_emit_c.cpp` (declaration and expression emission)

`ref int r = x` (madc reference) should compile to `int *r = &x` in C, and uses of `r` should emit `*r`. Currently the emitter copies the value.

- [ ] **Step 1: Find how AN_REF_DECL is handled**

Search `src/madc_emit_c.cpp` for `AN_REF_DECL` or `ref_decl`. Understand the current emission path.

- [ ] **Step 2: Fix declaration emission**

When `AN_REF_DECL` wraps a declarator, emit `type *name = &init` instead of `type name = init`. Track reference variables in a set so that later uses can emit `*name`.

- [ ] **Step 3: Fix reference access**

When emitting an identifier that's in the reference set, emit `(*name)` to dereference it. When taking address of a reference (`&r`), emit `r` (it's already a pointer).

- [ ] **Step 4: Test**

Test: testref, testconstref

Run: `make -C src fulltest`

- [ ] **Step 5: Commit**

```
feat: reference variables emit pointer semantics in transpiler
```

---

### Task 4: Fix SIGSEGV crashes in emitter (3 tests)

**Files:**
- Modify: `src/madc_emit_c.cpp` (null checks in expression emission)

Three tests crash the transpiler itself:
- testdoubleptrwrite — double-pointer write `**p = val`
- testparamptrtoarray — pointer-to-array parameter decay
- testshadowlocalglobal — local variable shadowing global

- [ ] **Step 1: Reproduce each crash and find the stack trace**

Run each test and examine the error. Use `bin/madc --emit-c tests/foo.mad` to see if the crash is during emission or execution.

- [ ] **Step 2: Add null checks**

The crashes are likely null `gp_tree_node*` dereferences in the emitter's recursive expression walker. Add null checks where nodes are accessed.

- [ ] **Step 3: Fix the underlying emission bugs**

After preventing the crash, fix the actual emission for double-pointer writes, pointer-to-array params, and variable shadowing.

- [ ] **Step 4: Test**

Test: testdoubleptrwrite, testparamptrtoarray, testshadowlocalglobal

Run: `make -C src fulltest`

- [ ] **Step 5: Commit**

```
fix: prevent SIGSEGV in emitter for double-pointer, array-param, shadow
```

---

### Task 5: Fix misc c2mir type rejections (batch of individual fixes)

**Files:**
- Modify: `src/madc_emit_c.cpp`
- Modify: `src/madc_mir_backend.cpp` (errno wrapper if needed)

Fix as many of these as possible — each is a separate emitter bug:

- [ ] **Step 1: Fix errno emission**

`errno` is a macro expanding to `(*__errno_location())`. The transpiler emits it as a plain integer. Add `errno` to the preamble via `#include <errno.h>` or emit `(*__errno_location())`.

- [ ] **Step 2: Fix void parameter emission**

Some functions emit `(void, void*)` instead of `(void*, void*)` — the first `void` should be `void*`. Find and fix the parameter type emission bug.

- [ ] **Step 3: Fix union cast emission**

`testunionscalarcast` — union-to-scalar cast needs to be emitted as member access, not a C cast.

- [ ] **Step 4: Fix switch-case variable scoping**

`testcolon` — variables declared in case blocks need to be in the switch scope. Emit braces around case bodies, or move declarations before the switch.

- [ ] **Step 5: Fix function pointer type emission**

`testfuncptr` — function pointer variables need proper `typedef` or inline function pointer syntax: `int (*fp)(int, int) = &func;`

- [ ] **Step 6: Test all fixed tests**

Run each individually, then `make -C src fulltest`

- [ ] **Step 7: Commit**

```
fix: batch emitter fixes — errno, void params, union cast, switch scope, funcptr types
```

---

### Task 6: Fix wrong-output tests (arithmetic/string fixes)

**Files:**
- Modify: `src/madc_emit_c.cpp`

Fix tests that run but produce wrong values:

- [ ] **Step 1: Fix char array string initialization**

`testchararraystringinit` — `char arr[] = "12"` only copies first byte. The initializer needs to be emitted as a string literal, not byte-by-byte.

- [ ] **Step 2: Fix char subscript output format**

`testparenexprsub` — `str[i]` prints integer char code instead of character. Ensure cout type inference recognizes char subscript expressions.

- [ ] **Step 3: Fix uint32→double coercion**

`testuint32realcoerce` — unsigned int extending as signed. Ensure unsigned types use zero-extension.

- [ ] **Step 4: Fix math library linkage**

`testmixedfuncvardecl` — `log(10.0)` returns `inf`. Ensure `libm` functions are properly declared with correct signatures.

- [ ] **Step 5: Test and commit**

Run each, then `make -C src fulltest`

```
fix: char array init, char subscript format, uint32 coercion, math linkage
```

---

### Task 7: VLA → alloca lowering (7 tests)

**Files:**
- Modify: `src/madc_emit_c.cpp` (declaration emission)

c2mir doesn't support VLA (`int arr[n]`), but it supports `alloca()`.
Lower VLA declarations to alloca in the emitted C:

```c
// Input:  int arr[n];
// Output: int *arr = (int *)alloca(n * sizeof(int));
```

- [ ] **Step 1: Find where array declarations with runtime sizes are emitted**

Search `src/madc_emit_c.cpp` for `AN_ARRAY_DECL` or `AN_VLA_DECL`. When
the array size expression is non-constant (not a literal or `sizeof`),
it's a VLA.

- [ ] **Step 2: Emit alloca instead of VLA**

When a VLA is detected, emit:
```c
type *name = (type *)__builtin_alloca(size_expr * sizeof(type));
```

Use `__builtin_alloca` which c2mir supports (it's a GCC builtin). If
c2mir doesn't support it, use `alloca` with `#include <alloca.h>` in
the preamble.

- [ ] **Step 3: Handle multi-dimensional VLA**

For `int arr[m][n]`, emit:
```c
int (*arr)[n] = (int (*)[n])__builtin_alloca(m * n * sizeof(int));
```

Or flatten to single dimension if multi-dim VLA is too complex.

- [ ] **Step 4: Test**

Test: testvla, testmultidimvla, testparamvlaruntimeexpr, testtypedefvlasizeof,
testnestedvlaparam, testvlaglobalboundnested, testvlastructmember

Run: `make -C src fulltest`

- [ ] **Step 5: Commit**

```
feat: lower VLA to alloca in transpiler output
```

---

### Task 8: Verify baseline and update status

- [ ] **Step 1: Run full legacy test suite**

Run: `make -C src fulltest`

- [ ] **Step 2: Run transpiler test suite**

Run: `bash scripts/run_tests.sh --backend=mir 2>&1 | tee /tmp/mir_phase6a.txt`

- [ ] **Step 3: Commit status update**

Update KG and docs with new transpiler count.
