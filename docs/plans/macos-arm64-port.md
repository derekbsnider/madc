# macOS / Apple Silicon (ARM64) Port Plan

Analysis performed 2026-05-24.

## Feasibility: VIABLE

asmjit v1.14 already has full ARM64 support (`a64::Compiler` with parallel
API). The front-end (lexer + parser = 19,683 lines) has zero architecture
dependencies. The main work is translating ~1,300 x86 instruction emissions
in the compiler backend.

## Current Architecture Dependencies

### Clean (zero changes needed): ~28,500 lines (63%)

| Component | Lines | Status |
|-----------|-------|--------|
| Lexer | 4,230 | Fully portable |
| Parser | 15,453 | Fully portable |
| All namespace files | 2,130 | Fully portable |
| va_helpers.cpp | 549 | Pure C/C++ |
| libmadc API | ~5,500 | Fully portable |
| pch.cpp | ~400 | Fully portable |
| Storage backends | ~5,400 | Fully portable |

### x86-Specific: ~18,200 lines (37%)

| File | x86 refs | Scope |
|------|----------|-------|
| compiler.cpp | 1,003 | Every codegen path — the bulk of the port |
| typesafe.cpp | 271 | safemov/safeadd/safecmp overloads |
| madc_ir.cpp | 53 | IR builder (smallest x86 surface) |
| madc_elf.cpp | 168 | 100% ELF-specific — rewrite for Mach-O or defer |
| madc_program.cpp | 8 | Callback thunk only |

### Linux-Specific: ~200 lines

| Area | Issue | Fix |
|------|-------|-----|
| Include paths | Hardcoded `/usr/include/x86_64-linux-gnu/` | Add macOS SDK paths via `xcrun` |
| Crash handler | `ucontext_t` register names differ | `#ifdef __APPLE__` |
| `RTLD_DEEPBIND` | Doesn't exist on macOS | Conditional define |
| Embedded headers | Linux-specific numeric constants (signals, fcntl, errno) | Compile-time detection or platform variants |
| ELF dynamic linker | Hardcoded `/lib64/ld-linux-x86-64.so.2` | Mach-O uses dyld |

## Key Enablers

1. **asmjit ARM64 support.** `a64::Compiler` provides `newGpx()`/`newGpw()`,
   `newVecD()`/`newVecS()`, `newStack()`, `invoke()`, labels — all parallel
   to x86. `FuncSignature::build<>()` and `CallConvId::kCDecl` work
   cross-platform. JitAllocator handles macOS MAP_JIT/W^X automatically.

2. **The IR layer is the right abstraction point.** `madc_ir.cpp` (53 x86
   refs) already sits between tokens and asmjit. Completing the IR migration
   (typed-register-ir.md) before porting would create a clean arch-neutral
   boundary. Each `IRBuilder` method gets x86 and a64 implementations.

3. **Front-end is completely clean.** All parsing, type resolution, symbol
   tables, macro expansion — zero architecture coupling.

## Instruction Translation: x86 → ARM64

| x86 Instruction | Count | ARM64 Equivalent |
|-----------------|-------|-------------------|
| `cc.mov` | 251 | `cc.mov` (direct) |
| `cc.lea` | 92 | `cc.add` (ARM64 has no LEA) |
| `cc.invoke` | 90 | `cc.invoke` (uses BLR, automatic) |
| `cc.movsxd`/`movsx`/`movzx` | 40+ | `cc.sxtw`/`cc.uxtb`/etc. |
| `cc.movsd`/`cc.movss` | 32 | `cc.fmov`/`cc.ldr` |
| `cc.cvttsd2si`/`cc.cvtsi2sd` | 30+ | `cc.fcvtzs`/`cc.scvtf` |
| `cc.imul`/`cc.idiv` | 29 | `cc.mul`/`cc.sdiv`/`cc.udiv` |
| `cc.cmp` + `cc.je`/`cc.jne` | many | `cc.cmp` + `cc.b_eq`/`cc.b_ne` |
| SSE SIMD (~170 refs) | 170 | NEON equivalents (hardest subset) |
| Physical regs (rax,rsp,etc.) | 7 | x0/x1/sp per AAPCS64 |

## Calling Convention: SysV AMD64 → AAPCS64

