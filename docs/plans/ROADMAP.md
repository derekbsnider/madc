# madc Roadmap

Master plan linking all workstreams. Updated 2026-05-25.

## Current State

- **Version:** 0.21.0
- **GCC parity:** 1649/1685 (97.9%)
- **Integration tests:** 475 passing
- **C++ model:** Ctors/dtors, operators, refs, new/delete, inheritance, vtables, exceptions + unwinding, access control, const enforcement
- **Generic extern class:** One `register_extern_ctor_dtor()` call per libc type — replaces per-type boilerplate
- **SMAUG port:** Startup through serpent combat on native exe
- **libmadc:** C++ embedding API with eval, security policy, invoke limits
- **PCH:** Phase 1 infrastructure (.madh format, 38 headers pre-compiled)

---

## Track 1: Language Core

*Make the compiler correct, clean, and fast.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 1.1 | C foundation (GCC parity) | — | **DONE** 97.9% | — |
| 1.2 | Code cleanup Phase A — dispatch table, AST visitor, file split | 2-3 wk | **DONE** (v0.20.1) | [code-cleanup.md](code-cleanup.md) |
| 1.3 | Typed-register IR + MIR backend — token migration then asmjit→MIR swap | 10-14 wk | **Active** (Stage 0 done) | [typed-register-ir.md](typed-register-ir.md) |
| 1.4 | Code cleanup Phase B — parser dereference/subscript unification | 3 wk | Ready | [code-cleanup.md](code-cleanup.md) |
| 1.5 | Code cleanup Phase C — macro system, token hierarchy | 3 wk | Ready | [code-cleanup.md](code-cleanup.md) |

**Dependencies:** 1.2 before 1.3. 1.3 before macOS port.

---

## Track 2: C++ Support

*Extend from C scripting convenience to practical C++ OOP.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 2.1 | Constructors & destructors (RAII foundation) | 3-5 d | **DONE** | [cpp-support.md](cpp-support.md) |
| 2.2 | Operator overloading completion | 2-3 d | **DONE** | [cpp-support.md](cpp-support.md) |
| 2.3 | References `T&`, const enforcement | 1 wk | **Mostly done** | [cpp-support.md](cpp-support.md) |
| 2.4 | `new` / `delete` | 1-2 wk | **DONE** (v0.21.0) | [cpp-support.md](cpp-support.md) |
| 2.5 | Single inheritance | 1-2 wk | **DONE** (v0.21.0) | [cpp-support.md](cpp-support.md) |
| 2.6 | Virtual functions / vtables | 2-3 wk | **DONE** (v0.21.0) | [cpp-support.md](cpp-support.md) |
| 2.7 | Exception handling (SJLJ) | 3-4 wk | **DONE** (v0.21.0) — Phase A + B (unwinding) | [cpp-support.md](cpp-support.md) |
| 2.8 | Quality of life | Ongoing | **Started** — access control, auto token position | [cpp-support.md](cpp-support.md) |
| 2.9 | Generic extern class ctor/dtor | — | **DONE** (v0.21.0) — replaces per-type switch boilerplate | — |

**2.3 remaining:** pointer-to-const enforcement (`*p` writes), const methods.
**2.8 remaining:** enum class, auto type deduction, `cin>>`, scope-level destruction.

**Dependencies:** All met. 2.1-2.7 complete.

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

## Track 7: Rendering Abstraction (`ui::`)

*Universal semantic rendering: teletype to Unreal Engine. WCAG by design.
Hardware × user preference × accessibility three-way negotiation.
JIT-time capability resolution for zero runtime overhead.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 7.1 | Semantic IR + Level 0 (text stream) | 2-3 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.2 | Level 1 — curses/terminal backend | 3-4 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.3 | Reactivity + compiler-tracked state deps | 2-3 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.4 | Level 2 — 2D graphics (Skia/Cairo) | 3-4 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.5 | Level 3 — Web backend (WebSocket + thin JS) | 4-6 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.6 | Level 3 — Native GUI (SDL2/GTK) | 4-6 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.7 | Level 4 — GPU/3D (WebGPU/Metal) | Future | Future | [rendering-abstraction.md](rendering-abstraction.md) |

