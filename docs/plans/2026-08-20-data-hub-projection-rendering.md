# The Data Hub & Projection–Rendering Abstraction (Track 7 revisit)

Status: **design draft for owner review** (2026-08-20). This is the successor
framing for [rendering-abstraction.md](rendering-abstraction.md) (2026-05-24),
which remains the reference for the capability levels, the three-way
negotiation, the WCAG mapping, and the 60-years-of-UI lessons table — none of
that is invalidated. What changed since May: the Track 5 data substrate
shipped (`include/madcdis/` — SchemaInfo, SourceAdapter/ExtractedRecordType,
DataSet, Relation, Cursor, Mapper), and the owner's rethinking connected
rendering to the whole lifetime of data. This doc merges the two tracks into
one layered architecture and re-cuts the pilots.

No implementation is planned from this doc until the owner approves it.

## The north star (the 1992 test)

Design the game's data properly and one world serves every interface: a
telnet player reads dynamic prose composed from what is in their field of
view, a 3D client renders the same rooms — same session, same world,
different projections. (Ultima Underworld proved the world model — everything
on screen is a tangible object with state and description; MUDs proved the
text projection; nobody unified them over one substrate.)

Generalized: **data has one authoritative life in the hub; everything else —
formats, schemas-on-disk, screens, reports, saves — is a projection of it or
an adapter into it.** The same architecture must carry a game, a website, a
shop, project management, database administration, and an IDE, rendered to a
dumb teletype, curses, a graphical terminal, a GUI, a browser, or Unreal.
Nothing may be fixed to one application type or one interface type.

## One pattern, second instance

