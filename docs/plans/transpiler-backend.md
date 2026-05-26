# madc Next-Generation Architecture: Gecko + MIR Transpiler

## Context

madc currently has a three-phase pipeline: lexer (4.2K lines) → parser (16K lines) → compiler backend (23K lines emitting x86-64 via asmjit). The compiler backend is 100% coupled to asmjit and x86-64 — no optimization passes, single architecture, 519KB binary overhead.

After extensive research (13 JIT engines cross-referenced, MIR benchmarked at 3.4x faster than madc+asmjit, c2mir architecture studied, Ruby mirjit.c transpiler pattern analyzed, Gecko parser proof-of-concept passing 15/15 madc grammar tests), the decision is:

**Replace both the parser and the compiler backend in a single architectural move.**

- **Parser:** Replace 16K-line recursive descent parser with Gecko GLR parser (3.8K lines, MIT, 2M lines/sec, handles ambiguous grammars natively) + a semantic analysis walk.
- **Backend:** Replace 23K-line asmjit compiler with a ~3K-line C11 text emitter that feeds c2mir → MIR for optimized JIT/AOT compilation.

This is proven by:
- **Cfront (1983-1990):** C++ transpiled to C for a decade.
- **Ruby mirjit.c:** 3,253 lines generating C strings fed through c2mir → MIR.
- **Gecko proof-of-concept:** 15/15 madc constructs parsed (variables, functions, classes, inheritance, namespaces, try/catch, operators, structs, pointers, references).

The new pipeline:

```
madc source → [madc tokenizer] → tokens
  → [Gecko 3.8K] → raw AST
  → [semantic walk] → typed AST (DataDefs, scopes, symbols)
  → [C emitter ~3K] → C11 text
  → [c2mir 14K] → MIR
  → [MIR optimizer 12 passes] → machine code (x86-64, aarch64, ppc64, s390x, riscv64)
```

**Total new pipeline:** ~25K lines (mostly MIT-licensed, battle-tested Makarov code)
**Replaced:** ~39K lines of hand-written madc internals (parser + compiler + typesafe + IR + ELF)

## What Stays, What Changes, What Goes

