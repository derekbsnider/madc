# JIT IR Design Research — Cross-Reference with Industry (2026)

**Date:** 2026-05-25
**Purpose:** Inform madc's typed-register IR design by studying how every
major JIT engine handles IR architecture, optimization, and multi-platform
support. This document captures the raw findings; the design decisions
derived from it live in `docs/plans/typed-register-ir.md`.

---

## Engines Studied

| Engine | Language | IR Type | Tiers | RA | Archs | Code Quality |
|--------|----------|---------|-------|-----|-------|--------------|
| V8 (Turboshaft) | JavaScript | CFG SSA | 4 | Linear scan | 9 | Best for JS |
| HotSpot C1 | Java | SSA CFG | 5 (shared) | Linear scan | Many | ~70% of C2 |
| HotSpot C2 | Java | Sea of Nodes | 5 (shared) | Graph coloring | Many | ~95% GCC -O2 |
| RyuJIT (.NET) | C#/IL | Linear IR (GenTree→SSA) | 2 | Linear scan | 4 | Good |
| LuaJIT | Lua | Linear SSA, 64-bit compact | 1 (trace) | Linear scan (backward) | 6 | ~95% GCC -O2 |
| MIR | C11 | SSA basic blocks | 1 | Linear scan | 5 | ~91% GCC -O2 |
| dstogov/ir | PHP | SoN-inspired, on-the-fly fold | 1 | Linear scan | 2 | 15M lines/sec |
| PyPy | Python (RPython) | Linear trace SSA | 2 | Per-arch | 6 | Good for dynamic |
| Julia | Julia | LLVM IR | 1+interp | (LLVM) | (LLVM) | Best, slow compile |
| Cranelift | Wasm/Rust | SSA CFG (CLIF) | 1 | Linear scan | 3 | ~86% LLVM |
| TPDE (2025) | Adapter-based | Adapts to existing IR | 1 pass | Integrated | Multi | On par -O0 |
| GCC | C/C++/etc | GIMPLE + RTL (two-level) | N/A (AOT) | Graph coloring (IRA) | Many | Reference |
| LLVM | Many | SSA three-address | N/A (AOT) | Graph coloring | Many | Reference |

---

## V8 (Google, JavaScript)

### Architecture

Four tiers: Ignition (interpreter/bytecode) → Sparkplug (baseline, no IR) →
Maglev (mid-tier SSA CFG) → TurboFan/Turboshaft (top-tier).

**Sparkplug** has zero IR — it translates bytecode to machine code in a single
linear pass. Per-architecture implementation (no shared lowering). Near-zero
compile time. Frame layout matches Ignition for free OSR.

**Maglev** uses a traditional SSA control-flow graph. Nodes are JS-aware
(`CheckedSmiAdd`, `LoadNamedProperty`). Built directly from bytecode + type
feedback in a single forward pass. Simple linear-scan RA. No inlining.

**TurboFan** (legacy) used Sea of Nodes. **Turboshaft** (replacement, 2022+)
uses CFG with typed operations (`WordBinopOp`, `FloatBinopOp`, `LoadOp`,
`StoreOp`). Still SSA. Compiles ~2x faster than TurboFan's SoN for the same
optimization quality.

**Turbolev** (in development 2025) replaces TurboFan's frontend entirely:
Maglev's CFG IR feeds directly into Turboshaft's backend. Full CFG pipeline
end-to-end.

### Sea of Nodes Retrospective (V8 blog, March 2025)

After ~10 years with Sea of Nodes in TurboFan, V8 moved away. Key lessons:

1. "It's too complex." State tracking is hard and expensive.
2. Scheduling is a liability — the scheduler must reconstruct linear order,
   often producing worse schedules than an AOT compiler.
3. Debugging is painful — no linear listing to step through.
4. Turboshaft CFG compiles ~2x faster for the same optimization quality.
5. Migration worked: Turboshaft was introduced as a backend first, then
   gradually absorbed optimization passes (incremental, not big-bang).