This is the transpiler vision (docs/plans/madc-vision-and-invariants.md)
applied to data. madc reads any source standard into one IR (MC11-IR) and
emits any target; the hub reads any external format into one substrate and
exports any shape. N+M adapters, never N×M converters. The invariants
transfer: no hardcoded formats, everything through a registry, ONE hub
representation, no per-format special-casing inside general machinery
(design-principles rule #7). `SourceAdapter` already is a lexer for data —
down to `SourceLocator` positions, the same discipline as MC11-IR nodes
carrying their originating tokens.

## Prior art (verified 2026-08-20; pieces exist, the unification does not)

- **ANSI/SPARC three-schema architecture (1975)** — external / conceptual /
  internal: per-user-type views over one conceptual model, independent of
  physical storage. The data half. The external schema is also the security
  envelope (see demand 5).
- **CAMELEON / W3C model-based UI** — task model → abstract UI → concrete UI
  → final UI, parameterized by context of use (user × platform ×
  environment). Maps ~1:1 onto the May doc's semantic IR + levels +
  negotiation. Stayed academic; no winning implementation.
- **Naked Objects / Apache Causeway** (and NeXT Direct-to-Web) — the UI
  generated at runtime as a projection of the domain metamodel; methods
  become UI affordances. Proven for back-office/curator users.
- **Lenses / bidirectional transformations** (view-update problem 1981;
  Foster–Pierce) — the formal round-trip theory for "data entry is the
  inverse of display": get/put with round-trip laws.
- **JetBrains MPS projectional editing** — no parser; one AST, many EDITABLE
  notations (text, tables, forms, diagrams). MC11-IR's philosophy applied to
  editing.
- **Entity–component systems** (Flecs/Bevy archetypes) — entity = identity,
  typed components compose dynamically, and archetype storage regroups
  entities into densely packed tables at runtime: "the software determines
  the best shape" is shipping tech in game engines.
- **Commercial** — Airtable/Notion (one table, many user-created views),
  Retool-class tools (introspect the DB, role-specific CRUD), server-driven
  UI, and the 2025-26 generative-UI JSON schemas (Google A2UI, OpenAI
  JSON-UI): the industry is converging on declarative semantic-UI trees —
  validation, and interop targets.
- **Immersive sims** (UW → System Shock → Thief) — simulation-first world
  data where UI is a lens; the genre never generalized it into a substrate.

What nobody has: one substrate owning parse-in, typed storage + relations,
transformation/query, and role/purpose projection down through
capability-tiered rendering — with a JIT that can specialize any of it.
Everyone integrates three vendors and loses type information at each seam.
madc owns every layer already.

## Architecture — five layers, each its own owner

Per data-storage-federation.md §6 (no monolithic metadata objects): schema,
binding, projection, and rendering never collapse into one god object.

```
sources ──adapters──▶ HUB ──projections──▶ semantic trees ──renderers──▶ targets
```

1. **Hub containers.** The atom is the **entity**: an identity (id_table)
   with dynamically composable **typed components** (record-family
   memberships) and **relations**. A classic "record in a schema" is the
   degenerate one-component case. Property-bag data (`madc::value`) is native.
2. **Schema as observation, not gate.** Per field/family, three hardness
   states: *dynamic* → *observed* (inferred from the data actually seen) →
   *declared/locked*. Hardening enables packing (archetype/columnar; the
   5A.7 encoding catalog is the physical half); a surprising record triggers
   deopt-style softening or segregation, never a crash. Observation and
   physical packing are automatic; **semantic locking is always a human
   act** — the system never promises the future (the Ruby/PHP/graph lesson).
   This is the data analog of JIT type specialization, guards included.
3. **Adapters (in/out).** `SourceAdapter`/`FormatAdapter` families, N+M.
   Legacy-anything in (SMAUG areas, mbox, DSV, binary records), anything
   out. External forms are inconsequential; locators are hints back to a
   source, never identity.
4. **Projections.** For access-role R and purpose P: project hub data
   (usually a query result) into a **semantic tree** — the May doc's UINode,
   value-typed (below) — marking which parts carry **actions** (verbs) and
   which bindings are **bidirectional** (entry lenses whose `put` validates
   against the same schema the adapters use). Role selects the projection;
   it is also the security boundary. Projections are themselves hub data
   (demand 1) and are JIT-compiled when defined or loaded.
5. **Renderers.** Capability levels 0–4 **plus feature flags** (a
   sixel-capable terminal is level 1 + an image flag — capability is never a
   rigid ladder; the TERMINFO lesson, already modeled by `RenderCaps`
   bools). Effective rendering = caps × user wants × accessibility needs
   (the May negotiation, unchanged). Per-connection, not per-binary: a
   server JIT-specializes one pipeline per active level, all resident.
   Batch renderers (PDF, CSV, static HTML) are first-class siblings —
   export and render share one seam. The renderer owns cadence; the
   projection owns content.

The wire can sit between 4 and 5 (telnet, WebSocket) — or between any two
layers across a THREAD or PROCESS boundary: the semantic tree/diff is the
wire protocol, and it rides an in-memory channel, a FIFO/UDS, or a socket
identically (demand 15). The May doc's "no serialization boundaries" lesson
applies within one thread of one process; across any boundary the diff
stream IS the design.

## The value-first rule (owner directive, 2026-08-20)

The May sketch used hard C types (`char *label`, fixed enums in structs).
This is a madc feature, so the semantic IR uses madc's own carrier:

- **Content is `madc::value`.** Labels, text, hints, state payloads,
  bindings — anything that is DATA — is a `value`. The UI tree thereby
  becomes ordinary hub data: storable, projectable, diffable, inspectable by
  the same machinery (the meta-level demand closing on itself).
- **Classification is an interned id.** Roles, state flags, action kinds are
  registry-interned integer ids — enum-fast at every hot boundary
  (enum-over-strings rule) yet runtime-extensible (the segmented-typeid
  model), never a closed C enum a user cannot add to and never a string
  compare in a render loop.
- **Structural/platform facts stay native** where it makes sense:
  `RenderCaps` bools, sizes, counts.

The hardness spectrum applies to the UI tree itself: a locked projection over
a locked schema JIT-compiles to direct code with no value-tag dispatch —
value flexibility by default, static speed where the data has earned it.

Dependency note: this leans on the value/type-table arc
(2026-06-12-type-table-value-abi-design.md) and touches known opens
(`value f()` return lowering; `value` std::string ingestion). Those feeders
get scheduled with Phase 1, not silently absorbed into it.

## The demands — what keeps us unlocked

Each demand names the cases that force it and what it forbids. A design
change that violates one of these locks somebody's future out.

1. **Projections are data, not just code.** (Saved user views in PM tools;
   DB-admin ad-hoc views; MUD moddability.) Forbids: projection layer
   existing only as compile-time `render` blocks. Both authoring paths —
   code and stored definition — compile through one mechanism.
2. **Verbs are data too.** (Text-adventure verbs; mudprogs; shop rules; PM
   automations.) Forbids: actions only as statically linked host functions.
   madc is the mod language: a verb is madc code attached to an entity.
   Depends on the eval/exec track (stubbed); interim = compiled verb
   registry bound by data, same action interface.
3. **The meta-level lives inside the system.** (DB management IS a
   projection of schema data; the IDE browses compiler data.) Schemas,
   projections, relations, verbs are entities; `SchemaInfo` is describable
   by a `SchemaInfo`. Forbids: metadata in opaque structures projections
   cannot reach.
4. **Identity outlives representation.** (Saves, ETL round-trips, sync,
   undo.) Stable entity ids across export/import; locators are hints, not
   identity. Round-trip is the acid test of the hub claim.
5. **Projection is the security boundary.** (MUD visibility; shop
   customer/admin.) Project THEN transmit; filtering is never a renderer
   courtesy. Access lives at the projection stage.
6. **Access filters, wants select, needs veto.** Three user axes, never
   conflated: access → which data/verbs exist in the projection; wants →
   which variant/level; needs → constraints the renderer MUST honor.
7. **Granularity below the record.** (IDE char ranges; spreadsheet cells;
   mesh vertices.) Deltas reach component- and range-level. An editor
   buffer = entity with a piece-table component, natively.
8. **Live propagation to N simultaneous heterogeneous connections.** (MUD;
   collaborative PM; dashboards.) Server-side reactivity graph;
   per-connection projection instances; semantic diffs on the wire.
9. **Batch is a first-class renderer.** (Reports, PDFs, static sites, CSV.)
   A projection rendered once to a file; exporters and renderers behind one
   seam.
10. **Time is representable.** (Undo, audit, replay; state evolution — the
    moldy food.) Snapshots + diffs (5A.12/5A.11 seams); never lock in
    "only the present exists."
11. **Schema hardness is a spectrum; softening is legal.** One application
    exercises all three states at once (shop money locked, game bags
    dynamic, PM columns observed).
12. **Renderer owns cadence, projection owns content.** (Unreal frame loop;
    curses refresh.) The projection is never assumed to run per frame.
13. **Multi-user access and collaboration.** (Owner-confirmed 2026-08-20.)
    Concurrent sessions over shared live data build on 5 + 8, plus
    collaboration semantics: every mutation flows through a verb or an
    entry-lens `put`, so **conflict policy is a property of the verb/
    binding** — one owner, not scattered. Component-level granularity makes
    conflicts rarer by construction; policies range from per-component
    last-writer-wins through lock/lease verbs to merge/CRDT (the
    collaborative-text case — hardest, deferred, seam reserved). Presence
    ("who is viewing/editing") is itself hub data projected like any other.
    Server-authoritative state; optimistic local echo is a renderer
    courtesy rolled back through the same diff machinery.
14. **Event streams are projectable.** (Notifications, inboxes, activity
    feeds, combat logs.) A projection over an event log rather than current
    state; falls out of 8 + 10 via derivation relations — named so it is
    tested, not assumed.
15. **The hub is the concurrency substrate — threads and IPC included.**
    (Owner, 2026-08-20: multi-core is table stakes; a language that ignores
    it stays a hobby language.) Collaboration (13) and concurrency are the
    same design at different latencies: two threads writing are two users
    editing. The contracts above ARE the concurrency model — mutations flow
    through verbs (serializable units), reads flow through projections over
    snapshots (5A.12 → MVCC: readers never block writers), propagation is
    diff streams over channels — and the SAME semantic-diff protocol rides
    an in-memory channel between threads, a FIFO/UDS between processes, or
    a socket between machines. One protocol, three transports (the 5A.14
    channel substrate already ships all three). ECS precedent: systems that
    declare their read/write component sets are scheduled in parallel
    automatically — declared access is the parallelism plan. Forbids: any
    API whose contract assumes single-threaded mutation (raw component
    pointers held across verb boundaries; non-atomic `value` cell
    refcounts). Phase 1 implements single-threaded; the contracts are
    concurrency-ready from day one so going multi-threaded changes no API.

## The pilots

**Phase 1 — the text adventure** ("a maze of twisty little passages, all
alike"). Single-player, level 0, and the entire hub end-to-end in miniature:

- world loaded from a tagged-text file through a `SourceAdapter` (ingestion);
- entities with dynamic `value` property bags; relations for geography and
  containment (demand 11 exercised in the dynamic state);
- verbs from data — the compiled-registry interim for demand 2;
- field-of-view / room description as a **query projection** composed from
  entity labels + state at level 0 (a text MUD interface is a screen reader
  for the game world — semantics-first data makes level 0 free);
- save/load as the export/import round-trip (demand 4's acid test);
- the lamp burns down: state evolution over turns (demand 10, minimally);
- **a second role**: a world-builder `debug` view over the same world —
  demands 5 and 6 tested without multiplayer.

Gate: the same world data drives the play projection and the builder
projection; a save exported and re-imported is behaviorally identical
(oracle: scripted playthrough transcripts byte-identical).

**Phase 2 — madcide.** Level 1 (curses), and everything the game cannot
reach: sub-record granularity + high-frequency bidirectional editing (7);
keybinding/theme profiles as wants (6); live build/diagnostic updates as
reactive propagation (8, single-user form); and the meta-level (3) — the
IDE's most interesting data is the compiler's own: tokens, AST, diagnostics,
symbols, all already carrying locators. madcide browsing madc's forest
through the hub is the dogfood moment. Track 8's piece table (8.1) enters as
a component type.

Gate: one buffer edited through an entry lens with undo; diagnostics pane
and outline are projections of live compiler data; a keybinding profile
swap re-renders without touching projections.

**Deferred, not locked out** (each holds a demand as its seat): levels 2–4
and web renderers; true multi-user serving (the MUD); transactions/shop;
federation-driven projections; script-attached verbs (eval/exec); CRDT
collaborative text.

## Track impact (applies only after owner approval)

- **Track 7 re-cut**: 7.1 becomes *Hub projections + semantic IR
  (value-typed) + Level 0*, with the text adventure as its gate; 7.2 stays
  Level 1/curses with madcide as its gate (pulling Track 8.1 forward);
  7.3 reactivity gains the per-connection/diff-wire framing; 7.4–7.7
  unchanged in substance.
- **Track 5 touchpoints**: schema observation/hardening becomes a named 5A
  phase (logical sibling of 5A.7's physical encodings); "projections are
  data" lands beside 5A.2's schema entities; entity/component reading of
  id_table + record families documented in madcdis-plan.md.
- **Multi-core is three separate fronts** (owner, 2026-08-20), kept
  distinct so none blurs the others: **F1** — the compiler uses cores
  (parallel `--project` TU compilation; pure engineering, no language
  design); **F2** — madc PROGRAMS use cores (language surface: threads/
  async/parallel loops, per-thread MIR contexts, atomic `value` cells) —
  its own design arc, to be planned as a sibling doc; **F3** — the hub as
  the safe shared substrate (demand 15, this doc). F3 is what makes F2
  humane: the language arc inherits a data-race-free default because shared
  mutable state lives in the hub behind verbs.
- **Dependencies flagged, not absorbed**: eval/exec (verbs-as-scripts),
  value ABI arc (value-first IR; atomic cell refcounts per demand 15),
  `value` std::string ingestion.
- rendering-abstraction.md gains a pointer to this doc; its levels/
  negotiation/WCAG sections stay authoritative.

## Open questions (for the owner, not blockers for Phase 1)

- Verb conflict/permission model: are verb permissions per-role flags on the
  verb entity, or a relation between role and verb? (Phase 1 can ship with
  flags; the relation shape is more graph-native.)
- The projection definition language: how much of a stored view is
  declarative data vs. attached madc code? (Phase 1: code-defined
  projections only; the stored-view surface designs against real usage.)
- Where the NL description composer lives: a level-0 renderer concern or a
  projection-library concern? (Lean: projection library — the prose IS the
  semantic tree's level-0 content, and other renderers may want the same
  sentences as tooltips.)
