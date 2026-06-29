# madc as a Unified Development Substrate — North-Star Vision

**Date:** 2026-06-29 · **Status:** VISION / NORTH STAR (multi-year). Not a
roadmap. Captures intent so it persists as authoritative context rather than
living only in one conversation.

**Reads with:**
- `docs/plans/madc-vision-and-invariants.md` — the polyglot-transpiler arc + invariants I1–I8
- `docs/plans/madcdis-plan.md` — the core data-substrate design (pools, values, interning, datasets, GQL, federation)
- `docs/plans/madcdat-plan.md` — the external-driver companion
- `docs/plans/2026-06-23-materialize-from-ast-instantiation-design.md` — tinycc L1 (id-stream) + the perf substrate
- `docs/plans/2026-06-09-frontend-representation-refactor.md` — the phased arena/interning/forest work

> **Why this doc exists (and proves its own thesis):** this vision was developed
> across one long conversation. That conversation is *exactly* the kind of
> context that drifts and evaporates between sessions/compactions — the problem
> the vision itself is designed to dissolve. Capturing it here (and in
> `madc-knowledge`) is the hand-built version of what the substrate would do
> automatically.

---

## 0. The thesis in one sentence

Standardized `madc::dis` in-memory primitives → generic `madc::dat` parse/emit
drivers → one dual-fidelity **MC11-IR graph** → indexed across **past/present/future**
→ exposed to agents via **MCP** as semantic operations → **federated** across the
whole tool ecosystem: a unified, temporal, queryable semantic substrate for
software development, with the codebase as a *living object* instead of a pile of
text files — and madc uniquely able to anchor it because **it owns the faithful,
round-trippable IR.**

---

## 1. The arc, rung by rung

Each rung delivers standalone value, so the foundation is paid for by near-term
wins even if the apex takes years or shifts.