| Component | Lines | Fate | Why |
|-----------|-------|------|-----|
| lexer.cpp | 4,243 | **SIMPLIFIED** → tokenizer callback | Keep tokenization + preprocessor (#include, #define, #load). Strip AST-building. Feed tokens to Gecko via `read_token()` callback. |
| parser.cpp | 16,047 | **REPLACED** by Gecko grammar + semantic walk | Gecko handles syntax; new `madc_sema.cpp` does type resolution, variable registration, scope management. |
| compiler.cpp | 8,912 | **REPLACED** by C emitter | `madc_emit_c.cpp` walks typed AST, emits C11 text. |
| compiler_operators.cpp | 5,814 | **REPLACED** | Operators become C expressions. Type promotion is C's job. |
| compiler_control_flow.cpp | 1,155 | **REPLACED** | if/while/for → identical C. try/catch → setjmp/longjmp (already implemented). |
| compiler_builtins.cpp | 1,296 | **REPLACED** | Builtins become C stdlib calls or extern wrappers. |
| typesafe.cpp | 1,729 | **DELETED** | C handles type conversions natively. |
| madc_ir.cpp | 606 | **DELETED** | No IR layer needed — c2mir handles lowering. |
| madc_elf.cpp | 3,347 | **DELETED** | c2mir/MIR handles object output natively. |
| tokens.h | 1,483 | **SIMPLIFIED** | Token classes stay for lexer output; compile()/operand() methods removed. |
| datadef.h | 1,250 | **STAYS** | Type system is sound and reusable. Semantic walk needs it. |
| madc.h | 1,590 | **MODIFIED** | Remove asmjit state (cc, jit, CodeHolder). Add Gecko grammar, MIR context. |
| ns_*.cpp | ~2,500 | **STAYS** as runtime helpers | C++ wrapper functions become import_resolver entries for c2mir. |
| include/madc/* | embedded headers | **STAYS** | Still needed for #include resolution. |

## External Dependencies

| Component | Source | License | Size | Role |
|-----------|--------|---------|------|------|
| Gecko | /workspace/gecko | MIT | 3.8K SLOC | GLR parser |
| MIR | /workspace/mir (fork) | MIT | ~16K SLOC | Optimizer + codegen |
| c2mir | /workspace/mir/c2mir | MIT | ~14K SLOC | C11 → MIR compiler |
| asmjit | /usr/local | Zlib | ~80K SLOC | **REMOVED** (except SIMD, deferred) |

## Architecture Design

### A. Tokenizer (lexer.cpp → simplified)

The existing lexer becomes a pure tokenizer. It keeps:
- Character-by-character lexing (`_getToken()`)
- Preprocessor: `#include`, `#define`, `#ifdef`, `#load`
- Macro expansion (object-like and function-like)
- Embedded header resolution
- Source location tracking (file, line, column)

It loses:
- All AST-building code (that's Gecko's job now)
- `parseExpression()`, `parseStatement()`, `parseCompound()` — all gone
- Token subclass `parse()` methods — gone

It gains:
- A `read_token()` callback for Gecko: maps internal token types to Gecko terminal codes
- Token attribute storage: each token's text, value, and source location stored for the semantic walk to retrieve

### B. Gecko Grammar (`madc_grammar.cpp`)

A single C++ file containing:
1. The madc grammar as a string constant (proven in POC, ~200 productions)
2. Terminal code declarations matching the lexer's token types
3. Operator precedence/associativity declarations
4. Abstract node translation annotations (`# name (children)`)
5. Rule guard callback for typedef/identifier ambiguity (same pattern as Gecko's ANSI C test)

The grammar covers: types, declarations, functions, expressions (with full C operator precedence), statements (if/while/for/do/switch/return/break/continue/goto), classes (with constructors, destructors, methods, virtual, inheritance), structs, enums, try/catch/throw, new/delete, namespaces (`ns::func`), pointers, references, arrays, casts, ternary, compound literals, designated initializers.

### C. Semantic Analysis Walk (`madc_sema.cpp`)

A `gp_tree_node` visitor that walks Gecko's AST and produces a **typed AST** — essentially the same data structures the current parser produces (DataDef, Variable, FuncDef, scope chains) but populated from Gecko nodes instead of during recursive descent.

Key responsibilities:
1. **Type resolution:** When visiting a `decl` node, look up the type specifier in `datadef_map`, create DataDef entries for new types.
2. **Variable registration:** Create Variable objects, assign to scope (`TokenCpnd::variables` or `variable_map`).
3. **Function registration:** Create FuncDef entries, register in `funcdef_map`.
4. **Scope management:** Track compound statement nesting, push/pop scopes.
5. **Class/struct building:** Populate DataDefCLASS/DataDefSTRUCT members, methods, vtable slots, inheritance chain.
6. **Namespace resolution:** Map `ns_call` nodes to namespace function lookups.
7. **Constant folding:** Evaluate constant expressions at semantic time.
8. **Lazy symbol loading:** Trigger `lazy_resolve()` for standard library symbols.

Output: The existing Program data structures (funcdef_map, variable_map, datadef_map, namespace_map) populated and ready for the C emitter.

### D. C Text Emitter (`madc_emit_c.cpp`)

Walks the typed AST and emits C11 text into a buffer, following the Ruby mirjit.c pattern:

```cpp
// Buffer + output macro (from mirjit.c pattern)
class CEmitter {
    std::string buf;
    void O(const char *fmt, ...) { /* vsnprintf append */ }

    // Three-section output:
    std::string forward_decls;   // function prototypes, extern declarations
    std::string type_defs;       // struct/typedef definitions
    std::string func_bodies;     // function implementations
};
```

Key translation patterns:

| madc construct | C11 output |
|----------------|------------|
| `int x = 42;` | `int64_t x = 42;` |
| `string s = "hello";` | `madc_string_t s; string_construct(&s); string_assign_cstr(&s, "hello");` |
| `class Foo { ... };` | Two structs: `Foo_instance_t` + `Foo_vtable_t` |
| `obj.method(args)` | `ClassName__methodName(&obj, args)` |
| `new Foo()` | `Foo_instance_t *p = malloc(sizeof(Foo_instance_t)); Foo__ctor(p);` |
| `delete p` | `Foo__dtor(p); free(p);` |
| `try { } catch { }` | `setjmp/longjmp` with `MadcCleanupEntry` stack (already implemented) |
| `php::strlen(s)` | `__php_strlen(&s)` (extern, resolved via import_resolver) |
| `virtual void f()` | Indirect call through `__vptr->slot[N]` |
| `#line` directives | Emitted for source mapping |

