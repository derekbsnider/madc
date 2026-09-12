# The code-graph MCP — an LLM navigates & edits the live `cir_node` forest as a graph

**Status:** design (approved to write 2026-09-12; L1 to start after the next
compaction). Architectural brainstorm → this plan → `writing-plans` for L1.
**Branch:** builds on the V6 MCP seat (`feature/client-server-views-claude`,
`tools/madcide/madcide_mcp.inc` @ `9620e32b`). Branch/arc placement is an open
item (§10).
**Realizes:** `docs/plans/2026-06-29-madc-development-substrate-vision.md`
**Rung 3** ("the IR is a graph; GQL is the query layer") and **Rung 6**
("agentic development via MCP — the apex"). This plan is the concrete,
buildable form of that vision, scoped for an LLM agent as the consumer.

> The MCP is for the *agent*. The thesis (owner, 2026-09-12): the agent should
> understand and change the program as an **AST graph** — insert *nodes*, not
> text — with source as a reverse-render. "The object graph becomes master;
> source-on-disk becomes a projection" (substrate vision, Rung 6, verbatim).

## 1. Goal

Give an LLM coding agent (Claude, over MCP) a **semantic, node-addressed view
of the live compiler forest** so it navigates and edits by *meaning* — symbols,
types, calls, references, subtrees — instead of grep/sed over text. The graph is
the running madc's own `cir_node`/`DefArena` state (never a re-parse, never a
copy); text is one reverse-rendered projection of it.

This is not a new subsystem bolted beside the compiler — it is an **exposure +
verb layer over structures the compiler already holds**, plugged into the
existing MCP seat as a new tool family.

## 2. Where this sits

- **Substrate vision (2026-06-29), the 7-rung ladder.** Rung 1 `madc::dis`
  primitives · Rung 2 `madc::dat` (parser,emitter) drivers · **Rung 3** the IR
  *is* a property graph, GQL is the query layer, "a Glean/CodeQL/Kythe-class
  capability over the compiler's actual parsed IR" · Rung 4 backends via drivers
  · Rung 5 time as a first-class axis (past/present/future) · **Rung 6** agentic
  development via MCP · Rung 7 federation. This plan builds Rung 3+6 first;
  Rung 5 is the nexus (§7 L4); Rung 7 is out of scope.
- **The nexus (2026-09-08 vision; 2026-09-09 cross-reference + client-server
  design).** Triad: *git = past, AST = present, project mgmt = future.* The AST
  graph here is the **present** axis, made queryable. The nexus relation
  vocabulary (`calls, depends_on, implements, tests, fixes, introduced_by,
  modified_by, requested_by, supersedes, blocked_by, planned_for, …`) is the
  superset; our code edges (§6.2) are the *present/code* subset of it — reconcile,
  don't reinvent.
- **The V6 transport (2026-09-09 client-server design §2.7).** "The MCP seat is
  an adapter over `api`." The code-graph tools are a **new tool family in the
  same MCP seat** (a new raw-projection kind), not a second server.
- **Relationship to the V6c LSP adapter.** The forest query layer here *is* what
  an LSP adapter consumes (definition/references/documentSymbol/hover/
  semanticTokens are the standard-protocol face of these same queries). Build the
  query layer once; LSP becomes a thin second face of it. This plan **subsumes
  and supersedes** the standalone "V6c-2 LSP adapter" as originally sketched.

## 3. Evidence the shape is right (recon 2026-09-12)

**It comes full circle** — every element predates LLMs and each root carries a
design lesson we adopt:
- **LISP homoiconicity** (code *is* data) → the graph is primary, text is a
  reverse-render, edits are graph ops by construction.
- **Programmer's Apprentice** (Rich & Waters, ~1980; a plan/knowledge program
  representation for an AI assistant, "multiple points of view") → the direct
  ancestor of this goal; also independently validates MC11-IR's "both lowered
  and high-level at once." Higher-level *cliché* nodes are a later enrichment.
- **Syntax-directed editors** (Cornell Program Synthesizer, MENTOR, Gandalf) →
  edits are grammar-legal transformations, validated by the *same* parser before
  commit.
- **Smalltalk live image** → query/edit the live in-process forest, never a
  snapshot (validates "the running madc IS the compiler", `loaded == parsed`).
