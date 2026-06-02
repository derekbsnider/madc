# madc Roadmap

Master plan linking all workstreams. Updated 2026-05-30 (v0.25.0).

**Backend reality:** `madc parser → cir_node (MC11-IR) → c2mir → MIR → JIT` is
the **sole** backend — asmjit and the Gecko parser/MIR-transpiler are gone. The
**central near-term goal is feature parity with `master`** (which still carries
the removed asmjit backend at full C89 coverage): `develop` is not promoted to
`master` until the CIR path's test coverage matches or exceeds it. Track 1.3
(CIR coverage) is therefore the gating workstream — most other tracks are
re-established or unblocked behind it. Where a track below was completed on the
old backend, it is marked "proven on old backend, re-establishing on CIR."

## The Intermediate Representation — MC11-IR (SET IN STONE, 2026-05-29)

The primary in-memory representation is the **Mad-enhanced-C11 IR (MC11-IR)** —
the `cir_node` AST tree. `cir_node` **derives from c2mir's `node_t`** (so c2mir
consumes the lowered C11 view directly) AND each node **carries its originating
lexed tokens + parse subtree + file/line/column** (so madc retains the original
high-level structure without reconstruction). It is deliberately **both**:
lowered for c2mir, high-level for madc. The `.mc11` text is the on-disk
serialization of the extra info; render targets (C11/MC11/C++/madc) share the
`--std=` language enum to pick which view to emit. See
[`docs/rules/mc11-ir.md`](../rules/mc11-ir.md). **Do not re-pose "lowered vs
high-level" — the answer is both.**

## Current State

- **Version:** `0.25.0` (per `VERSION`) — released on `develop` (CIR backend).
  `master` still holds the v0.24.0 asmjit/Gecko backend at full C89 coverage;
  develop is **not** promoted to master until the CIR path reaches feature
  parity.
- **Backend:** `madc parser → cir_node (MC11-IR) → c2mir → MIR` is the **sole**
  backend. The asmjit JIT and the Gecko parser/MIR-transpiler were both removed
  (commits `42e9b6e`, `64f44b3`). There is no `--backend=jit`; `--backend=mir`
  aliases to cir. Builds against the **madc MIR fork**
  (github.com/derekbsnider/mir, branch `feature/complex-support`).
- **★ SMAUG 1.8 boots, runs, and is playable** through the CIR path: it boots to
  a live server (`Realms of Despair ready … port 4000`) and a client can create
  a character, navigate the world, and fight (the Newgate serpent fight runs).
  The project's north-star goal — running a real C89 codebase end-to-end —
  is now demonstrated on CIR.
- **CIR baseline (2026-06-02):** **455 integration pass / 8 fail / 55 skip**;
  **gcc.c-torture 1564/1685 (92.8%)** vs the old asmjit backend's 1645 (97.6%) — gap 81.
  The failures are the active CIR coverage worklist — see Track 1.3 — and the
  gate for promotion to master.
- **C++ model — proven on the old backend, being re-established on CIR:**
  ctors/dtors, operator overloading, references, `new`/`delete`, single
  inheritance, vtables, SJLJ exceptions + unwinding, access control, const
  enforcement. These all worked on the asmjit backend; CIR parity is the
  current push.
- **libmadc:** C++ embedding API (security policy, structured diagnostics,
  engine-owned IO). In-process compile/exec/`eval` is **currently stubbed**
  pending reimplementation on CIR→c2mir→MIR (deferred; ~100 unit tests skipped
  as its future spec).
- **AOT (native object/executable):** deferred, low priority. Near-term native
  builds come from emit-`.c` + an external compiler; `save_object` /
  `save_executable` are stubbed (signatures kept) for a later MIR-based revisit.
- **Legacy reference (asmjit backend, pre-removal):** GCC-torture parity reached
  ~97.9% and ~475 integration tests passed. Retained only as the parity target
  the CIR path is climbing back to — NOT the current state.

---

## Track 1: Language Core

