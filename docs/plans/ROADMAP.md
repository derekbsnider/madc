# madc Roadmap

Master plan linking all workstreams. Updated 2026-08-23 (v0.94.0 on
`develop`, v0.92.1 promoted on `master`). **This file is forward-looking:**
release history lives in [CHANGELOG.md](../../CHANGELOG.md) and
[docs/release-notes/](../release-notes/), and the authoritative live snapshot
in `claude_status.json`. Completed work appears here only as a status cell in
its track's table — never as a narrative entry.

**Backend reality:** `madc parser → cir_node (MC11-IR) → c2mir → MIR → JIT` is
the **sole** backend — asmjit and the Gecko parser/MIR-transpiler are gone.
The old parity-with-asmjit-master goal is HISTORY: the CIR backend met the
re-defined promote gate (all class-(a) torture failures fixed, stamped in
[failset-classification.md](../parity/failset-classification.md)) and has been
promoted to `master` repeatedly — v0.38.0 (2026-07-23, the first CIR master)
through **v0.92.1 (2026-08-20, current)**. `master` now tracks
develop's release cadence at owner-called promote points; the standing gate is
`.claude/rules/branching.md` (torture class-(a) burndown, currently satisfied;
the 10 remaining failset entries are class-(b) GNU extensions = roadmap items).
Where a track below was completed on the old backend, it is marked "proven on
old backend, re-establishing on CIR."

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

- **develop = v0.96.0** (2026-08-28): the variadic-class arc —
  `bin/madc examples/embed_hello.cpp` compiles AND RUNS at g++ parity
  (both embedding-example legs are a fulltest gate); libmadc embedding
  fixes (invocation-scoped guest iostream capture, eval-child host
  callbacks); two forest-artifact fixes the push-gate battery caught
  (rebound ranked ctors stamp `local_emit_name`; missing-content husks
  re-include by canonical path). v0.95.x was the `ui::` data-hub
  surface (Track 7 Phase 1) + Colossal Cave Adventure as a pure madc
  `--project` program + the cold-startup arc + the zero-include
  dialect contract; v0.94.0 the MIR hardening wave; v0.93.0 the x86-64
  scalar-convert false-dependency fix. Upstream wave 2 submitted:
  vnmakarov/mir issue #469 + PRs #470/#471 — awaiting review.
- **master = v0.92.1** (promoted 2026-08-20 with six assets: deb, rpm, macOS
  arm64 + x86_64, Windows zip, SHA256SUMS). Every master promotion is
  three-platform gated (`.claude/commands/promote.md` step 5).
- **Baselines (v0.95.0):** counts in [docs/test-status.md](../test-status.md)
  (JIT 1134/0/0TO/9skip; EXE/OBJ + packed + headerless per the v0.95.0
  merge-wave battery), MIR c2mir-gen-test 1143/2286/0, Wine + Mac lanes
  re-validated at promotion.
