# Typed-Register IR Plan

**Status:** Active — Stage 0 (scaffolding) complete, Stage 1 next.
**Date:** 2026-05-25 (revised from 2026-04-24 draft)
**Research:** See `docs/research/jit-ir-design-2026.md` for the full
cross-reference with V8, HotSpot, LuaJIT, MIR, dstogov/ir, RyuJIT,
PyPy, Julia, Cranelift, TPDE, GCC, and LLVM.

---

## Design Goals (Priority Order)

1. **Blazing fast runtime** — generated machine code quality approaching
   MIR's 91% of GCC -O2 benchmark.
2. **Fast JIT compilation** — minimize overhead between source and
   running code. Obj-caching amortizes this further.
3. **Binary output at reasonable speed** — AOT/EXE path shares the same
   IR pipeline.
4. **Multi-architecture support** — Linux x86-64 first, then macOS
   ARM64, iOS, Android. The IR is the portability boundary.

## Key Design Decision: MIR as Backend

After researching V8, HotSpot, LuaJIT, RyuJIT, PyPy, Julia, Cranelift,
TPDE, GCC, LLVM, dstogov/ir, and MIR, the conclusion is clear:
**don't reinvent the optimizer.** MIR (MIT-licensed, ~16K lines of C)
already provides:

- 12 optimization passes (inlining, GVN/CSE, DCE, SCCP, LICM, copy
  propagation, register coalescing, instruction combining)
