# Unified Parser Architecture — Phase 1: libc2mir + Pipeline Rewiring

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the Gecko parser and double-parse overhead by exposing c2mir as a library (`libc2mir`) that accepts AST nodes directly, then rewire madc's original parser to feed c2mir's checker+generator instead of emitting C11 text.

**Architecture:** Three layers: (1) madc's hand-written parser produces the existing TokenBase AST with full semantic context, (2) a translation layer walks the TokenBase AST and builds c2mir `node_t` trees (the "CIR" — C Internal Representation), (3) c2mir's existing `check()` + `gen_mir()` consume the CIR and produce MIR. C++ and madc constructs are lowered to C11-shaped `node_t` trees during translation. Language-specific metadata hangs off nodes for tooling (syntax highlighting, diagnostics, pretty-printing). Future language parsers (Python, Ruby, etc.) target the same CIR format.

**Tech Stack:** C/C++11, MIR (our fork at `/workspace/mir/`), c2mir (same fork), asmjit (legacy JIT path preserved)

---

## Background & Research

### Why this change

The current MIR transpiler pipeline is:

```
madc tokens → Gecko GLR parse → gp_tree_node tree → sema pass → C11 text emission → c2mir re-parses text → c2mir AST → check → gen → MIR
```

Problems discovered:
- **Double parsing:** c2mir re-parses the C11 text we emit, wasting cycles
- **Lost context:** Gecko is a generic CFG parser with no semantic awareness — it can't distinguish typedefs from variables (the classic C ambiguity), causing 79+ misparsed declarations in SMAUG
- **Lost AST:** The original parser's rich TokenBase tree (with types, scopes, variables) is discarded; the Gecko tree is a pale shadow rebuilt from scratch
- **Fragile emitter:** 6000+ lines of C11 text emission with workarounds for grammar ambiguities, buffer truncation, cast/bitand confusion
- **Text bottleneck:** C11 text is a lossy serialization format — information present in the AST is lost and must be re-inferred by c2mir

The target pipeline:

```
madc tokens → parser.cpp → TokenBase AST → translate to c2mir node_t → check → gen → MIR
                                          ↘ also available for:
                                            - asmjit JIT (existing)
                                            - syntax highlighting
                                            - pretty-printing
                                            - IDE tooling
                                            - static analysis
```

### Key research findings

- **c2mir internals:** All node creation (`new_node`), type checking (`check`), and MIR generation (`gen_mir`) functions are `static` in `c2mir.c`. We need to expose ~10-15 functions as a library API. We own the fork (13 commits already).
- **c2mir AST:** `node_t` with `node_code_t` enum (~130 codes), children in doubly-linked list (`DLIST`), attributes via `void *attr`. Arena-allocated via `reg_malloc`.
- **MIR public API:** Fully supports programmatic construction (`MIR_new_func`, `MIR_new_insn`, `MIR_append_insn`). This is our fallback if the c2mir AST approach hits walls.
- **TCC/c2mir speed tricks:** String interning, arena allocation, token pre-materialization, pre-mapped node codes, incremental typedef tracking. These are Phase 2 (optimization) — not in this plan.
- **Industry consensus:** GCC, Clang, TCC, c2mir, chibicc all use hand-written recursive descent. Every production C compiler switched away from generated parsers. Gecko solves a problem we don't have.

### The CIR concept

The c2mir `node_t` tree becomes the **C Internal Representation** (CIR) — a universal compilation target:

```
┌─────────────────────────────────────┐
│          madc AST node              │
│  ┌─────────────────────────────┐    │
│  │  C11 spine (node_t tree)    │────│──→ c2mir check + gen_mir
│  │  struct, func, calloc,      │    │    (standard C11 semantics)
│  │  setjmp, pointer arithmetic │    │
│  └──────────┬──────────────────┘    │
│             │                       │
│  ┌──────────▼──────────────────┐    │
│  │  Source-language metadata   │────│──→ syntax highlighting,
│  │  (class info, vtable,       │    │    diagnostics, IDE,
│  │   Python/Ruby/Rust origin,  │    │    pretty-printing
│  │   template source, etc.)    │    │
│  └─────────────────────────────┘    │
└─────────────────────────────────────┘
```

