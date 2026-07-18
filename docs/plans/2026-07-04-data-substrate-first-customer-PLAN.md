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
  event with automated watching. **OPERATIVE DESIGN:
  `2026-07-04-forest-default-mode-design.md`** — the design-owner directive
  makes the frozen forest the DEFAULT mode of operation (`#include` binds a
  grove; live parse is the fallback); B4 = parse-time grove binding via
  frozen token slices + a decl index consumed lazily through the existing
  lazy_resolve/nested-parse machinery, sliced B4a–B4d there.
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

---

## Landing history

**✅ A1+A2 slice 1 LANDED `62c1b91b` (2026-07-04, branch
`feature/data-substrate-spine-claude`).** `frozen_intern_table` (read-only view
over the three serialized blocks + `valid()` load gate + `find()` parity) with
serialization accessors on the live table; the **pool-snapshot container**
(`include/madcdis/snapshot.h`, `src/madcdis_snapshot.cpp`): header /
16-aligned segment frames / directory / footer-at-EOF, BOTH placements
(standalone file + appended-to-binary with pad-to-16 so payloads bind in
place), compression via the one `madc_pch` implementation, reader rejects
corrupt/absent blobs cleanly. 7 unit cases / 9861 assertions
(`tests/unit/test_madcdis_snapshot.cpp`); Makefile adds `madcdis_snapshot.o` +
fixes the `MADCDISHDRS`-missing-from-`DEPENDS` staleness hazard. Gate: build
clean 0 warnings; fulltest 677/0/0/16 exit 0, ratchet GREEN. NEXT: A3 (value
slices) and/or the frozen `id_table` segment at B1 (from the real consumer).

**✅ A3 / P0-slice-2 LANDED `7d7c0e5d` (2026-07-04).** `madc::dis::value_pool`
(deduping uint32 handles over (nlimbs, uint64-limb) records; intern_table
three-block discipline → serializes with zero fixup; `Program::valpool`);
lexer dec/hex/oct/binary readers accumulate at 128 bits; gcc canon PROBED and
matched (`tmp/wide_lit*.c`): >64-bit literal = warn "integer constant is too
large for its type" + truncate to low 64 bits, TYPE chosen from the truncated
value — full value retained on `TokenInt::wide_handle`. Also fixed
`Source::showerror` destructive rewind (save/restore cursor — first RESUMABLE
diagnostic exposed it; all prior callers were fatal). Gates: fulltest
678/0/0/16 (new `testwideliteral`); emit-C oracle matches gcc natively;
**torture failset byte-identical to the 51-name baseline** (1571 passed, 0
timeouts). **NEXT — A3 / P0-slice-3 (one coherent unit, own session):** widen
the int64-capped fold spine (`ioperate()`/`ival()` overrides across the
tokens.h operator classes, `parse_constant_*` rungs, `TokenVar` const reads,
`evaluate_type_query`) with host `__int128`; `CirBuilder::integer_typed`
composes >64-bit constants as `((unsigned __int128)hi << 64) | lo` (Tier-1);
fixes the documented residual (case labels >64 bits truncate,
`tests/testint128.mad`). The 32-byte `madc_value` struct already landed via
the eval track — A3 completes at slice 3. B1 is NOT blocked on slice 3 for
the handle SHAPE (slice 2 provides it); wide-fold correctness lands before B1
serializes literal payloads.