| Aspect | x86-64 (SysV) | ARM64 (AAPCS64) |
|--------|---------------|-----------------|
| Int args | rdi,rsi,rdx,rcx,r8,r9 | x0-x7 |
| Float args | xmm0-xmm7 | v0-v7 |
| Return | rax/rdx | x0/x1 |
| Variadic | AL = # XMM args | No special register |

asmjit handles this automatically via `CallConvId::kCDecl`. Only the 7
hardcoded physical register uses and the callback thunk need manual
conversion.

## Implementation Strategy

### Option A: IR-First (Recommended)

1. Complete the typed-register IR migration (typed-register-ir.md)
2. The IR becomes the arch-neutral boundary
3. Add `a64` backend to IRBuilder alongside existing x86 backend
4. The 1,003 x86 refs in compiler.cpp become ~50 in madc_ir.cpp

**Pros:** Cleanest architecture. One-time investment pays off for any
future arch (RISC-V, WebAssembly).
**Cons:** IR migration is 4-6 weeks before the port can start.

### Option B: `#ifdef` Dual Backend

1. Add `#ifdef __x86_64__` / `#elif __aarch64__` guards around x86 code
2. Write parallel ARM64 code alongside each x86 block
3. compiler.cpp grows by ~40% but both paths coexist

**Pros:** Can start immediately. Incremental.
**Cons:** Doubles the maintenance burden. Gets ugly fast with 1,003 ifdefs.

### Option C: Backend Split (Pragmatic)

1. Extract instruction emission from compiler.cpp into `compiler_emit.cpp`
2. Create `compiler_emit_x86.cpp` and `compiler_emit_a64.cpp`
3. Common AST-walking logic stays in compiler.cpp
4. Build system selects the right emit file

**Pros:** Clean separation without full IR. Smaller scope than Option A.
**Cons:** Still requires defining the emit interface (essentially a mini-IR).

**Recommendation:** Option A if the IR is in progress anyway. Option C if
the port is urgent and the IR isn't ready.

## Effort Estimate

| Work Item | Dev-Weeks | Notes |
|-----------|-----------|-------|
| Compiler backend abstraction | 3-4 | Wrap 1,003 x86 refs |
| ARM64 instruction selection | 4-6 | 661 emissions, SIMD hardest |
| ABI adaptation | 1-2 | 7 physical reg uses, callback thunk |
| macOS OS adaptation | 1 | Include paths, ucontext, RTLD |
| Embedded header constants | 1-2 | Platform-conditional values |
| Mach-O writer (AOT) | 4-6 | Full rewrite of madc_elf.cpp — **deferrable** |
| Testing and debugging | 3-4 | 452 tests on ARM64 |
| Build system | 0.5 | Makefile, Homebrew, Xcode |
| **JIT-only MVP** | **10-15** | Skip Mach-O, JIT execution only |
| **Full parity** | **16-24** | JIT + AOT + native executables |

## Phased Approach

### Phase 1: JIT-only macOS/ARM64 MVP (10-15 weeks)
- compiler_emit abstraction or IR completion
- ARM64 instruction selection for core paths
- macOS OS adaptation (`#ifdef __APPLE__`)
- Skip: Mach-O writer, AOT, SIMD (can add later)
- Gate: 400+ of 452 integration tests pass

### Phase 2: SIMD/NEON (2-3 weeks)
- Translate 170 SSE references to NEON equivalents
- `__builtin_shuffle` → NEON TBL/TBX
- Gate: All SIMD tests pass

### Phase 3: AOT/Mach-O (4-6 weeks)
- Write Mach-O object file emitter (replaces ELF for macOS)
- Code signing (ad-hoc minimum)
- `madc -o binary` produces native ARM64 Mach-O executable

## Prerequisites

1. **Code cleanup Phase A** (dispatch table, file split) — makes the
   compiler backend easier to abstract
2. **Typed-register IR** (if choosing Option A) — creates the arch-neutral
   boundary
3. **PCH Phase 1 completion** — macOS headers via `gcc -E` (Xcode clang)
   instead of `/usr/include`

## What macOS Gives Us

- Apple Silicon Macs are the dominant developer hardware in 2026
- Homebrew provides asmjit, zlib, zstd, and all dependencies
- macOS `clang` can substitute for `gcc -E` in the PCH pipeline
- Access to the iOS/macOS app ecosystem for future madc tooling