### E. c2mir Integration (`madc_mir_backend.cpp`)

The glue between the C emitter and MIR:

```cpp
// Following mirjit.c's 6-line pattern:
MIR_context_t ctx = MIR_init();
c2mir_init(ctx);

// Feed generated C text to c2mir
struct c2mir_options opts = {0};
c2mir_compile(ctx, &opts, getc_from_buffer, &buf, "madc_generated.c", NULL);

// Load, link, and generate
MIR_module_t mod = /* get module from context */;
MIR_load_module(ctx, mod);
MIR_link(ctx, MIR_set_gen_interface, import_resolver);
void *code = MIR_gen(ctx, main_func);

// Execute
typedef int (*main_fn_t)(void);
int result = ((main_fn_t)code)();
```

### F. Import Resolver (`madc_imports.cpp`)

Static table mapping function names to addresses, used by MIR's linker to resolve extern calls in the generated C:

```cpp
struct import_entry { const char *name; void *addr; };
static import_entry imports[] = {
    // C stdlib
    {"printf", (void*)printf},
    {"malloc", (void*)malloc},
    {"free", (void*)free},
    {"strlen", (void*)strlen},
    // madc runtime helpers
    {"string_construct", (void*)string_construct},
    {"string_destruct", (void*)string_destruct},
    {"string_assign", (void*)string_assign},
    {"string_cstr", (void*)string_cstr},
    // PHP namespace
    {"__php_strlen", (void*)php_strlen},
    {"__php_trim", (void*)php_trim},
    // ... ~45 runtime helpers + all namespace functions
};

void *import_resolver(const char *name) {
    for (auto &e : imports)
        if (strcmp(e.name, name) == 0) return e.addr;
    // Fallback: dlsym(RTLD_DEFAULT, name)
    return dlsym(RTLD_DEFAULT, name);
}
```

## Implementation Phases

### Phase 0: Foundation ✅ (complete)

Gecko vendored in `lib/gecko/`, MIR linked from `/workspace/mir/`,
unit tests for both (test_gecko.cpp, test_c2mir.cpp).

### Phase 1: Tokenizer Adaptation ✅ (complete)

`madc_grammar.cpp` (grammar string + terminal mapping),
`madc_tokenizer.cpp` (lexer→Gecko read_token adapter, contextual
keyword remapping, `std::string` collapsing, `__attribute__` skipping).

### Phase 2: Semantic Walk ✅ (complete)

`madc_sema.cpp/h` — SemaInfo struct with TypeClass enum, variable/
function type tracking, class info, inheritance, virtual methods,
operator overloads. Used by the emitter for type-aware code generation.

### Phase 3: C Emitter — Core ✅ (complete)

`madc_emit_c.cpp` — CEmitter class, two-buffer pattern (header + body),
full expression/statement/declaration emission. `madc_mir_backend.cpp` —
c2mir→MIR→execute pipeline with import resolver (dlsym). `--backend=mir`
and `--emit-c` CLI flags wired into `madc.cpp`. Test runner supports
`--backend=mir` passthrough.

### Phase 4: Strings & Objects ✅ (complete, 2026-05-26)

- [x] Itanium ABI name mangler (`madc_mangle.cpp/h`) — types, functions,
  methods, ctors, dtors, operators. 17 test cases, 73 assertions.
- [x] String assignment: `string_assign_cstr()` instead of bare `=`
- [x] fstream/sstream extern C wrappers (21 functions)
- [x] fstream method dispatch: `.open()/.close()/.good()` → wrapper calls
- [x] `<<`/`>>` chain extended for ofstream/ifstream variables
- [x] va_list: emit `va_start`/`va_end` instead of `__va_args`
- [x] Global string init/cleanup functions
- [x] STRING_T / `std::string` type mapping
- [x] `std::stoi/stod/to_string` runtime wrappers
- [x] `prefer calloc` support via existing `prefer` keyword

### Phase 5: Classes & OOP ✅ (complete, 2026-05-26)