**✅ A3 / P0-slice-3 LANDED `956e7030` (2026-07-04, branch
`feature/p0-slice3-fold-widening-claude`) — TRACK A (spine) COMPLETE.**
Recon finding first: the plan-named `ioperate()`/`foperate()` operator web was
DEAD code (zero callers; its consumer was the asmjit-era `optimize` pass) —
deleted @59653106 rather than widened (no-parallel-implementations). The LIVE
spine then widened to a 128-bit carrier (`madc_wide_int` = host `__int128`,
datadef.h): all `parse_constant_*` rungs; leaves via new virtual
`TokenBase::wival()` (full value ONLY for semantically-wide ddINT128/ddUINT128
constants — a gcc-canon-truncated literal stays truncated; resolves through
`TokenBase::_active_valpool`, same discipline as `_active_strpool`);
`read_constant_integer` + `Variable::get<T>` gain 16-byte arms (64-bit reads
sign-carried → bit-identical folds for every ≤64-bit program, no-regression by
construction); `apply_integer_cast_value` sz==8 truncates / sz==16 identity;
case labels fold wide and materialize via `Program::make_folded_integer_token`
(>64-bit values park in valpool, type ddINT128/ddUINT128);
`CirBuilder::integer_typed(madc_wide_int)` Tier-1-composes
`((unsigned __int128)hi << 64) | lo` (signed cast when ddINT128) — c2mir folds
it back to one constant; wide case-range bounds keep the GNU N_CASE form.
Gates: fulltest 678/0/0/16 ratchet GREEN; testint128 wide/neg/unsigned-max
case labels byte-identical to gcc AND clang; emit-C oracle MATCH (labels render
as the composed expression, portable C); torture 1571 passed, failset
byte-identical to the 51-name baseline, 0 timeouts; SMAUG soak compiles all 51
TUs and boots. Known fenced divergences (documented in the typedef note):
unsigned-64 compare/div edges are the PRE-EXISTING untyped-fold behavior
(unchanged); wide static-const class members capture at int64
(static_member_const_values stays int64); nontype template args stay int64
domain. NEXT: **B1** (serializable cir_node references) — the forest track.

