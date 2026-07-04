# Data Substrate, First Customer: madc — GOVERNING PLAN

**Date:** 2026-07-04 · **Status:** GOVERNING PLAN for the substrate/serialization
arc. Orders the work from "parse-once complete (v0.33.0)" through the embedded
header forest to language-intrinsic relational containers.

> **Design owner directive (2026-07-04):** madcdis + madcdat functionality must
> *intrinsically exist and function properly for madc itself as first customer* —
> the compiler dogfoods the substrate. On that basis madc then offers intrinsic
> advanced in-memory data structures: what the STL is to C++ containers, taken to
> the next level — **relational, queryable in-memory structures composed with the
> C++20/26 ranges vertical-pipe model** (`data | filter | transform | join | …`),
> where the pipe builds a lazy, plannable, federatable query instead of a chain of
> eager iterator adaptors.

**Reads with (cross-referenced; do not duplicate their content):**
- `madcdis-plan.md` — the core substrate design (pools, values, interning,
  datasets, GQL, federation). This plan REBASES its sequencing note (see §2).
- `madcdat-plan.md` — the external-driver companion; its `snapshot://` driver is
  the convergence point (§4, Phase A2).
- `data-storage-federation.md` — the outward-facing madcdat design
  (StorageBinding, layering, query IR split, delivery order V1–V5). Its
  "Recommended Next Step" (federated planning across sqlite + keyed stores)
  lands here as Phase C4.
- `2026-06-29-madc-development-substrate-vision.md` — the north star; this plan
  executes Rungs 1–2 and the zstd-forest driver of Rung 4.
- `2026-06-22-embedded-header-forest-execution-plan.md` — the forest format
  decisions (SETTLED 2026-06-22) and phase gates; this plan is its funding order.
- `2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md` — Phases 0–5 COMPLETE
  (v0.33.0); its Phase 6 (serialize Tree-1) is Track B here.
- `2026-06-12-type-table-value-abi-design.md` — typeid table + 32-byte
  `madc_value` ABI; its sequencing items 1–2 are Phase A3 prerequisites.

---

## 0. The one-paragraph shape

There is ONE serialization/container machinery, not three. The zstd-compressed
regions appended to the end of the `madc` binary with a directory/index table at
the end (the forest container), the `snapshot://` pool-snapshot file driver
(madcdat V1), and the existing `.madh` PCH container (`include/madc_pch.h`,
`src/pch.cpp`, zstd/zlib live today) are the SAME format family: a **madc::dis
pool-snapshot container** read/written by **madc::dat driver machinery**. The
compiler is the first customer: Tree-1 (the parse-once immutable pattern corpus,
now genuinely immutable since v0.33.0) freezes into pool segments, ships appended
to the binary, and mmaps back in — and the identical machinery, pointed at a
user's pool, is `snapshot://` persistence for the language's own data
structures. Build it once, consume it twice.

## 1. What already exists (inventory, grounded)

**madc::dis primitives (Rung 1 — underway, live in the compiler):**
- `include/madcdis/intern_table.h` — index-linked (NOT pointer-chained) intern
  table, *deliberately built so its three blocks serialize / mmap in place with
  zero fixup*. Consumed throughout lexer/parser (`intern_keyed_map`, spelling
  ids, `strpool`, `type_name_pool`, …).
- `include/madcdis/arena.h` — bump allocator; `TokenArena` HAS-A
  `madc::dis::arena` (the token slot registry stays the token-specific consumer).
- `include/madcdis/id_table.h` — segmented stable-id registry; LIVE as the type
  table: `Program::type_id_for` / `type_from_id` (parser.cpp:9203/9213),
  `project_types{MADC_TYPEID_PROJECT_BASE}` (madc.h:1594), primitive slots in
  `include/madc_typeid.h`.

**madcdat runtime (gated by `./configure --enable-madcdat`):**
- `include/madcdat/` (schema, mapper, query, relation, dataset, driver,
  source_adapter) + forwarding headers under `include/libmadc/`.
- Drivers: `file://`, `dsv://`, `flr://`, `vlr://`, `bdb://`, `gdbm://`,
  `qdbm://`, `sqlite://` (`src/madcdat_storage*.cpp`). FLR tombstone/reap/
  restore lifecycle, FLR-index→VLR-payload `offset` relation, first
  equality-filter query pushdown all work.
- `madc::Query` builder IR exists (`include/madcdat/query.h`); no `snapshot://`
  pool driver yet; no federated planner yet.

