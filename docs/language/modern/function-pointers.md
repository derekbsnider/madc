# Function Pointers

Store a function's address and call through it indirectly.

## Syntax

```c
auto fn = my_function;    // type inferred as function pointer
fn(args);                  // indirect call through pointer
```

## Type System

`DataDefFPTR` wraps a `FuncDef*` carrying the target function's signature. It reports `is_function()=true` and `is_numeric()=true` — this dual identity is how the parser/compiler distinguishes function pointers from actual function definitions (which have `is_numeric()=false`).

## How It Works

**Parsing:** When `parseExpression()` encounters a variable with `is_function() && is_numeric()` (a FPTR variable) followed by `(`, it creates a `TokenCallFunc` that routes to the indirect-call path in the compiler.

**Compilation:** `TokenCallFunc::compile()` detects the FPTR case, builds a `FuncSignatureBuilder` from the wrapped `FuncDef`'s parameter types, and calls `cc.invoke(&call, ptr_reg, funcsig)` — the same pattern as `dlcall`.

**Address emission:** `TokenVar::compile()` for a real function variable (non-FPTR) emits `lea(reg, label)` for JIT functions or `mov(reg, imm(x86code))` for extern functions.

## Key Design Decision

Bare function names WITHOUT `(` are only treated as addresses for `DataDefFPTR` variables. Regular function names (like `endl`) keep the original `TokenCallFunc` behavior on the operator stack — this prevents breaking stream manipulators.

## Files

- `include/datadef.h` — `DataDefFPTR`, `DataDefAUTO`
- `src/parser.cpp` — `auto` handling in `parseDeclaration()`, FPTR path in `parseExpression()`
- `src/compiler.cpp` — FPTR call in `TokenCallFunc::compile()`, address emission in `TokenVar::compile()`