**✅ B1 LANDED `0b1e618a` (2026-07-04, branch
`feature/b1-serializable-cir-refs-claude`) — forest Phase 1: serializable
`cir_node` references.** Every madc EXTENSION
field on `cir_node` is now position-independent — an index, a handle, or a
`(seg, idx)` ref; raw pointers remain ONLY in the c2mir-visible `base` (op
links, `u.s` string payloads, `attr`), which stays pointer-based live by
decision (forest SETTLED #1/#7) and maps to refs/handles at freeze time (B2).
The conversions:
- `datadef` (DataDef*) → `datadef_id` (uint32 typeid, `madc_typeid.h`
  segments). The id policy chokepoints hoisted to free functions
  `madc_type_id_for` / `madc_type_from_id` over a `madc_active_project_types`
  global (bound at `_parser_init` and by the Program methods per call — the
  `_active_strpool` discipline); `Program::type_id_for/type_from_id`
  bind-and-delegate. Exact DataDef* round-trip (table stores pointers; memo
  on `dd->type_id`).
- `typedef_name` / `error_msg` / `tsubst_pack_value_name` (const char*) →
  uint32 handles in the ONE shared serializable string pool
  (`TokenBase::_active_strpool`; SETTLED #5 "shared pools"), accessors with
  the transient-c_str contract (consume immediately, hold the id).
  `CirArena::intern` + its non-deduped `strings` side pool DELETED. Builder
  state `m_tsubst_copy_pack_value_name` → `..._value_id` (a held pointer
  would dangle on pool growth).
- `tree1_origin` (cir_node*) → `cir_ref{seg, idx}` (two uint32s) behind THE
  one resolve accessor `madc_cir_node_for(ref)` (SETTLED #5): `CirArena`
  self-registers in the segment registry (seg 0 = null, so a zeroed record
  is a null ref), `alloc()` stamps each node's own `self` ref, `node_at()`
  does the page math. Frozen segments (B2/B3) join the same id space behind
  the same chokepoint. Destroyed arenas unregister — a stale ref resolves
  NULL, never to freed memory.
- Literals: node scalar payloads are POD-inline in the record (serialize
  as-is); wide literals already ride value-pool handles at the token layer
  (A3); `u.s` string payloads are c2mir-visible base fields that map to
  intern handles in B2's record freeze, like op links.
Gates: build 0 warnings (plus fixed the ONE pre-existing
-Wmisleading-indentation in `DataDef::is_object` — whitespace only); test_cir
99 cases / 16191 asserts incl. 4 new B1 cases (segment identity across page
boundaries, unregister-ends-resolution, string-handle round-trip + dedup,
typeid round-trip primitive + project); fulltest 678/0/0/16 all ratchets
GREEN; **`--emit=c11` byte-identical on 688/694 tests** — the 6 divergent
files differ ONLY in per-compilation PROJECT-segment typeid constants inside
eval value shims (`madc_value_get_type_id` guards /
`madc_value_make_instance`): `set_datadef` now lazy-stamps node-attached
types, shifting first-ask order; ids are per-compilation by design
(2026-06-12 §2; pinned primitive slots unchanged), and self-consistency was
proven by compiling the divergent emitted C with gcc against libmadc and
matching the JIT output. All c2mir-visible emission byte-identical — the
references stayed c2mir-blind. Torture 1571 passed, failset byte-identical
to the 51-name baseline, 0 timeouts; SMAUG soak compiles and boots.
NEXT: **B2** (single-segment freeze/thaw through the A2 container).

**✅ B2 LANDED `b62089ad` (2026-07-04, branch
`feature/b2-segment-freeze-thaw-claude`) —
forest Phase 2: single-segment freeze/thaw through the A2 container.**
New `src/cir_freeze.{h,cpp}`:
- **Freeze** (`cir_freeze_subtree`): flattens a built cir_node sub-DAG into
  fixed-size POD records + a CSR child-index pool. The B1 extension block
  copies as-is (already position-independent); scalar leaf payloads inline
  (raw 16-byte union image — arena nodes are zero-initialized on both
  sides); string payloads (`u.s`) intern to shared-pool handles with exact
  byte length (embedded NULs survive); child lists flatten via c2mir's own
  `ext_node_is_leaf`-guarded walk (the N_CF/N_CD/N_CLD above-N_ID leaf trio
  handled by mirroring that single source of truth). Shares (the c2mir
  N_SHARE one-spec-under-many-parents pattern) freeze to ONE record via
  first-touch index assignment; genuine cycles (__max_size_type) terminate;
  the walk is iterative (real trees exceed the dump walker's 800 cap).
- **Container**: two consumer kinds over the content-blind A2 snapshot
  (`SNAP_KIND_CIR_RECORDS` / `SNAP_KIND_CIR_CHILDREN`); `cir_freeze_read`
  bounds-validates every child ref so a corrupt container fails at load,
  not as a wild read at materialize. Child-pool entries are in-segment
  indices; the high bit is reserved for B3 cross-segment connectors.
- **Thaw** (`CirFrozenSegment`): registers in the B1 segment registry —
  frozen records join the ONE `(seg, idx)` id space behind the ONE
  `madc_cir_node_for` chokepoint (the registry generalized to a
  `cir_segment_source` interface; CirArena implements it). `node_at()` is
  resolve-on-touch: two-phase materialization (memoized shells first, then
  child appends, so shares/cycles terminate) into real pointer-linked
  cir_nodes at the c2mir edge (SETTLED #7), mirroring CirBuilder::make
  (fresh uids, uniq strings, position derived from the origin token).
- **Oracle** (`cir_trees_structurally_identical`): iterative parallel walk
  (seen-pair set) over codes, payload classes, extension fields, child
  sequences.
**Phase-0.3 mechanism check re-measured on the real path (tmp/b2_measure):
`testsubscript` module tree = 90,647 records, 456 KB container;
open+decompress+thaw+materialize = 55.8 ms vs parse+translate 2508 ms — 45×;
materialize (35.7 ms) does not erode the win; structural identity YES.**
B3 fence (documented in cir_freeze.h): datadef_id / origin_id / string
handles resolve against the LIVE process substrate; cross-process closure
(binding the container's own frozen intern/type segments, context-hash pin)
is B3.
Gates: build 0 warnings; new `tests/unit/test_cir_freeze.cpp` 6 cases / 58
asserts (payload classes, share dedup→one record AND one materialized node,
cycle termination, 10k-deep iterative walks, file placement, module-tree
identity + thawed-tree compiles-and-runs through production cir_compile);
fulltest 678/0/0/16 all ratchets GREEN; torture 1571 passed, failset
byte-identical to the 51-name baseline; **--emit=c11 byte-identical
694/694**; SMAUG soak compiles and boots.
NEXT: **B3** (multi-segment forest + append-to-binary + context pin).

**✅ B3 LANDED `75830eec` (2026-07-04, branch
`feature/b3-multi-segment-forest-claude`) —
forest Phase 3: multi-segment forest + append-to-binary + context-hash pin +
CROSS-PROCESS CLOSURE.** The B2 fence is cleared: a FRESH madc process now
thaws, compiles, and runs a frozen module tree with no parse, no parser
state, and none of the freezing process's pools.
- **Per-unit groves + connectors** (`cir_freeze_forest`): ONE partitioned
  walk (the B2 single-blob freeze now delegates to it — one walk, two
  formats) splits the module sub-DAG into per-source-file units (a node's
  unit = its origin token's file; origin-less nodes inherit their
  discovering parent's; the unit key is an interned spelling, so a C++20
  module name slots into the same directory). A child crossing units is a
  CONNECTOR: high bit of the child entry + an index into the owning unit's
  connector pool of (target_unit, target_record) — a reference, never a
  node KIND (SETTLED #6; c2mir stays blind). Resolving a connector into a
  cold unit triggers that unit's decompress+register: **groves load on
  demand** — `CirFrozenForest::open` reads ONLY the directory, pin, string
  pool, type names, and libs; `units_loaded()` pins the laziness in tests.
  The thaw driver is one iterative worklist ACROSS units (a connector hop
  is a worklist step, not recursion — cross-unit chains can't blow the
  stack; memoized shells terminate shares and cycles that cross units).
- **Cross-process closure**: the container carries its OWN string pool (the
  A1 `frozen_intern_table` three-block serialization of the freezing pool —
  every record handle resolves against it by construction), a per-record
  `{fname_id,line,col}` position side-car (c2mir positions no longer need
  the freezing process's token arena), the typeid→name closure (project
  segment; primitives are pinned process-invariant; foreign project ids
  READ as NULL on the compile path — DataDef reconstruction from thawed
  decl trees is the parser-resume slice, B4+), and the required-library
  list (`Program::loaded_lib_paths` records #load/-l dlopens; the frozen
  run re-dlopens them before link). Extension string ids re-intern into the
  live pool at materialize — in-process that dedups to the identical id,
  cross-process it yields a fresh valid id (the identity oracle now
  compares those three fields by CONTENT).
- **Context-hash pin** (`madc_cir_context_hash`): madc version + record/
  position layout + the c2mir node-code enum tail + the typeid primitive
  tail, stamped into the container header; readers REJECT a mismatch
  loudly (replaces the static `compiler_hash` discipline for this format).
- **Append-to-binary + /proc/self/exe loader**: `--freeze=<f>` /
  `--freeze-append=<bin>` (A2 placement 2) / `--run-frozen[=<f>]` (bare =
  the blob appended to the running executable, via readlink+mmap+EOF
  footer) / `--freeze-run` (freeze to temp + re-exec self in a genuinely
  fresh process — the one-invocation round-trip the integration test
  drives). `CirJitSession::build_frozen` reuses the production link/run
  half (shared `init_contexts`/`load_and_link`); `--emit=c11` takes
  precedence over freeze modes (an explicit render request).
- **B4 hooks reserved**: each directory unit carries `anchor_idx`
  (`CIR_FOREST_ANCHOR_NONE` in B3) — the grove entry a parse-time
  `#include` / C++20 `import` binds to instead of re-parsing.
- **Fenced to B4** (explicitly, not silently): the shared trained zstd
  dictionary (this build compresses zlib; the per-segment codec field
  carries the flip), `madc -dM` macro parity (its consumer — forest-
  supplied PP state — arrives with pack-time parse binding; B3's payload
  is post-PP module trees, the live PP path untouched), and system-segment
  typeid occupancy (B3 ships identity + names).
**MEASURED: a real `<iostream>/<string>/<vector>` program freezes to 93
header units / 47,178 records / 683 KB (zlib); live parse+run 1.659 s vs
`--run-frozen` 0.082 s END-TO-END process wall (start + thaw + c2mir + JIT
+ run) = 20×; output == g++. The appended-blob copy of bin/madc runs it
from its own EOF footer.**
Also fixed en route: `mmap` failure compared against MIR's redefined
`MAP_FAILED` (NULL) instead of the real `(void*)-1`; `--run-frozen`
program args ending in `.json` were eaten by the project-manifest sniff.
Gates: build 0 warnings; test_cir_freeze 11 cases / 117 asserts (5 new B3:
two-file partition+connectors thaw-identical-and-runs, on-demand grove
loading, FRESH-pool closure incl. re-intern + compile+run, pin reject,
directory/libs/type-name round-trip); fulltest 679/0/0/16 (new
`tests/testfreezerun.mad` == g++ under `--freeze-run`) all ratchets GREEN
incl. the new `scripts/forest_selfexe_gate.sh` (appended forest runs from
/proc/self/exe, wired into fulltest); torture 1571 passed, failset
byte-identical to the 51-name baseline; --emit=c11 byte-identical 694/694
(the +1 new test emits clean); SMAUG soak compiles and boots.
NEXT: **B4** (pack pipeline: build-time pre-parse of the stdlib closure →
freeze → append; anchor binding at parse time; qualification gate).

**✅ B4a LANDED `54aff2ce` (2026-07-05, branch
`feature/b4a-forest-pack-format-claude`) —
forest Phase 4 slice a: grove payload v2 + pack-time recording + oracles +
build modes (design doc `2026-07-04-forest-default-mode-design.md` §9). The
format + pack side of forest-default; NO consumption yet (suite-neutral by
construction — the container gains the payloads a B4b parse-time bind reads).**
- **Grove payload v2** (`CIR_FOREST_FORMAT_VERSION = 2`; the versioned
  context-hash string re-pins automatically, so v1↔v2 containers reject each
  other): per-unit segment slots grew 4 → 8 (+post-PP token slice in `.madh`
  record form via the extracted `serialize_token_seq`; +decl index
  `cir_forest_decl_entry{name_id,kind,slice_begin,slice_end,aux}`; +PP-export
  event stream; +include edges) plus two container-global segments
  (branch-relevant macro set, canonical unit order). `anchor_idx` now = the
  decl-entry count when a grove payload exists (`ANCHOR_NONE` = module-only
  unit). Units may be token-only or fully empty — directive-only headers
  (`<ios>`, `features.h`, `cdefs.h`) carry PP exports + edges, zero stream
  tokens.
- **Pack-time recording** (`Program::pack_*`, live ONLY under `--freeze` /
  `--freeze-append`, one predicted branch per site otherwise): PP-export
  deltas in directive order with `#undef` tombstones; include EDGES recorded
  pre-Source-swap incl. the once-only-skip paths (the edge exists even when
  dedup skips re-tokenization); the container-global BRANCH-MACRO set (one
  hook in `expandIfMacros` catches every `#if`/`#elif` consult incl.
  `defined` operands; plus `#ifdef/#ifndef` and the include-guard definedness
  check); and per-top-level-decl boundary FRAMES at the three decl loops
  (`Program::parse`, `parse_namespace_block`, `extern`-linkage blocks — the
  last collapsed 335 SPANS-flagged glibc entries to 0), with registration
  TAPS attaching the exact map keys to the innermost frame (types
  flat+ns-qualified, struct tags, funcdef ids, all five template maps
  bare+qualified, variables incl. scoped enumerators under their
  pseudo-namespace, `using` imports incl. `using ::X`, and
  `mirror_inline_namespace_into_parent` copies — how `std::X` mirrors out of
  `std::__cxx11`). Taps gate on `compounds.empty()` and `_inst_depth == 0`.
  The Program→payload bridge (`cir_forest_fill_pack_payloads`) lives in
  madc_cir.cpp so the container layer stays Program-blind.
- **Observability + oracles**: `--dump-forest[=f]` (every v2 surface),
  `--dump-registered` (post-parse lookup surface — instantiation products by
  canonical-spelling `<`, class methods by `method_display_name`, and
  function-typed namespace vars excluded), `-dM` (macro table, gcc `-dM -E`
  analogue). `scripts/forest_index_oracle.sh`: (registered − empty-TU
  baseline − synthetic emit schemes) ⊆ decl index, modulo a documented
  41-entry allowlist (4 classes: instantiation-time member machinery,
  madc-internal symbols, nested-classes-of-instantiations, lazily-registered
  libc builtins) — GREEN: 4,872 indexed names cover 3,796 lookups.
  `scripts/forest_dm_oracle.sh`: macro-NAME-set parity vs g++ as a RATCHET
  (305-line baseline; new divergence fails; `--rebaseline` ratchets down).
  Both wired into fulltest.
- **Pack driver + canonical-order artifact**: `scripts/forest_pack.sh` builds
  the standard-header TU from the versioned `scripts/forest_pack_headers.txt`
  (v1: 18 headers, `cstddef`→`fstream`, streams before `cmath`/`algorithm`;
  `cstdint`/`cmath`/`algorithm`/`iomanip` documented OUT with their
  blockers), freezes onto a binary COPY (ETXTBSY), and verifies the blob
  reads back + a frozen run works.
- **Build modes** (`MODE=develop|debug|release`): per-mode object trees
  `obj/<mode>/` (modes never share objects; `-MMD` tracks headers not flags);
  `make debug` = `-O0 -ggdb`; `make release` = `-O2` → **strip BEFORE
  pack+append** (mandatory ordering; plain strip keeps `.dynsym` for the
  `-rdynamic`/dlsym import resolver) → `forest_pack.sh` → verify. Pack
  failure = build failure.
- **Also fixed en route**: `tests/unit/test_cir.cpp` "unsubstituted template
  parameter" built synthetic error nodes without binding the test substrate,
  silently borrowing whatever stale string pool a prior case's dead Program
  left in `TokenBase::_active_strpool` (a latent dangling read this change's
  Program footprint surfaced as a SIGSEGV); it now binds like every other
  synthetic case.
**MEASURED: the real `<iostream>/<string>/<vector>` program freezes to 190
units / 47,178 records / 167,750 sliced tokens / 4,677 decl-index entries /
726 branch macros (container 1.53 MB vs 683 KB module-only, zlib);
`--run-frozen` output still == g++. The 18-header pack closure: 234 units /
241,853 tokens / 6,701 decl entries / 796 branch macros; SPANS-flagged slices
= 0. `make release` binary: 18.7 MB → 7.3 MB stripped + 234-unit forest
appended, runs its own blob.**
Gates: fulltest 679/0/0/16 + all unit suites incl. the new B4a v2 round-trip
(test_cir_freeze 12 cases / 172 asserts, through the PRODUCTION
`madc_cir_freeze` path) + both new oracles + `forest_selfexe_gate` GREEN;
gcc-torture 1571 passed, failset byte-identical to the 51-name baseline;
`--emit=c11` corpus 695/695 byte-identical; SMAUG `--project` boots and serves
("Realms of Despair ready"); `make release` end-to-end (stripped -O2 binary +
appended pack + self-exe frozen run).
NEXT: **B4b** (flag-gated bind + materialize-on-use: the forest-lookup chain
in `lazy_resolve`/`lazy_resolve_type`, PP-export install along the include
DAG, nested slice parse via the `parse_deferred_lazy_body` reentrancy
pattern).

**✅ C1 madcdis core-ification LANDED `bdd129fe` (2026-07-18, branch
`feature/class-parse-once-codex`, task #58).** The six interface headers
(schema, mapper, query, relation, dataset, driver) moved `include/madcdat/` →
`include/madcdis/` (git mv; guards `__LIBMADCDAT_*` → `__LIBMADCDIS_*`;
intra-includes retargeted). Forwarding shims hold the old madcdat/ paths for
out-of-tree consumers (deletion horizon: first release after libmadcdat ships
against madcdis paths); every in-tree consumer — the `include/libmadc/` public
shims, `madcdat/source_adapter.h`, the four external-driver TUs — points at
the new home directly. madcdat keeps external drivers + source_adapter behind
`--enable-madcdat`, unchanged. The substrate was verified ALREADY core
(`madcdis_snapshot.o` in `CORE_OFILES`, built with madcdat=no) — no gating
change; no `mem://`/`snapshot://` driver invented (A2's deliverable).
`install-libmadc` now ships `include/madcdis/` (the installed `libmadc/`
shims previously dangled without install-madcdat). `DataSource` stays in
`include/libmadc/`. Gate: BOTH configure modes build clean 0 warnings; the
=yes mode (all backends: bdb/gdbm/qdbm/sqlite) ran fulltest 717/0/0/16 with
every madcdat storage/relation/contract unit suite green through the moved
headers + packed release arbiter 717/0/0/16 blob-verified; tree restored to
the =no baseline. NEXT in track C: C2 (intrinsic types) after B proves the
substrate.
