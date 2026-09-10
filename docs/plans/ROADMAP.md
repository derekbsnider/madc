# madc Roadmap

Master plan linking all workstreams. Updated 2026-09-01 (v0.97.0 on
`develop`, v0.95.2 promoted on `master`). **This file is forward-looking:**
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

- **develop = v0.98.0** (2026-09-06): the macOS full-suite release — the
  darwin full-suite lane (GitHub's arm64 + Intel mac runners) green on
  both arches (1293/0/0TO/24skip / 1294/0/0TO/23skip) after the seven-wave D4
  burndown; the SIMD floor (MIR_T_V128, NEON) + the vector calling
  convention gated against the host compiler; Apple stack-argument
  packing; the libc++ `<list>` completion site; the stdlib flavor
  boundary for `cout << value` / `println(std::string)`; `-w`; the
  owner law that every platform lane's FULL suite gates master
  (`lane_ledger.sh check --release`, release tier: libcxx, darwin-suite,
  genuine-win — all driven from the container). v0.97.0 was the madcide
  interaction arc + carrier literal ergonomics; v0.96.0 the
  variadic-class arc.
- **previous develop lines:** v0.96.0 (2026-08-28) the variadic-class arc —
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
- **master = v0.98.0** (promoted 2026-09-06 with every platform lane's FULL
  suite green at the candidate — `lane_ledger.sh check --release` 7/7; six
  CI-owned assets: deb, rpm, linux tarball, Windows zip, macOS arm64 + x86_64). Every master promotion is
  three-platform gated (`.claude/commands/promote.md` step 5).
- **Baselines (v0.98.0):** counts in [docs/test-status.md](../test-status.md)
  (linux JIT 1308 passed / 0 failed / 0 timed out / 9 skipped; EXE 1249/0, OBJ 1249/0, packed 1308/0/0/9,
  headerless 1274/0/0/43; libc++ flavor lane 1303/0/0TO/14skip; wine64 1251/0/0TO/66skip;
  genuine Windows 1253/0/0TO/64skip; the darwin FULL suite on GitHub's mac runners
  arm64 1293/0/0TO/24skip / Intel 1294/0/0TO/23skip; c-testsuite 220/220, baseline empty) — every
  lane recorded in docs/lane-status.tsv at the promoted content.
- **Standing opens:** `value` std::string ingestion; std::vformat phase 2;
  the libc++ nested-map construct relowering (KG Gap
  libcxx_nested_map_value_type_construct_relower — testphpdumpiter carries a
  .libcxx_skip with the reason); the darwin mac-battery known-open (the
  include-free value intrinsic, 10/1 on both runner arches); Windows C-lane
  policy (task #58); front-end
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
| 1.6 | **SIMD — add a minimal generic-vector extension to MIR (types + insns + per-target codegen) and a c2mir `vector_size` front-end** | large | **Floor LANDED in-tree (v0.98.0)** — MIR `MIR_T_V128` on x86-64 and aarch64 (NEON lane arithmetic, compares, shifts), the c2mir `vector_size` / `ext_vector_type` front-end, and the 128-bit vector CALLING CONVENTION on SysV, AAPCS64, Apple and win64 gated against the host compiler (`scripts/vector_abi_gate.sh`, 28 lines, in fulltest). Open: wider vectors (256/512-bit), upstreaming to vnmakarov/mir (held until the master promotion). History: branch `feature/simd-vector-support-codex` on `/workspace/mir` @`2ffebff`: partial MIR `v128` floor + c2mir `vector_size` / `ext_vector_type` front-end; all 37 GCC c-torture vector-construct execute tests pass under C2MIR `-ei`/`-eg`; no known ≤16-byte SIMD gap remains. Remaining: ≥32-byte (AVX/YMM) vector ABI and the broader generic-vector floor (registers, interpreter, per-target codegen); design for **upstream** | — |
| 1.7 | **Cold JIT startup toward tinycc latency** | ongoing | **In progress** — packed Adventure has landed positional auto-include filtering, lazy MIR generation, shared forest/prelude state, lazy MEMBER hydration, demand-driven derived restore, and c2mir registry pages (@`ad9be08d`, another −2.06% Ir). Remaining measured work: re-attribute host STL/string allocation after the arena; the zstd spine/arena raw-vs-compressed size trade needs owner direction | [cold-jit-startup.md](2026-08-22-cold-jit-startup.md) |
| 1.8 | **`import` — the module binding** (owner 2026-09-06: the C++20 module import made whole — interface AND binding; `-l` is the build-system spelling of the same resolver; `#load` STAYS as the low-level verbatim-file directive, owner ruling during the slice). Module map as data (name → interface + per-OS image), ONE platform-spelling owner `src/madc_modules.cpp` (fixed the Windows-less `MADC_DSO_SUFFIX` `-l` mapping), per-TARGET binding policy (JIT open · native link closure · alias-form members runtime-resolved in every lane — closed the `testdlopen.exe_skip` follow-on), recognized in directive position under `--std=c++20`+ / the dialect | 2-3 wk | **MERGED to develop 2026-09-06**; slice 1 found alias statement/cast lookup and namespaced C-linkage gaps, to close before 7.5 slice 2 — [spike findings](2026-09-06-ASTRA-HANDOFF-webview-spike.md#findings) | [design §3.1](2026-09-06-ui-web-target-and-madcide-gui.md) · [docs](../language/import.md) |

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
| 2.12 | **C++11 conformance measured by a third-party suite** — gcc's g++ testsuite (`g++.dg` + `g++.old-deja` `dg-do run` tests with no target clause or `target c++11`: ~2330) driven by the existing gcc-torture runner, ratcheted against a baseline | 1-2 wk to first measurement | **Planned (2026-09-04)** — a ROADMAP GOAL, never a push or release gate (owner ruling); the metric is class-(a) C++11 failures → 0 | [2026-09-04-cxx-conformance-lane.md](2026-09-04-cxx-conformance-lane.md) |

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
| 6.2 | macOS SIMD (NEON) | 2-3 wk | **Complete** (v0.98.0: MIR_T_V128 on aarch64 — NEON lane arithmetic, compares, shifts; the vector calling convention on AAPCS64 and Apple gated against the host compiler by `scripts/vector_abi_gate.sh`; Apple stack-argument packing) | [macos-arm64-port.md](macos-arm64-port.md) |
| 6.3 | macOS AOT (Mach-O writer + aarch64 cross-gen) | 4-6 wk | **Complete** (v0.76.0: `-o` for C and C++; deferred residue: `libmadc.dylib`, in-process `.o` loader) | [2026-08-07-macos-release-lane-plan.md](2026-08-07-macos-release-lane-plan.md) |
| 6.4 | Windows port (a working Windows build + release artifacts) | large | **Complete** (shipped in the v0.82.0 three-platform release; current: wine64 1251/0/0TO/66skip, genuine Windows 1253/0/0TO/64skip — a release-tier lane driven from the container over the W0.2 channel). Follow-ups: GitHub-Actions release automation, C-lane policy (task #58) | [2026-08-12-windows-release-lane.md](2026-08-12-windows-release-lane.md) |

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
| 7.5 | **Level 3 — the WEB target**: the value tree → DOM through the platform's own webview (WebView2 / WKWebView / WebKitGTK / Android WebView; a browser over a socket = the same page on another transport). `web_model` (engine) + a `web_target` fragment bound through `import madcwebview` + one embedded page; target-generic `ui::open/render/event/bind_keys`; the chord/key owner shared with `tui_model` | 4-6 wk | **Slice 1 complete 2026-09-06**: Linux GO, Windows GO (Session 1 + window evidence), macOS Intel GO (native build, on-screen window + DOM reply, exit 0 in 6.861 s). **Slice 2 library/build half complete on Astra branch (2026-09-07)**: upstream typed C API, platform libraries, GUI JIT/EXE gate; [build/interface hand-back](../building-webview.md). the two 1.8 binding gaps CLOSED 2026-09-07 (cc848ea8 `extern "C"` in a namespace keeps the C name; 6cb90e4d one module-member lookup owner); both halves merged (s159). **Slice 2 ENGINE half complete 2026-09-07** (`feature/web-provider-engine-claude`, [plan](2026-09-07-web-provider-engine-plan.md)): `keys.h` / `ui_focus.h` / `ui_input.h` owners out of `tui_model`, `web_model`, `ui::open(target)` + the script-hosted target seam (`register_host` / `post_event`, a display-free fake), `<ns_ui_web>` + one embedded page, `ui::eval_page`, `tests/gui/ui_web_hello` + `ui_web_edit` green JIT/exe/`.o` under Xvfb, `vised --web`; guards default off; objects carry `__madc_module_deps`; lazy module rows. Next = slice 3 (8.6 madcide GUI mode) — [findings](2026-09-06-ASTRA-HANDOFF-webview-spike.md#findings) | [design](2026-09-06-ui-web-target-and-madcide-gui.md) |
| 7.6 | Level 3 — Native GUI (SDL2/GTK) | — | **Merged into 7.5** (2026-09-06): the platform webview IS the native GUI host; no toolkit-per-platform row | [design](2026-09-06-ui-web-target-and-madcide-gui.md) |
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
| 8.6 | **madcide GUI mode** — the first customer of the 7.5 web target: ONE client loop picking the target by flag, ONE composer (`compose_ide_tree` gains layout hints the TUI ignores: region / tabs / popup), the common-denominator workbench of VS Code · CLion · Atom · Qt Creator in our own palette (inspiration, never a clone), `@gui` theme sections, requests' output into a panel | 3-4 wk | **LANDED 2026-09-07** (slice 3 on `feature/madcide-gui-claude`, [plan](2026-09-07-madcide-gui-plan.md)): the `--gui` flag over one target-generic client + one composer; region/tabs/popup layout hints; `@gui` themes; status items; the live-build panel; GUI DOM-snapshot fixtures. **slice 3.1 2026-09-07**: the two live-window fixes the owner found — syntax colour flows to the window (semantic span class per renderer, `@gui`-themeable) and the caret scrolls into view — plus the dialect-literals rule/gate; bug #3 (resize-fill, library layer) held. **Chrome milestone S0–S5 LANDED 2026-09-08** (`feature/web-editor-incremental-claude`, [design](2026-09-07-madcide-gui-chrome-and-modular-ui-design.md)): S0 resize-fill (GTK GL over a no-SHM TCP X display → `GSK_RENDERER=cairo` default); the web editor's incremental patch protocol + newline index (perf); mouse caret / drag-select; the ui event vocabularies as enums; S5 split layout (slots; the ^K O window stack); S1 menus/commands as data (`default.menu`, the registry gate); S2 the GTK4 native menu bar (`madcwebview_chrome`); S3 the status bar as chrome (format seats as items); S4 native file dialogs (a request/response verb, `GtkFileDialog`); plus four carrier fixes found on the way (`var &r = o["h"]`, `6 == v` / `v == E::z`, the mixed conditional, `var &` in a `%s` position). **S6 2026-09-08** (`feature/madcide-gui-prompts-claude`, the owner's post-release round): the prompts as dialogs (quick input / confirm with buttons that post the scope's actions by name; the core owns the text), a click picks the window (the window-index `tag` echoed on pointer events; a header press activates at no position), the status bar's chrome look, and the `(^C aborts)` fix (a modal scope consults an action's sequence). **S7 2026-09-08** (`feature/native-chrome-win-mac-claude`): the native menu bar and file dialogs on Cocoa (the application's main menu; `NSOpenPanel` / `NSSavePanel` sheets) and Win32 (an `HMENU` bar through a comctl32 subclass; `IFileOpenDialog` / `IFileSaveDialog`) behind the SAME `madcwebview_chrome` API, one key-spelling reader for the three arms; the release targets build the webview library and the packagers ship it (weak dependency on Linux). **The local-IDE polish for master** ([plan](2026-09-08-madcide-local-polish-for-master.md); owner ruling 2026-09-08: the next master release is the polished local IDE, then the client-server + headless arc): **P1 colours 2026-09-08** — one theme, two renderers (`madcdis/ui_style.h` the one style + spec parser; the DOM model renders the scheme's spec as `st-*`/`fg-*`/`bg-*` classes over a sixteen-colour `pal-*` palette, bold-as-bright; `page.css` loses its private syntax palette; gate `check-one-style-vocabulary.sh`); **P2 2026-09-08** the list overlays as dialogs (the `dialog` hint; a click chooses through the one focus owner) + File → Open Project… (one manifest reader) + the project kind console\|gui (the manifest's `kind`, the `^T` row, the PE subsystem via `-mwindows`/`-mconsole`, the PE gate's subsystem read); **P3a 2026-09-08** the bottom panel with tabs (Problems / Output; the strip as data, one `show_diags` surface, one `diag_items` builder; a click picks any list row); **P3b-1 2026-09-08** Run in the window (`madcrun://` / `madcproj://` channel schemes over the one spawn owner's `child_body`; the Output tab streams the program, `[exit N]`; the silent no-op Run fixed; a refused request is said); **P3b-2 2026-09-08** the Terminal tab (`Process` pty + `pty://`, `?pty` on the run schemes, `ui::term_feed` over `madcdis/term_screen.h`, `ui::key_bytes` = `tui_key_bytes`, `termfocus` routing with `@terminal ^] termunfocus`, Run by kind: console → Terminal, gui → Output, the Shell on the pty); **P4 2026-09-08** editor tabs over the buffer ring (the `tabs` array on a node docked first into the editor slot; commands take ARGUMENTS — the action input's `arg`, `bufsel <index>`, the palette's prompt). NEXT = verify P2–P4 on the container, battery, merge → the master release. Follow-ups: a browser-view tab, the DOM command palette, pane drag / persisted layout, the remote transport / multi-user arc | [design](2026-09-06-ui-web-target-and-madcide-gui.md) **Hands-on round 2026-09-08 (post-P4, on the staged Win/Mac sets):** a dialog's Close button closes it (the @pane cancel-by-name admitted in the action region); the Build menu carries every ^B row as a direct item (Build → Run, no overlay); the window's wait is the cooperative scheduler's bounded wait (the `tick` host op; `madcwebview_tick` on GTK/Cocoa/Win32) so a program's output streams into the Terminal tab with no keystroke; a closed dialog's furniture no longer lingers in the panel (a re-used element wipes its old kind); the panel and sidebar are resizable (splitters; double-click maximizes; sizes remembered). **Post-master, in order (owner 2026-09-09):** the Terminal (and any panel tab) in its OWN WINDOW — a second window on the same session, the first client-server step (a window is a client; the web target's loop serves several windows); paned views and tabs in TUI mode too (the Turbo-C / RHIDE shape); the darwin prelude's per-header scoping (KG Gap darwin_umbrella_prelude_whole_set_visibility); a browser-view tab; SGR colours in the Terminal onto the one style; ConPTY; the DOM command palette; then the client-server + headless arc proper (design doc first). **Cross-reference 2026-09-09** (owner request before the client-server step): the owner's [Nexus vision](2026-09-08-madc-ide-nexus-vision.md) and [Multi-View architecture](2026-09-08-madc-ide-multi-view-architecture.md) documents reviewed against the settled design in [the cross-reference](2026-09-09-nexus-and-multiview-cross-reference.md): same vision, extended in four places (event sourcing, per-node authority, the View abstraction with correlation maps, debugger/disassembly Views); consequence — "the Terminal in its own window" is an instance of ANY VIEW IN ANY CONTAINER (pane / tab / window) and a second window is a second CLIENT of the one session, so the first client-server slice is the View/container generalization + persisted change events; the six rulings ACCEPTED 2026-09-09 (+ a seventh: emitted views indented and coloured — LANDED the same day: the emitter indents by block depth with precedence-aware parentheses, the lens buffers carry their own spans; gate `emit_layout_gate.sh`). **Design doc 2026-09-09:** [the client-server design](2026-09-09-nexus-client-server-design.md) — View objects, containers + `.layout`, a window is a client, the change event log, correlation, tiers, `ws`/`api` transports + headless, thread contracts, slices V0–V7 — **REVIEWED and RULED by the owner 2026-09-09** (§0 vocabulary: hub vs nexus, madc vs madcide; §2.1 ONE file-kind vocabulary grouped in ranges, depth of support per layer; §2.2 the editor region a split tree, chrome panes fixed-slot — the VS Code / CLion shape; §6: caret on the container, the event log follows the manifest, a rich `api` offering both the composed tree and raw projections; the release boundary V1–V5 first, V6 second, V7 later). **Slice V0.5 enums-not-strings LANDED + PUSHED 2026-09-09** (develop 9a518c4c; [plan](2026-09-09-v05-enums-not-strings.md)): the UI level enum, action codes end to end, the madcide command + discriminator enums with load-time refusal and gates; four defects found by the wave fixed in their own commits (a tagged enum's enumerator type, the ranker's fn-ptr viability, the ambiguous-call refusal with its provenance rule, enum promotion by type identity on LLP64). **Forest overload identity 2026-09-09** (3231f210 on `feature/client-server-views-claude`, the arc branch): a restored overload-set member carries its declaration identity (`FuncDef::overload_spelling` / `overload_template_args` on the DK_FUNC record, format v46) and the instantiation memo comes back with it — a bound consumer no longer re-instantiates a specialization the forest holds (`forest_crosstu_gate` leg D; KG gap closed). **Cadence (owner 2026-09-09):** the battery runs ONCE at the SEAM — the V1–V5 release boundary — never per slice; the slices bank on the arc branch. V1 Views LANDED 2026-09-09 (6851dc66, arc branch, targeted gates): the lens is a View row (kind + lang enums per container; navigation the container's), the ONE file-kind vocabulary `<bits/file_kinds>` with `LanguageStd` = its ranges (forest v47), `madc::emit` by kind, `doc_set_path`. **V1.5 the `ui::NONE` client LANDED 2026-09-09** (36788b7a, arc branch, targeted gates): `madcide <file> -c "<command> [arg]"` — the name converts once at the command-line boundary (exit 2 on an unknown name), the session opens as the TUI client's does, `IdeSession::command` posts the code and feeds the argument to the prompt it opened (committed by code), the composed tree is typeset onto stdout, the exit status is the verdict (0 clean / 1 unreadable or error rows / 2 line refused); `IdeSession::post` the code-speaking primitive the vi grammar shares; gate `testmadcide_cli`. **V2a containers + layouts COMPLETE 2026-09-10** (arc branch, three commits, targeted gates; [plan](2026-09-09-v2-containers-layouts-plan.md)): the `.layout` profile grammar (`parse_layout` / `load_layout` / a baked default; `ui::split` / `ui::side` enums + name owners; the words convert once at load, a misspelling refuses with its line); the tool Views come FROM the layout's chrome panes (`show_view` replaces the panel/paneltab flags and the diags/outline popup arms; the panel persists; `haspanel` stops gating it); the grid renderer learns RECTANGLES (`madcdis/tui_model.h`: chrome bands by side/size, `split` recursion, pane headers; BYTE-IDENTICAL with no chrome shown and no split), the DOM's `split` / `side` / `size` op fields + the page's split flex, the composer emits the side WORD on the wire. The madcide integration tests render through the LINE linearizer not the grid, so their pins did NOT move and the 17 GUI snapshots are unchanged; the rectangular compose is exercised by `test_tui_model` (27) + `test_web_model` (20). **V2b — the editor region a REAL split tree — CORE COMPLETE 2026-09-10** (arc branch, three commits, targeted gates): part 1 (6e202e7f) the composer WALKS `layout["editor"]` (a sole leaf byte-identical, a split emits a `split` group of leaf groups) + `viewsplit right|bottom` / `viewfocus next|prev` / `viewclose` (enum + registry + dispatch + colon; value-typed-tree mutations; an unfocused leaf parks its caret on its node, the focused leaf IS live es); part 2 (ae2ef3b5) `viewsplit right mc11` = the code-View split — **SOURCE LEFT / MC11 RIGHT, the owner's side-by-side** (`make_code_view`; the full focus handoff `editor_enter_leaf` = fview + caret + read-only verdict; `viewopen mc11\|c11\|cpp\|source` re-represents the focused View in place); part 3 (35d2dff5) the split group carries `region:editor` to dock into the editor slot (the leaf's status/edit carry none = each a flex column) + `tests/gui/madcide_layout` the DOM gate (`split=vertical flex=row cols=2 side-by-side=1 content-differs=1`). Gates: testmadcide `view-*` pins (5/5 madcide tests), GUI 18/18 x3, `test_tui_model` 27 / `test_web_model` 20 unaffected. V2b follow-up deferred (menu titles + key spellings, `viewtab`). **V2c PART 1 COMPLETE 2026-09-10** (939dce7c, arc branch, targeted gates — MEETS the plan's V2c gate): `viewdock <left|right|top\|bottom>` moves the focused chrome pane to a slot+side; `layout_to_text` is parse_layout's structural inverse; `layout_persist` writes `<base>.prj.layout` beside the manifest ONLY when a manifest is open (the implicit project writes no artifact), `proj_load` restores it at project open (testmadcide_layout round-trips save→parse; testmadcide v2c-implicit/persist/reopen). **V2c COMPLETE 2026-09-10 — V2 DONE** (parts 1-3, arc branch, targeted gates): part 1 `viewdock` + `<base>.prj.layout` persistence (939dce7c); part 2 `viewsize` — the splitter feeds the session's size (4096c832, OWNER-approved Option C: a command reusing the action+arg path, no new wire event kind; a fresh web viewer renders bands at the layout size, matching the TUI); part 3 docs (docs/madcide.md, design doc §4 row V2 ✅). `view*` menu titles / key spellings + `viewtab` remain a deferred owner key-seat decision. **V2.5 the `ui::LINE` client LANDED 2026-09-10** (1d165538, arc branch, targeted gates): the ex / edlin line mode `madcide <file> --line` — `ui_line_frontend` (the level-0 typesetter to stdout, one stdin line per event) + `ui::open(ui::LINE)`; the engine frontend stays dumb, the line grammar is `IdeSession::line_input` (`:` = a colon command posted through `command(cmdCOLON, …)`, else text inserted verbatim), run by `run_line` (parallel to `run_once`, V1.5). Gate `tests/testmadcide_line` (a scripted stdin transcript through the real frontend + a byte-identical direct-`line_input` twin) green jit/exe/obj; madcide group + registry/dialect/lean/twins gates OK; zero warnings. Follow-up: the colon line reaching the FULL registry by name (`:check`, `:find x`) — a colon-interpreter enhancement, both faces. **V3 (clients + windows) UNDERWAY:** V3a the ANCHOR REGISTRY (1f0168e5: `shift_offset` the one splice primitive + `shift_anchors` the one pass, mark/bend follow the text, gate `check-one-anchor-owner.sh`); V3b: **V3b-1** `ui::event_any` (8f9a0ed3 — the ONE blocking decision over N frontends, a sole target byte-identical to `ui::event`, N targets demuxed via the non-blocking `poll_events` tick pump; gate `testuieventany`), **V3b-2** the dialect MULTI-CLIENT loop + `viewwindow` (78d54569 `run_ide` over a roster of general client records `{id,t,es,transport,level,tier,last_seq,capabilities}` via `event_any`, N=1 byte-identical; e91fdc6b `spawn_view_es` — a second window is a second client with its OWN es over the SHARED document, `open()`'s init factored into ONE builder `init_view_es`; 60225e20 the `viewwindow` command parks a spawn a windowed client only, `run_ide`'s `spawn_client` opens the frontend + joins the roster; gate `tests/testmadcide_window2` — an edit in one client seen in both, per-client carets; a ring-discipline bug fixed in the factoring). The real webview multi-window pump (tick across GTK/Cocoa/Win32) + window-close detection + multi-client teardown = the flagged platform follow-up (GTK smoke + seam lanes). NEXT: V3c presence (carets in `@presence` colours through the V3a registry), then V4/V5 → the V1–V5 seam (one battery). |

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

26.  Track 6.2  macOS SIMD (NEON)                       [DONE v0.98.0]

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
| C++ conformance lane — g++ testsuite, C++11 parity as a roadmap goal (never a gate) | [2026-09-04-cxx-conformance-lane.md](2026-09-04-cxx-conformance-lane.md) |
| Cross-Cutting Insights | [cross-cutting-insights.md](cross-cutting-insights.md) |
| Data Storage & Federation (legacy) | [data-storage-federation.md](data-storage-federation.md) |
| madcdis Core Substrate | [madcdis-plan.md](madcdis-plan.md) |
| MAD C IDE Nexus — the project as a live, federated semantic object (owner vision, 2026-09-08) | [2026-09-08-madc-ide-nexus-vision.md](2026-09-08-madc-ide-nexus-vision.md) |
| MAD C IDE Multi-View Architecture — every representation a View, correlated (owner vision, 2026-09-08) | [2026-09-08-madc-ide-multi-view-architecture.md](2026-09-08-madc-ide-multi-view-architecture.md) |
| Nexus + Multi-View cross-referenced with the settled design; what it changes about the client-server step (2026-09-09) | [2026-09-09-nexus-and-multiview-cross-reference.md](2026-09-09-nexus-and-multiview-cross-reference.md) |
| Nexus, sessions and clients — the client-server design: Views, containers/layouts, a window is a client, the change event log, correlation maps, tiers, transports + headless, thread contracts, slices V0–V7 (2026-09-09) | [2026-09-09-nexus-client-server-design.md](2026-09-09-nexus-client-server-design.md) |
| Slice V0.5 — enums, not strings: the measured inventory, the decided enum shape (UI level, action codes end to end, discriminator enums, one name→code converter at every input boundary), sub-slices a–e with gates (2026-09-09) | [2026-09-09-v05-enums-not-strings.md](2026-09-09-v05-enums-not-strings.md) |
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