Every source language parser (C, C++, madc, future Python/Ruby/etc.) lowers to the C11 `node_t` spine. C++ classes become structs + free functions. Python classes become the same. The metadata preserves the source-language identity for round-tripping.

---

## Scope

This plan covers **Phase 1 only** — proving the concept and establishing the pipeline. Future plans will cover:

- **Phase 2:** Parser speed optimization (string interning, arena allocation, pre-materialization)
- **Phase 3:** `--std=` mode switching in `parser.cpp` (C78-C23, C++98-C++26)
- **Phase 4:** Canonical AST specification document
- **Phase 5:** Remove Gecko from the build (or keep as optional for experimental grammars)

## File Structure

| File | Responsibility |
|------|---------------|
| `/workspace/mir/c2mir/c2mir.h` | **New.** Public libc2mir API header — exposes node creation, check, gen_mir |
| `/workspace/mir/c2mir/c2mir.c` | **Modify.** Remove `static` from ~15 functions, add `c2mir_` prefix |
| `src/madc_cir.h` | **New.** CIR translation interface — TokenBase AST → c2mir node_t |
| `src/madc_cir.cpp` | **New.** CIR translation implementation |
| `src/madc_mir_backend.cpp` | **Modify.** Replace text-based c2mir_compile with direct node_t feeding |
| `src/madc.cpp` | **Modify.** Wire new pipeline: parse → CIR translate → check → gen |
| `tests/unit/test_cir.cpp` | **New.** Unit tests for CIR translation |
| `tmp/poc_libc2mir.c` | **Scratch.** Proof-of-concept: build node_t externally, feed to check+gen |

---

### Task 1: Proof of Concept — Build c2mir node_t Externally

The first task is purely experimental: can we create c2mir AST nodes from outside `c2mir.c` and feed them to the checker and generator? This validates the entire approach before we commit to it.

**Files:**
- Modify: `/workspace/mir/c2mir/c2mir.c` (remove `static` from key functions)
- Create: `/workspace/mir/c2mir/c2mir_api.h` (minimal public header)
- Create: `tmp/poc_libc2mir.c` (proof of concept program)

- [ ] **Step 1: Identify the minimal set of c2mir functions to expose**

Read `c2mir.c` and list every function needed to:
1. Create a `c2m_ctx` (the c2mir context)
2. Create nodes (`new_node`, `new_node1`...`new_node5`, `new_str_node`, `new_pos_node`)
3. Create the top-level structure (`N_MODULE` node with `N_FUNC_DEF` children)
4. Run the context checker (`check` or `do_context`)
5. Run the generator (`gen_mir`)
6. Access the typedef tracking API (`tpname_add`, `tpname_find`)
7. Access the symbol table

Search for these functions in c2mir.c. Document their signatures, line numbers, and dependencies. Record which internal types (`c2m_ctx_t`, `node_t`, `node_code_t`, etc.) must also be exposed.

- [ ] **Step 2: Create the minimal c2mir API header**

Create `/workspace/mir/c2mir/c2mir_api.h` with forward declarations for:
- `c2m_ctx_t` (the context type)
- `node_t` (AST node)
- `node_code_t` (node type enum)
- Node creation functions
- The `check` and `gen_mir` entry points
- A `c2mir_init_ctx()` function that creates a c2m_ctx without starting the preprocessor/parser

This header should be the MINIMAL surface — only what we need to build and feed nodes.

- [ ] **Step 3: Remove `static` from the identified functions**

In `c2mir.c`, remove the `static` keyword from each function identified in Step 1. Add the `c2mir_` prefix to avoid namespace collisions (e.g., `new_node` → `c2mir_new_node`). Update all internal call sites of the renamed functions.

- [ ] **Step 4: Write the proof-of-concept program**

Create `tmp/poc_libc2mir.c` that:
1. Initializes a MIR context via `MIR_init()`
2. Initializes a c2mir context via `c2mir_init_ctx()`
3. Builds the AST for `int main(void) { return 42; }` using `c2mir_new_node()` calls:
   - `N_MODULE` containing `N_FUNC_DEF`
   - `N_FUNC_DEF` with: specifiers (`N_INT`), declarator (`main`), empty params, body
   - Body: `N_BLOCK` containing `N_RETURN` with `N_I` (integer literal 42)