- [x] Contextual keyword remapping in tokenizer (Cfront approach):
  CLASS→IDENT when not followed by IDENT/{, MAP/SET/VECTOR/LIST→IDENT
  when not followed by <
- [x] c2mir reserved word renaming: `safe_ident()` prefixes C++ keywords
  with `_` in generated C output
- [x] Operator overloading: emit operator methods as free functions,
  rewrite binary operator call sites to dispatch through them
- [x] Virtual dispatch: vtable structs, `__vptr` field, static vtable
  instances per class, vptr init in ctors, indirect dispatch at call sites
- [x] Inheritance: base-class cast for inherited method calls, base
  destructor chain in derived dtors
- [x] new/delete: correct `struct ClassName` (not `_t`), ctor/dtor calls
- [x] Dtor+return bracing fix
- [x] Function-call-as-declaration GLR disambiguation

**Baseline:** 315/475 transpiler tests (66.3%). 475/475 legacy. 10/10
Phase 5 target tests pass.

### Phase 6: C Emitter — Exceptions & Advanced
**Goal:** Emit C for try/catch/throw, namespaces, STL containers, and remaining features.

- [ ] Emit C for: setjmp/longjmp exception handling (MadcTryContext pattern)
- [ ] Emit C for: cleanup stack (MadcCleanupEntry) for destructor unwinding
- [ ] Emit C for: namespace function calls (ns_call → extern wrapper)
- [ ] Emit C for: STL containers (vector, map, set, list) via monomorphized
  typed wrappers (same pattern as legacy ns_stl.cpp)
- [ ] Emit C for: function pointers, typedef'd function types
- [ ] Emit C for: enum types
- [ ] Emit C for: compound literals, designated initializers
- [ ] Emit C for: casts (integer/float/pointer conversions)
- [ ] Run: exception/namespace/container/advanced tests pass

### Phase 7: Full Test Suite Green (1 week)
**Goal:** All 475+ integration tests pass on both backends.

- [ ] Fix remaining test failures one by one
- [ ] Verify `--emit-c` flag dumps correct C for all tests
- [ ] Verify error messages include correct file/line via `#line` directives
- [ ] `make -C src fulltest` green
- [ ] `bash scripts/run_tests.sh --backend=mir` green
- [ ] Performance benchmark: compile speed and runtime vs asmjit pipeline

### Phase 8: Cleanup & Documentation (2-3 days)
**Goal:** Remove dead code, update docs.

- [ ] Remove old compiler files once MIR backend reaches full parity
- [ ] Remove asmjit dependency from Makefile and includes
- [ ] Update ROADMAP.md, TODO.md, CHANGELOG.md, AGENTS.md
- [ ] Release new version

### Phase 9: Advanced C++ (future)
**Goal:** Broader C++ support toward the C++23 transpiler vision.

- [ ] Multiple inheritance (Itanium ABI layout, thunks)
- [ ] Template instantiation / monomorphization
- [ ] RTTI (`dynamic_cast`, `typeid`)
- [ ] Lambda expressions → closure struct + function
- [ ] Move semantics / rvalue references
- [ ] MIR SIMD support (requires libmir fork work)
- [ ] AOT ELF output via MIR
- [ ] Object caching (compiled MIR modules)

## AOT / Binary Output

MIR natively supports AOT output. The current madc_elf.cpp (3,347 lines) that manually constructs ELF files is replaced by:

```cpp
// MIR has its own binary output
MIR_write(ctx, output_file);  // Write MIR binary
// Or use MIR's built-in compilation to object file
```

For `.o` output: MIR can produce relocatable objects via its own mechanisms. For `.exe`: link with system linker (gcc/ld) as a post-step — same as any C compiler.

## SIMD

MIR does not support SIMD types. madc's `__m128`/`__m256`/`__builtin_shuffle` support (~5% of the compiler) is deferred. Options:
1. **Emit SIMD intrinsics in the C output** — c2mir may or may not handle them
2. **Keep asmjit for SIMD-only** — hybrid mode during transition
3. **Contribute SIMD to MIR** — longer-term, benefit the ecosystem
4. **Defer** — most madc programs don't use SIMD

## Risk Assessment

| Risk | Mitigation |
|------|-----------|
| Gecko is 1 month old | Author is GCC RA designer, MIR author. POC passed 15/15. MIT license means we can fix bugs ourselves. |
| Semantic walk is large new code | It's a cleaner rewrite of existing logic (parser.cpp's semantic analysis), not new functionality. Test-driven: compare output against current parser. |
| C emitter can't express some madc feature | Cfront compiled all of C++ to C for 7 years. madc is simpler than C++. SJLJ exceptions already work. |
| c2mir chokes on generated C | We control the C output — can adapt to c2mir's C11 subset. c2mir already handles Ruby's generated C. |
| Performance regression | MIR benchmarks show 3.4x faster runtime than asmjit. Compilation speed: c2mir is 178x faster than GCC. |
| Two-front war (parser + backend) | The fronts are decoupled: Gecko grammar can be tested independently of the C emitter. Semantic walk can be validated against current parser output before the emitter exists. |