**Serialization already in-tree:** `.madh` token-PCH container with
`MadhHeader` (`source_hash` + `compiler_hash` + zstd-preferred/zlib-fallback).

**Parse-once (v0.33.0):** Tree-1 is immutable and is the copy source for every
instantiation — tsubst, never re-parse. This is what makes freezing Tree-1
*sufficient*: a loaded forest can instantiate without any token text.

**Forest baseline (measured 2026-06-22):** decl-parse is a ~1.9 s FLAT
header-closure tax per compile (~78 % of wall). That number is the prize.

## 2. SETTLED — do not re-litigate

1. **Forest format decisions** (execution plan §SETTLED 2026-06-22) stand:
   post-parse AST payload; per-header-file segments; zstd segments **appended to
   the `madc` binary** + segment directory + fixed footer (magic + directory
   offset) found via `readlink(/proc/self/exe)` → mmap; `(segment_id,
   node_index)` addressing behind ONE inline `resolve(ref)` accessor; shared
   trained zstd dictionary; context-hash pin with reject-and-reparse; green/red
   two layers (cold handles, materialize to real pointers at the c2mir edge).
2. **REBASE of madcdis-plan sequencing.** madcdis-plan said "Track 5 starts only
   after Track 1.3 parity." The design-owner directive above supersedes the
   *order of first consumption*: the substrate is built NOW with the compiler as
   first customer (that work IS front-end work and serves the parity/perf
   goals). The outward LANGUAGE surface (Track C below) remains sequenced after
   the compiler-side spine, and the develop→master promote gate
   (`.claude/rules/branching.md`) is untouched.
3. **One container family** (§0). Extending `MadhHeader`/pch.cpp into the pool
   container is in scope; writing a second, parallel serializer is not
   (`no-parallel-implementations.md`).
4. **Script-facing data types are header-defined C++ over libmadc, resolved
   mangled-direct** (`cpp-first-api.md`; the dtSTRING/ns_stl lesson in
   `project_legacy_cpp_shortcuts`). NO new compiler-builtin container types.
5. **The pipe surface builds Query IR** (lazy, plannable), not eager adaptors;
   builder/GQL/SQL/pipe all lower to the SAME IR
   (`data-storage-federation.md` §Query IR; `madcdis-plan.md` Principle 9).

## 3. Track structure and dependency spine

```
Track A  — substrate spine (madc::dis serialization tiers + container + value ids)
   A1 frozen-tier primitives      A2 pool-snapshot container (+ snapshot://)   A3 typeid/value-handle completion
        │                                   │                                        │
        └────────────┬──────────────────────┴────────────────────────────────────────┘
Track B  — the forest (two-tree Phase 6; compiler = first customer)
   B1 serializable cir_node refs → B2 one-segment freeze/thaw → B3 multi-segment
   + append-to-binary → B4 pack pipeline + qualification
        │
Track C  — language surface (relational containers + pipe queries)
   C1 madcdis core-ification → C2 intrinsic types via embedded headers →
   C3 ranges-pipe query composition → C4 federation broadening
```

Priority order: **A then B** (B is the funded next step — the ~1.9 s lever);
**C1 may interleave** with B (it is header/library reorganization, not compiler
internals); C2–C4 follow B unless the design owner pulls them forward. The
O(n²) `c2mir_node_op` walks (feature/front-end-performance-claude) are
orthogonal and stack with B — not part of this plan.

## 4. Track A — the substrate spine

### A1 — frozen-tier madc::dis primitives
- `intern_table` **frozen variant**: construct a read-only view over the three
  blocks (byte arena / entry array / bucket array) placed in mmap'd memory; the
  live class already stores indices precisely for this. Interface identical;
  `intern()` on a frozen table is a contract error (or copy-on-first-write into
  a thin overlay table — decide at implementation from the forest's need:
  loaded segments never intern, the live TU does).
- `id_table` **frozen segment**: a read-only id→record segment with a fixed
  base — the "system forest" segment of the typeid space the 2026-06-12 design
  reserves. The live `project_types` tail grows above it unchanged.
- `arena` **snapshot layout**: give the arena (or a sibling `pool` type per
  madcdis-plan §Pool subsystem) a defined dump form: contiguous chunk payload +
  a small header. Do NOT build shm/refcount/COW tiers now — only what the
  forest consumes (`vision doc §4: generalize from real consumers`).