4. Calls `c2mir_check()` on the tree
5. Calls `c2mir_gen_mir()` to produce MIR
6. Uses `MIR_gen()` to JIT the function
7. Calls the JIT'd function and verifies it returns 42
8. Prints "POC SUCCESS: got 42"

- [ ] **Step 5: Build and run the PoC**

```bash
# Compile PoC against our MIR fork
gcc -o tmp/poc_libc2mir tmp/poc_libc2mir.c \
    -I/workspace/mir -I/workspace/mir/c2mir \
    /workspace/mir/libmir.a -ldl -lm -lpthread
./tmp/poc_libc2mir
```

Expected output: `POC SUCCESS: got 42`

If this fails, document exactly what broke — missing types, missing state initialization, checker expectations. This tells us how much surgery c2mir needs.

- [ ] **Step 6: Evaluate and document findings**

Write a brief evaluation (in this plan file or a separate `docs/research/libc2mir-poc.md`):
- Did the PoC work? If not, what was the blocker?
- How many functions needed to be exposed?
- How much internal state does `check()` need that's hard to set up externally?
- Is the c2mir AST approach viable, or should we fall back to direct MIR emission?
- Decision: proceed with libc2mir, or pivot to direct MIR API?

- [ ] **Step 7: Commit the PoC**

```bash
cd /workspace/mir
git add c2mir/c2mir.c c2mir/c2mir_api.h
git commit -m "feat: expose minimal c2mir node API for external AST construction (PoC)"
cd /workspace/madc
git add tmp/poc_libc2mir.c
git commit -m "research: libc2mir proof of concept — build c2mir AST externally"
```

---

### Task 2: Translate a Simple C Function (TokenBase → node_t)

Once the PoC validates that c2mir accepts externally-built nodes, this task proves we can translate our parser's AST into c2mir nodes. Start with the simplest possible function.

**Files:**
- Create: `src/madc_cir.h`
- Create: `src/madc_cir.cpp`
- Create: `tests/unit/test_cir.cpp`

- [ ] **Step 1: Define the CIR translation interface**

Create `src/madc_cir.h`:
```cpp
#ifndef __MADC_CIR_H
#define __MADC_CIR_H 1

// Forward declarations
struct node;
typedef struct node *node_t;
struct c2m_ctx;
typedef struct c2m_ctx *c2m_ctx_t;
class TokenBase;
class TokenCpnd;
class Program;

// Initialize a c2mir context for CIR translation (no preprocessing/parsing)
c2m_ctx_t cir_init(MIR_context_t mir_ctx);

// Translate a madc TokenBase AST into a c2mir node_t tree
node_t cir_translate(c2m_ctx_t c2m_ctx, Program *prog);

// Run c2mir's type checker on the translated tree
int cir_check(c2m_ctx_t c2m_ctx, node_t root);

// Run c2mir's MIR generator on the checked tree
int cir_gen(c2m_ctx_t c2m_ctx, node_t root);

// Clean up
void cir_finish(c2m_ctx_t c2m_ctx);

#endif
```

- [ ] **Step 2: Implement minimal translation for `int main() { return N; }`**

Create `src/madc_cir.cpp` with:
- `cir_translate_expr()` — handles integer literals (`TokenInt` → `N_I`)
- `cir_translate_return()` — handles return statements (`N_RETURN`)
- `cir_translate_block()` — handles compound statements (`TokenCpnd` → `N_BLOCK`)
- `cir_translate_func()` — handles function definitions (`TokenFunc` → `N_FUNC_DEF`)
- `cir_translate()` — top-level: walks `TokenProgram::statements`, builds `N_MODULE`

Each function walks the madc TokenBase tree and builds the corresponding c2mir `node_t` subtree using the exposed `c2mir_new_node*()` functions.

- [ ] **Step 3: Write the unit test**

Create `tests/unit/test_cir.cpp`:
```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

TEST_CASE("CIR: translate int main() { return 42; }") {
    // 1. Tokenize + parse "int main() { return 42; }" via parser.cpp
    // 2. Call cir_translate() on the resulting AST
    // 3. Call cir_check() + cir_gen() to produce MIR
    // 4. JIT and call the function
    // 5. CHECK(result == 42)
}
```

- [ ] **Step 4: Build and run**

```bash
make -C src test
```

Expected: test passes, function returns 42 through the full pipeline without any C11 text generation.

- [ ] **Step 5: Commit**