- **NLS/Augment (1968)** → stable structural addresses (its positional numbering
  broke under edits — prefer a persistent id) + **view depth** as a first-class
  query parameter.
- **CODASYL / Bachman network model (1969)** → our edge taxonomy *is* a Bachman
  network schema; keep navigational verbs, but keep a declarative layer for ad
  hoc questions (the relational counter-revolution's lesson).
- **MPS projectional editing** → node ops atomic to tree-well-formedness; text
  always regenerated from the tree.

**2026 evidence it beats grep for agents** (measured, not hypothesis):
LocAgent (92.7% localization; +12% pass@10), RepoGraph (+32.8% SWE-bench),
CodeStruct (named-AST-entity edits: +pass@1 while cutting tokens 12–38%).
"AST-Derived vs LLM-Extracted KGs" (arXiv 2601.08773): **compiler-derived graphs
are strictly more reliable** than LLM-inferred — every edge must be ground truth.
codebadger (Joern+MCP) and Serena (LSP+MCP) converge on **bounded named verbs,
not a raw query language**. Joern *left Neo4j* for an embedded store because
"needs a DB service" contradicts "an agent just invokes this" → **stay
in-process**.

## 4. What exists vs. what's greenfield

**Shipped substrate (reuse — do not duplicate):**
- `include/cir_arena.h` — `DefArena`/`FrozenDefArena`: every `DataDef` a flat
  record keyed by **type-id**, tagged `enum DefKind` (`DK_STRUCT/DK_UNION/
  DK_CLASS/DK_FUNC/DK_VAR/DK_ENUM/DK_TYPEDEF/DK_PTR/DK_REF/DK_CONST/DK_FPTR/
  DK_MEMBERPTR/DK_NSLINK/DK_NSBIND/DK_NSALIAS/DK_SIMD/DK_PRIM/…`), cross-refs as
  ids (`ref0`, `members_*`/`bases_*`/`methods_*`/`params_*` slices), read via
  `get_def_at(id, defrec&)`. `src/cir_freeze.h`: "the arena segments ARE the
  type-graph serialization." → **a declaration/type graph already exists.**
- `cir_node` is position-independent: `cir_ref{seg,idx}` behind one resolver
  `madc_cir_node_for(ref)` (Track B, `2026-07-04-data-substrate-first-customer-
  PLAN.md`). → **tree nodes are already graph-addressable.**
- `include/madc/ns_madc` per-handle projections: `parse_open`/`parse_open_file`/
  `parse_refresh`/`parse_close`, and `parse_outline`/`parse_check`/
  `parse_enclosing`/`parse_spans` (retained state, no re-parse per query),
  `project_open`/`project_tus` (a project = one handle per TU). → **the accessor
  surface and the per-TU model we extend.**
- The **PAST axis** is shipped: the ChangeEvent log (client-server §2.4, V4) —
  `{seq, actor, verb, object(entity handle), payload:{at,del,ins}, causal_parent}`.
- Correlation maps (§2.6, V5): source↔MC11↔MIR↔machine at *statement*
  granularity.
- The query *engine* (FalkorDB + Cypher; `sql::`/`cypher::`/`gql::` sub-grammars
  → one Query IR, `madcdis-plan.md` §9–§12) is in daily production — for project
  memory. The gap is **populating** a code graph, not the engine.

**Greenfield (well-lit, not blank):**
1. No node/edge **schema for bodies** — DefArena stops at declarations;
   statements/expressions/call-sites/name-references/flow live only in raw
   `cir_node` bodies.
2. **Semantic edges not materialized** — CALLS/REFERENCES must be derived by
   walking bodies + resolving to defs (cross-TU by symbol).
3. No **query/verb surface binds to code** — `cypher::MATCH` is demonstrated only
   over generic `User` data.
4. No **node-level mutation** — the event log payload is a byte-range *text*
   splice; the arena is write-once. "Edit nodes, text is a projection" is the
   real inversion vs. what's built.
5. The live MCP seat has **never touched the arena** — it is IDE-command-level
   (~110 registry verbs).

## 5. Settled decisions (invariants — do not re-litigate)

1. **Live, in-process.** The graph is the running forest. No external graph DB in
   the code path. Any FalkorDB projection is optional, later, federation/
   cross-revision-analytics only, and an explicitly **disposable** IDE-session
   cache (never ambiently consulted, never on the run path) — the no-user-program-
   cache law, with the ChangeEvent log's "never force a manifest into existence"
   as the precedent.
2. **Bounded named verbs**, not a query language, as the agent surface. The
   `gql::`/`cypher::` layer is a later *escape hatch*, a declarative projection
   over the same live graph.
3. **Entity handles / stable ids, never byte offsets** (day-one rule; `cir_ref`/
   type-id precedent; View `subject` precedent).
4. **Node-kind / edge-kind are enums** (`DK_*` precedent + the enum law); text
   only at the wire boundary, converted once.
5. **Every edge is compiler ground truth.** Never an LLM-inferred edge.
6. **Edits flow through a verb** (`madcdis/verbs.h`), **logged** (extend the
   ChangeEvent log with a node-op payload beside the byte-splice one),
   **grammar-validated before commit**, **atomic** to tree-well-formedness, text
   **reverse-rendered** — never hand-patched.
7. **Context economy is first-order:** terse default projection (`id, kind, name,
   span`), **view-depth** a parameter (signature vs. body), **pre-correlated
   batched results** (one call returns symbol+callers+refs). Validates the lean
   structured result shipped at `9620e32b`.
8. **No parallel implementation.** The graph is a projection/adapter over the
   existing arena + cir_node + event log — never a second AST serialization nor a
   second mutation path.
9. **MCP-as-adapter.** A new tool family in the existing seat, not a second server.

## 6. Architecture

### 6.1 One live graph, two faces
The code-graph **is** the live `DefArena` + `cir_node` forest. Two faces over the
one graph: (a) **navigational verbs** — the primary agent surface (§6.4); (b) a
later **declarative `gql::`/`cypher::` driver** that queries the *same* live graph
(reusing the shipped Query IR). This reconciles the two previously-separate
efforts (the `madcdis`/`madcdat` federation track vs. the `cir_arena` type table):
they meet as one graph with a navigational and a declarative face — no copy.

### 6.2 Node & edge schema (the shared contract)
Node kinds = the DefArena `DK_*` set (declarations/types) **plus** a new body-node
enum for `cir_node` bodies. Represent as enums; convert to wire names once.

| Node kind (labels) | Source |
|---|---|
| Function, Struct, Union, Class, Enum, Typedef, Var, Param, Member, Pointer, Ref, Const, FuncPtr, Namespace | `DefArena` `DK_*` (exists) |
| TranslationUnit, Block, Statement, Call, NameRef, Expr, Literal | `cir_node` bodies (**new**) |

| Edge kind | Meaning | Source |
|---|---|---|
| CONTAINS | parent → child (structural) | cir_node tree |
| ENCLOSES | node → enclosing function/scope | `parse_enclosing` |
| HAS_TYPE | expr/var → type | DefArena / cir_node |
| DEFINES / DECLARED_IN | decl → def; symbol → TU | DefArena / declindex |
| REFERENCES | name-use → decl | **derive (L2)** |
| CALLS | call-site/function → callee func | **derive (L2)** |
| MEMBER_OF | member → aggregate | DefArena slices |
| INHERITS / DERIVES_FROM | class → base | DefArena `bases_*` |
| INSTANTIATED_FROM | instance → template | DefArena |
| INCLUDES | TU → header | lexer |

These CODE edges are the *present*-axis subset of the nexus relation vocabulary
(§2); the nexus adds the temporal/cross-domain edges (`introduced_by`,
`requested_by`, `tests`, `fixes`) at L4.

### 6.3 Addressing & node identity
- **Within a parse snapshot:** a node id is `cir_ref{seg,idx}` (tree nodes) or the
  type-id (DefArena decls), surfaced as an opaque stable handle. Stable for the
  handle's current parse; after a mutating command the agent re-queries.
- **Across edits/time (the hard crux, deferred to L3/L4):** node identity across
  re-parses is the GumTree/AST-diff problem, named in the substrate vision §3.
  L1/L2 do not need it; L3 confronts it (informed by GumTree), L4 needs it for the
  PAST axis. Prefer a persistent/content-derived id over a positional one (NLS's
  positional numbering broke under edits).

### 6.4 The verb set (MCP tools — a new family)
A distinct `tools/call` family in `madcide_mcp.inc`, dispatched by tool name
*before* the registry-command path (the editor commands stay as they are). Each
tool takes node handles / a name / a position and returns a **terse** projection
by default (`id, kind, name, span`), with `depth` and `fields` opt-in.

- **L1 (read, from existing structures):** `graph.symbols(scope?)`,
  `graph.definition(name)`, `graph.node(id, fields?)`, `graph.type_of(id)`,
  `graph.members(type)`, `graph.bases(type)`, `graph.enclosing(pos)`,
  `graph.children(id, depth?)`, `graph.body(func, depth?)`.
- **L2 (derived edges):** `graph.references(def)`, `graph.callers(func)`,
  `graph.callees(func)`, `graph.search(kind, name~, type?, scope?)`,
  `graph.impact(id)` (batched: callers + references + enclosing in one call).
- **L3 (mutation, via verb):** `graph.insert(target, spec)`,
  `graph.replace(id, spec)`, `graph.delete(id)`.
- **Escape hatch (later):** `graph.query(cypher|gql)` over the live graph.

Tool names/shape follow the enum-law and terse-result rules; the exact wire
names are settled in the L1 writing-plans pass.

### 6.5 Result shape
Structured JSON, terse by default (extends the `session_state` model shipped at
`9620e32b`): `{ nodes: [{id, kind, name, span}], edges?: [...], next?: cursor }`.
`fields` opts into `type`, `signature`, `qualified_name`, `source`; `depth`
controls subtree/view depth. Large results paginate (`cursor`). Never dump a full
subtree by default.

### 6.6 Editing (L3)
`graph.insert/replace/delete` construct/replace `cir_node` subtrees, not text.
Each is a `madcdis` **verb** so it composes with undo, the event log, and
multi-client propagation for free. Before commit: the edit is validated by the
same parser/sema (reject an ill-formed tree — the syntax-directed-editor
guarantee); on commit it is **logged as a node-op** (a new ChangeEvent payload
kind beside `{at,del,ins}`) and the buffer text is **reverse-rendered** from the
tree (the existing view-seam `emit`), never hand-patched. Node specs are supplied
in a structural form (a small builder DSL or an MC11/C11 fragment parsed to a
subtree) — settled in the L3 writing-plans pass.

## 7. Layering

- **L1 — expose what exists (the first slice).** New engine graph-query
  accessors on the `ns_madc` parse-handle surface (mirroring `parse_*`, resolved
  mangled-direct — *not* the blocked madcdis script-export path, §9) reading
  `DefArena` + `declindex` + `cir_node`; the L1 verb family in `madcide_mcp.inc`
  wrapping them; terse results + `depth`/`fields`. Mostly *exposure* → fast, low
  risk, and it fixes the addressing + verb + result ergonomics everything builds
  on. Gate: a unit test for the accessors + a dialect integration test driving
  the L1 tools over the MCP seat (symbols/definition/type_of/enclosing/node/body
  on a fixture file, asserting structured node results).
- **L2 — materialize semantic edges.** Derive CALLS/REFERENCES by walking
  cir_node bodies + resolving to defs; cross-TU by symbol; kept as a cheap
  incremental layer (stack-graphs lesson) so one edit doesn't rebuild the world.
  Adds `references`/`callers`/`callees`/`search`/`impact`.
- **L3 — node editing.** The `insert/replace/delete` verb (§6.6); confronts
  node-identity-across-edits; scoped and last of the core.
- **L4 — the nexus.** PAST (query across the event log + git-revision subgraphs),
  FUTURE (the `propose` tier — node-ops as reviewable proposals; client-server
  §3.2 reserves it). Optional FalkorDB projection for cross-revision analytics
  (disposable). Federation (Rung 7) stays out.

## 8. Owner laws honored
Running-madc-IS-the-compiler (live handle only) · no-user-program-cache (any
projection is disposable) · entity-handles-never-byte-offsets · index-is-not-the-
graph (each verb states which structure answers it: declindex = name lookup,
DefArena = type graph, cir_node = bodies) · enums-not-strings (`DK_*` + a body-kind
+ an edge-kind enum) · thread-safety (UI-thread-confined; MCP = cooperative tasks)
· no-parallel-implementations (projection over the one arena+tree+log) ·
every-mutation-through-a-verb · MCP-is-an-adapter · madcdis-stays-DataDef-agnostic.

## 9. Hard cruxes & how sequenced
- **Node identity across edits (GumTree)** — deferred to L3; L1/L2 use
  snapshot-stable ids + re-query.
- **Two-graph reconciliation** — decided (§6.1): one live graph, navigational +
  declarative faces; no second serialization.
- **Script-facing madcdis export blocked** (`2026-07-06-madcdis-export-surface.md`
  step 2c, on a `<cstdint>` parser gap) — does **not** block us: L1 accessors ride
  the `ns_madc` parse-handle surface (the proven `parse_*` path), not the generic
  madcdis DataSet export.
- **Statement/expression correlation is statement-granularity today** (V5) — L3's
  reverse-render is whole-subtree (function/statement), avoiding the missing
  expression-level map; finer correlation is a later refinement.

## 10. Open decisions (for the owner / the L1 writing-plans pass)
- **Branch/arc placement:** continue on the arc branch (it extends V6's MCP seat)
  vs. a fresh `feature/ast-graph-mcp-claude` off the arc HEAD. Lean: continue on
  the arc branch as a distinct track with its own slice gates; the V6 seam battery
  still gates the develop merge.
- **Verb wire-naming** (`graph.*` vs a namespace) and the exact L1 tool set.
- **Node-spec form for L3** (builder DSL vs. parse-a-fragment).
- **Whether L2 edges are computed on demand vs. cached-per-handle-incremental.**

## 11. Testing / seam
Targeted gates per slice (L1, L2, L3 each: unit test for accessors/verbs +
dialect integration test over the MCP seat). The full battery runs once at the
track's release seam (owner law — never per slice). Engine-touching slices carry
rule trailers; `--exe`/`--obj` lanes when native codegen is touched.

## 12. References
- `docs/plans/2026-06-29-madc-development-substrate-vision.md` (Rungs 3, 5, 6, 7)
- `docs/plans/2026-09-08-madc-ide-nexus-vision.md` (the triad, relation vocab,
  event-sourcing)
- `docs/plans/2026-09-09-nexus-and-multiview-cross-reference.md` (ruled decisions,
  Nexus/session/client vocabulary)
- `docs/plans/2026-09-09-nexus-client-server-design.md` (View §2.1, ChangeEvent
  log §2.4, correlation §2.6, transports/MCP-as-adapter §2.7, propose tier §3.2)
- `docs/plans/data-storage-federation.md` (DataSource/Driver, `falkordb://`,
  `mcp://`)
- `docs/plans/madcdis-plan.md` §9–§12 (GQL/`cypher::`/`gql::` → Query IR)
- `docs/plans/2026-07-04-data-substrate-first-customer-PLAN.md` §Track B
  (`cir_ref`, `madc_cir_node_for`)
- `docs/plans/2026-07-06-madcdis-export-surface.md` (export; 2c blocked)
- `docs/plans/FOREST-SUBSTRATE-READ-FIRST.md` (invariants)
- `include/cir_arena.h` (`DefArena`, `enum DefKind`, `get_def_at`); `src/cir_freeze.h`
- `include/madc/ns_madc` (`parse_*`, `project_*`)
- `tools/madcide/madcide_mcp.inc`, `tools/madcide/madcide_api.inc` (`session_state`,
  `api_run` — the MCP seat this extends)
- External (recon 2026-09-12): Joern CPG / codebadger (arXiv 2603.24837), Serena,
  GitHub stack-graphs, SCIP/Kythe/Glean, LocAgent (2503.09089), RepoGraph,
  CodeStruct (2604.05407), "AST-Derived vs LLM-Extracted KGs" (2601.08773).
- Historical: Rich & Waters *The Programmer's Apprentice*; Teitelbaum & Reps
  *Cornell Program Synthesizer* (CACM 1981); Bachman / CODASYL network model;
  Smalltalk live image; Engelbart NLS (1968); JetBrains MPS.