*Make the compiler correct, clean, and fast.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 1.1 | C foundation (GCC parity) | — | **DONE** (97.9% on the old backend; the CIR target) | — |
| 1.2 | Code cleanup Phase A — dispatch table, AST visitor, file split | 2-3 wk | **DONE** (v0.20.1) | [code-cleanup.md](code-cleanup.md) |
| 1.3 | **CIR coverage — drive `cir_node` (MC11-IR) → c2mir → MIR to full parity** | ongoing | **Active — the parity-to-master gate** (455 pass / 8 fail / 55 skip; gcc-torture 1564/1685 = 92.8% vs asmjit 97.6%) | — |
| 1.4 | Code cleanup Phase B — parser dereference/subscript unification | 3 wk | Ready | [code-cleanup.md](code-cleanup.md) |
| 1.5 | Code cleanup Phase C — macro system, token hierarchy | 3 wk | Ready | [code-cleanup.md](code-cleanup.md) |
| 1.6 | **SIMD — add a minimal generic-vector extension to MIR (types + insns + per-target codegen) and a c2mir `vector_size` front-end** | large | **Planned (raise the floor)** — design for **upstream** to vnmakarov/mir | — |

**Track 1.6 (SIMD) raises the *floor*, not just c2mir.** MIR today has no vector
type/insns (locals are `i64/f/d/ld` only), so real SIMD-in-JIT requires adding
vectors to MIR itself + per-target codegen (x86-64 SSE/AVX, aarch64 NEON, …) +
interpreter support + ABI/serialization, plus a c2mir front-end for GNU
`vector_size` / generic vector ops. **Design it for upstream** — it benefits MIR
directly (WASM→MIR, a stated MIR future goal, *requires* SIMD since WASM has
fixed-width SIMD; every MIR target has a vector ISA; it lifts ~11 deferred SIMD
torture tests). Keep it a **minimal generic-vector core** to fit MIR's
lightweight ethos. Interim until it lands: madc **scalarizes** for the JIT and
**emit-C → gcc/clang** for real SIMD (AOT). Feeds Track 6.2 (macOS NEON). See
the lowering-vs-raising rule (`.claude/rules/`) and ADR 0001.

**Track 1.3 is the central workstream.** It is the sole backend, so its
coverage *is* the bar for promoting `develop → master`. SMAUG 1.8 now boots,
runs, and is playable on this path (a real-world end-to-end proof); the ~95
remaining integration failures are the worklist between here and parity. Build
the `.mc11`/`.c` renderer + the gcc-`-fverbose-asm` fidelity gate + the
`cir_node`-vs-`c2m -d` differential to make those failures mechanical and
localizable, then reimplement eval/exec + REPL on MIR.

**Dependencies:** 1.2 before 1.3. **1.3 (full CIR parity) gates promotion to
master and unblocks Tracks 3, 5, 6, and AOT.**

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

## Track 5: Data Substrate & Storage

*Three-tier data architecture: core substrate (madcdis) + external
drivers (madcdat) + language-conventional interfaces.*

### Track 5A: madcdis — Core Data Substrate (`libmadcdis`)

*Typed in-memory data substrate. Ships as optional `libmadcdis.so`.
Pools, values, interning, datasets, relations, query IR, planner.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 5A.1 | Library restructure — split madcdis from madcdat | 2-3 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.2 | DataSet/Relation/Query/Schema/Mapper → `include/madcdis/` | 1 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.3 | DataSource moves from libmadc to madcdis | 1 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.4 | Memory pools (arena, slab, size-class, intern) | 3-4 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.5 | Value system — NaN-boxing, refcounting, interning | 3-4 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.6 | Multiplicity dedup for collections | 2 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.7 | Column encoding catalog (dict, RLE, FoR, delta, prefix, GCD) | 4-6 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.8 | mem:// and shm:// pool-backed drivers | 2-3 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.9 | Federated query planner (core, capability-aware) | 4-6 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.10 | GQL as canonical query language + SQL/Cypher lowering | 4-6 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.11 | Derivation relations (keyframe aggregation, retention) | 3-4 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.12 | COW snapshots (fork-based, page-level) | 2-3 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |

### Track 5B: madcdat — External Storage Drivers (`libmadcdat`)

*Optional companion library. Depends on libmadcdis. External backends.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 5B.1 | Library restructure — libmadcdat depends on libmadcdis | 1-2 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |
| 5B.2 | File-format drivers (CSV/DSV, FLR, VLR, snapshot) | Ongoing | **Partial** (DSV, FLR, VLR exist) | [madcdat-plan.md](madcdat-plan.md) |
| 5B.3 | Keyed local DB drivers (BDB, GDBM, QDBM) | — | **DONE** | [madcdat-plan.md](madcdat-plan.md) |
| 5B.4 | SQLite driver | — | **DONE** | [madcdat-plan.md](madcdat-plan.md) |
| 5B.5 | Network DB drivers (MySQL, PostgreSQL) | 3-4 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |
| 5B.6 | Graph DB drivers (FalkorDB, Neo4j) | 3-4 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |
| 5B.7 | Service drivers (HTTP/REST, MCP, S3) | 4-6 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |
| 5B.8 | Structured text adapters (SMAUG areas, mbox, TOML) | 2-3 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |

