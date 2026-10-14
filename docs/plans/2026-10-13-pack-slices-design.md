# Pack slices — a bind loads only the units a program includes

**Date:** 2026-10-13
**Status:** DESIGN (owner review) — implementation on Opus 5.5, in its own
focused session.
**Brief (owner-approved 2026-10-13):** one shipped pack, cut per unit; a bind
loads only the slices of the units the program's include closure reaches — no
whole-pack sweep, no whole-arena judging; the dialect prelude is a slice like
any header, loaded under `--std=madc`. Target: the Test 6 column of
`2026-10-13-pack-load-cost-experiments.md` on the shipped binary, at a pack
size ≤ 13,000,000 bytes (owner 2026-10-13: today's + 1.2 MB — measured +0.9 MB, a small increase
accepted; raised 2026-10-14 to 13M for headroom, then to 15M for the node
units read block by block — §3, format 61).
**Provision (owner-approved 2026-10-13):** the slice format must not change
again for the later pull-on-lookup step (clang PCH / GCC CMI model: the unit
of load is an entity cluster, the trigger a name-lookup miss).
**Parent:** `2026-07-09-forest-full-evaluation-lazy-materialize-PLAN.md`
(settled #7 "only used nodes materialize" — this design delivers its load
half); `2026-08-08-packed-include-latency-plan.md` (its baseline is Test 1
here). Decision `dialect_headers_internal_indexed` (KG) lands on this.
Closes Gap `pack_bind_cost_scales_with_container`.

## 1. Targets (the gate numbers)

Measured on a symbolized release build with the pack built in, warm, median
of 30 interleaved runs + callgrind Ir (`tmp/pc/t6/t6.py` is the harness):

| workload | today | target (Test 6 floor) | ratchet (Ir) |
|---|---|---|---|
| `<stddef.h>` | 30.9 ms / 174M | 5.1 ms / 12.4M | ≤ 14M |
| `<stdio.h>` | 30.5 / 180M | 5.8 / 18.2M | ≤ 20M |
| `<limits>` | 32.7 / 183M | 6.3 / 21.4M | ≤ 24M |
| `<type_traits>` | 42.0 / 258M | 7.1 / 28.7M | ≤ 32M |
| `<vector>` | 64.6 / 352M | 15.0 / 76.7M | ≤ 85M |
| 8 everyday "C" headers | 79.7 / 394M | 26.6 / 131M | ≤ 145M |
| cxx_stl.cpp | 316.7 / 1,661M | 270.4 / 1,420M | ≤ 1,500M |
| dialect prelude (`println("hi")`) | 78.7M live parse | — | < 78.7M bound |
| pack size (release, Linux) | 11.6 MB | ≤ 12.8 MB | gate |

The ratchets become `scripts/parse_cost/baseline.tsv` rows (recorded with
`parsecost-record` once green, never hand-typed).

As closed (part 2, 2026-10-14; symbolized release, pack built in, every
output identical to live): `<stddef.h>` 12.7M, `<stdio.h>` 19.7M, `<limits>`
23.4M, `<type_traits>` 28.3M, `<vector>` 76.3M, the 8 C headers 135.8M,
cxx_stl.cpp 1,574.5M / 247 ms (a container frozen from its own includes:
1,491M / 234 ms), pack 12.51 MB. cxx_stl meets its time target, not its Ir
ratchet; its row is recorded at the measured value (owner 2026-10-14) and the
remaining excess over its own container moves to part 3: a record the closure
only names restores complete (KG Gap
`pack_restores_forward_named_product_complete`), a node unit decodes whole
(the TU unit: 388K records, its own container's 303K), and the shared
tables' blocks are laid out by single-header binds (cxx_stl decodes 722
blocks, its own container 605).

## 2. What changes, in one paragraph

Mechanism revised at implementation start (2026-10-13; same targets, same
gates): the arena's cross-references — record ids, payload offsets, string
ids — are global and are also baked into node records (`datadef_id`),
globals, template records and class-pattern payloads, so physically cutting
the tables per unit would mean rewriting ids across every segment, and a
missed field would be a silent wrong type. Instead the tables stay where they
are and a bind stops touching what the program does not include, on three
axes: (1) WHICH records are visited — a frozen per-unit SEED index replaces
the whole-arena walks; (2) how they are JUDGED — a frozen inverted
declaration index (name → declaring units) replaces the all-units sweep and
its name maps; (3) which BYTES are decoded — the shared tables are stored as
independently compressed blocks, decoded on first touch (the zstd seekable /
clang PCH offset-addressed model). Record ids, offsets and string ids are
unchanged.