## Files to Create

| File | Lines (est.) | Purpose |
|------|-------------|---------|
| `src/gecko/` | vendor | Gecko library (gecko.c + headers) |
| `src/madc_grammar.cpp` | ~400 | Grammar string + terminal mapping |
| `src/madc_tokenizer.cpp` | ~200 | Lexer → Gecko read_token adapter |
| `src/madc_sema.cpp` | ~3,000 | Semantic analysis walk |
| `src/madc_emit_c.cpp` | ~3,000 | C11 text emitter |
| `src/madc_mir_backend.cpp` | ~300 | c2mir → MIR → execute glue |
| `src/madc_imports.cpp` | ~200 | Import resolver table |
| `tests/unit/test_gecko.cpp` | ~100 | Gecko grammar unit tests |
| `tests/unit/test_mir_backend.cpp` | ~100 | MIR integration unit tests |

## Files to Modify

| File | Changes |
|------|---------|
| `src/Makefile` | Add gecko, MIR, new source files; remove old compiler objects |
| `include/madc.h` | Remove asmjit state, add MIR context, Gecko grammar |
| `include/tokens.h` | Remove compile()/operand() virtual methods |
| `src/lexer.cpp` | Simplify to pure tokenizer; remove AST-building |
| `src/madc.cpp` | Change pipeline: tokenize → gecko parse → sema → emit C → c2mir → run |

## Files to Delete (Phase 8)

| File | Lines | Why |
|------|-------|-----|
| `src/compiler.cpp` | 8,912 | Replaced by madc_emit_c.cpp |
| `src/compiler_operators.cpp` | 5,814 | Replaced by C expressions |
| `src/compiler_control_flow.cpp` | 1,155 | Replaced by C control flow |
| `src/compiler_builtins.cpp` | 1,296 | Replaced by C stdlib calls |
| `src/typesafe.cpp` | 1,729 | C handles type conversions |
| `src/madc_ir.cpp` | 606 | No IR layer needed |
| `src/madc_elf.cpp` | 3,347 | MIR handles binary output |
| `include/madc_ir.h` | 106 | No IR layer needed |
| **Total removed** | **22,965** | |

## Verification

After each phase:
- `make -C src test` — unit tests green
- `make -C src fulltest` — integration tests green (progressive: more tests pass each phase)
- `--emit-c` flag dumps generated C for manual inspection
- Compare generated C against `gcc -S -O0` output for correctness (GCC is canon)

Final verification:
- All 475+ integration tests pass
- All 261+ unit tests pass
- Performance benchmark shows ≥3x runtime improvement over asmjit
- `bin/madc` binary size is smaller (MIR 175KB vs asmjit 519KB)
- Multi-arch: aarch64 JIT works on ARM64 (if available for testing)

## Timeline Estimate

| Phase | Duration | Cumulative |
|-------|----------|-----------|
| Phase 0: Foundation | 1-2 days | 2 days |
| Phase 1: Tokenizer | 2-3 days | 5 days |
| Phase 2: Semantic Walk | 1-2 weeks | 2.5 weeks |
| Phase 3: C Emitter Core | 1-2 weeks | 4.5 weeks |
| Phase 4: Strings & Objects | 1 week | 5.5 weeks |
| Phase 5: Classes & OOP | 1 week | 6.5 weeks |
| Phase 6: Exceptions & Advanced | 1 week | 7.5 weeks |
| Phase 7: Full Test Suite | 1 week | 8.5 weeks |
| Phase 8: Cleanup | 2-3 days | 9 weeks |

## Future Work (post-Phase 8)

- **SIMD support in MIR:** Contribute `__m128`/`__m256` to upstream MIR
- **Gecko parser for C output validation:** Syntax-check generated C before feeding to c2mir
- **Grammar evolution:** As madc's language grows, Gecko's GLR handles new productions without parser rewrites
- **Multi-architecture testing:** ARM64, RISC-V, etc.
- **`--emit-mir` flag:** Dump MIR intermediate representation for debugging
- **Object caching:** Cache compiled MIR modules for unchanged source files