### Rung 1 — `madc::dis`: standardized in-memory primitives (dogfooded by the compiler)
madc's proven internal data machinery becomes a **catalog of generic, namespaced,
standardized primitives**, reusable inside madc *first* and exportable later —
not bespoke, not hardcoded per use-site. Families of tuned variants, not single
modes behind a flag:
- **Intern tables:** non-counted (literals/keywords), refcounted (lifetime-managed),
  frozen/read-only (PCH/forest, mmap'd).
- **Arenas/pools:** bump (drop-all), slab/typed, size-class, page-refcounted, shm-backed.
- **The one type-id table** (segmented `u32`; the type sibling of the string table).
- **Allocator-aware collections** built on the above.

The **standardized interface** is load-bearing: standardize once and "write to X"
becomes "add a driver," not "write N bespoke serializers." The eventual interface
vehicle is the **C++ allocator model** — static `Alloc` template parameter for the
zero-overhead hot path, `pmr::memory_resource` for runtime swap, fancy-pointer
(`offset_ptr`, à la Boost.Interprocess) for position-independent cross-process —
but full C++ exposure is **deferred**; first establish the primitives madc itself
needs.

Today these exist as parallel shapes begging to be unified behind one contract:
`StringPool`, `InternKeyedMap`, the `findVariable` interned-`qsid` path, and the
still-`std::map<string>` `datatype_map`. Consolidating them is simultaneously the
no-parallel-implementations cleanup *and* the export surface.

### Rung 2 — `madc::dat`: generic drivers = (parser, emitter) pairs for any format
A driver is fundamentally a **(parse, emit) pair** for a format. Once stated that
way, *there is no special category for "source code."* A C parser, a `.are`
parser, an ELF reader, a FalkorDB reader are all **input drivers** producing
`madc::dis`; a C11 emitter, a MIR emitter, an ELF writer, a FalkorDB writer are
all **output drivers** consuming it. Therefore:
- **The compiler's front-end and back-end *are* `madc::dat` drivers.** The
  lexer/parser is the C/C++ source→`dis` input driver; `--emit=c11` already is a
  `dis`→C11 output driver; MIR emission is another. They exist today as
  proto-drivers; this names and unifies them.
- This is the **polyglot-transpiler north star** as a mechanism: read language X
  via an input driver → one MC11-IR → emit target Y via an output driver. The
  vision-and-invariants discipline (one IR, no hardcoded standards/targets, every
  feature gated by the `--std=`/target enum) is exactly what this requires.
- **Binaries fit too.** "ELF as a `madc::dat` format, a MIR code segment as
  `madc::dis`" — precedent is **GNU BFD**, which abstracts ELF/COFF/Mach-O behind
  one interface for the linker/`objcopy`/`nm`. madc grows a BFD-equivalent as a
  *consequence* of the substrate. The highest-leverage piece is **programmatic
  format definition** (declare a format as a schema the substrate reads/writes —
  Kaitai-Struct class), which makes "support format X" = "declare a schema + driver."

The whole toolchain's artifacts — source → tokens → AST forest → symbol/type graph
→ MIR → machine code → ELF → knowledge — become one uniformly-represented,
federatable, queryable substrate; the compiler, linker, loader, JIT, PCH cache,
and code-intelligence index all become drivers/consumers of it.

### Rung 3 — the IR is a graph; GQL is the query layer
MC11-IR is already a property graph: `cir_node`s with parent/child edges;
`DataDef`/`Variable`/`FuncDef` with type-of, member-of, calls, derives-from,
instantiated-from edges. The madcdis plan already declares **GQL canonical** with
a federated planner; querying madc's code-graph (in-memory or in a graph DB) is
the same `gql::`/`cypher::` surface. This yields a **Glean/CodeQL/Kythe-class
capability intrinsically**, over the compiler's *actual* parsed IR rather than a
separate reverse-engineering indexer.

### Rung 4 — backends via `madc::dat` drivers
One `madc::dis` source, many drivers, planner federates:
- **zstd forest** — the hot, fine-grained, round-trippable tier (parse-once /
  load-many; the front-end perf lever — parse dominates header-heavy wall time).
- **FalkorDB** — the warm, queryable, symbol/type/declaration-level projection
  (the code-intelligence graph). Notably the **same engine as `madc-knowledge`**,
  so the code-graph and project-memory graph are the same kind of thing in the
  same store.
- **ELF/object/MIR** — the BFD-equivalent.
- **Granularity/tiering is the design fork:** likely the zstd forest is the
  primary round-trippable store (every node) and FalkorDB is a query projection
  (symbol-level), federated — not every `cir_node` as a graph vertex.

### Rung 5 — time as a first-class axis (past / present / future)
Three **tenses of the same MC11-IR graph**:
- **Past** = revision control (history).
- **Present** = the live IR.
- **Future** = project management / intended code (planned deltas, target states).

Two things make it land:
- **The substrate already has the temporal machinery.** The madcdis plan's
  derivation/keyframe/retention/as-of-query-rewriting (specced for time-series) *is*
  version control applied to code: keyframes = commits, deltas = edits, "answer
  from nearest keyframe + replay" = checkout.
- **Structural redundancy across time = content-addressed semantic VCS.** Interning
  + multiplicity + COW applied to history: a function unchanged across 100 commits
  is stored once, referenced 100×. Git dedups blobs; this dedups *semantic nodes*.
  The substrate's founding principle and version control are the same idea.

Precedent (so it is principled, not fantasy): **Datomic** (DB as immutable
time-indexed facts; query "as-of"; speculative "with" for hypotheticals — the
"future" tense) and **Unison** (content-addressed codebase, no text files). This
is those fused with a compiler IR.

### Rung 6 — agentic development via MCP (the apex)
The largest pain of agentic development is the **three-body drift**: code in source
files, intent in plan files, and the agent's in-memory context — three
representations of the same thing that constantly diverge, with the agent burning
effort re-reading/re-parsing/re-deriving and losing it all at every context
boundary. (This project already fights it by hand: a `madc-knowledge` graph
mirrored into `claude_status.json`/`ROADMAP.md`/`CHANGELOG.md`/handoff/resume files,
with standing rules — "rehydrate," "keep the KG and flat files in sync,"
"compaction over-compacts, re-read the plan," "execute the plan don't re-derive,"
"verify over the stale handoff." That apparatus is a hand-built, lossy simulation
of this rung.)

Funnel **everything through MCPs over the substrate**: the agent manipulates code
**directly in object form** — with full history (past), intended change (future),
current state (present), what it touches (edges), and what it's supposed to do
(intent linked to the same graph) — via *semantic* operations (query the call
graph; fetch a symbol *with* its history/callers/intent; apply a delta) instead of
grep/read/edit over text. No mirroring, because the three bodies are three views of
one substrate.

**The inversion:** the object graph becomes the master; **source-on-disk becomes a
projection** (a `madc::dat` emit driver kept in sync), not the source of truth.
Plans become future-tense nodes attached to the code graph, so plan and code
*cannot* drift — the agent queries the delta between present and intent directly.

### Rung 7 — federation across the ecosystem
The substrate is the **semantic hub**, not a walled garden. GitHub, Jira, Notion,
IDEs, LLMs, humans are each a capability-aware `madc::dat` driver/view: code+history
↔ GitHub, intent+tasks ↔ Jira, docs+knowledge ↔ Notion, live edits ↔ IDE, semantic
ops ↔ LLM agents, rendered views ↔ humans. The planner pushes down what each system
natively does and reconciles the rest. "Maximum visibility across domains and
angles" falls out: every facet of a project is the same queryable object, projected
per consumer. **This is the answer to the coexistence crux — you federate the
existing ecosystem, you don't replace it.**

---

## 2. Why madc is uniquely positioned
Most agentic-dev / code-intelligence tools bolt onto text because they don't own a
faithful, round-trippable IR. madc does: the dual-fidelity **MC11-IR** is *both* the
lowered C11 view (for c2mir) *and* the high-level view (retained tokens + parse
subtree + positions, so it reverse-renders to C++/madc/source). That is exactly
what lets madc hold code as a queryable object *and* re-emit faithful source on
demand — the precondition for every rung above.

---

## 3. The cruxes (where it is hard, not where it is doubted)
- **Round-trip fidelity is load-bearing for *correctness*, not just transpile.** If
  object→source projection isn't lossless, the human/git/CI world and the agent
  world fork. Same fidelity bet as the transpiler, higher stakes.
- **Node identity across edits** — semantic diff/merge needs "this node at commit A
  is the same entity as that node at commit B" (the GumTree/AST-diff problem).
- **Coexistence / bidirectional projection** — humans, editors, git, CI live in text;
  the projection must be seamless and two-way, or you fork the world.
- **Verification-as-query** — an agent mutating the graph needs "what does this delta
  break?" as a *query* (compiles? which tests touch it?), which the substrate can
  answer because the dependency graph is right there. (Today: the by-hand
  fulltest+torture discipline; there it becomes intrinsic.)
- **Future/speculative facts** — intent is partial/uncertain; representing
  not-yet-real code needs a speculative-fact model (Datomic's "with").
- **Federation reconciliation** across heterogeneous external models (git vs Jira vs
  Notion) is itself hard.

---

## 4. Engineering discipline (how this becomes real)
- **North star, not roadmap.** It is effectively Datomic + Unison + GNU BFD + LSP +
  a polyglot compiler + a federated ETL + an agent runtime, fused. Build the
  *bottom*, where each rung pays for itself standalone (the compiler gets faster →
  queryable → persistent → federatable).
- **Generalize from real consumers** (ELF, MIR, the zstd forest, FalkorDB, the
  compiler's own tables) — not from a speculative upfront taxonomy. Lift each
  interface from proven internal use.
- **Pay-for-what-you-use.** The compiler's hot paths must keep their speed; the
  substrate offers optional layers (arena/intern cheap; refcount/COW opt-in).
- **No parallel implementations.** Consolidating the bespoke shapes onto one
  interface *is* the cleanup, not extra work.

---

## 5. The grounded first step (and it is already underway)
**Establish the `madc::dis` primitives, dogfooded by the compiler, with a
standardized interface — starting with the intern table** (proven, already
index-based/mmap-friendly, 3–4 ad-hoc variants to unify), then arena/pool, then the
type-id table. Build them *to spec* (index-handles, position-independent,
mmap-serializable, one type vocabulary) from the start — exactly as `StringPool`
was deliberately built index-based "so it mmaps zero-copy."

This is not a detour from the vision; it is rung 1. The front-end perf/interning
work already done this year is the foundation of this entire arc.