## 3. Container layout

`CIR_FOREST_FORMAT_VERSION` 57 → 58 (59, then 60, as built below); an older pack is refused at
`open_header` (no compatibility case). New segments:

| segment | content |
|---|---|
| DECLNAMES | every declared name (sorted, unique; the FUNC_NAMES string-table form) + its declaring units (a word run per name). Replaces the all-units decl-index sweep. |
| SEEDS | per unit, the arena slots, template records and global records a bind of that unit visits (CSR: offsets per unit, then three id arrays); unit index `unit_count` = the always-visited list. |
| RECORDABLE | the recordability fixpoint (one bit per arena slot) + the anon-tag floor, computed at freeze — today every process recomputes it over the whole arena. |

Shared tables (string pool 2/3/4, arena 10–14, templates 15–17, token bytes
19) are written as BLOCKED segments: the segment's bytes split into
fixed-size blocks, each compressed alone, with a block table. The reader
decodes a block on first access (process cache, as today's segment cache).
Blocks under 1 KB raw stay codec None.

**As built (format 59, 2026-10-14).** Measured during part 2, each change
kept only where it measured better than the plan's form:

- **String pools** (2/3, arena 12/13): bytes (BLOCKED) + an OFFSETS block —
  one `uint32` start per id and a trailing bytes size, stored raw and read in
  place; length = next − start − 1. The entries/buckets segments (4, 14) are
  retired: no production path looks a spelling up in a frozen pool (the
  decl-index and seed lookups are their own sorted tables), so the hash
  buckets were pure decode cost. Smaller pack, fewer bytes touched per id.
- **DECLNAMES / SEEDS / RECORDABLE** are stored raw (codec None) and read in
  place — no decode on open.
- **BLOCKED set:** string bytes, arena defs / payload / token bytes, template
  payload / tokens, and the globals (9) and templates (15) record tables,
  which bind reads lazily by index (`ForestRecordTable`).
- **Block size 16 KiB.** 4 KiB measured +0.67 MB (12,785,936 bytes) and a
  slower 8-header C bind (more blocks, each with frame overhead).
- **Point of creation** is a property of the entity: every `DataDef` keeps
  the creation point open at its birth (`DataDef::pack_poi`), so a record the
  freeze writes after the parse (an instantiation product, a namespace
  binding — stamped with its function's) is still placed with the unit whose
  parse made it, not the always list. A creation point in the producer's own
  source (the root unit, never bound by a consumer) places nothing; the record
  falls back to its declaring file.

**As built (format 60, 2026-10-14) — the LAYOUT pass.** The pack step lays
the container out by what binds read:

- **Recording.** `scripts/forest_pack_layout.sh` (sourced by all three pack
  scripts) binds each root of the pack's header list alone against the new
  container, `--emit=c11` (parse + lower, no execution — a cross freezer runs
  it too), with `--forest-touch-log=<file>`: every lazy-block `ensure` of a
  frozen table (`lazy_touch_sink`) is written as `seg off len`.
- **Relayout** (`--forest-relayout=<image> <logs...>`, `cir_forest_relayout`).
  Every string-pool record and arena record a log names gets a SIGNATURE (the
  roots that read it); the pools (2/3, 12/13) and the arena defs (10) are
  rewritten ordered by signature (lexicographic over the root order; first
  read breaks ties), then the unread items in id order. Ids never change: a
  pool record moves through its offsets block, an arena record through the
  new raw ARENA_DEF_POS segment (slot → position). Simulated against the
  per-unit touch logs, this ordering beat per-unit placement by seed owner
  (stdio 13 blocks vs 21, c8 99 vs 168: only 665 of the 1,453 records a
  `<vector>` bind reads are seeds; the rest are pulls).
- **Pool records carry their length.** A record is LEB128 length, spelling,
  NUL; the offsets block maps id → record start. String-literal spellings
  hold NULs, so a length found by scanning for the NUL is wrong.
- **Hot blocks are stored raw.** A block (any BLOCKED segment, counted in
  the new layout) that at least half of the root binds read is written
  uncompressed — copying a 16 KiB block costs ~16K Ir, decoding it ~230K.
  17 blocks, +0.23 MB. The reader needs nothing new: stored == raw already
  meant a raw block.
- **Open is O(1) in the directory.** The snapshot directory (5,627 records
  in the pack) is read in place when aligned in the image and each record is
  checked when handed out (`find` / `segment_at`); open keeps only a
  sortedness scan. The unit-name index keeps its bounds checks and drops the
  strcmp order sweep (a misordered index can only miss).
- **Content digests are layout-invariant**: DEF_POS is skipped and the laid
  out tables digest in logical (id) order, so a laid-out pack digests as the
  freeze that produced it.

Measured (symbolized release, pack built in, Ir): stddef 12.7M, stdio 19.8M,
limits 23.6M, type_traits 28.8M, vector 79.2M, c8 140.0M; pack 12,545,432
bytes.

**As built (format 61, 2026-10-14) — node units by block.** A unit's node
records, child entries, connectors and positions are BLOCKED too (128 KiB
blocks; the records keep their byte-plane and the children their delta as
each block's inner transform), and a bound unit reads them in place: a record
is decoded with its block and bounds-checked when first resolved, never as a
whole-unit decode + validation pass. cxx_stl.cpp's 16 units hold 569,504
records, of which it materializes 2,121, and its reads cluster (96 of the
3,381 16 KiB blocks). Block size moves the pack, not the bind — measured on
the release, 16 / 64 / 128 KiB: pack 14.61 / 13.65 / 13.35 MB; cxx_stl
1,436.4M / 1,442.0M / 1,447.2M Ir, 218 ms at each (from 1,574.6M / 247 ms);
subscript 2,397.2M → 2,269.3M Ir, 367 → 338 ms at 128 KiB; the ladder rungs
unchanged (they bind no node unit). The owner took 128 KiB and a 15 MB cap.

## 4. Seeds — which records a unit's bind visits

Each restorable entity is placed by the SAME predicate the bind uses to keep
it (one owner, called at freeze and at bind):

- **Decided by name** — its judged names (qualified, bare, the head of a
  template-id canonical spelling) are declared by some units: placed in the
  seed list of every unit in those sets. Keep predicates are monotone in the
  closure, so a record kept for closure C is in a list of some unit of C.
- **Undecided** (no judged name declared anywhere — instantiation products,
  `__oN` instances, function-local classes): placed with its POINT OF
  CREATION, stamped at write-through — the unit of the innermost open
  top-level declaration (`pack_decl_stack`), the same token → unit mapping
  the decl index uses. Unstamped records (created after the parse) go to the
  always list. This is the one semantic change: today an undecided record is
  restored for every closure; after, only when its creating unit is bound —
  what a live parse of the closure creates (Test 6 measured it correct).
- **Instantiation products** (as built, 2026-10-14) — a class product or a
  function-template instance is placed with every unit whose parse REQUIRES
  it: the unit that created it, every unit that reused it (an instantiation
  memo hit), and — through the requiring product — the units requiring the
  instantiation whose class body, signature or method body made or reused it.
  Not with its template's declarers: measured, the head rule gave a `<vector>`
  bind ~6,600 records its own container lacks (`basic_string<char16_t>` and
  its methods, made by `<string>`'s parse), 244M Ir against the own
  container's 68.9M. The parser records this as it instantiates
  (`Program::PackCtxFrame`, `pack_note_require`; `DataDef` / `Variable`
  `pack_ctx` = the instantiation an entity was born in, which it rides). A
  product no unit requires keeps the head rule. The keep predicate is
  unchanged; placement only decides which binds visit the record.
  As built, second pass (2026-10-14): a unit requires a product only when its
  include closure DEFINES the product's template (the template's body-run
  unit, by qualified name; an unknown template is not filtered) — the
  producer parsed every root in one TU, so libstdc++'s `__try_use_facet`
  instantiated `moneypunct<char>` (and every other standard facet) under
  `<istream>`, where a live parse has only the forward declaration. What a
  product on the requirement graph carries stays on it with no requirer of
  its own, never falling back to its template's declarers. A function-
  template record is listed with the one unit declaring it (`std::swap`'s
  overloads in `<bits/move.h>`, `<sstream>`, `<fstream>`, ... are separate
  records). A function-local class born in an instantiation rides its
  requirers only. Measured on the release: cxx_stl 1,628M → 1,576M Ir
  (249 → 246 ms), `<vector>` 79.2M → 76.4M, the 8 C headers 140.0M →
  135.8M. Open: a record the closure only NAMES (an `<iosfwd>` typedef of a
  product `<sstream>` instantiates) still restores complete through the
  typedef's pull (KG Gap `pack_restores_forward_named_product_complete`).
- **Namespace bindings / deferred bodies** — placed with the function or
  owner class they bind; inline-namespace links and namespace aliases in the
  always list.
- **Derived types** (pointer, reference, cv, array, function pointer, member
  pointer) — not seeded: observable only through the forest's id map, so they
  are built on first lookup (operand first), which is identical to building
  all of them.
- **Pulls** — a seeded record's references are followed exactly as today's
  admitted-set closure does, reading any record by id.

## 5. Freeze side

`madc_cir_freeze` gains one step after `cir_forest_arena_complete`:
`cir_forest_build_seeds` — builds DECLNAMES from the units' decl indexes,
places every entity (§4), computes RECORDABLE. `cir_forest_write` writes the
three segments and the blocked shared tables. The POI stamp is a side vector
on `Program` (`forest_arena_poi`, one origin token per arena slot), set by
every parse-time write-through. `--dump-forest` prints per-unit seed counts
and the always list; the freeze prints a one-line placement summary.

## 6. Bind and restore side

- ONE verdict owner on `CirFrozenForest` answers "declared by a bound unit /
  only by unbound units / by none" for a name, from DECLNAMES against
  `forest_chain_set`. The all-units sweep, the `swept_bound` /
  `swept_system` maps, the compile-group verdict cache and the
  `CirMaterializeFilter` name map go; `forest_name_verdict`,
  `forest_product_permitted`, `declared_system`, `verdict_str` and
  `record_kept` call the owner.
- `materialize_pass` takes its per-kind lists from the bound units' seeds plus
  the always list (bucketed by kind as they are read); recordability from
  RECORDABLE; `forest_recordable_cached` goes (no filter = one slot scan).
  The `exact` mode stays: the adoption probe's one-name filter keeps only
  that name's chain, less than a closure filter over its declaring units.
- Derived types build on first swizzle (`arena_swizzle` becomes a member that
  builds a missing derived record, operand first); pass 1b goes.
- Globals and templates iterate their seeded indices.
- The flush's namespace-binding / deferred-body lists hold only what the
  bound seeds produced.
- `forest_adopt_declared_function` (the FUNC_NAMES probe) is unchanged here;
  its gap is fixed in part 3.
- Oracle during development: an env-gated full-walk arm runs today's path
  beside the seeded one and diffs the restored sets; deleted at the end of
  part 2 (no-parallel-implementations: written deadline). **Deleted
  2026-10-14** (`MADC_FOREST_WHOLE_WALK`); the gate is now the closure-only
  invariant (§8).

## 7. The dialect prelude

The `--std=madc` auto-include fragments (`include/madc/ns_madc`,
`ns_madc_host`, `ns_php`, `ns_perl`, `ns_python`, `ns_ruby`, `ns_js`,
`ns_rust`, `ns_git`, `ns_markdown`, `ns_ui`, `ns_ui_web`, `ns_ui_ws`,
`bits/*`) join the release freeze TU under the `--std=madc` config they are
served in (`scripts/forest_pack_headers.txt`; the share step keeps the
gnu++17 / c++17 labels for C++ consumers). They are then units like any
header: the auto-include binds their slices instead of parsing their text.
Gate: `println("hi")` under `--std=madc` binds the prelude for less than its
78.7M-Ir live parse; strict `--std=c*` / `--std=c++*` compiles never bind
them (the config gate, `serves_config`). This is the whole of Decision
`dialect_headers_internal_indexed`; Gap `dialect_prelude_parsed_per_compile`
closes with it.

## 8. Gates

- **parse-cost ratchet** — rows per §1 (`scripts/parse_cost/` gains the
  ladder workloads; the seam battery's `parse-cost` stage runs them).
- **pack size** — `forest_pack_gate.sh` records the pack's byte size in its
  baseline; growth past the 15,000,000-byte cap (+3,418,024 over the
  recorded 11,581,976; 13,000,000 until format 61) is red.
- **seed parity** — `scripts/check-pack-seed-parity.sh`: for each ladder
  workload and a spread of single-header programs, the restored set (types,
  typedefs, enums, functions, globals, templates, namespace bindings) of the
  seeded bind equals today's full-walk bind, except undecided records whose
  creating unit is outside the closure (§4) — listed, never silent. Runs on
  the release binary while the full-walk arm exists (part 2), then becomes
  the closure-only invariant: no restored entity's declaring or creating unit
  lies outside the bound closure. **As built:** the probe labels every
  restored record by how the bind reached it — LISTED (in a visited seed
  list: a bound unit's, which holds what that unit declares, created or
  required, or the always list) or CARRIED (a reference pull, a
  function-local class by structure, an admitted owner's dependent — §4
  "Pulls") — and the gate fails any record reached neither way
  (`check-pack-seed-parity.sh`, with a negative control). A name only
  unbound units declare is legitimate when listed by requirement (the locale
  facets' `id` statics ride the units requiring their facet) or carried (a
  pulled `__basic_file<char>`'s method bindings); the gate reports their
  count. Measured at the conversion: the ladder rungs restore 71–2,736
  records.
- **existing** — `forest_pack_gate.sh` (load-log losses stay at zero),
  `forest_index_oracle.sh`, `forest_dm_oracle.sh`, the forest `make gates`
  set, `check-system-headers-from-libmadc.sh`, byte-identical suite output
  (the batch lane per batch; the full packed headerless suite at the seam).

## 9. Thread safety

Unchanged contract: the pack is immutable bytes in the mapped image; the
per-process decode cache is the one shared structure and keeps today's
locking; per-bind state (the id map, the per-unit restored lists) lives on
the `Program` / compile group. No new mutable globals.

## 10. Implementation order (four parts, one feature branch, one seam)

1. **Freeze.** POI stamps, DECLNAMES, SEEDS, RECORDABLE, format 58,
   `--dump-forest` per-unit output. Checkpoint: placement summary sane;
   every keep-able entity in a list of each unit that can keep it.
2. **Bind/restore.** The verdict owner, seeded passes, lazy derived types,
   the full-walk oracle (then deleted); then the BLOCKED shared tables. Checkpoint: suite output
   byte-identical (batch lane); the ladder at the §1 ratchets on a symbolized
   release build; the rows recorded. Closed 2026-10-14 with cxx_stl's row at
   its measured Ir (§1, "As closed").
3. **Bodies, provision, probe.** DEFBODY runs decoded on instantiation;
   `cir_forest_decl_entry` gains `record_id` (the name → record column the
   pull-on-lookup step will read; entry version bump); the FUNC_NAMES index
   answers "adoptable". Carried from part 2: a record the bound closure only
   names restores in its declared-only shape (completed when a later bind
   requires it); a node unit decodes by block, not whole; the shared-table
   layout accounts for multi-header binds. Checkpoint: cxx_stl / testsubscript
   timings not worse; live C++ compiles no longer open the pack; cxx_stl's Ir
   row ratcheted down toward ≤ 1,500M.
4. **Prelude.** The dialect fragments frozen as `--std=madc` units; the
   auto-include binds them. Checkpoint: the prelude row < 78.7M Ir; strict
   stds unaffected.

Each part is a few commits at most (Tier 1 + Tier 2 per commit, the static
gate loop, the batch lane per part). The seam is 0.105.0: the full battery
once, after part 4, with the headerless packed suite on Linux and win64, the
on-disk subsets, exe + obj, macOS and aarch64 builds.

## 11. Measurement protocol for every checkpoint

Symbolized -O2 release build with the pack built in (`./configure`, `make
release`, no-op strip — the `tmp/relbuild/head-sym4` recipe), never a
pack-less build with a pack attached by environment. Harness
`tmp/pc/t6/t6.py` (callgrind Ir + 30 interleaved runs, every arm's rc and
stdout checked against live). Record ms and Ir side by side against §1.
