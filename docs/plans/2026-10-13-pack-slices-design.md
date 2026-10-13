# Pack slices — a bind loads only the units a program includes

**Date:** 2026-10-13
**Status:** DESIGN (owner review) — implementation on Opus 5.5, in its own
focused session.
**Brief (owner-approved 2026-10-13):** one shipped pack, cut per unit; a bind
loads only the slices of the units the program's include closure reaches — no
whole-pack sweep, no whole-arena judging; the dialect prelude is a slice like
any header, loaded under `--std=madc`. Target: the Test 6 column of
`2026-10-13-pack-load-cost-experiments.md` on the shipped binary, at a pack
size ≤ today's + ~1 MB.
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
| pack size (release, Linux) | 11.6 MB | ≤ 12.6 MB | gate |

The ratchets become `scripts/parse_cost/baseline.tsv` rows (recorded with
`parsecost-record` once green, never hand-typed).

## 2. What changes, in one paragraph

Today every arena record, every arena string, every template and every token
run sits in ONE shared table per kind (segments 10–17, 19), and the string
pool for the per-unit token streams in another (2/3/4). A bind decodes all
of them, sweeps every unit's declaration index, and judges all 19k records by
name. After this design, each of those tables is cut per UNIT: a unit's slice
holds exactly the records its own parse created, with its own string table,
its own template records and tokens, its own deferred bodies. Record ids stay
global (each unit owns a contiguous id range). The bind walks the include
closure it already computes (`forest_chain`, post-order) and, per unit,
decodes that unit's slice and materializes it whole — no name judging, no
filter. Everything else in the container (DIR, per-unit tokens / PP exports /
edges / source, globals, FUNC_NAMES, MIR cache, ledger, shared configs) is
unchanged.

## 3. Container layout

`CIR_FOREST_FORMAT_VERSION` 57 → 58. An older pack is refused at
`open_header` (the binary is self-contained; there is no compatibility case).

`CIR_FOREST_SEGS_PER_UNIT` 10 → 24. Per-unit segment ids stay
`CIR_FOREST_SEG_UNIT_BASE + unit * SEGS_PER_UNIT + slot`:

| slot | content | today |
|---|---|---|
| +0 … +9 | records, children, connectors, positions, tokens, decl index, PP exports, edges, branch deps, raw source | unchanged |
| +10 | ARENA_DEFS — this unit's `defrec` run (owned records, then structural copies) | was shared seg 10 |
| +11 | ARENA_PAYLOAD — the member/base/method/param/… runs those records reference | was seg 11 |
| +12 +13 +14 | arena strings (bytes / entries / buckets) for the names and spellings of +10/+11 | was 12/13/14 |
| +15 +16 +17 | TEMPLATES / TEMPLATE_PAYLOAD / TEMPLATE_TOKENS this unit's parse declared or instantiated | was 15/16/17 |
| +18 | ARENA_TOKBYTES — param-default token runs of this unit's records | was seg 19 |
| +19 | DEFBODIES — the DK_DEFBODY token runs (4 runs per body), decoded only when a body is instantiated | was inside seg 11 |
| +20 +21 +22 | token string pool (bytes / entries / buckets) for this unit's token stream (+4) | was shared 2/3/4 |
| +23 | spare |

Shared segments 2/3/4, 10–17, 19 are no longer written. `cir_forest_dir_unit`
gains two words: `arena_base` (the unit's first global record id) and
`arena_count`; the DIR therefore carries the id → unit map (binary search on
`arena_base`, the DIR is already bound in place).

Codec policy is `add_seg`'s today (RECORDS → BYTEPLANE, CHILDREN → DELTA32,
zstd-15 on a release append), plus one rule: a segment under 1 KB raw is
written codec None (zstd's own guidance; most tiny units' slices).

### Record ids

`MADC_TYPEID_PROJECT_BASE` and the pinned ids are unchanged. At freeze the
arena is renumbered: unit 0's owned records get `[PROJECT_BASE, +n0)`, unit
1's the next range, and so on, in unit order; every `ref0` / payload id is
rewritten through the renumbering map. A reference is therefore the same
32-bit id it is today, and "which unit's slice holds id X" is one binary
search over `arena_base`. No per-slice remap at bind, no (unit, slot) pairs.