### Multi-Architecture

Shared IR for Maglev and Turboshaft. Per-architecture instruction selectors
(`instruction-selector-x64.cc`, `instruction-selector-arm64.cc`). Shared
linear-scan register allocator. Per-arch code generators. CodeStubAssembler
provides a portable assembly layer for builtins.

9 architectures: x64, arm, arm64, ia32, mips, mips64, ppc, s390, riscv64.

### Tier-Up Triggers

- Ignition → Sparkplug: near-immediate (first or second call)
- Sparkplug → Maglev: invocation count + backedge count threshold
- Maglev → TurboFan: higher threshold, stable type feedback required
- OSR for long-running loops
- Profile-Guided Tiering (2024): per-function decisions from actual profiles

---

## HotSpot JVM (Oracle/OpenJDK)

### C1 (Client Compiler)

Traditional SSA-based linear IR in basic blocks. HIR (SSA `Value` nodes in
`BlockBegin`/`BlockEnd` structures) → LIR (linear `LIR_Op` instructions) →
machine code. ~15 optimization passes including null check elimination, range
check elimination, constant folding, simple inlining, local value numbering.

### C2 (Server Compiler)

Sea of Nodes ("Ideal Graph"). Nodes represent both data flow AND control flow
in a single graph. Three edge types: value, effect, control. Nodes float
freely with no fixed schedule until the scheduling phase. ~30+ optimization
passes including aggressive inlining (up to 9 levels), iterative GVN, escape
analysis (huge for Java — eliminates heap allocation), lock elision, loop
vectorization. Graph-coloring RA (Chaitin-Briggs).

**170K lines of dense C++**. Widely considered one of the hardest codebases
in OpenJDK to modify.

**Pros:** Implicit code motion (loop-invariant values float above loops
naturally), trivial GVN, trivial DCE. ~95% of GCC -O2 quality.

**Cons:** Slow to compile (10-50x slower than C1 per method), O(n²) scheduling
in pathological cases, hard to debug, immense implementation complexity.

### 5 Tiers

| Tier | Compiler | Description |
|------|----------|-------------|
| 0 | Interpreter | Bytecode with profiling |
| 1 | C1 | Full opt, no profiling |
| 2 | C1 | Limited opt, with profiling |
| 3 | C1 | Full opt, with profiling |
| 4 | C2 | Aggressive optimization using profiles |

Typical path: 0 → 3 → 4.

### Multi-Architecture

Architecture Description Language (`.ad` files) define instruction patterns,
register sets, encoding rules. ADLC compiles these into a pattern-matching
tree. Per-platform `MacroAssembler` handles final encoding.

---

## RyuJIT (.NET CLR)

### IR Structure

**GenTree**: linked list of tree-structured nodes in `BasicBlock`s. Each node
has an opcode (`GT_ADD`, `GT_IND`, `GT_LCL_VAR`), type, and child pointers.
"Almost SSA" — converts to SSA for optimization, underlying structure remains
tree-based. Before codegen, trees are linearized into LIR (flat node sequence).

### Tiers

- **Tier 0 (Quick JIT):** Minimal optimization. Goal: sub-millisecond per method.
- **Tier 1 (Full RyuJIT):** Inlining, SSA, value numbering, CSE, assertion
  propagation, loop optimization, range check elimination, lowering, linear
  scan RA, peephole.
- **R2R (ReadyToRun):** AOT pre-compilation as tier 0.5.
- OSR added in .NET 7+ for long-running loops.

### Key Design Choices

- **Linear scan RA** instead of graph coloring: ~5% worse code, ~2-3x faster
  compile. Right trade for JIT.
- **No instruction scheduling pass.** Modern x86 OOO handles this.
- **Clean lowering boundary:** `lowerxarch.cpp` vs `lowerarm64.cpp`. Generic
  nodes become target-specific before codegen.