```bash
git add src/madc_cir.h src/madc_cir.cpp tests/unit/test_cir.cpp
git commit -m "feat: CIR translation layer — TokenBase AST to c2mir node_t (int main return N)"
```

---

### Task 3: Expand CIR Translation — Expressions and Variables

Incrementally add support for the constructs needed to pass basic tests.

**Files:**
- Modify: `src/madc_cir.cpp`
- Modify: `tests/unit/test_cir.cpp`

- [ ] **Step 1: Add binary expression translation**

Implement `cir_translate_expr()` cases for:
- `TokenAdd` → `N_ADD(left, right)`
- `TokenSub` → `N_SUB(left, right)`
- `TokenMul` → `N_MUL(left, right)`
- `TokenDiv` → `N_DIV(left, right)`
- `TokenAssign` → `N_ASSIGN(left, right)`

Each walks `left`/`right` recursively.

- [ ] **Step 2: Add variable declaration translation**

Implement translation for:
- Local variable declarations (`int x;` → `N_DECL` with `N_INT` specifier + declarator)
- Variable references (`TokenVar` → `N_ID` with the variable name)
- Initializers (`int x = 5;` → `N_DECL` with `N_ASSIGN` initializer)

- [ ] **Step 3: Add function call translation**

Implement:
- `TokenCallFunc` → `N_CALL` with function ref + argument list
- External function references (printf, etc.) → `N_ID` referencing the declaration

- [ ] **Step 4: Write tests for each construct**

Add test cases to `tests/unit/test_cir.cpp`:
```cpp
TEST_CASE("CIR: arithmetic expression") {
    // int main() { return 2 + 3 * 4; }
    // Expected: returns 14
}

TEST_CASE("CIR: local variable") {
    // int main() { int x = 10; return x + 1; }
    // Expected: returns 11
}

TEST_CASE("CIR: function call") {
    // int add(int a, int b) { return a + b; }
    // int main() { return add(3, 4); }
    // Expected: returns 7
}
```

- [ ] **Step 5: Run tests and commit**

```bash
make -C src test
git add src/madc_cir.cpp tests/unit/test_cir.cpp
git commit -m "feat: CIR translation — expressions, variables, function calls"
```

---

### Task 4: Expand CIR — Control Flow

**Files:**
- Modify: `src/madc_cir.cpp`
- Modify: `tests/unit/test_cir.cpp`

- [ ] **Step 1: Add if/else translation**

`TokenIF` → `N_IF(condition, then_body, else_body)`

- [ ] **Step 2: Add while/for loop translation**

- `TokenWhile` → `N_WHILE(condition, body)`
- `TokenFor` → `N_FOR(init, condition, increment, body)`

- [ ] **Step 3: Add switch/case translation**

`TokenSWITCH` → `N_SWITCH(expr, body)` with `N_CASE` labels

- [ ] **Step 4: Write tests, run, commit**

---

### Task 5: Expand CIR — Structs, Pointers, Arrays

**Files:**
- Modify: `src/madc_cir.cpp`
- Modify: `tests/unit/test_cir.cpp`

- [ ] **Step 1: Add struct definition translation**

`DataDefSTRUCT` → `N_STRUCT` with `N_MEMBER` children

- [ ] **Step 2: Add pointer operations**

- Pointer declaration → `N_POINTER` declarator
- Dereference (`*p`) → `N_DEREF`
- Address-of (`&x`) → `N_ADDR`
- Arrow member access (`p->x`) → `N_DEREF_FIELD`

- [ ] **Step 3: Add array operations**

- Fixed arrays → `N_ARR_DECLARATOR` with size
- Array subscript → `N_IND` (indexing)
- Array initializers → `N_LIST` of `N_ASSIGN`

- [ ] **Step 4: Write tests, run, commit**

---

### Task 6: Expand CIR — C++ Lowering (Classes → Structs)

This is where the "dual AST" concept comes in. C++ constructs are lowered to C11 `node_t` equivalents.

**Files:**
- Modify: `src/madc_cir.cpp`
- Create: `src/madc_cir_cpp.cpp` (C++ lowering helpers)

- [ ] **Step 1: Class → struct lowering**