**Dependencies:** 2.1 (constructors) for widget lifecycle. 7.1-7.2 can
start after 1.2 (cleanup) makes the parser ready for `render` blocks.

---

## Track 8: Tooling (madcide + libmadcedit)

*A Turbo-C style IDE and reusable editor library, built in madc.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 8.1 | libmadcedit core — piece table, cursor, undo, CUA keys | 3-4 wk | Future | [madc-ide.md](madc-ide.md) |
| 8.2 | libmadcedit curses rendering | 2-3 wk | Blocked on 7.2 | [madc-ide.md](madc-ide.md) |
| 8.3 | Syntax highlighting + keybinding profiles (Vim, Emacs, Turbo-C) | 2-3 wk | Future | [madc-ide.md](madc-ide.md) |
| 8.4 | madcide shell — file tree, tabs, build, errors | 3-4 wk | Blocked on 8.2 | [madc-ide.md](madc-ide.md) |
| 8.5 | Advanced — find/replace, split views, go-to-def | Ongoing | Future | [madc-ide.md](madc-ide.md) |

**Dependencies:** 7.1-7.2 (rendering Level 0-1). Config via TOML + madc scripts.

---

## Track 9: Multi-Syntax Support

*Write madc programs in Python, Ruby, or Rust syntax. Controlled via
`#pragma syntax python`. Syntax is skin-deep — AST, compiler, IR unchanged.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 9.1 | Syntax profile infrastructure + lexer config | 2-3 wk | Future | [multi-syntax.md](multi-syntax.md) |
| 9.2 | Python-style indentation mode | 3-4 wk | Future | [multi-syntax.md](multi-syntax.md) |
| 9.3 | Type annotation variants (suffix syntax) | 2 wk | Future | [multi-syntax.md](multi-syntax.md) |
| 9.4 | Ruby/Rust profiles | 2-3 wk ea | Future | [multi-syntax.md](multi-syntax.md) |
| 9.5 | Mixed-syntax files (`#pragma syntax`) | 2 wk | Future | [multi-syntax.md](multi-syntax.md) |

**Dependencies:** 1.2 + 1.4 (parser cleanup). Editor highlighting reuses profiles.

---

## Track 10: Future Language Evolution

*Safety, modern features, and long-term direction.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 10.1 | Optional bounds checking (`--check-bounds`) | 1-2 wk | Future | [future-considerations.md](future-considerations.md) |
| 10.2 | Ownership annotations (RAII-based) | TBD | Future | [future-considerations.md](future-considerations.md) |
| 10.3 | Go-style error returns (multi-return convention) | 1 wk | Future | [future-considerations.md](future-considerations.md) |
| 10.4 | `-O` optimization levels | 2-3 wk | Future | — |
| 9.5 | TOML parser (for config files) | 1-2 wk | Future | — |

**Dependencies:** 2.1 (RAII) before 9.2. Existing multi-return enables 9.3.

---

## Ideal Execution Order

Each step builds on the previous. Items at the same indent level can
run in parallel.

