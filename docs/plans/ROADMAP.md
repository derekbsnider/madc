# madc Roadmap

Master plan linking all workstreams. Updated 2026-05-24.

## Current State

- **Version:** 0.20.0
- **GCC parity:** 1649/1685 (97.9%)
- **Integration tests:** 452 passing
- **SMAUG port:** Startup through serpent combat on native exe
- **libmadc:** C++ embedding API with eval, security policy, invoke limits
- **PCH:** Phase 1 infrastructure (.madh format, 38 headers pre-compiled)

---

## Track 1: Language Core

*Make the compiler correct, clean, and fast.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 1.1 | C foundation (GCC parity) | — | **DONE** 97.9% | — |
| 1.2 | Code cleanup Phase A — dispatch table, AST visitor, file split | 2-3 wk | Ready | [code-cleanup.md](code-cleanup.md) |
| 1.3 | Typed-register IR — stages 0-3 | 4-6 wk | Draft | [typed-register-ir.md](typed-register-ir.md) |
| 1.4 | Code cleanup Phase B — parser dereference/subscript unification | 3 wk | Ready | [code-cleanup.md](code-cleanup.md) |
| 1.5 | Code cleanup Phase C — macro system, token hierarchy | 3 wk | Ready | [code-cleanup.md](code-cleanup.md) |

**Dependencies:** 1.2 before 1.3. 1.3 before macOS port.

---

## Track 2: C++ Support

*Extend from C scripting convenience to practical C++ OOP.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 2.1 | Constructors & destructors (RAII foundation) | 3-5 d | Ready | [cpp-support.md](cpp-support.md) |
| 2.2 | Operator overloading completion | 2-3 d | Ready | [cpp-support.md](cpp-support.md) |
| 2.3 | Explicit references `T&`, const enforcement | 1 wk | Ready | [cpp-support.md](cpp-support.md) |
| 2.4 | `new` / `delete` | 1-2 wk | Blocked on 2.1 | [cpp-support.md](cpp-support.md) |
| 2.5 | Single inheritance | 1-2 wk | Blocked on 2.1 | [cpp-support.md](cpp-support.md) |
| 2.6 | Virtual functions / vtables | 2-3 wk | Blocked on 2.5 | [cpp-support.md](cpp-support.md) |
| 2.7 | Exception handling (SJLJ) | 3-4 wk | Blocked on 2.1 | [cpp-support.md](cpp-support.md) |
| 2.8 | Quality of life (enum class, auto, cin>>, namespaces) | Ongoing | — | [cpp-support.md](cpp-support.md) |

**Dependencies:** 1.2 (cleanup) before 2.5+. 2.1 is the keystone.

---

## Track 3: Build Infrastructure

*Pre-compiled headers, modules, and portable builds.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 3.1 | PCH Phase 1 — post-lexer token serialization | 2 wk | **Partial** | [precompiled-headers.md](precompiled-headers.md) |
| 3.2 | PCH transition — replace text-embedded stubs | 2-3 wk | Blocked on parser | [precompiled-headers.md](precompiled-headers.md) |
| 3.3 | PCH Phase 2 — AST serialization | 4-6 wk | Future | [precompiled-headers.md](precompiled-headers.md) |
| 3.4 | PCH Phase 3 — C++20-style modules (.madm) | 6-8 wk | Future | [precompiled-headers.md](precompiled-headers.md) |

**Dependencies:** 1.4 (parser cleanup) unblocks 3.2. 1.5 (token cleanup) before 3.3.

---

## Track 4: Embedding & Library

*Make madc usable as a library in other programs.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 4.1 | libmadc C++ API | — | **DONE** | [libmadc-phase4.md](libmadc-phase4.md) |
| 4.2 | C ABI shim (`extern "C"`) | 2-3 wk | Partial | [libmadc-phase4.md](libmadc-phase4.md) |
| 4.3 | Fork-based isolation / worker mode | 3-4 wk | Partial | [libmadc-phase4.md](libmadc-phase4.md) |
| 4.4 | Node.js integration | 4-6 wk | Future | [libmadc-phase4.md](libmadc-phase4.md) |
| 4.5 | Rust bindings (madc-sys + madc crate) | 2-3 wk | Future | [perry-rust-integration.md](perry-rust-integration.md) |

**Dependencies:** 4.2 before 4.4 and 4.5.

---

## Track 5: Data Storage & Federation (madcdat)

*Structured data access from madc scripts.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 5.1 | DataSource / DataSet core | — | **DONE** | [data-storage-federation.md](data-storage-federation.md) |
| 5.2 | Backend drivers (BDB, GDBM, QDBM, SQLite) | Ongoing | Partial | [data-storage-federation.md](data-storage-federation.md) |
| 5.3 | Query pushdown & federation | 4-6 wk | Planned | [data-storage-federation.md](data-storage-federation.md) |
| 5.4 | libmadcdat separate library | 2-3 wk | Planned | [data-storage-federation.md](data-storage-federation.md) |

**Dependencies:** Independent of other tracks. `--enable-madcdat` gate exists.

---

## Track 6: Platform Support

*Run madc on more than just x86-64 Linux.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 6.1 | macOS/ARM64 JIT-only MVP | 10-15 wk | Planned | [macos-arm64-port.md](macos-arm64-port.md) |
| 6.2 | macOS SIMD (NEON) | 2-3 wk | Blocked on 6.1 | [macos-arm64-port.md](macos-arm64-port.md) |
| 6.3 | macOS AOT (Mach-O writer) | 4-6 wk | Future | [macos-arm64-port.md](macos-arm64-port.md) |
| 6.4 | Windows port | TBD | Not started | — |