- 91% of GCC -O2 code quality
- 5 architectures (x86-64, aarch64, ppc64, s390x, riscv64)
- 178x faster compilation than GCC -O2
- 175KB binary footprint (smaller than asmjit's ~557KB)

Building our own optimizer would be 10-15K lines of extremely tricky
code (SSA construction, GVN, DCE, LICM, register allocation with live
range splitting, instruction combining). MIR already has 4,162 commits
of this work done, written by a GCC register allocator author.

### asmjit → MIR Transition

The existing `IRBuilder` (load/store/coerce) is the abstraction seam.
Phase 1 ports all tokens to use IRBuilder. Phase 2 retargets IRBuilder
to emit MIR instructions instead of asmjit instructions. The token
`compile()` methods never change.

**What MIR doesn't have:** SIMD/vector types. madc's `__m128`/`__m256`/
`__builtin_shuffle` support must remain asmjit-based. This is a small,
contained surface (~5% of the compiler).

### Size and License Budget

| Component | Current (asmjit) | After (MIR) |
|-----------|-----------------|-------------|
| Backend binary | ~519KB | ~175KB |
| Backend source | ~80K lines C++ (Zlib) | ~21K lines C (MIT) |
| madc `cc.*` calls | 1,899 direct | 0 (all via IRBuilder) |
| Optimization passes | 0 | 12 |
| Architectures | 1 (x86-64) | 5 |
| Code quality | ~GCC -O0 | ~91% GCC -O2 |

SIMD paths may still link a minimal asmjit subset, but the bulk of
code generation moves to MIR.

## Resolved Design Questions

These were open in the 2026-04-24 draft:

1. **IR owner:** Per-function. IRBuilder takes a backend reference,
   lives as long as the function compilation. *(Already implemented.)*
2. **Node lifetime:** Emit-as-you-build for Phase 1. MIR manages its
   own node lifetime in Phase 2.
3. **Emit-as-you-build vs build-then-emit:** Start immediate (Phase 1),
   MIR handles buffered optimization (Phase 2).
4. **First token port:** TokenInt/TokenReal (simplest leaves), then
   TokenMember (most bugs).
5. **Feature flags:** Not needed. `builder.cc()` escape hatch allows
   per-token coexistence during migration.
6. **Optimization passes:** Provided by MIR. No need to build our own
   FOLD engine, CSE hash table, DCE, or LICM.

---

## Architecture

```
Source (.mad file)
    │
    ▼ Lexer → Parser → AST (Token tree)
    │
    ▼ Token::compile()
    │     │
    │     ▼ IRBuilder (abstract interface)
    │         │
    │         ├── IRBuilder_asmjit    [Phase 1: current, emit-as-you-build]
    │         │       └── load/store/coerce/binop/cmp/call → asmjit x86::Compiler
    │         │
    │         └── IRBuilder_mir       [Phase 2: optimizing backend]
    │                 ├── load/store/coerce/binop/cmp/call → MIR instructions
    │                 ├── MIR optimizer (12 passes, automatic)
    │                 │    ├── Function inlining
    │                 │    ├── SSA construction
    │                 │    ├── Global Value Numbering (CSE + const prop)
    │                 │    ├── Dead code elimination
    │                 │    ├── SCCP (sparse conditional const propagation)
    │                 │    ├── Loop-invariant code motion
    │                 │    ├── Copy propagation
    │                 │    ├── Register coalescing
    │                 │    └── Instruction combining
    │                 └── MIR code generator → native x86-64 / aarch64 / ...
    │
    ▼ SIMD-only path (retained)
        └── asmjit x86::Compiler (for __m128/__m256/__builtin_shuffle)
```

---

## What Already Exists (Stage 0 — Complete)

**Files:**
- `include/madc_ir.h` — `IRShape` enum (Reg/Mem/Imm/Addr), `IRValue`
  triple (Operand, DataDef*, IRShape), `IRBuilder` class declaration.
- `src/madc_ir.cpp` — 607 lines. `load()`, `store()`, `coerce()` fully
  implemented with all hard cases: sign/zero extension, float/double
  conversion, unsigned int→real halving trick, string→char* via
  `string_cstr`, narrow integer canonicalization.
- `tests/unit/test_ir.cpp` — 24 doctest cases covering load (8 shapes),
  store (3 shapes), coerce (13 conversions).

**Already in use:** `IRBuilder` is used in `compiler.cpp`,
`compiler_operators.cpp`, `compiler_control_flow.cpp`, and
`compiler_builtins.cpp` for specific paths (ternary merge, coercions,
bitfield stores, condition normalization). Not yet used for general
token compilation.

**Integration pattern:** Local `IRBuilder ir(pgm.cc)` per operation,
`ir_from_operand()` helper converts raw asmjit operands to IRValue,
chain operations (coerce→load→store), extract raw operand when done.

**Test baseline:** 475 integration tests, 261 unit tests. All green.

---

## Phase 1 — Token Migration (Correctness)

*Port all tokens to use IRBuilder. Eliminate operand-shape bugs.
This is prerequisite for any backend swap.*

### Why

Since v0.9.1, every operand-shape bug has the same root cause: a
`TokenXxx::compile()` got Mem-vs-Reg, narrow-vs-wide, pointer-vs-value,
or Gp-vs-Xmm wrong. The IR centralizes these decisions so they're made
once, correctly, in `load()`/`store()`/`coerce()`.

More importantly: **once all tokens go through IRBuilder, the backend
can be swapped from asmjit to MIR without touching any token code.**

### Stage 1A — Leaf Values (1 week)

Port value-producing tokens to return `IRValue` instead of bare
`asmjit::Operand`:

1. `TokenInt` / `TokenReal` / `TokenChar` — literal producers (simplest)
2. `TokenVar` — variable load, exercises the `load()` path
3. `TokenMember` — struct member access, the #1 bug source
4. `TokenAddrOf` — produces Addr shape
5. `TokenDeref` / `TokenDerefExpr` — Addr→Reg (dereference)
6. `TokenSubscript` / `TokenSubscriptExpr` — array/vector element

Each port: modify `compile()`/`operand()`, run `make -C src fulltest`,
commit. One token at a time. If a port regresses, revert it — don't
work around.

### Stage 1B — Add `binop()` and `cmp()` to IRBuilder (2-3 days)

Before porting arithmetic tokens, the builder needs:

```cpp
IRValue binop(BinOp op, const IRValue &lhs, const IRValue &rhs,
              DataDef *result_type);
IRValue cmp(CmpOp op, const IRValue &lhs, const IRValue &rhs);
```

Both normalize operands to Reg before emission. Unit tests for each
operation × type combination.

### Stage 2 — Arithmetic and Comparison (1 week)

Port operators:

1. `TokenAdd` / `TokenSub` / `TokenMul` / `TokenDiv` / `TokenMod`
2. Comparison ops (`==`, `!=`, `<`, `>`, `<=`, `>=`)
3. Bitwise ops (`&`, `|`, `^`, `<<`, `>>`)
4. Compound-assigns (`+=`, `-=`, `*=`, `/=`, `%=`, `<<=`, `>>=`, `&=`,
   `|=`, `^=`)

The `safeadd`/`safecmp`/`safemul` dispatch trees collapse — the builder
already normalized shapes.

### Stage 3 — Calls (1-2 weeks)

Add `call()` to IRBuilder:

```cpp
IRValue call(void *target, const FuncSignature &sig,
             const std::vector<IRValue> &args, DataDef *ret_type);
```

Port:
1. `TokenCallFunc` — regular function calls
2. `TokenCallMethod` — method dispatch (including virtual)
3. dlsym variadic path
4. Multi-return call sites
5. Struct-member function-pointer dispatch

This is the densest concentration of coercion bugs. The `call()` builder
handles varargs float→double promotion, string→char* coercion, and ABI
argument placement in one place.

### Stage 4 — Control Flow (1 week)

Port:
1. `TokenIf` / `TokenFor` / `TokenWhile` / `TokenDo`
2. `TokenSwitch` / `TokenTerQ`
3. Labels and branches become IR-managed

`regdp` reset rule becomes unnecessary — the IR owns dataflow between
sub-expressions.

### Stage 5 — Cleanup (2-3 days)

1. Remove `safemov`/`safeadd`/`safesub`/`safecmp` dispatch trees from
   `typesafe.cpp` — only the low-level emitter bodies remain.
2. Remove `resolveCompoundLHS` and `CompoundLHS` struct.
3. Remove `builder.cc()` escape hatch — all tokens now go through IR.

**Phase 1 total: 4-6 weeks.** Tests green after every commit.

---

## Phase 2 — MIR Backend Integration

*Swap the code generation backend from asmjit to MIR. Get 12
optimization passes and multi-architecture for free.*

### Why

asmjit's `x86::Compiler` does register allocation but zero
optimization. MIR provides inlining, GVN, DCE, SCCP, LICM, copy
propagation, register coalescing, and instruction combining — all in
16K lines of C. Building these ourselves would be 10-15K lines of
extremely tricky code and years of bug-fixing. MIR is MIT-licensed
and written by a GCC RA author.

### 2A — MIR Integration and Build System (1 week)

- Add MIR as a vendored dependency (or git submodule) under `lib/mir/`
- Update Makefile to build MIR's `.c` files alongside madc
- Add `./configure --with-mir` / `--without-mir` flag
- Verify MIR builds and its own test suite passes on our system
- Wire up `MIR_context_t` initialization in madc's startup

### 2B — IRBuilder Interface Extraction (3-5 days)

Extract the current `IRBuilder` into a base class with virtual methods:

```cpp
class IRBuilder {
public:
    virtual ~IRBuilder() = default;
    virtual IRValue load(const IRValue &src) = 0;
    virtual void store(const IRValue &dst, const IRValue &src) = 0;
    virtual IRValue coerce(const IRValue &src, DataDef *to) = 0;
    virtual IRValue binop(BinOp op, const IRValue &lhs,
                          const IRValue &rhs, DataDef *type) = 0;
    virtual IRValue cmp(CmpOp op, const IRValue &lhs,
                        const IRValue &rhs) = 0;
    virtual IRValue call(void *target, /* ... */) = 0;
    // branch, label, jump, etc.
};
```

Current implementation becomes `IRBuilder_asmjit : public IRBuilder`.
Token `compile()` methods see only `IRBuilder&`.

### 2C — IRBuilder_mir Implementation (3-4 weeks)

Implement `IRBuilder_mir` that emits MIR instructions:

```cpp
class IRBuilder_mir : public IRBuilder {
    MIR_context_t ctx_;
    MIR_item_t    func_;
public:
    IRValue load(const IRValue &src) override {
        // Emit MIR_MOV / MIR_FMOV / MIR_DMOV
        // with appropriate extension ops
    }
    void store(const IRValue &dst, const IRValue &src) override {
        // Emit MIR_MOV to memory operand
    }
    IRValue coerce(const IRValue &src, DataDef *to) override {
        // Emit MIR_I2F / MIR_F2I / MIR_I2D / MIR_D2I / etc.
    }
    IRValue binop(BinOp op, const IRValue &lhs,
                  const IRValue &rhs, DataDef *type) override {
        // Emit MIR_ADD / MIR_SUB / MIR_MUL / MIR_DIV / etc.
    }
    // ...
};
```

MIR type mapping:

| madc DataDef | MIR Type |
|-------------|----------|
| ddINT8/ddUINT8 | MIR_T_I8/MIR_T_U8 |
| ddINT16/ddUINT16 | MIR_T_I16/MIR_T_U16 |
| ddINT32/ddUINT32 | MIR_T_I32/MIR_T_U32 |
| ddINT64/ddUINT64 | MIR_T_I64/MIR_T_U64 |
| ddFLOAT | MIR_T_F |
| ddDOUBLE | MIR_T_D |
| pointers | MIR_T_P |

### 2D — External Function Binding (1 week)

MIR needs external functions registered via `MIR_load_external()`.
madc uses many runtime helpers (string_construct, string_destruct,
string_cstr, printf, dlsym fallbacks, etc.). Build the binding table
that registers all of these with MIR's linker.

### 2E — Validation and Switchover (1-2 weeks)

- Run `make -C src fulltest` with MIR backend
- Compare runtime output of every test: MIR vs asmjit
- Benchmark: compile time and runtime speed on SMAUG
- Fix any MIR-specific issues
- Make MIR the default, keep `--with-asmjit` for SIMD fallback

**Phase 2 total: 6-8 weeks.** Depends on Phase 1 being complete.

---

## Phase 3 — Multi-Architecture (Nearly Free with MIR)

*MIR already supports aarch64. The remaining work is platform
integration, not code generation.*

### 3A — ARM64 / macOS JIT (2-3 weeks)

MIR already generates aarch64 code. Remaining work:
- Test madc + MIR on macOS/ARM64
- Handle Apple-specific variadic calling convention differences
- Platform detection in configure/Makefile
- libc++ instead of libstdc++ on macOS

### 3B — macOS Platform Integration (2-3 weeks)

Not code generation — infrastructure:
- Mach-O object format for AOT binary output
- Compact unwind tables (`__unwind_info`)
- SDK/sysroot integration (`xcrun`)
- dyld / two-level namespace symbol resolution

See `inbox/ir_layer_cross_platform.md` for the full Linux→macOS delta.

### 3C — iOS / Android (future, effort TBD)

MIR supports aarch64. iOS and Android are aarch64 targets with
additional platform constraints (code signing, sandboxing, NDK).

**Phase 3 total: 4-6 weeks** for macOS/ARM64. Much less than the
15-25 weeks estimated when we planned to build our own ARM64 backend.

---

## Phase 4 — Advanced Optimization (Peak Performance)

*Contribute to MIR or add madc-specific optimizations on top.*

### 4A — MIR Optimization Level Tuning (1-2 weeks)

MIR supports optimization levels 0-2. Profile madc workloads at each
level. Determine the right default for JIT mode (likely level 1 —
fast compile with core optimizations) vs AOT mode (level 2 — full
optimization suite).

### 4B — SIMD Support in MIR (contribution, 4-6 weeks)

If madc's SIMD usage grows beyond the current `__m128`/`__m256` paths,
contributing vector type support to MIR benefits both projects. MIR's
architecture (typed operands, per-arch emitters) is ready for this.

### 4C — Allocation Sinking / Escape Analysis (3-4 weeks)

Track whether heap allocations (MadArray, std::string temporaries,
`new`-allocated objects) escape the current scope. If they don't,
eliminate the allocation. LuaJIT and PyPy identify this as their
highest-impact optimization. Could be contributed to MIR or
implemented as a pre-pass before MIR emission.

### 4D — Profile-Guided Optimization (future)

MIR supports LBBV (Lazy Basic Block Versioning) for speculative
optimization with automatic deoptimization. This could enable
type-specialized code paths for madc's dynamic features (MadValue,
dlsym calls) without the complexity of tiered compilation.