### A2 — the pool-snapshot container + `snapshot://`
- Define the **container format** (one family, two placements):
  `[container header][segment 0..n (zstd frames, shared trained dict)]
  [segment directory][footer: magic + directory offset]`.
  - Placement 1: standalone file → the madcdat **`snapshot://` driver**
    (madcdat-plan V1 names it: "reads/writes madcdis pool snapshots directly —
    the fastest possible disk persistence path").
  - Placement 2: appended to the `madc` executable → the forest blob (footer
    read from end-of-file; no magic → no blob → live parse).
- Container header carries: magic/version, pool kind, **context-hash pin**
  (replaces today's static `compiler_hash`), type-metadata segment refs, intern
  table segment refs. Directory maps segment-id → {offset, compressed/raw
  sizes, codec, flags}.
- Implement it as madc::dat machinery (a driver/emitter pair), extending the
  proven pch.cpp zstd code; `.madh` token-PCH either migrates onto the new
  container or is retired when the forest supersedes it — no third format.
- **Gate:** unit round-trip (write→read→byte/structural identity) for each
  primitive; fulltest green; `--enable-madcdat=no` builds keep working (the
  *container* lives with madc::dis in core — only external drivers stay gated).

### A3 — typeid / value-handle completion (2026-06-12 design, items 1–2)
- Finish the **value ABI** slices (P0 slices 2–3): literal consumers on
  value-pool handles; the 32-byte `madc_value` struct + cell runtime per the
  agreed design. Needed twice over: B1 serializes literals as pool handles, and
  C2's dynamic values marshal through the same struct.
- The identity layer is live (type table); B1 consumes it as-is.
- **Gate:** the design doc's own guardrails (ONE table, ONE struct; doctests pin
  slots/layout); fulltest; torture byte-identical.

## 5. Track B — the forest (two-tree Phase 6, compiler as first customer)

Phases and gates are the forest execution plan's — restated here only as the
order of consumption of Track A. Follow that doc for detail.

- **B1 — serializable `cir_node` references** (forest Phase 1): `datadef`
  pointers → typeids (A3/A1 frozen segment); tree links → `(seg, node_index)`
  behind the single `resolve()` accessor; literals → value-pool handles;
  provenance/origin tokens → interned handles (kept — they are payload for
  `--emit`/transpile, per MC11-IR law). Gate: fulltest; torture byte-identical;
  `--emit=c11` byte-identical (proves the refs stayed c2mir-blind).
- **B2 — single-segment freeze/thaw** (forest Phase 2): ONE header's parsed
  subtree → flat node records + pool handles → zstd segment **via the A2
  container**; load → decompress to arena → register base → resolve-on-touch →
  materialize at the c2mir edge. Oracle: loaded-from-frozen tree structurally
  identical to re-parsed (round-trip identity). Re-measure the Phase-0.3
  mechanism check (load ≪ parse) on the real path.
- **B3 — multi-segment forest + append-to-binary** (forest Phase 3): per-file
  segments over the stdlib closure; cross-segment references trigger
  load-on-resolve; the appended-blob placement + `/proc/self/exe` loader;
  context-hash pin live; shared trained dictionary. Gate: real
  `<iostream>/<string>/<vector>` compile+run == g++ from the mmap'd forest;
  `madc -dM` parity; SMAUG soak unaffected.
- **B4 — pack pipeline + qualification** (forest Phase 4): build-time pre-parse
  → freeze → compress → append; toolchain re-pin is a manual qualification
  event with automated watching.
- **Success metric:** the ~1.9 s flat decl-parse tax becomes a load measured in
  tens of ms; `--show-stats` before/after published in the landing doc.

## 6. Track C — the language surface (the STL-next-level deliverable)

### C1 — madcdis core-ification (mechanical, may interleave with B)
- Execute the migration both madcdis-plan and madcdat-plan spec: interface
  headers (`schema`, `mapper`, `query`, `relation`, `dataset`, `driver`) move
  `include/madcdat/` → `include/madcdis/`; madcdat keeps external drivers +
  `source_adapter`; forwarding shims during transition; `DataSource` stays in
  `include/libmadc/`.
- The substrate (pools, value, query IR, `mem://` pool-backed driver,
  `snapshot://`) becomes CORE — available without `--enable-madcdat`. The
  configure gate keeps gating only external drivers (bdb/gdbm/qdbm/sqlite/…).
- **Gate:** clean build both configure modes; existing madcdat unit/integration
  tests green through the moved headers.

### C2 — intrinsic types in the language
- Script-facing surface per `cpp-first-api.md`: an embedded header (working
  name `<madc/data>`) declares the substrate types — `madc::value`,
  `madc::array/map/set` (interned, multiplicity-capable per madcdis-plan
  Principles 5–6), `madc::DataSet<T>`, `madc::Relation<A,B>`, `madc::Cursor<T>`
  — as declaration-only C++ resolved **mangled-direct** into libmadc. No
  compiler builtins, no wrapper flattening.
- madc programs get them like STL containers: `#include <madc/data>` and go.
  Type-first mapping is automatic from `DataDef` metadata
  (data-storage-federation §Automatic Mapping Rules); keys/relations are the
  explicit overlay (`bind<T>(src).key("id")`).
- **Gate:** integration tests exercising DataSet/Relation CRUD + scan from .mad
  scripts through the production JIT path; `--emit=c11` of those tests compiles
  and matches (the emit-C oracle).

### C3 — the pipe: relational queries with ranges ergonomics
- **The model:** `operator|` composition over datasets/containers builds the
  SAME `madc::Query` IR the builder produces — lazily. Nothing executes until
  iteration opens a `Cursor<T>`; the planner sees the whole pipeline first.
  ```cpp
  auto adults = users
      | where([](const User &u){ return u.age >= 18; })   // filter → IR predicate
      | join(profiles, user_profile)                       // Relation<A,B> → IR join
      | order_by(&User::name)
      | select<NameCard>(&User::name, &Profile::avatar);   // projection
  for (auto &row : adults) { ... }                         // plan → cursor
  ```
- Ranges-compatible adaptor names where semantics coincide (`filter`/`where`,
  `transform`/`select`, `take`/`limit`, `drop`); RELATIONAL adaptors are the
  next-level step beyond C++26 ranges: `join` (via `Relation<A,B>`),
  `group_by`+aggregates, `order_by`, graph traversal (`expand` over
  `graph_edge` relations). Plain in-memory containers get the same pipe
  (degenerate scan source) so one idiom covers vector→view and
  dataset→federated query.
- Predicate capture: start with the callable-object form (runs local-side,
  always correct); structured predicate forms (`where(field == literal)`
  expression templates) come second so drivers can push down — capability
  honesty per madcdat-plan Principle 3 decides push vs local, never silently
  wrong results.
- This satisfies invariant discipline: `|` is ordinary C++ operator overloading
  parsed by the existing front end (templates are parse-once now) — no grammar
  change, no `--std=` gate needed for the C++ surface.
- **Gate:** pipe results == equivalent builder-API results == an oracle (g++
  compiling the same C++ against libmadc natively); fulltest; a dedicated
  `tests/testdatapipe.mad`.
- Detailed adaptor/IR design gets its own doc at C3 start; this section fixes
  the model, not the full catalog.

### C4 — federation broadening (data-storage-federation "next step")
- Federated planning across `mem://` pool datasets + `sqlite://` + keyed local
  stores; pushdown beyond equality+limit; the pipe and builder both benefit
  (same IR). Narrow V1 per that doc: scans/lookups/filters/projection/limit
  pushed; joins local; no distributed aggregation claims.
- GQL/SQL textual front-ends and `sql::/cypher::/gql::` schema helpers stay
  later (madcdat-plan V5 / madcdis-plan V4) — the pipe + builder come first.

## 7. Discipline / gates (every phase)

- `make -C src fulltest` green per commit; heavy suites serialized.
- Track B additionally: torture failset byte-identical until the forest is ON;
  round-trip identity oracles; never perf-gate correctness, but PUBLISH the
  perf numbers (the forest exists to be measured).
- Both configure modes (`--enable-madcdat` on/off) stay green once C1 lands.
- No name-keyed special cases; primitives generalize from real consumers only.
- Promote gate (gcc-torture class-(a)) unchanged; this plan does not jump it.
- Mirror-sync (claude_status.json / ROADMAP / KG) at track milestones.

## 8. What lands when (order of execution)

1. **A1 + A2 + A3** — the spine. A3 (value slices) and A1/A2 (frozen tiers +
   container) are parallelizable; A2 delivers `snapshot://` as its test vehicle.
2. **B1 → B2 → B3 → B4** — the forest, phase-gated, each on the spine.
3. **C1** — interleave during B as schedule allows (mechanical moves).
4. **C2 → C3 → C4** — the language surface, after B proves the substrate.

Every landed slice appends its landing block to THIS doc's history (the
two-tree plan's convention).