```
 COMPLETED:
 ──────────
 1.  Track 1.2  Code cleanup Phase A                    [DONE v0.20.1]
 2.  Track 2.1  Constructors & destructors              [DONE v0.21.0]
 3.  Track 2.2  Operator overloading                    [DONE v0.21.0]
 4.  Track 2.3  References T& + const enforcement       [MOSTLY DONE]
 5.  Track 2.4  new / delete                            [DONE v0.21.0]
 6.  Track 2.5  Single inheritance                      [DONE v0.21.0]
 7.  Track 2.6  Virtual functions / vtables             [DONE v0.21.0]
 8.  Track 2.7  Exception handling (SJLJ + unwinding)   [DONE v0.21.0]
 9.  Track 2.8  Access control + auto token position    [DONE]
10.  Track 2.9  Generic extern class ctor/dtor          [DONE v0.21.0]

 NEXT UP (recommended order):
 ────────────────────────────
11.  Track 1.3  Typed-register IR (stages 0-3)          [4-6 wk]
     ├── Fixes operand-shape bugs at the source
     ├── Creates arch-neutral boundary for ARM64
     └── Enables IR Stage 3 (calls) with clean dispatch table
     ** RECOMMENDED NEXT — high leverage for ARM64 + correctness **

 ║── Track 7.1  Rendering: Semantic IR + Level 0         [2-3 wk]
 ║   └── render { } blocks, UINode, text output
 ║       Can start in parallel with Track 1.3

12.  Track 1.4  Code cleanup Phase B                    [3 wk]
     └── Parser dereference & subscript unification
         Unblocks: PCH transition, parser resilience

 ║── Track 7.2  Rendering: Level 1 curses backend        [3-4 wk]

13.  Track 8.1  libmadcedit core                         [3-4 wk]
     ├── Piece table, cursor, undo/redo, CUA keybindings
     └── Requires: Level 0 rendering (step 11b)

14.  Track 8.2  libmadcedit curses + syntax highlight    [4-6 wk]
     └── Requires: Level 1 rendering (step 12b)

15.  Track 8.4  madcide shell                            [3-4 wk]
     └── Self-hosting milestone: edit madc in madcide

16.  Track 3.2  PCH transition                           [2-3 wk]
     └── Replace text-embedded stubs with pre-compiled

17.  Track 6.1  macOS/ARM64 JIT MVP                     [10-15 wk]
     └── Requires IR (step 11) for arch-neutral boundary

18.  Track 4.2  C ABI shim                               [2-3 wk]

19.  Track 1.5  Code cleanup Phase C                    [3 wk]
     └── Macro system unification, token hierarchy flattening

20.  Track 7.3-7.6  Rendering Levels 2-3                [4-6 wk each]

21.  Track 4.3  Fork-based worker isolation              [3-4 wk]

22.  Track 3.3  PCH Phase 2 — AST serialization         [4-6 wk]

23.  Track 5.3  Query pushdown & federation              [4-6 wk]

24.  Track 6.2  macOS SIMD (NEON)                       [2-3 wk]

25.  Track 4.4  Node.js integration                      [4-6 wk]
     Track 4.5  Rust bindings                            [2-3 wk]

26.  Track 3.4  Modules (.madm)                          [6-8 wk]

27.  Track 6.3  macOS AOT (Mach-O writer)               [4-6 wk]

28.  Track 7.7  Rendering: Level 4 GPU/3D               [future]

29.  Track 9    Multi-syntax (Python/Ruby/Rust modes)     [ongoing]

30.  Track 10   Safety, optimization levels              [ongoing]
```

**Recommended next:** Track 1.3 (Typed-register IR) is the highest-leverage
item. It fixes operand-shape bugs structurally, creates the arch-neutral
boundary needed for the macOS/ARM64 port, and makes future compiler work
cleaner. Track 7.1 (rendering) can run in parallel if desired.

## The SMAUG Goal

The concrete test case driving Tracks 1-3 is compiling SMAUG 1.8
(~158K LOC C89) end-to-end. Current state: `smaug.exe` survives startup,
login, and serpent combat. The next milestone is broader post-combat
gameplay with longer session stability.

SMAUG does NOT need C++ features (Tracks 2, 8) — it's pure C. But the
C++ features make madc useful as a general-purpose scripting language
beyond the SMAUG port. The rendering abstraction (Track 7) would let
SMAUG target terminal, web, and GUI from the same game code.

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
| Rendering Abstraction | [rendering-abstraction.md](rendering-abstraction.md) |
| madc IDE & Editor | [madc-ide.md](madc-ide.md) |
| Multi-Syntax Support | [multi-syntax.md](multi-syntax.md) |
| Typed-Register IR | [typed-register-ir.md](typed-register-ir.md) |
| Revival Plan (archived) | [archived/revival-plan.md](archived/revival-plan.md) |
