# madc Architecture

## Overview

madc is a single-pass JIT compiler. Source text goes through three sequential phases and is immediately executed:

```
Source (.mad file)
    |
    v src/lexer.cpp      — tokenize into a flat token stream
    |                      (#include and #load handled here)
    |
    v src/parser.cpp     — parse tokens into AST, namespace resolution,
    |                      type registration (struct/class)
    |
    v src/compiler.cpp   — walk AST, emit x86-64 via asmjit
    |
    v JIT execute         — run machine code in-process (no disk I/O)
```

## Source Files

| File | Lines | Purpose |
|------|-------|---------|
| `src/madc.cpp` | ~80 | Entry point: parse flags, open file, run all phases |
| `src/lexer.cpp` | ~830 | Tokenize source; handle `#include`, `#load` directives |
| `src/parser.cpp` | ~2360 | Parse tokens into AST; struct/class/namespace resolution |
| `src/compiler.cpp` | ~3020 | Compile AST nodes to x86 via asmjit; stream I/O, dlcall |
| `src/typesafe.cpp` | ~1010 | Type-safe register helpers for Gp/Xmm ops |
| `src/ns_php.cpp` | ~615 | php:: namespace: 36 functions + MadArray operations |
| `src/ns_perl.cpp` | ~330 | perl:: namespace: 21 functions |
| `src/ns_python.cpp` | ~290 | python:: namespace: 16 functions |
| `src/ns_ruby.cpp` | ~255 | ruby:: namespace: 12 functions |
| `src/ns_js.cpp` | ~240 | js:: namespace: 6 functions (base64, URL encoding, JSON) |
| `src/ns_stl.cpp` | — | STL container helpers: vector<T>, map<K,V>, set<T>, list<T> method compilation |

> **Note:** Line counts above are from the initial documentation pass and may be outdated as the codebase has grown significantly through Phase 3.5.

## Header Files

| File | Lines | Purpose |
|------|-------|---------|
| `include/datadef.h` | ~645 | `DataType` enum, `DataDef` hierarchy, `MadValue`/`MadArray`, `varflag_t` |
| `include/madc.h` | ~490 | Core classes: `Program`, `Source`, `Variable`, `FuncDef`, `Method` |
| `include/tokens.h` | ~1070 | Token class hierarchy (`TokenBase` -> `TokenIf`, `TokenFor`, etc.) |
| `include/datatokens.h` | ~180 | Concrete data type token classes (`TokenINT`, `TokenSTRING`, etc.) |
| `include/doctest.h` | single-header | doctest v2.5.0 unit test framework |

> `datadef.h` is always the **first** project header included in every source file
> (before `madc.h`). This is required because `extern bool madc_verbose` lives there
> and must be visible before any header that uses the `DBG()` macro.

## Token Class Hierarchy

All AST nodes inherit from `TokenBase`. Key subclasses:

- **TokenProgram** — root node, wraps entire compilation unit
- **TokenFunc** — function definition
- **TokenCpnd** — compound statement (block); owns variable scope and cleanup
- **TokenCallFunc** — function call expression
- **TokenCallMethod** — method call (e.g. `stream.open()`, `stream.good()`)
- **TokenMember** — struct/class member access (`obj.field`)
- **TokenAssign** — assignment operator (`=`, `+=`, `-=`, etc.)
- **TokenIf / TokenFor / TokenWhile / TokenDo** — control flow
- **TokenFOREACH** — range-based for loop (`for (type var : container)`)
- **TokenReturn** — return statement
- **TokenBSL** — `<<` stream-output operator
- **TokenInt / TokenReal / TokenChar** — integer, float, and char literals
- **TokenVar** — variable reference
- **TokenDecl** — variable declaration
- **TokenREGISTER** — `register` keyword; sets `vfREGISTER` on the declared variable
- **TokenSTRUCT** — `struct` keyword with `parse()` for definitions
- **TokenCLASS** — `class` keyword with `parse()` for definitions
- **TokenUSING** — `using` keyword with `parse()` for namespace imports
- **TokenNS** — `::` namespace resolution operator

