# madc Architecture

## Overview

madc is a single-pass JIT compiler. Source text goes through three sequential phases and is immediately executed:

```
Source (.mad file)
    │
    ▼ src/lexer.cpp      — tokenize into a flat token stream
    │
    ▼ src/parser.cpp     — parse tokens into AST (tree of Token* objects)
    │
    ▼ src/compiler.cpp   — walk AST, emit x86-64 via asmjit
    │
    ▼ JIT execute         — run machine code in-process (no disk I/O)
```

## Source Files

| File | Lines | Purpose |
|------|-------|---------|
| `src/madc.cpp` | ~80 | Entry point: parse flags, open file, run all phases |
| `src/lexer.cpp` | ~720 | Tokenize source into token stream |
| `src/parser.cpp` | ~1800 | Parse tokens into AST |
| `src/compiler.cpp` | ~2650 | Compile AST nodes to x86 via asmjit |
| `src/typesafe.cpp` | ~1010 | Type-safe register helpers for Gp/Xmm ops |

## Header Files

| File | Lines | Purpose |
|------|-------|---------|
| `include/datadef.h` | ~515 | `DataType` enum, `DataDef` base class, `varflag_t`, register helpers |
| `include/madc.h` | ~480 | Core classes: `Program`, `Variable`, `FuncDef`, `DataStruct`, `Method` |
| `include/tokens.h` | ~1040 | Token class hierarchy (`TokenBase` → `TokenIf`, `TokenFor`, etc.) |
| `include/datatokens.h` | ~175 | Concrete data type token classes |
| `include/doctest.h` | single-header | doctest v2.4.11 unit test framework |

> `datadef.h` is always the **first** project header included in every source file
> (before `madc.h`). This is required because `extern bool madc_verbose` lives there
> and must be visible before any header that uses the `DBG()` macro.

## Token Class Hierarchy

All AST nodes inherit from `TokenBase`. Key subclasses:

- **TokenProgram** — root node, wraps entire compilation unit
- **TokenFunc** — function definition
- **TokenCpnd** — compound statement (block); owns variable scope and cleanup
- **TokenCallFunc** — function call expression
- **TokenAssign** — assignment operator (`=`, `+=`, `-=`, etc.)
- **TokenIf / TokenFor / TokenWhile / TokenDo** — control flow
- **TokenReturn** — return statement
- **TokenBSL** — `<<` stream-output operator
- **TokenInt / TokenReal / TokenChar** — integer, float, and char literals
- **TokenVar** — variable reference
- **TokenMember** — struct member access (`obj.field`)
- **TokenREGISTER** — `register` keyword; sets `vfREGISTER` on the declared variable

Each token implements:
- `compile(Program& pgm, regdefp_t& regdp)` — emits JIT code; returns the result operand via `regdp`
- `operand(Program& pgm)` — returns the cached asmjit `Operand` for this value

`regdefp_t` is `pair<Operand*, DataDef*>` — the result register and its type.

## Program Class

`Program` (in `include/madc.h`) is the central state object threaded through all phases:

- `cc` — the asmjit `x86::Compiler`
- `code` — the asmjit `CodeHolder`
- `jit` — the asmjit `JitRuntime`
- `tkFunction` — pointer to the current `TokenFunc` being compiled
- Global variable map and function registry
- `safemov`, `safeadd`, `safesub`, `safecmp` helpers (in `typesafe.cpp`)

## Type System

`DataType` enum (in `include/datadef.h`) defines all supported types:
- Primitives: `dtVOID`, `dtBOOL`, `dtCHAR`, `dtINT8`–`dtINT64`, `dtUINT8`–`dtUINT64`, `dtFLOAT`, `dtDOUBLE`
- Complex: `dtSTRING`, `dtOSTREAM`, `dtSSTREAM`, `dtOSSTREAM`, `dtMUTEX`, `dtTHREAD`
- Pointer variants: base type + 10000 (e.g. `dtSTRINGptr = dtSTRING + 10000`)
- Reference variants: base type + 20000

`DataDef` is the base class for all type descriptors. Subclasses (`DataDefINT`, `DataDefSTRING`, `DataDefSTRUCT`, etc.) provide:
- `size` — byte size of the type
- `is_numeric()` — true for integer and float types
- `compile()` — emit a load of this type from memory into a register
- `movsd()`, `movgp()` — type-appropriate move helpers

## Variable Storage Model

Variables use `varflag_t` (a `uint16_t` bitmask) to track storage:

| Flag | Value | Meaning |
|------|-------|---------|
| `vfLOCAL` | 1 | Local to current scope |
| `vfSTACK` | 2 | Backed by an asmjit stack slot |
| `vfSTATIC` | 4 | Static lifetime |
| `vfPARAM` | 8 | Function parameter |
| `vfREGISTER` | 16 | Register-only; never written back to memory |
| `vfREGSET` | 64 | Gp register has been assigned |
| `vfXREGSET` | 128 | Xmm register has been assigned |
| `vfALLOC` | 256 | Heap-allocated (address stored as immediate) |
| `vfSTACKSET` | 512 | Stack slot has been allocated |
| `vfMODIFIED` | 1024 | Value has been changed (needs writeback) |
| `vfCONSTANT` | 2048 | Compile-time constant |

### The `register` Keyword

`register int x = 0;` sets `vfREGISTER` on the variable, preventing any store back to
memory. This is madc's natural execution model: asmjit virtual registers handle
allocation. The `register` keyword makes it explicit.

Complex types (structs, strings, streams) always require memory backing — the `register`
keyword has no effect on them.

## Struct Member Access

Struct variables are allocated as asmjit stack slots. The `operand_map` in `TokenCpnd`
maps `Variable*` → `x86::Mem` pointing to the top of the struct's stack slot.

`TokenMember::operand()` computes the member's location by calling `addOffset(member_offset)` on that Mem operand — this adds to the existing displacement, giving `[rbp - slot + offset]`. (Using `setOffset` would replace the displacement, producing the wrong address.)

For reads of numeric members, the Mem operand is loaded into a fresh Gp register.
For string/object members, the address is LEA'd into a Gp register (to pass as a pointer).

String members in structs require explicit lifetime management:
- **Construction:** `string_construct(address)` called in `TokenCpnd::voperand()` when the struct is initialized
- **Destruction:** `string_destruct(address)` called in `TokenCpnd::cleanup()` when the scope exits

## Register Conventions

- Integer/pointer values: `asmjit::x86::Gp` registers
- Float/double values: `asmjit::x86::Xmm` registers
- `typesafe.cpp` provides `safemov`, `safeadd`, etc. to handle cross-type operations
  (e.g. moving an int from a Gp into an Xmm via `cvtsi2sd`)

## Error Handling

Parse errors go through the `throwstream` / `throwbuf` mechanism (in `lexer.cpp`):
- `pgm.Throw << "message" << flush` — formats a message with filename:line:col and a source caret, then throws `std::runtime_error`
- Errors are printed to stderr before throwing
- The top-level `main()` in `madc.cpp` catches and displays them

## Unit Tests

Unit tests live in `tests/unit/*.cpp` and use doctest. They are built by `make -C src test` and linked against `TESTOBJ` (all `.o` files except `madc.o`).

Each test file must:
1. `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` (exactly once per binary)
2. Define `bool madc_verbose = false;` and `#define DBG(x) do { if(madc_verbose){x;} } while(0)` before including any project headers
3. NOT define the `dd*` global instances — they come from `parser.o`