Walk class definitions from the TokenBase AST:
- Class → `N_STRUCT` with same fields
- Methods → free functions with `__this` pointer as first parameter
- Constructor → `ClassName__ClassName(self, args)` function
- Destructor → `ClassName__dtor(self)` function
- Register in a class metadata table (for tooling/diagnostics)

- [ ] **Step 2: new/delete → calloc/free + ctor/dtor**

- `new Foo(args)` → `calloc(1, sizeof(struct Foo))` + `Foo__Foo(ptr, args)`
- `delete p` → `Foo__dtor(p)` + `free(p)`

- [ ] **Step 3: Virtual dispatch → vtable struct + indirect call**

- Virtual methods → function pointer in vtable struct
- Virtual call → `((vtable*)obj->__vptr)->method(obj, args)`

- [ ] **Step 4: Write tests, run, commit**

---

### Task 7: Wire the New Pipeline into madc.cpp

Replace the Gecko pipeline with the CIR pipeline.

**Files:**
- Modify: `src/madc.cpp`
- Modify: `src/madc_mir_backend.cpp`

- [ ] **Step 1: Add the CIR pipeline path**

In `madc.cpp`, alongside the existing `use_mir_backend` block:
```cpp
if (use_mir_backend) {
    // NEW: Parse with original parser, translate to CIR, feed c2mir
    if (!prog->parse(tp))
        return 1;
    MIR_context_t mir_ctx = MIR_init();
    c2m_ctx_t c2m_ctx = cir_init(mir_ctx);
    node_t cir_root = cir_translate(c2m_ctx, prog.get());
    if (cir_check(c2m_ctx, cir_root))
        return 1;
    cir_gen(c2m_ctx, cir_root);
    // JIT and execute...
}
```

- [ ] **Step 2: Keep the old Gecko pipeline as `--backend=gecko` fallback**

Don't delete the Gecko path — keep it accessible via `--backend=gecko` for comparison and regression testing. The default switches to the CIR path.

- [ ] **Step 3: Run the full test suite**

```bash
make -C src fulltest
```

Compare pass counts between old (Gecko) and new (CIR) pipelines. Document any regressions.

- [ ] **Step 4: Run SMAUG through the new pipeline**

```bash
bin/madc --std=c MadSMAUG/src/SMAUG.mad
```

The typedef ambiguity should be gone — the original parser handles it correctly. Document the result.

- [ ] **Step 5: Commit**

```bash
git add src/madc.cpp src/madc_mir_backend.cpp
git commit -m "feat: wire CIR pipeline — parser.cpp → c2mir node_t → MIR (Gecko now fallback)"
```

---

### Task 8: Preserve --emit-c for Debugging

The ability to emit C11 text is still useful for debugging. Rather than deleting the old emitter, add a CIR → C11 text dumper that walks `node_t` trees.

**Files:**
- Create: `src/madc_cir_dump.cpp`

- [ ] **Step 1: Write a c2mir node_t → C11 text printer**

Walk the `node_t` tree and emit readable C11 text. This is much simpler than the current Gecko-based emitter because:
- The tree is already in C11 shape (lowered from C++)
- c2mir's node codes map directly to C11 syntax
- No ambiguity reconstruction needed

- [ ] **Step 2: Wire to `--emit-c` flag**

- [ ] **Step 3: Verify output matches what c2mir would parse**

- [ ] **Step 4: Commit**

---

## Self-Review

**Spec coverage:**
- PoC validation of libc2mir approach: Task 1
- Incremental CIR translation (expressions → control flow → structs → C++): Tasks 2-6
- Pipeline wiring: Task 7
- Debug output preservation: Task 8
- `--std=` mode switching: NOT in this plan (Phase 3)
- Parser speed optimization: NOT in this plan (Phase 2)
- Python/Ruby/other parsers: NOT in this plan (future, but architecture supports it)

**Placeholder scan:** All tasks have concrete steps. Tasks 3-6 are less detailed on exact code because the c2mir node API shapes will be discovered during Task 1 (the PoC). The PoC findings will inform the exact function signatures.

**Type consistency:** `c2m_ctx_t`, `node_t`, `node_code_t` used consistently throughout. `cir_*` prefix for all new public functions.

**Risk:** Task 1 (the PoC) is the highest risk. If c2mir's `check()` requires too much internal state that can't be set up externally, we may need to pivot to direct MIR emission (Path B). The plan is structured so Task 1 answers this question before significant implementation work begins.