Each token implements:
- `compile(Program& pgm, regdefp_t& regdp)` — emits JIT code; returns the result operand via `regdp`
- `operand(Program& pgm)` — returns the cached asmjit `Operand` for this value

`regdefp_t` is `pair<Operand*, DataDef*>` — the result register and its type.

### Token Queue (pushToken)

The parser uses a `deque`-based token queue (`pushToken`/`popToken`) to handle lookahead situations where tokens need to be re-examined. This is used when the parser speculatively consumes a token and then needs to put it back for a different parse path.

### Class Method Dispatch

Class methods are compiled as regular functions with a hidden `__this` pointer injected as the first parameter. When `obj.method()` is called:
1. The compiler looks up the method in the class's `DataDefCLASS` method vector
2. A pointer to the object's stack memory is passed as the implicit first argument
3. Inside the method body, `this.member` accesses are compiled as offsets from the `__this` pointer

### Ternary Operator

The ternary operator (`cond ? a : b`) is compiled using asmjit labels for branching. Both branches emit code to produce a value into the same stack slot (stack-slot merge), and execution resumes after the false-branch label. The merge ensures both paths write to a single result location.

### Multi-Return (`__retbuf` Mechanism)

Functions with multiple return values use a hidden `__retbuf` parameter — a pointer to caller-allocated stack memory. The callee writes return values into `__retbuf` slots, and the caller reads them back after the call. This avoids register-pressure issues when returning more than one value.

## Program Class

`Program` (in `include/madc.h`) is the central state object threaded through all phases:

- `cc` — the asmjit `x86::Compiler`
- `code` — the asmjit `CodeHolder`
- `jit` — the asmjit `JitRuntime`
- `tkFunction` — pointer to the current `TokenFunc` being compiled
- `keyword_map` — reserved keywords
- `datatype_map` — data type tokens (built-in + user-defined classes)
- `struct_map` — struct/class definitions
- `funcdef_map` — function definitions
- `literal_map` — string literal variables
- `namespace_map` — namespace registries (`std::`, `php::`, `perl::`, etc.)
- `dlopen_map` — dlopen handles for `#load` libraries
- `safemov`, `safeadd`, `safesub`, `safecmp` helpers (in `typesafe.cpp`)

## Type System

`DataType` enum (in `include/datadef.h`) defines all supported types:
- Primitives: `dtVOID`, `dtBOOL`, `dtCHAR`, `dtINT8`-`dtINT64`, `dtUINT8`-`dtUINT64`, `dtFLOAT`, `dtDOUBLE`
- C++ classes: `dtSTRING`, `dtOSTREAM`, `dtSSTREAM`, `dtIFSTREAM`, `dtOFSTREAM`, `dtFSTREAM`
- Mixed-type array: `dtARRAY` (MadValue-based)
- Pointer variants: base type + 10000 (e.g. `dtSTRINGptr = dtSTRING + 10000`)
- Reference variants: base type + 20000

`DataDef` is the base class for all type descriptors. Key subclasses:
- `DataDefSTRUCT` — struct types with member vector and offset/type lookup
- `DataDefCLASS` — class types, extends struct with methods vector
- `DataDefARRAY` — MadArray container type
- `DataDefFPTR` — function pointer type, wraps a `FuncDef*` for the target signature
- `DataDefAUTO` — placeholder for `auto` type inference (resolved at parse time)

### MadValue / MadArray

`MadValue` is a tagged union used by php:: array functions for mixed-type storage:
- `type` field: `DataType` tag
- Union of `int64_t`, `double`, `void*` (string pointer)
- Deep-copies strings on copy/assign, destructs on scope exit

`MadArray` wraps `std::vector<MadValue>` for indexed storage and `std::vector<pair<string, MadValue>>` for associative access.

## Variable Storage Model

Variables use `varflag_t` (a `uint16_t` bitmask) to track storage:

