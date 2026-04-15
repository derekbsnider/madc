# madc Architecture

## Overview

madc is a single-pass JIT compiler. Source text goes through three sequential phases and is immediately executed:

```
Source (.mad file)
    │
    ▼
[Lexer]  src/lexer.cpp
    │  → Token stream
    ▼
[Parser]  src/parser.cpp
    │  → AST (tree of Token* objects)
    ▼
[Compiler]  src/compiler.cpp
    │  → x86-64 machine code (via asmjit)
    ▼
[JIT Execute]  madc.cpp
```

## Source Files

| File | Lines | Purpose |
|------|-------|---------|
| `src/madc.cpp` | 75 | Entry point: open file, run all phases |
| `src/lexer.cpp` | 719 | Tokenize source into token stream |
| `src/parser.cpp` | 1799 | Parse tokens into AST |
| `src/compiler.cpp` | 2623 | Compile AST nodes to x86 via asmjit |
| `src/typesafe.cpp` | 1008 | Type-safe register helpers for Gp/Xmm ops |

## Header Files

| File | Lines | Purpose |
|------|-------|---------|
| `include/madc.h` | 478 | Core classes: Program, Variable, FuncDef, DataStruct, Method |
| `include/tokens.h` | 1034 | Token class hierarchy (TokenBase → TokenIf, TokenFor, etc.) |
| `include/datadef.h` | 511 | Data type system: DataType enum, DataDef base class, register helpers |
| `include/datatokens.h` | 175 | Concrete data type token classes |

## Token Class Hierarchy

All AST nodes inherit from `TokenBase`. Key subclasses:

- **TokenProgram** — root node, wraps entire compilation unit
- **TokenFunc** — function definition
- **TokenCpnd** — compound statement (block), scopes variables
- **TokenCallFunc** — function call expression
- **TokenAssign** — assignment operator
- **TokenIf / TokenFor / TokenWhile / TokenDo** — control flow
- **TokenReturn** — return statement
- **TokenBSL** — `<<` stream-output operator
- **TokenInt / TokenReal / TokenVar** — literal values and variable references

Each token implements:
- `compile(Program& pgm, regdefp_t& regdp)` — emits JIT code, returns operand in `regdp`
- `operand(Program& pgm)` — returns the asmjit `Operand` for this value

## Program Class

`Program` (in `include/madc.h`) is the central state object threaded through
all phases. It holds:
- `cc` — the asmjit `x86::Compiler`
- `code` — the asmjit `CodeHolder`
- `jit` — the asmjit `JitRuntime`
- Global variable map and function registry
- The `safemov`, `safeadd`, `safesub`, etc. helpers (implemented in `typesafe.cpp`)

## Type System

`DataType` enum (in `include/datadef.h`) defines all supported types:
- Primitives: `dtVOID`, `dtBOOL`, `dtINT8`–`dtINT64`, `dtUINT8`–`dtUINT64`, `dtFLOAT`, `dtDOUBLE`
- Complex: `dtSTRING`, `dtOSTREAM`, `dtSSTREAM`, `dtOSSTREAM`, `dtMUTEX`, `dtTHREAD`
- Pointer variants: add 10000 (e.g. `dtSTRINGptr = dtSTRING + 10000`)
- Reference variants: add 20000

`DataDef` is the base class for all type descriptors and includes methods for emitting
register loads/stores into asmjit-generated code.

## Variable Storage

Variables can live:
- On the JIT stack (`vfSTACK` flag) — asmjit stack slot
- In heap memory (`vfALLOC` flag) — allocated with `new`, address embedded as immediate
- As global objects — constructed at program start, stored at fixed addresses

## Register Conventions

- Integer/pointer values use `asmjit::x86::Gp` registers
- Float/double values use `asmjit::x86::Xmm` registers
- `typesafe.cpp` provides `safemov`, `safeadd`, etc. to handle cross-type operations
  (e.g. moving an int from a Gp into an Xmm via `cvtsi2sd`)
