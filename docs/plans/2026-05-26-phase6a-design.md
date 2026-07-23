# Phase 6A Design: Core C Correctness

**Date:** 2026-05-26
**Scope:** Fix ~30 transpiler failures in structs, pointer types, varargs, references, function pointers, scoping, and misc emitter bugs
**Baseline:** 315/475 transpiler tests passing (66.3%)
**Target:** ~340-345/475 (~72%)

## Root cause groups and fixes

### 1. va_arg grammar rule (6 tests)

`va_arg(ap, int)` passes a type as the second argument. Gecko's grammar
rejects it because `argument_expression_list` only accepts expressions.

Fix: Add a grammar production for `va_arg(expr, type_name)` as a
primary expression, similar to `sizeof(type)`. The tokenizer already
maps `va_arg` — Gecko just needs the rule. Emit as `va_arg(ap, int64_t)`
in the C output (mapping madc types to C11 types).

### 2. char* members emitted as int64_t (4 tests)

Struct members of type `char*` are emitted as `int64_t` in the struct
definition, causing `printf("%s")` to receive an integer instead of a
pointer. Fix: track pointer types through struct emission and emit
`char *` or `const char *` correctly.

### 3. Reference semantics (2 tests)

`ref int r = x` should emit `int *r = &x` (pointer semantics), but
the emitter copies the value instead. Fix: detect `AN_REF_DECL` in
declarations and emit pointer type + address-of on the initializer.
Reference access `r` should emit `*r` (dereference).

### 4. c2mir type rejections (8 tests)

Individual emitter bugs producing invalid C:
- `errno` emitted as integer, not `(*__errno_location())`
- `void` as first param alongside others (`void, void*`)
- Function signature emission dropping/mangling param types
- Union-to-scalar cast emitted incorrectly
- Aligned attribute emitted as VLA dimension
- `FD_ZERO` / timer macros not expanded

Fix each individually in the emitter.

### 5. Wrong output (5 tests)

- Bitfield widths not applied as masks
- `char arr[] = "str"` only copies first byte
- `log(10.0)` resolves wrong (math lib linkage)
- Char subscript prints integer not char
- uint32_t→double uses signed extension

Fix each in emitter type tracking or runtime.

### 6. SIGSEGV crashes (3 tests)

Null pointer dereference in transpiler for:
- Double-pointer write `**p = val`
- Pointer-to-array parameter decay
- Local variable shadowing global

Fix: add null checks in emitter expression paths.

### 7. Missing bridge symbols (3 tests)

- `get_argv()` not registered in MIR runtime
- `__attribute__((alias))` not implemented

Fix: add argv bridge; emit alias as assignment.

### 8. Function pointer types (1 test)

Function pointer variable declared without proper type. Fix: emit
correct function pointer typedef.

### 9. Switch case scoping (1 test)

Variables declared in switch case body not visible at printf. Fix:
handle case-block variable declarations in emitter scope tracking.

## Deferred

- VLA (7 tests) — c2mir limitation, needs alloca lowering
- GNU extension parse failures (7 tests) — grammar additions
- Complex/SIMD/builtins — separate phases

## Files modified

| File | Change |
|------|--------|
| `src/madc_grammar.cpp` | va_arg production |
| `src/madc_tokenizer.cpp` | va_arg token handling |
| `src/madc_emit_c.cpp` | Multiple individual fixes |
| `src/madc_mir_backend.cpp` | argv bridge, errno wrapper |