- **Standing opens:** `value` std::string ingestion; std::vformat phase 2;
  the darwin known-opens (exec:// silent-empty output, the value intrinsic,
  groves os.str()); no macOS full-suite lane (the wine-lane equivalent on Mac
  hardware is the successor arc); Windows C-lane policy (task #58); front-end
  `__attribute__((cleanup))` on locals.
- **Legacy reference (asmjit backend, pre-removal):** GCC-torture parity
  reached ~97.9% and ~475 integration tests passed. Retained only as the
  parity target the CIR path climbed back to — NOT the current state.

---

## Track 1: Language Core

*Make the compiler correct, clean, and fast.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 1.1 | C foundation (GCC parity) | — | **DONE** (97.9% on the old backend; the CIR target) | — |
| 1.2 | Code cleanup Phase A — dispatch table, AST visitor, file split | 2-3 wk | **DONE** (v0.20.1) | [code-cleanup.md](code-cleanup.md) |
| 1.3 | **CIR coverage — `cir_node` (MC11-IR) → c2mir → MIR correctness** | ongoing | **Promote gate MET** (2026-08-12, torture class-(a) burndown complete; stamp in [failset-classification.md](../parity/failset-classification.md)) and develop→master promotions resumed. Remains the standing correctness workstream: class-(b) GNU extensions, new-feature parity, and the reducer-per-fix discipline continue on it | — |
| 1.4 | Code cleanup Phase B — parser dereference/subscript unification | 3 wk | Ready | [code-cleanup.md](code-cleanup.md) |
| 1.5 | Code cleanup Phase C — macro system, token hierarchy | 3 wk | Ready | [code-cleanup.md](code-cleanup.md) |
| 1.6 | **SIMD — add a minimal generic-vector extension to MIR (types + insns + per-target codegen) and a c2mir `vector_size` front-end** | large | **In progress (raise the floor)** — branch `feature/simd-vector-support-codex` on `/workspace/mir` @`2ffebff`: partial MIR `v128` floor + c2mir `vector_size` / `ext_vector_type` front-end; all 37 GCC c-torture vector-construct execute tests pass under C2MIR `-ei`/`-eg`; no known ≤16-byte SIMD gap remains. Remaining: ≥32-byte (AVX/YMM) vector ABI and the broader generic-vector floor (registers, interpreter, per-target codegen); design for **upstream** | — |
| 1.7 | **Cold JIT startup toward tinycc latency** | ongoing | **In progress** — packed Adventure has landed positional auto-include filtering, lazy MIR generation, shared forest/prelude state, lazy MEMBER hydration, demand-driven derived restore, and c2mir registry pages (@`ad9be08d`, another −2.06% Ir). Remaining measured work: re-attribute host STL/string allocation after the arena; the zstd spine/arena raw-vs-compressed size trade needs owner direction | [cold-jit-startup.md](2026-08-22-cold-jit-startup.md) |

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

Per-checkpoint history for Track 1.6 — one entry per commit with its
GCC/clang/C2MIR validation evidence — lives in the branch's own git log and
in `claude_status.json`'s deferred entry, not in this roadmap.


**Track 1.3 is the central workstream.** It is the sole backend, so its
coverage *is* the bar for promoting `develop → master`; the gate is MET and
promotions run at owner-called points. SMAUG 1.8 boots, runs, and is playable
on this path (a real-world end-to-end proof). Remaining on this track:
class-(b) GNU extensions, and reimplementing eval/exec + REPL on MIR.

**Dependencies:** 1.2 before 1.3 (both satisfied).

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
| 2.7 | Exception handling (SJLJ) | 3-4 wk | **Mostly done** (v0.21.0) — Phase A + B (scalar throw/catch + RAII unwind); **object/class-typed catch OPEN** | [cpp-support.md](cpp-support.md) |
| 2.8 | Quality of life | Ongoing | **Started** — access control, auto token position | [cpp-support.md](cpp-support.md) |
| 2.9 | Generic extern class ctor/dtor | — | **DONE** (v0.21.0) — replaces per-type switch boilerplate | — |
| 2.10 | **Single-name local instantiations (flattened→Itanium-mangled)** | 1-2 wk | **Planned** | — |
| 2.11 | **Self-host: madc compiles its own C++11 source** (ultimate dogfood) | large | **Planned (2026-06-26)** — feature audit done | failure-driven `selfhost` harness |

**2.3 remaining:** pointer-to-const enforcement (`*p` writes), const methods.
**2.7 remaining:** exceptions are SCALAR-ONLY (int/double/cstr/`catch(...)`).
Throwing/catching user-class or `std::` exception objects, and inheritance-aware
`catch (const Base &)` of a derived throw, are unsupported — the SJLJ runtime
carries no thrown object + catch dispatch is an integer-tag chain, not an RTTI
type match. Tracked as **P1.1e** in [cpp-support.md](cpp-support.md).
**2.8 remaining:** enum class, auto type deduction, broader real-iostream
output replacement, scope-level destruction.
**2.11 — Self-hosting (the ultimate dogfood test).** Audit (2026-06-26) of madc's own
`src/`+`include/` C++11: heavy templates/variadics, `dynamic_cast` (871), range-for (386),
lambdas (158), `std::move`, decltype, full STL (vector/map/set **+ stack/queue/deque/list**),
streams (sstream/fstream), `<algorithm>` (`std::sort` w/ lambda comparators), `<functional>`
(`std::function`×16), `unique_ptr`×32. Real pure-virtual/abstract = **0 uses** (not a blocker).
Gaps to build, hardest-first: **`std::function`** (type erasure over lambdas) → **`unique_ptr`**
(move-only RAII) → **`<algorithm>`** over iterators → **stack/queue/deque/list** → full
**stringstream/fstream** classes → decltype/alias-template/enum-class/`=default`/`=delete`
coverage. First step = a failure-driven `selfhost` harness: run madc (parse/sema) on each
src/include file, pass/fail → the empirical gap list; climb smallest TU → `parser.cpp`.

**2.10 — name every madc-local template instantiation by its Itanium mangled
name, retiring the flattened-key scheme.** Today madc carries TWO naming schemes
for the same entity: libstdc++-exported symbols are referenced mangled-direct
(`_ZNSt6vectorIiSaIiEE…`, via `madc_mangle`), while madc-monomorphized local
bodies (class-template instances like `vector<int>` + nested types, free-fn-
template instances `__ns_std__Destroy`/`__addressof`, member-template instances)
get flattened keys (`vector_int32_t_std__allocator_int32_t_…`). Carrying two
names for one entity is a standing source of confusion and drift (the mangler
should be the single name source). Unifying on the mangled name everywhere
(symbol table, emitted C, call sites, struct tags) gives: (a) `--emit=c11`
diffability against g++; (b) **free linker dedup** — a local instantiation whose
mangled name coincides with a libstdc++ weak export ODR-merges automatically, so
the "exported vs inline-only" decision disappears (always mangle; emit a body
only when nothing else defines it). **Cost/risk:** mangler completeness —
correct Itanium for nested types, member templates, and substitution compression
(`S_`/`S0_`); a wrong name becomes a link error or a silent wrong-symbol bind, so
migrate one category at a time behind the full gate. The member-template
convergence (Phase 2.10's first consumer — emit a local body under the mangled
name `itanium_mangle_member_template_sub` already computes when the owner is
local/not-exported) establishes the pattern.

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
| 3.5 | Project build driver v1 (`--project`) | — | **DONE** | [madc-project-build-driver](../superpowers/plans/2026-06-08-madc-project-build-driver.md) |

**Dependencies:** 1.4 (parser cleanup) unblocks 3.2. 1.5 (token cleanup) before 3.3.

- **Unified front-end representation refactor (FULL DESIGN, later-stage optimization):**
  [2026-06-09-frontend-representation-refactor.md](2026-06-09-frontend-representation-refactor.md)
  is the comprehensive design that **details and reframes 3.1/3.3/3.4** (Phase-2 is now
  pre-PARSE `cir_node` AST, not just pre-lex; Phase-3 modules = the embedded forest) **and adds
  the Track-1 front-end prerequisites they depend on**: flat value-record token buffer + index
  cursor + source-stack (PoC: 3.6–4.5× over `deque<TokenBase*>`); an arbitrary-precision
  out-of-line value pool (fixes `__int128`/`_BitInt`, currently a 64-bit alias); `uid` as the
  universal handle with madc metadata in `uid`-keyed side-arrays (the c2mir-blind superset) +
  `uid`→MIR for debug info; static-immutable/project-volatile forest with materialize-on-resolve;
  and an optional, fenced c2mir hook seam (HIR/LIR; OSR/deopt + polyglot dynamic execution are
  flagged research-grade). Subsumes [2026-06-09-embedded-header-forest-design.md](2026-06-09-embedded-header-forest-design.md)
  and [2026-06-09-lazy-member-body-instantiation-plan.md](2026-06-09-lazy-member-body-instantiation-plan.md)
  (LANDED). **After** the current real-header correctness work, not before.

- Project build driver v1 landed (`--project compile_commands.json`, multi-TU compile+link+JIT-run of `main`). Replaces the need for a hand-written umbrella translation unit (like SMAUG's `SMAUG.mad`) for multi-file C programs. Deferred follow-ons: Makefile-subset reader + a link-description section (compile_commands.json carries no link rule); native `.madproj`; other-ecosystem readers; honoring `ProjectTU.working_dir` for include resolution (SMAUG will need it); `--project` + `--emit=c11` (project mode currently ignores `--emit`); real object-code-to-disk (parity-recovery item — asmjit on master had it); parallel/incremental build + manifest auto-detection. **Next concrete step: SMAUG bring-up via a generated `compile_commands.json` (separate follow-on plan).**

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

- **Type table + value ABI (DESIGN AGREED 2026-06-12):**
  [2026-06-12-type-table-value-abi-design.md](2026-06-12-type-table-value-abi-design.md)
  — one segmented uint32-typeid table (primitives / system-forest / project
  segments) as the canonical type identity, plus a 32-byte `madc_value`
  interchange struct (16-byte payload inlines every madc primitive incl.
  `__int128`/`_Complex`/`v128`; SSO; refcounted cells; gradual-typing flags
  LOCKED/COERCE/NULLABLE; re-tag unrestricted by default). **Eval package C is
  the first consumer**; the cir_node `datadef` side-array (refactor P3), forest
  type-ref serialization (P4), and the tag-arithmetic retirement are later
  campaigns on top. NaN-boxing (5A.5) stays internal-madcdis-only.

---

## Track 5: Data Substrate & Storage

*Three-tier data architecture: core substrate (madcdis) + external
drivers (madcdat) + language-conventional interfaces.*

### Track 5A: madcdis — Core Data Substrate (`libmadcdis`)

*Dependency-free core data substrate. The interfaces and implementations are
currently delivered through `libmadc`; the standalone `libmadcdis.so` split
remains planned. Pools, values, datasets, relations, query IR, typed flows,
raw channels, processes, and standard dependency-free drivers belong here.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 5A.1 | Library restructure — split madcdis from madcdat | 2-3 wk | **Partial** (ownership split; standalone library pending) | [madcdis-plan.md](madcdis-plan.md) |
| 5A.2 | DataSet/Relation/Query/Schema/Mapper → `include/madcdis/` | 1 wk | **DONE** | [madcdis-plan.md](madcdis-plan.md) |
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
| 5A.13 | Lazy Cursor/Sink/Flow + ABI-compatible streaming extensions | — | **DONE** @cd1f19c6 | [2026-08-07-data-channel-streaming-process-flow-plan.md](2026-08-07-data-channel-streaming-process-flow-plan.md) |
| 5A.14 | Raw channels + format bridge + explicit Process (`memory/file/FIFO/TCP/UDP/UDS/exec`) | — | **DONE** @cd1f19c6 | [2026-08-07-data-channel-streaming-process-flow-plan.md](2026-08-07-data-channel-streaming-process-flow-plan.md) |
| 5A.15 | Standard dependency-free record drivers (DSV/FLR/VLR); DSV native streaming | — | **DONE** @079ca8c3/@533947e1 | [madcdat-plan.md](madcdat-plan.md) |
| 5A.16 | Schema observation & hardening (dynamic → observed → locked; deopt-style guards; logical sibling of 5A.7's physical encodings) | 3-4 wk | Planned (design approved 2026-08-20) | [2026-08-20-data-hub-projection-rendering.md](2026-08-20-data-hub-projection-rendering.md) |

### Track 5B: madcdat — External Storage Drivers (`libmadcdat`)

*Optional companion library. Depends on libmadcdis. External backends.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 5B.1 | Library restructure — libmadcdat depends on libmadcdis | 1-2 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |
| 5B.2 | Optional external-library file/storage integrations (`snapshot://` remains planned) | Ongoing | Planned; DSV/FLR/VLR moved to core 5A.15 | [madcdat-plan.md](madcdat-plan.md) |
| 5B.3 | Keyed local DB drivers (BDB, GDBM, QDBM) | — | **DONE** | [madcdat-plan.md](madcdat-plan.md) |
| 5B.4 | SQLite driver | — | **DONE** | [madcdat-plan.md](madcdat-plan.md) |
| 5B.5 | Network DB drivers (MySQL, PostgreSQL) | 3-4 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |
| 5B.6 | Graph DB drivers (FalkorDB, Neo4j) | 3-4 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |
| 5B.7 | External-library service drivers (libcurl HTTP/HTTPS/REST/FTP/S3, MCP, mail) | 4-6 wk | Planned; raw TCP/UDP/UDS complete in core | [madcdat-plan.md](madcdat-plan.md) |
| 5B.8 | Structured text adapters (SMAUG areas, mbox, TOML) | 2-3 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |

### Track 5C: Language-Conventional Interfaces

*Multiple syntactic surfaces over the same data substrate.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 5C.0 | Script-facing channel surface (`madc::channel` in `<ns_madc>`, `exec://` scheme, tcp/exec/httpget suite legs) | — | **DONE** v0.72.0 | [2026-08-08-track5c-script-channels-plan.md](2026-08-08-track5c-script-channels-plan.md) |
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
- The host-facing Track 5 core is active and validated through `libmadc`.
  Compiler-integrated language surfaces still depend on complete CIR coverage
  for their chosen C++ standard.
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
| 6.1 | macOS/ARM64 MVP (via MIR — c2mir + MIR are already cross-platform) | 10-15 wk | **Complete** (v0.45.0 hosted binaries; v0.76.0 public tarballs) | [macos-arm64-port.md](macos-arm64-port.md) |
| 6.2 | macOS SIMD (NEON) | 2-3 wk | Blocked on Track 1.6 (raise MIR) | [macos-arm64-port.md](macos-arm64-port.md) |
| 6.3 | macOS AOT (Mach-O writer + aarch64 cross-gen) | 4-6 wk | **Complete** (v0.76.0: `-o` for C and C++; deferred residue: `libmadc.dylib`, in-process `.o` loader) | [2026-08-07-macos-release-lane-plan.md](2026-08-07-macos-release-lane-plan.md) |
| 6.4 | Windows port (a working Windows build + release artifacts) | large | **Complete** (shipped in the v0.82.0 three-platform release; current: Wine 1061/0, genuine Windows 1010/0). Follow-ups: GitHub-Actions release automation, C-lane policy (task #58) | [2026-08-12-windows-release-lane.md](2026-08-12-windows-release-lane.md) |

**Dependencies:** 1.3 (IR) dramatically reduces 6.1 effort.

---

## Track 7: Data Projection & Rendering (`ui::` + the data hub)

*One substrate for the whole lifetime of data; every UI is a projection of
it. Semantic rendering from teletype to Unreal. WCAG by design. Access ×
wants × needs × capability negotiation; per-connection JIT specialization.
Design **APPROVED 2026-08-20**:
[2026-08-20-data-hub-projection-rendering.md](2026-08-20-data-hub-projection-rendering.md)
— 15 demands, keys+levels access model, value-first semantic IR, the two
pilots. [rendering-abstraction.md](rendering-abstraction.md) stays the
reference for levels/negotiation/WCAG detail.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 7.1 | Hub projections + value-typed semantic IR + Level 0 (text) + access model (keys/levels) + verb registry + `ui::prompt` | 3-5 wk | **DONE — v0.95.0**: the text-adventure pilot is fully playable (94/94 logs byte-identical, gated); surface documented in [docs/language/ns-ui.md](../language/ns-ui.md) | [phase 1](2026-08-20-track7-phase1-text-adventure.md) |
| 7.2 | Level 1 terminal backend + interaction rework (pulled 8.1's piece table forward as a component type) | 3-4 wk | **DONE — 2026-08-25** (R1–R5: interaction core, projection-as-data, script-entity verbs + availability checks, the line + visual editors over one action set; the target is the HAND-ROLLED VT100/xterm provider — owner-decided over vendoring curses — behind the `tui_provider` seam); madcide (8.x) now unblocked | [plan](2026-08-24-ui-interaction-rework-and-texteditor.md) |
| 7.3 | Reactivity — compiler-tracked deps + per-connection projection instances + semantic-diff wire | 2-3 wk | Planned | [design](2026-08-20-data-hub-projection-rendering.md) |
| 7.4 | Level 2 — 2D graphics (Skia/Cairo) | 3-4 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.5 | Level 3 — Web backend (semantic diffs over WebSocket + thin JS) | 4-6 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.6 | Level 3 — Native GUI (SDL2/GTK) | 4-6 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.7 | Level 4 — GPU/3D (WebGPU/Metal) | Future | Future | [rendering-abstraction.md](rendering-abstraction.md) |

**Dependencies:** Track 5's shipped substrate (entities/relations/adapters)
— met. Feeders flagged, not absorbed: the value ABI arc (atomic cell
refcounts per demand 15), `value` std::string ingestion, eval/exec for
script-attached verbs (post-Phase-1). Phase 1 is **library-surface only** —
no new parser syntax; `render { }` blocks are a later ergonomic layer
(cpp-first-api).

---

## Track 8: Tooling (madcide + libmadcedit)

*A Turbo-C style IDE and reusable editor library, built in madc.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 8.0 | madcide v1 — the hub-doc Phase-2 gate: buffer + undo through the entry lens, diagnostics/outline panes as projections of live compiler data (`madc::diagnostics`/`outline`), keybinding profiles as data (JOE/WordStar `^K` chords default, owner-directed; configurable via `ui::tui_bind_keys`) | — | **DONE — 2026-08-25** (`tools/madcide` — relocated from examples/ per the owner's tool-not-example ruling; wave 1149/0/0/9 + EXE 1105/0) | [plan](2026-08-25-madcide-phase2.md) |
| 8.1 | libmadcedit core — piece table, cursor, undo, CUA keys | 3-4 wk | Largely subsumed: piece table + undo shipped as the hub text component (7.2/8.0); remaining = the reusable-library packaging question | [madc-ide.md](madc-ide.md) |
| 8.2 | libmadcedit curses rendering | 2-3 wk | Subsumed by the 7.2 TUI provider (hand-rolled VT100 target behind the seam) | [madc-ide.md](madc-ide.md) |
| 8.3 | Syntax highlighting + keybinding profiles (Vim, Emacs, Turbo-C) | 2-3 wk | Profiles-as-data SHIPPED (8.0: joe/pico; more profiles = data files); highlighting = a projection-hints design, Future | [madc-ide.md](madc-ide.md) |
| 8.4 | madcide shell — file tree, tabs, build, errors | 3-4 wk | Unblocked by 8.0 (diagnostics pane shipped; file tree/tabs/build integration next) | [madc-ide.md](madc-ide.md) |
| 8.5 | Advanced — find/replace, split views, go-to-def | Ongoing | Future (find shipped in 8.0; go-to-def wants the outline's deeper walk) | [madc-ide.md](madc-ide.md) |

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
     ├── ★ PARITY-TO-MASTER GATE MET (2026-08-12); promotions resumed
     └── Remaining: class-(b) GNU extensions; eval/exec + REPL on MIR

 ║── Track 7.1  Hub projections + Level 0                [DONE v0.95.0]
 ║   └── pilot: the text adventure — FULLY PLAYABLE, 94/94 logs
 ║       byte-identical (fulltest gate); ui:: documented

12.  Track 1.4  Code cleanup Phase B                    [3 wk]
     └── Parser dereference & subscript unification
         Unblocks: PCH transition, parser resilience

 ║── Track 7.2  Rendering: Level 1 terminal backend      [DONE 2026-08-25]
 ║   └── R1–R5 shipped (VT100 provider, checks, lined+vised editors)
 ║── Track 8.0  madcide v1 (hub Phase 2 gate)            [DONE 2026-08-25]
 ║   └── tools/madcide: undo through the entry lens, diagnostics/
 ║       outline panes over madc::diagnostics/outline, keybinding
 ║       profiles as data (JOE ^K chords default); 8.1/8.2 subsumed

15.  Track 8.4  madcide shell                            [3-4 wk]
     ├── File tree, tabs, build integration (8.0 = the editor + panes)
     └── Self-hosting milestone: edit madc in madcide

16.  Track 3.2  PCH transition                           [2-3 wk]
     └── Replace text-embedded stubs with pre-compiled

17.  Track 6.1  macOS/ARM64 MVP (via MIR)               [DONE v0.76.0]

18.  Track 4.2  C ABI shim                               [2-3 wk]

19.  Track 1.5  Code cleanup Phase C                    [3 wk]
     └── Macro system unification, token hierarchy flattening

20.  Track 7.3-7.6  Rendering Levels 2-3                [4-6 wk each]

21.  Track 4.3  Fork-based worker isolation              [3-4 wk]

22.  Track 3.3  PCH Phase 2 — AST serialization         [4-6 wk]

     ── CIR PARITY GATE (MET 2026-08-12) ────────────────────
     Track 1.3 reached the promote gate; data work is unblocked
     (5A.13-15 and 5C.0 already shipped).

23.  Track 5A.1-3  madcdis library restructure            [3-4 wk]
     Track 5B.1    madcdat depends on madcdis             [1-2 wk]

24.  Track 5A.4-5  Pools + value system                  [6-8 wk]

25.  Track 5A.9   Federated query planner                [4-6 wk]

26.  Track 6.2  macOS SIMD (NEON)                       [2-3 wk]

27.  Track 4.4  Node.js integration                      [4-6 wk]
     Track 4.5  Rust bindings                            [2-3 wk]

28.  Track 3.4  Modules (.madm)                          [6-8 wk]

29.  Track 6.3  macOS AOT (Mach-O writer)               [DONE v0.76.0]

30.  Track 7.7  Rendering: Level 4 GPU/3D               [future]

31.  Track 9    Multi-syntax (Python/Ruby/Rust modes)     [ongoing]

32.  Track 10   Safety, optimization levels              [ongoing]
```

**Where things actually stand (2026-08-20):** the promote gate is MET, the
three platform lanes all ship, and the data-substrate core (5A.13-15, 5C.0)
is live. The open fronts are the Standing opens in Current State, Track 1.6
(the SIMD floor), Track 5's remaining substrate phases (5A.3-5A.12, 5A.16,
5B, 5C), Track 7 (data projection & rendering — design APPROVED 2026-08-20;
**Phase 1, the text-adventure pilot, is next up**), and Track 2.10/2.11
(mangled-name unification, self-hosting).

## The SMAUG Goal

The concrete test case driving Tracks 1-3 is compiling **and running** SMAUG
1.8 (~158K LOC C89) end-to-end. ★ **Achieved on the CIR path (v0.25.0,
2026-05-30):** SMAUG compiles through `cir_node → c2mir → MIR`, links, boots to
a live server (`Realms of Despair ready … port 4000`), and is playable — a
connected client creates a character, navigates the world, and fights (the
Newgate room-109 serpent fight runs). This matches and now exceeds the old
asmjit backend's startup → login → serpent-combat reach, on the sole supported
backend. Remaining: broader gameplay coverage. The port itself lives in the
external [MadSMAUG](https://github.com/derekbsnider/MadSMAUG) repo.

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
| **Data Hub & Projection–Rendering (Track 7, APPROVED 2026-08-20)** | [2026-08-20-data-hub-projection-rendering.md](2026-08-20-data-hub-projection-rendering.md) |
| Track 7 Phase 1 — the Text-Adventure Pilot | [2026-08-20-track7-phase1-text-adventure.md](2026-08-20-track7-phase1-text-adventure.md) |
| Rendering Abstraction (levels/negotiation/WCAG reference) | [rendering-abstraction.md](rendering-abstraction.md) |
| madc IDE & Editor | [madc-ide.md](madc-ide.md) |
| Multi-Syntax Support | [multi-syntax.md](multi-syntax.md) |
| Typed-Register IR (archived — asmjit-era) | [archived/typed-register-ir.md](archived/typed-register-ir.md) |
| Gecko+MIR Transpiler (archived — superseded by CIR) | [archived/transpiler-backend.md](archived/transpiler-backend.md) |
| Revival Plan (archived) | [archived/revival-plan.md](archived/revival-plan.md) |