**Phase 4 total: selective, 4-12 weeks depending on scope.**

---

## Phase 5 — SIMD Migration (Long-Term)

*Eliminate asmjit dependency entirely.*

Once MIR gains vector type support (via our contribution or upstream),
migrate the remaining SIMD paths from asmjit to MIR. At that point,
asmjit is no longer a dependency and the binary shrinks further.

**Phase 5 total: 2-4 weeks** (assuming vector support exists in MIR).

---

## Summary Timeline

| Phase | Work | Effort | Prerequisite |
|-------|------|--------|--------------|
| **1** | Token migration to IRBuilder (correctness) | 4-6 wk | Stage 0 (done) |
| **2** | MIR backend integration (optimization + multi-arch) | 6-8 wk | Phase 1 |
| **3** | macOS/ARM64 platform support | 4-6 wk | Phase 2 |
| **4** | Advanced optimization (selective) | 4-12 wk | Phase 2 |
| **5** | SIMD migration to MIR (eliminate asmjit) | 2-4 wk | MIR vector support |

Phases 3 and 4 can overlap. Phase 5 depends on upstream MIR work.

**Total to production-quality optimizing JIT on x86-64:** ~10-14 weeks
(Phase 1 + 2). Compare to the previous plan's 18-30 weeks for building
our own optimizer + multi-arch backends.