### Track 5C: Language-Conventional Interfaces

*Multiple syntactic surfaces over the same data substrate.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 5C.1 | C-native core API (DataSet, Cursor, Query builder) | 2-3 wk | **Partial** | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.2 | C++23 ranges integration (madc::linq::) | 3-4 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.3 | Ruby-style trailing blocks (madc::ruby::) | 2-3 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.4 | Python comprehensions (madc::python::) | 3-4 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.5 | Objective-C brackets (madc::objc::) | 2-3 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.6 | ORM-style records (madc::orm::) | 2-3 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.7 | Native query sub-grammars (sql::, cypher::, gql::) | 4-6 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |

**Library structure:**
```
libmadc          (core: compiler, runtime, embedding API)
  ↑
libmadcdis       (optional: data substrate — pools, values, datasets, query, planner)
  ↑
libmadcdat       (optional: external drivers — BDB, GDBM, SQLite, MySQL, etc.)
```

**Dependencies:**
- **Track 1.3 (CIR coverage) must reach full parity before any
  Track 5 work begins.** The data substrate needs a stable compiler
  foundation — templates, full C++ class support, and AOT output must
  work before DataSet<T>/Cursor<T>/Relation<A,B> can compile through
  the CIR → c2mir → MIR pipeline.
- 5A.1-5A.3 (restructure) first — moves existing code to new library boundary
- 5B.1 follows 5A.1 — madcdat depends on madcdis
- 5A.4-5A.5 (pools, values) before 5A.7-5A.12 (column encoding, COW, derivation)
- 5C.1-5C.2 (library-only surfaces) independent of compiler work
- 5C.3-5C.7 (compiler-integrated surfaces) require Track 9 (multi-syntax)

**Research:** [madcdis-memory-research.md](madcdis-memory-research.md) — design lineage from SMAUG, Lucene, modern arenas, refcounting

---

## Track 6: Platform Support

*Run madc on more than just x86-64 Linux.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 6.1 | macOS/ARM64 MVP (via MIR — c2mir + MIR are already cross-platform) | 10-15 wk | Planned | [macos-arm64-port.md](macos-arm64-port.md) |
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
| 10.4 | `-O` optimization levels (`-O0..-O3` flag landed; MIR-gen level) | 2-3 wk | **Partial** | — |
| 10.5 | TOML parser (for config files) | 1-2 wk | Future | — |

**Dependencies:** 2.1 (RAII) before 10.2. Existing multi-return enables 10.3.

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
11.  Track 1.3  CIR coverage — cir_node (MC11-IR) → c2mir → MIR  [ongoing]
     ├── ★ SMAUG 1.8 boots, runs, and is playable on this path (v0.25.0)
     ├── Burn down the ~95 CIR integration failures toward 0
     ├── Build the .mc11/.c renderer + gcc -fverbose-asm fidelity
     │   gate + cir_node-vs-`c2m -d` differential
     └── Then reimplement eval/exec + REPL on MIR (deferred)
     ** THE PARITY-TO-MASTER GATE — sole backend; nothing downstream
        ships, and develop is not promoted to master, until this reaches
        full parity with master's C89 coverage **

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

17.  Track 6.1  macOS/ARM64 MVP (via MIR)               [10-15 wk]
     └── c2mir + MIR are already cross-platform; CIR coverage (step 11) first

18.  Track 4.2  C ABI shim                               [2-3 wk]

19.  Track 1.5  Code cleanup Phase C                    [3 wk]
     └── Macro system unification, token hierarchy flattening

20.  Track 7.3-7.6  Rendering Levels 2-3                [4-6 wk each]

21.  Track 4.3  Fork-based worker isolation              [3-4 wk]

22.  Track 3.3  PCH Phase 2 — AST serialization         [4-6 wk]

     ── CIR PARITY GATE ─────────────────────────────────────
     Track 1.3 (CIR coverage) must reach full parity before
     data work begins.

23.  Track 5A.1-3  madcdis library restructure            [3-4 wk]
     Track 5B.1    madcdat depends on madcdis             [1-2 wk]

24.  Track 5A.4-5  Pools + value system                  [6-8 wk]