- **Pragmatic SSA:** Used for value numbering and CSE but the IR doesn't
  require everything in SSA form. Good enough > pure.

### Evolution Lessons

The old JIT32/JIT64 had separate codebases for x86 and x64. RyuJIT unified
them: one IR, target-specific lowering/codegen. Roughly halved maintenance.
Tree IR is simpler than DAG. Linear scan is the right RA for JIT.

---

## LuaJIT (Mike Pall)

### IR Format

Compact 64-bit SSA instructions. Each instruction is exactly 8 bytes:
- 16 bits: opcode + type
- 16 bits: left operand (reference to another instruction or constant)
- 16 bits: right operand
- 16 bits: previous instruction with same hash (CSE chaining)

Constants grow downward, instructions grow upward. 32-bit values inline in
operand slots. 64-bit constants interned separately. Extremely cache-friendly.

### FOLD Engine (Key Innovation)

As each IR instruction is about to be emitted, it's matched against a table
of fold rules. Constant folding, strength reduction, algebraic simplification,
and CSE all happen inline during IR construction. By the time recording
finishes, the IR is already heavily optimized. **No separate optimization
pass cost.**

### Per-Opcode Hash Chaining (Free CSE)

Each IR instruction is chained by opcode in a hash table. "Have I computed
this ADD before?" is a hash lookup. Essentially zero-cost CSE.

### Allocation Sinking (Escape Analysis)

Two-phase mark+sweep. Allocations that don't escape the trace are sunk past
their last use and eliminated. During deoptimization, `snap_unsink()`
reconstructs objects.

### Register Allocation

Linear-scan, runs backward over the IR in a single pass. Constants that fit
in a register are rematerialized rather than spilled, keeping the hot path
tight.

### Multi-Architecture

Shared IR, per-architecture emitters in separate files (`lj_asm_x86.h`,
`lj_asm_arm64.h`, etc.). 6 architectures: x86, x64, ARM, ARM64, MIPS32/64,
PPC. The optimizer and IR are completely architecture-independent.

### Why It's Fast

- Tiny compilation units (traces, typically tens to hundreds of instructions)
- No separate optimization passes — FOLD engine handles everything during construction
- Linear-scan RA in one backward pass
- Direct machine code emission — no intermediate assembler

---

## MIR (Vladimir Makarov)

### Overview

Lightweight SSA-based IR designed for JIT-compiling C. ~20K lines of C.

### Key Metrics

- Generated code quality: **91% of GCC -O2** (geomean)
- Compile speed: 180x faster than GCC -O2 for codegen phase
- C-to-MIR end-to-end: 15x faster than GCC -O2
- MIR interpreter: 6-10x slower than JIT-compiled code

### IR Design

SSA form in basic blocks. Each variable has one of four types: i64, float,
double, long double. Operands: register, immediate, memory, label, reference.
Three-address form (destination + two sources).

### Multi-Architecture

5 architectures: x86-64, aarch64, ppc64, s390x, riscv64. Single IR, per-arch
code generator. Porting guide (`HOW-TO-PORT-MIR.md`) shows ~2-3K lines per
new architecture.

### Relevance to madc

MIR is the closest existing analog. Both are: C-like language JIT compilers,
single-tier, targeting runtime code quality, small codebase. MIR's 91% of
GCC -O2 is the benchmark to aim for.

---

## dstogov/ir (PHP 8.4+ JIT)

### Overview

Shipped in PHP 8.4 as the production JIT. Running on millions of servers.
Strongest real-world validation of any lightweight JIT framework.

### Design

Single IR used through all phases. Sea-of-Nodes-inspired but with a critical
difference: **folding engine runs during IR construction** (constant folding,
copy propagation, algebraic simplification, CSE — all on-the-fly).

SCCP pass for deeper constant propagation. Global Code Motion (Cliff Click's
algorithm) for scheduling. Linear-scan register allocator. Direct machine
code emission for x86-64 and aarch64.

### Performance

15M lines of optimized native code per second.