---

## MIR Maintenance Strategy

MIR is MIT-licensed, ~16K lines of C, single maintainer (Vladimir
Makarov, GCC RA author at Red Hat). Both MIR and asmjit show reduced
commit activity in 2024-2025.

**Risk mitigation:**
- MIR is small enough to fork and maintain (~16K lines vs asmjit's
  ~80K lines)
- Contributing SIMD support (Phase 4B) builds relationship with
  upstream
- MIR's clean architecture makes targeted fixes feasible
- If MIR becomes unmaintained, the IRBuilder interface means we could
  swap to another backend (Cranelift, custom, etc.) without touching
  token code

---

## Verification

- `make -C src fulltest` after every commit (475+ tests green)
- Key correctness tests: all existing integration tests, plus specific
  tests for each ported token
- Performance measurement: compile a reference program (SMAUG or a
  synthetic benchmark) before/after each phase, track both compile
  time and runtime
- Compare generated code against GCC -O0 and -O2 at each phase to
  measure code quality progress toward the 91% MIR benchmark
- A/B testing: run fulltest under both asmjit and MIR backends during
  Phase 2 to catch behavioral differences

## Files to Modify/Create

| Phase | Files |
|-------|-------|
| 1 | `include/madc_ir.h` (add binop/cmp/call), `src/madc_ir.cpp` (implement), `src/compiler*.cpp` (port tokens), `tests/unit/test_ir.cpp` (new cases) |
| 2 | `lib/mir/` (vendored), `Makefile` (build MIR), `src/madc_ir_mir.cpp` (new MIR backend), `include/madc_ir.h` (abstract base), `src/madc_ir.cpp` → `src/madc_ir_asmjit.cpp` (rename) |
| 3 | Platform files, configure script, Mach-O support |
| 4 | MIR contributions, madc-specific pre-passes |
| 5 | Migrate SIMD paths from asmjit to MIR |