25.  Track 5A.9   Federated query planner                [4-6 wk]

26.  Track 6.2  macOS SIMD (NEON)                       [2-3 wk]

27.  Track 4.4  Node.js integration                      [4-6 wk]
     Track 4.5  Rust bindings                            [2-3 wk]

28.  Track 3.4  Modules (.madm)                          [6-8 wk]

29.  Track 6.3  macOS AOT (Mach-O writer)               [4-6 wk]

30.  Track 7.7  Rendering: Level 4 GPU/3D               [future]

31.  Track 9    Multi-syntax (Python/Ruby/Rust modes)     [ongoing]

32.  Track 10   Safety, optimization levels              [ongoing]
```

**Recommended next:** Track 1.3 (CIR coverage) is the highest-leverage item and
the gate for promoting `develop → master` — it is the sole backend, so nothing
downstream (data substrate, ARM64 port, AOT) proceeds, and master is not
updated, until `cir_node → c2mir → MIR` reaches full C89 parity. SMAUG 1.8
already boots/runs/plays on this path; the ~95 remaining integration failures
are the worklist. Build the `.mc11`/`.c` renderer and the gcc-`-fverbose-asm`
fidelity gate to make them mechanical and localizable, then reimplement
eval/exec + REPL on MIR. Latent items surfaced during the SMAUG bring-up:
other signed `int`-returning libc fns on the `long` fallback (declare them
`int`), and the flaky `testfortypedcomma` (uninitialized 2nd declarator in a
multi-declarator for-init).

## The SMAUG Goal

The concrete test case driving Tracks 1-3 is compiling **and running** SMAUG
1.8 (~158K LOC C89) end-to-end. ★ **Achieved on the CIR path (v0.25.0,
2026-05-30):** SMAUG compiles through `cir_node → c2mir → MIR`, links, boots to
a live server (`Realms of Despair ready … port 4000`), and is playable — a
connected client creates a character, navigates the world, and fights (the
Newgate room-109 serpent fight runs). This matches and now exceeds the old
asmjit backend's startup → login → serpent-combat reach, on the sole supported
backend. Remaining: broader gameplay coverage and driving the CIR integration
worklist to parity. The port itself lives in the external
[MadSMAUG](https://github.com/derekbsnider/MadSMAUG) repo.

SMAUG does NOT need C++ features (Tracks 2, 8) — it's pure C. But the
C++ features make madc useful as a general-purpose scripting language
beyond the SMAUG port. The rendering abstraction (Track 7) would let
SMAUG target terminal, web, and GUI from the same game code.

## Plan Index

| Plan | File |
|------|------|
| **ADR 0001 — CIR/c2mir backend (why c2mir, not direct-MIR)** | [../adr/0001-cir-c2mir-backend.md](../adr/0001-cir-c2mir-backend.md) |
| Code Cleanup | [code-cleanup.md](code-cleanup.md) |
| C++ Support | [cpp-support.md](cpp-support.md) |
| Cross-Cutting Insights | [cross-cutting-insights.md](cross-cutting-insights.md) |
| Data Storage & Federation (legacy) | [data-storage-federation.md](data-storage-federation.md) |
| madcdis Core Substrate | [madcdis-plan.md](madcdis-plan.md) |
| madcdis Memory Research | [madcdis-memory-research.md](madcdis-memory-research.md) |
| madcdat External Drivers | [madcdat-plan.md](madcdat-plan.md) |
| Language Interfaces | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| Future Considerations | [future-considerations.md](future-considerations.md) |
| libmadc Phase 4 | [libmadc-phase4.md](libmadc-phase4.md) |
| macOS/ARM64 Port | [macos-arm64-port.md](macos-arm64-port.md) |
| Pre-Compiled Headers | [precompiled-headers.md](precompiled-headers.md) |
| Perry/Rust Integration | [perry-rust-integration.md](perry-rust-integration.md) |
| Rendering Abstraction | [rendering-abstraction.md](rendering-abstraction.md) |
| madc IDE & Editor | [madc-ide.md](madc-ide.md) |
| Multi-Syntax Support | [multi-syntax.md](multi-syntax.md) |
| Typed-Register IR (archived — asmjit-era) | [archived/typed-register-ir.md](archived/typed-register-ir.md) |
| Gecko+MIR Transpiler (archived — superseded by CIR) | [archived/transpiler-backend.md](archived/transpiler-backend.md) |
| Revival Plan (archived) | [archived/revival-plan.md](archived/revival-plan.md) |