---

## PyPy (Meta-Tracing JIT)

### Overview

Traces the *interpreter* (written in RPython) as it executes the user program.
A meta-interpreter walks serialized flow-graphs of the interpreter functions.

### IR

Linear trace IR in SSA form. No branches — guard operations encode
assumptions. When a guard fails, execution deoptimizes to the interpreter.

### Optimization

Single forward pass: `intbounds:rewrite:virtualize:string:earlyforce:pure:heap:unroll`.

**Virtualization** is the most important: objects created within a trace that
don't escape are never allocated — fields kept in registers. Eliminates
enormous allocation overhead for dynamic languages.

### Multi-Architecture

Shared IR, per-arch backends. x86 (32/64), ARM (32/64), PPC64, s390x.

---

## Julia (LLVM-Based)

### Key Lesson: Don't Use LLVM for JIT

Julia uses LLVM for code generation. Excellent runtime code quality but:

- Compile time is a **constant battle** ("time to first plot" problem)
- Precompilation (Julia 1.9+) caches full LLVM native code to disk
- Package images serialize native code into shared libraries
- Years of engineering to hide LLVM's compile-time cost

If runtime code quality is priority #1 and you have obj-caching, LLVM is
viable. But for a JIT where compile speed matters, a direct-emission approach
(asmjit, DynASM, or custom) is superior.

---

## GCC and LLVM (AOT Reference)

### GCC: Two-Level IR

- **GIMPLE** (high-level): Three-address SSA. Language-independent. ~200
  optimization passes (inlining, IPA, SCCP, GVN, PRE, loop vectorization).
- **RTL** (low-level): Register Transfer Language. Architecture-specific
  patterns via `.md` files. ~50 passes (instruction selection, register
  allocation via IRA, instruction scheduling, peephole).

### LLVM: Single IR

SSA-based typed three-address code. Single IR from frontend through
optimization. ~100+ passes. SelectionDAG/GlobalISel lowers to machine code.

### Consensus (2025-2026): Two-level IR is the practical sweet spot.

High-level SSA for optimization (target-independent), low-level linear IR for
register allocation and codegen (target-specific). This is what Cranelift,
RyuJIT, and new designs converge on.

---

## Emerging Technologies (2024-2026)

### TPDE (2025)

Single-pass framework combining instruction selection, register allocation,
and encoding. Compiles LLVM IR 8-24x faster than LLVM -O0 with on-par
runtime performance. 4.27x faster than Cranelift. Adapts to existing IRs
via an adapter layer.

### Copy-and-Patch (CPython 3.13+)

Pre-compiled machine code stencils patched at runtime. CPython 3.15: 11-12%
speedup on ARM, 5-6% on x86-64. ~1400 lines. Best for bytecode interpreters;
less applicable to AST-based compilation of C-like languages.

### Turbolev (V8, 2025)

Replaces TurboFan's Sea-of-Nodes frontend with Maglev's CFG IR, keeping
Turboshaft's CFG backend. Full CFG pipeline end-to-end. The industry moving
decisively away from Sea of Nodes.

---

## Universal Consensus Points

1. **CFG-based SSA beats Sea of Nodes for JIT.** V8 abandoned SoN. HotSpot
   C2's SoN is unmaintainable. Every new project uses CFG.

2. **Linear scan RA is the right choice.** ~5% worse code than graph
   coloring, 2-3x faster. Every JIT except C2 uses it.

3. **Fold during construction.** LuaJIT's FOLD engine and dstogov/ir's
   folding engine do constant folding, CSE, strength reduction during IR
   building at zero additional cost.

4. **6-10 well-chosen passes deliver ~80% of optimization value:**
   - Constant folding / propagation (during construction — free)
   - CSE via hash lookup (during construction — free)
   - Dead code elimination (single backward pass — cheap)
   - Inlining of small functions (moderate cost, biggest single win)
   - Strength reduction (during construction — free)
   - Loop-invariant code motion (moderate cost, big win for loops)