| Flag | Value | Meaning |
|------|-------|---------|
| `vfLOCAL` | 1 | Local to current scope |
| `vfSTACK` | 2 | Backed by an asmjit stack slot |
| `vfSTATIC` | 4 | Static lifetime |
| `vfPARAM` | 8 | Function parameter (caller owns the object) |
| `vfREGISTER` | 16 | Register-only; never written back to memory |
| `vfREGSET` | 64 | Gp register has been assigned |
| `vfXREGSET` | 128 | Xmm register has been assigned |
| `vfALLOC` | 256 | Heap-allocated (address stored as immediate) |
| `vfSTACKSET` | 512 | Stack slot has been allocated |
| `vfMODIFIED` | 1024 | Value has been changed (needs writeback) |
| `vfCONSTANT` | 2048 | Compile-time constant |

### Function Parameters

Non-numeric parameters (strings, arrays, streams) are passed by reference. `voperand()` creates a bare Gp register for `vfPARAM` variables — no stack allocation or construction. `cleanup()` skips destruction of parameter objects since the caller owns them.

### The `register` Keyword

`register int x = 0;` sets `vfREGISTER` on the variable, preventing any store back to memory. Complex types (structs, strings, streams) always require memory backing.

## Struct/Class Member Access

Struct variables are allocated as asmjit stack slots. The `operand_map` in `TokenCpnd` maps `Variable*` -> `x86::Mem` pointing to the top of the struct's stack slot.

`TokenMember::operand()` computes the member's location by calling `addOffset(member_offset)` on that Mem operand. For reads of numeric members, the Mem operand is loaded into a Gp register. For string/object members, the address is LEA'd into a Gp register.

String members in structs require explicit lifetime management:
- **Construction:** `string_construct(address)` in `TokenCpnd::voperand()`
- **Destruction:** `string_destruct(address)` in `TokenCpnd::cleanup()`

## Namespace Resolution

When `parseExpression()` encounters `identifier::member`:
1. Look up `identifier` in `namespace_map`
2. Look up `member` in the namespace's variable map
3. If not found and namespace has a dlopen handle, try `dlsym` (lazy resolution)
4. Cache the resolved variable for future calls

`using namespace X;` copies all members into the global scope. `using X::member;` copies a single member.

## Stream I/O

Streams (`ostream`, `stringstream`, `ifstream`, `ofstream`, `fstream`) are stack-allocated C++ objects managed by the JIT:
- **Construction:** placement-new in `voperand()` (e.g. `ifstream_construct`)
- **Destruction:** explicit destructor call in `cleanup()` (e.g. `ifstream_destruct`)
- **`<<` operator:** `TokenBSL::compile()` checks `has_ostream()` and emits calls to `streamout_string()`, `streamout_numeric<T>()`, etc.
- **Methods:** `open()`, `close()`, `good()`, `eof()`, `is_open()` registered per-type

> Stream methods use **type-specific wrappers** (e.g. `ifstream_good`, `ofstream_good`)
> because `std::ios` is a virtual base class. Casting `void*` directly to `std::ios*`
> skips the vtable pointer adjustment and produces wrong results.

## Dynamic Library Loading

Two mechanisms:
1. **`#load "lib.so" as ns;`** — lexer-level directive, calls `dlopen()`, creates namespace with lazy `dlsym` resolution
2. **`dlopen()`/`dlsym()`/`dlcall()`** — runtime functions. `dlcall` is handled specially in the compiler to emit `cc.invoke()` through a Gp register

## Error Handling

Parse errors go through the `throwstream` / `throwbuf` mechanism (in `lexer.cpp`):
- `pgm.Throw << "message" << flush` — formats a message with filename:line:col and a source caret, then throws `std::runtime_error`
- Errors are printed to stderr before throwing
- The top-level `main()` in `madc.cpp` catches and displays them

## Unit Tests

Unit tests live in `tests/unit/*.cpp` and use doctest. They are built by `make -C src test` and linked against `TESTOBJ` (all `.o` files except `madc.o`).

Each test file must:
1. `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` (exactly once per binary)
2. Define `bool madc_verbose = false;` and the `DBG` macro before including project headers
3. NOT define the `dd*` global instances — they come from `parser.o`