**Dependencies:** 1.3 (IR) dramatically reduces 6.1 effort.

---

## Track 7: Future Language Evolution

*Safety, modern features, and long-term direction.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 7.1 | Optional bounds checking (`--check-bounds`) | 1-2 wk | Future | [future-considerations.md](future-considerations.md) |
| 7.2 | Ownership annotations (RAII-based) | TBD | Future | [future-considerations.md](future-considerations.md) |
| 7.3 | Go-style error returns (multi-return convention) | 1 wk | Future | [future-considerations.md](future-considerations.md) |
| 7.4 | `-O` optimization levels | 2-3 wk | Future | — |

**Dependencies:** 2.1 (RAII) before 7.2. Existing multi-return enables 7.3.

---

## Ideal Execution Order

Each step builds on the previous. Items at the same level can run in
parallel.

```
 1.  Track 1.2  Code cleanup Phase A                    [2-3 wk]
     ├── Builtin dispatch table (compiler.cpp)
     ├── AST visitor pattern (contains_label → general)
     └── File splitting (compiler.cpp → 5 files)
         Unblocks: everything below

 2.  Track 2.1  Constructors & destructors              [3-5 d]
     └── RAII foundation — keystone for C++, safety, exceptions

 3.  Track 2.2  Operator overloading completion          [2-3 d]
     └── Parser done; wire up compiler dispatch

 4.  Track 2.3  References & const enforcement           [1 wk]

 5.  Track 1.3  Typed-register IR (stages 0-3)          [4-6 wk]
     ├── Fixes operand-shape bugs at the source
     ├── Creates arch-neutral boundary for ARM64
     └── Enables IR Stage 3 (calls) with clean dispatch table

 6.  Track 1.4  Code cleanup Phase B                    [3 wk]
     └── Parser dereference & subscript unification
         Unblocks: PCH transition, parser resilience

 7.  Track 2.4  new / delete                             [1-2 wk]
     └── Requires constructors (step 2)

 8.  Track 3.2  PCH transition                           [2-3 wk]
     └── Replace text-embedded stubs with pre-compiled
         Requires: parser resilience from step 6

 9.  Track 2.5  Single inheritance                       [1-2 wk]
     └── Requires constructors (step 2)

10.  Track 2.6  Virtual functions / vtables              [2-3 wk]
     └── Requires inheritance (step 9)

11.  Track 6.1  macOS/ARM64 JIT MVP                     [10-15 wk]
     └── Requires IR (step 5) for arch-neutral boundary

12.  Track 4.2  C ABI shim                               [2-3 wk]
     └── Thin extern "C" over stable C++ API

13.  Track 1.5  Code cleanup Phase C                    [3 wk]
     └── Macro system unification, token hierarchy flattening

14.  Track 2.7  Exception handling (SJLJ)               [3-4 wk]
     └── Requires constructors (step 2) for stack unwinding

15.  Track 4.3  Fork-based worker isolation              [3-4 wk]

16.  Track 3.3  PCH Phase 2 — AST serialization         [4-6 wk]
     └── Requires stable token hierarchy from step 13

17.  Track 5.3  Query pushdown & federation              [4-6 wk]
     └── Independent — can start earlier if prioritized

18.  Track 6.2  macOS SIMD (NEON)                       [2-3 wk]
     └── After ARM64 MVP (step 11)

19.  Track 4.4  Node.js integration                      [4-6 wk]
     └── After C shim (step 12)

20.  Track 4.5  Rust bindings                            [2-3 wk]
     └── After C shim (step 12)

21.  Track 3.4  Modules (.madm)                          [6-8 wk]
     └── After AST serialization (step 16)

22.  Track 6.3  macOS AOT (Mach-O writer)               [4-6 wk]
     └── After ARM64 MVP (step 11)

23.  Track 7    Safety, optimization levels              [ongoing]
     └── Ownership annotations, bounds checking, -O flags
```

## The SMAUG Goal

The concrete test case driving Tracks 1-3 is compiling SMAUG 1.8
(~158K LOC C89) end-to-end. Current state: `smaug.exe` survives startup,
login, and serpent combat. The next milestone is broader post-combat
gameplay with longer session stability.

SMAUG does NOT need C++ features (Tracks 2, 7) — it's pure C. But the
C++ features make madc useful as a general-purpose scripting language
beyond the SMAUG port.

## Plan Index

| Plan | File |
|------|------|
| Code Cleanup | [code-cleanup.md](code-cleanup.md) |
| C++ Support | [cpp-support.md](cpp-support.md) |
| Cross-Cutting Insights | [cross-cutting-insights.md](cross-cutting-insights.md) |
| Data Storage & Federation | [data-storage-federation.md](data-storage-federation.md) |
| Future Considerations | [future-considerations.md](future-considerations.md) |
| libmadc Phase 4 | [libmadc-phase4.md](libmadc-phase4.md) |
| macOS/ARM64 Port | [macos-arm64-port.md](macos-arm64-port.md) |
| Pre-Compiled Headers | [precompiled-headers.md](precompiled-headers.md) |
| Perry/Rust Integration | [perry-rust-integration.md](perry-rust-integration.md) |
| Typed-Register IR | [typed-register-ir.md](typed-register-ir.md) |
| Revival Plan (archived) | [archived/revival-plan.md](archived/revival-plan.md) |