5. **Shared IR, per-architecture emitter.** MIR (5 archs in ~20K lines),
   LuaJIT (6 archs), Cranelift (ISLE per arch). The IR is platform-neutral;
   each target has its own emitter.

6. **Single tier works for compile-once languages.** Tiering is for
   long-running VMs (Java, JS). C-like languages that compile once benefit
   from a single well-optimized pass + obj-caching.

---

## Sources

### V8
- [Land ahoy: leaving the Sea of Nodes](https://v8.dev/blog/leaving-the-sea-of-nodes)
- [Maglev — V8's Fastest Optimizing JIT](https://v8.dev/blog/maglev)
- [Sparkplug — a non-optimizing JavaScript compiler](https://v8.dev/blog/sparkplug)
- [Expanding to Turbolev](https://blog.seokho.dev/development/2025/07/15/V8-Expanding-To-Turbolev.html)
- [Profile-Guided Tiering in V8](https://community.intel.com/t5/Blogs/Tech-Innovation/Client/Profile-Guided-Tiering-in-the-V8-JavaScript-Engine/post/1679340)

### HotSpot / JVM
- [Understanding V8 backend architecture (applies to HotSpot patterns)](https://github.com/riscv-collab/v8/wiki/Understand-V8-backend-architecture)
- [An Introduction to Speculative Optimization in V8](https://benediktmeurer.de/2017/12/13/an-introduction-to-speculative-optimization-in-v8/)

### LuaJIT
- [LuaJIT SSA IR](https://github.com/tarantool/tarantool/wiki/LuaJIT-SSA-IR)
- [LuaJIT Allocation Sinking](https://github.com/tarantool/tarantool/wiki/LuaJIT-Allocation-Sinking-Optimization)
- [LuaJIT Optimizations](https://github.com/tarantool/tarantool/wiki/LuaJIT-Optimizations)
- [Wrestling with the Register Allocator: LuaJIT Edition](https://gotplt.org/posts/wrestling-with-the-register-allocator-luajit-edition.html)

### MIR
- [MIR: A lightweight JIT compiler project](https://developers.redhat.com/blog/2020/01/20/mir-a-lightweight-jit-compiler-project)
- [MIR GitHub](https://github.com/vnmakarov/mir)

### dstogov/ir (PHP JIT)
- [dstogov/ir GitHub](https://github.com/dstogov/ir)
- [IR JIT Framework paper](https://www.researchgate.net/publication/374470404_IR_JIT_Framework_a_base_for_the_next_generation_JIT_for_PHP)

### PyPy
- [The Architecture of Open Source Applications: PyPy](https://aosabook.org/en/v2/pypy.html)
- [PyPy Trace Optimizer](https://rpython.readthedocs.io/en/latest/jit/optimizer.html)
- [PyPy Musings on Tracing](https://pypy.org/posts/2025/01/musings-tracing.html)

### Julia
- [Julia JIT Design](https://docs.julialang.org/en/v1/devdocs/jit/)
- [Julia's Latency: Past, Present and Future](https://viralinstruction.com/posts/latency/)

### Cranelift / TPDE / Copy-and-Patch
- [Cranelift](https://cranelift.dev/)
- [TPDE: A Fast Adaptable Compiler Back-End Framework](https://arxiv.org/abs/2505.22610)
- [Copy-and-Patch Compilation](https://arxiv.org/pdf/2011.13127)
- [PEP 744: CPython JIT](https://peps.python.org/pep-0744/)

### General
- [Escape Analysis in PyPy, LuaJIT, V8, and more](https://kipp.ly/p/escape-analysis)
- [V8 seminar: From CFG to Sea of Nodes and back again](https://www.irill.org/videos/IRILL-2024-2025/Irill-seminaire-Darius-Mercadier-2025-04-10.html)
- [Cranelift codegen primer](https://bouvier.cc/2021/02/17/cranelift-codegen-primer/)