A record in unit U's slice references only (a) pinned ids, (b) ids of units
in U's include closure — true by construction: U's parse can only name what U
had included — or (c) structural records, which U's slice carries copies of
(next section). The bind asserts it: a reference to an id that no bound slice
has registered is a hard error, not a silent miss (gate, §8).

## 4. Ownership — stamping the origin unit at write-through

The arena gets a parallel side array, `DefArena::origin_file` (one entry per
slot; never part of `defrec`, which stays 54 words). Every write-through
(`Program::forest_arena_record_unary` / `_aggregate` / `_func` / `_fptr` /
`_member_pointer`, `forest_record_enum`) stamps the slot with the file being
parsed at that moment — the same `TokenBase::_parse_file` the decl-index
taps and `cir_freeze_partitioned` already use. At freeze,
`cir_forest_fill_pack_payloads`'s existing file → unit map turns the file
into the unit number; that is the slice the record is written to.

Freeze-time synthetic records (`cir_forest_arena_complete`: TYPEDEF, NSLINK,
NSALIAS, NSBIND, DEFBODY) take the unit of their subject: a typedef its
declaring file, a namespace binding the decl-index entry it was made from, a
DEFBODY the function it belongs to.

Three ownership cases the data (Test 8) showed:

- **Named entities (structs, classes, enums, functions, variables, typedefs,
  templates, instantiations)** — owned by the stamping unit. A derived
  instantiation made during `<map>`'s parse (`_M_emplace_hint_unique`, the
  Test 5 exact-mode failure) is owned by `<map>`'s slice and binds with it.
- **Structural types** (DK_PTR, DK_REF, DK_CONST, DK_FPTR, DK_MEMBERPTR,
  DK_CARRAY, and a DK_FUNC with no name — a function TYPE) — the arena holds
  one record per spelling, minted by whichever unit needed it first; unrelated
  units reference it (`char**`, `_IO_FILE*`: 599 of Test 8's 629 shared
  records). They are not loaded, they are REBUILT: a slice carries a copy of
  every structural record it references whose owner is outside its own
  closure; at bind a structural record goes through the type minters
  (`getPointerType` / `getReferenceType` / `getQualifiedType` /
  `nest_carray_dims` / the fptr and member-pointer minters), which already
  dedupe by pointee; the first slice to register an id wins, later copies hit
  the id map and are skipped. Copies cost ≈ 0.19 MB raw (Test 8).
- **Defined in one unit, forward-declared in a sibling** (`struct _IO_FILE`:
  defined in `bits/types/struct_FILE.h`, declared in `bits/types/FILE.h`;
  15 of 629) — the record is owned by the unit that COMPLETED it; each unit
  that only declares it carries a stub record (same id, same kind, the
  incomplete flag) in its own slice. Whichever binds first registers;
  completion goes through the parser's existing redeclaration path. A program
  that includes only the declaring unit gets the incomplete type, as live.

## 5. Freeze side

In `madc_cir_freeze` (`src/madc_cir.cpp`) after `cir_forest_arena_complete`,
one new step, `cir_forest_slice_arena`: assigns each slot to a unit (origin
file → unit; synthetic kinds as above), computes structural copies and
forward stubs, renumbers ids per §3 and rewrites every reference, and
partitions `DefArena::{defs, payload, strings, tokbytes}` and the template
tables into per-unit `DefArena` instances. `cir_forest_write`'s per-unit loop
then writes slots +10…+22 from the unit's instance through `add_seg`; the
shared-table writes (:705-739, :747-758 of today's `cir_forest_write`) go.
`--dump-forest` prints per-unit record / string / template / body counts and
the id range; `forest_pack_gate.sh --dump` already diffs that output.

The token string pool (2/3/4) is cut the same way: `cir_forest_write` interns
each unit's token stream into a per-unit pool (`TokenBase::_active_strpool`
swapped per unit) and writes +20/+21/+22.

## 6. Bind and restore side

`forest_bind_include` (lexer.cpp) already walks the include closure
post-order and fills `forest_chain`. The restore becomes:

- `Program::forest_restore_decls`: the all-units decl-index sweep
  (parser.cpp:27219-27263) and the `CirMaterializeFilter` /
  `materialize_for` call go. In their place: for each unit in `forest_chain`
  (post-order — includes before includers, so every cross-slice reference is
  already registered when a record needs it), `forest.materialize_unit(u)`.
  The verdict maps `swept_bound` / `swept_system` are still built — from the
  BOUND units' decl indexes only — because the lexer's PP side and the
  registration lambdas read them; the per-group cache
  (`MadcCompileGroup::decl_verdicts`) stays.
- `CirFrozenForest::materialize_unit(u)` replaces `materialize_pass` over the
  whole arena: it decodes slots +10…+14 (one `forest_segment_bytes` each),
  binds the unit's `FrozenDefArena` view, and runs today's passes
  (1 aggregates, 1a enums, 1b derived, 2 fill, 3 typedefs, 3a namespace
  surfaces, 3b enums, free funcs, globals, templates) over that unit's id
  range only. Name judging (`verdict_str` / `name_verdict` / `record_kept`),
  the admitted-set and reference-pull closure, `forest_recordable_cached`'s
  whole-blob scan and the `exact` mode are deleted: everything in a slice is
  wanted by definition. `arena_swizzle` resolves an id through the per-bind id
  map (`_defs_by_tid`), filled slice by slice; a miss is the §3 hard error.
- `restored_nsbinds` / `restored_defbodies` / `restored_nslinks` /
  `restored_nsaliases` become per-unit vectors appended in bind order;
  `flush_forest_pending_globals` walks what the bound units produced (today
  it walks the whole pack's lists: 7.5k lookups for `<stddef.h>`).
- DEFBODY runs (+19) are decoded only inside `parse_deferred_lazy_body` when
  that body is first instantiated — `restored_defbodies` carries (unit, byte
  offset) instead of the deserialized runs.
- The per-process decode cache (`forest_decoded_segment_cached`, keyed by
  blob base + offset) serves slices exactly as it serves segments today; the
  REPL and compile groups re-binding the same unit pay once.
- `forest_adopt_declared_function` (the FUNC_NAMES probe, seg 23) is
  unchanged by this design; its known gap — the index answers "declared"
  where it must answer "adoptable", so every live C++ compile opens the pack
  for `__builtin_memset` — is Gap `adopt_probe_opens_pack_for_nonadoptable_name`,
  fixed in its own commit in the same session (the index records the
  adoptable shape at freeze).

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
  baseline; growth past +1 MB over the recorded size is red.
- **slice closure** — a new static gate on the release pack
  (`scripts/check-pack-slice-closure.sh`): `--dump-forest` lists, per unit,
  every id its slice references; each must be pinned, inside the unit's own
  closure, or a structural copy in the slice. Negative control: a hand-broken
  pack.
- **bind assertion** — a reference to an unregistered id at bind is a hard
  error with the unit and id named (never a silent drop).
- **existing** — `forest_pack_gate.sh` (load-log losses stay at zero),
  `forest_index_oracle.sh`, `forest_dm_oracle.sh`, the forest `make gates`
  set, `check-system-headers-from-libmadc.sh`, byte-identical suite output
  (the batch lane per batch; the full packed headerless suite at the seam).

## 9. Thread safety

Unchanged contract: slices are immutable bytes in the mapped image; the
per-process decode cache is the one shared structure and keeps today's
locking; per-bind state (the id map, the per-unit restored lists) lives on
the `Program` / compile group. No new mutable globals.

## 10. Implementation order (four parts, one feature branch, one seam)

1. **Freeze.** Origin stamping, `cir_forest_slice_arena`, per-unit slots
   written, shared tables gone, format 58, `--dump-forest` per-unit output,
   the slice-closure gate. Checkpoint: the gate green on the release pack;
   pack size within +1 MB; every record in exactly one owned slice.
2. **Bind/restore.** `materialize_unit`, the sweep and filter removed,
   per-unit restored lists, the bind assertion. Checkpoint: suite output
   byte-identical (batch lane); the ladder at the §1 ratchets on a symbolized
   release build; the rows recorded.
3. **Bodies, provision, probe.** DEFBODY runs decoded on instantiation;
   `cir_forest_decl_entry` gains `record_id` (the name → record column the
   pull-on-lookup step will read; entry version bump); the FUNC_NAMES index
   answers "adoptable". Checkpoint: cxx_stl / testsubscript timings not
   worse; live C++ compiles no longer open the pack.
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
