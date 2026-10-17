# The dialect pays per name — finishing pack-slices §7

**Date:** 2026-10-16
**Status:** PLAN — next in the 0.106.0 arc, ahead of section G (owner
2026-10-16: "We need that fix asap").
**Spec:** `docs/plans/2026-10-13-pack-slices-design.md` §7 (settled; owner law
2026-08-10, Decision `dialect_headers_internal_indexed` 2026-10-13, restated
2026-10-14), §8 gates, §11 measurement protocol. Closes Gap
`dialect_prelude_parsed_per_compile`.

## Where it stands

0.105.0 shipped part 4 in part (0527e4e23, 0afb3c97b, 063ace098, 0d9456b9e):
under `--std=madc` a dialect fragment binds its WHOLE frozen unit — no text
parse, no synthetic `#include`, raw blocks; the dialect workload went 77.7M →
42.0M Ir. Two parts of §7 are open:

- **Per name.** Every active fragment restores and registers every
  declaration. Measured on the release recipe (symbolized, `WITH_FOREST=none`)
  for `scripts/parse_cost/dialect.mad`, 43.0M Ir: `flush_forest_pending_globals`
  11.7M (`register_forest_func` 6.4M over 367 functions, `forest_restore_decls`
  4.9M), `CirBuilder::translate_module` 7.1M, `MIR_link` 5.4M. Each dialect
  declaration costs ~100K Ir per dialect compile whether used or not
  (aa67a4fa6, 8 declarations + `<bits/project_enums>`: +0.88M = register
  +0.34M, translate +0.32M, restore +0.10M). Most of the 367 come from
  `queue_fragment_prerequisites` activating `<ns_madc>` for every php program
  (`<ns_php>` mentions `madc::value`).
- **Out of the pack.** The fragments are units in the system-header pack
  (pack list v8), so a dialect change changes the pack.

## Decisions

1. **No new segment.** A name → DECL_NAMES (segment 24, in place) → its
   declaring units → that unit's decl index (`CirFrozenForest::unit_decl_index`,
   cir_freeze.cpp ~2727) and its `record_id` column (format 64, cir_freeze.h
   ~816-828) → records. A unit's decl index is decoded once per Program, the
   first time a lookup touches it, into a per-Program name → record ids map.
   The owner's provision is that the slice format does not change again for
   pull-on-lookup, and `record_id` was added as that column. DECL_NAMES
   already keys functions, types, templates, enumerators (their enum's record)
   and namespaces (no record by design).
2. **The lexer scan keeps ACTIVATING fragments; activation stops restoring
   them.** Which names a program sees is defined by activation today — the
   madc bare-name preference walk (parser.cpp ~31226-31247), host policy
   (`auto_include_permitted`, lexer.cpp ~1880/1895: "Unknown namespace 'php'"),
   the `WEB → ns_ui_web` member row (~1497) that exists to run ns_ui_web's
   `register_host` — so activation stays where it is.
   `forest_bind_dialect_fragment` (lexer.cpp ~4834) keeps: `forest_bind_include`
   (PP events, guards, `peImport` → lazy module import, the chain, the
   `included_files` key), `mark_embedded_include_flag` (`_include_ns_madc` →
   `__madc_sys_init`), the `<string>` decline (`forest_bind_env_ok` ~5039),
   the prerequisite scan. It adds: the unit's namespace scopes from its
   `pdkNamespace` entries, and its ANCHORS — globals with dynamic initializers
   (`register_host` at ns_ui_web ~347 and ns_ui_ws ~96, `__madc_run_child` at
   ns_madc ~447). It stops: seeding `materialize_pass` from the unit's seed
   list, and restoring or registering anything else up front.
3. **One materializer**, `Program::forest_dialect_demand(qualified)`, beside
   `forest_adopt_declared_function` (parser.cpp ~28381): answers only for
   active dialect units, memoizes hits and misses per Program, materializes
   every record of the name (overload sets complete), registers at once.
   Hooks — one per lookup kind, each an EXISTING owner:

   | lookup | owner | site |
   |---|---|---|
   | `ns::m` value / function / template (qualified, using-directive, preference walk) | `activate_forest_function_family`, widened (→ `activate_forest_member`); first-touch at the top of `find_namespace_member` (~30741) and `find_namespace_function_overload` (~23202), so a user `namespace php { … }` still merges with php's overloads | ~28542 |
   | `ns::m` in an expression, ahead of its template and type probes | the same owner | ~43092 |
   | free operator | `activate_forest_function_display` | ~28557 |
   | bare value (namespace-less records) | `lazy_resolve` | ~26333 |
   | bare type (incl. bare twins of namespaced classes) | `lazy_resolve_type` (callers ~4028, 5257, 14719, 81143) | ~26371 |
   | qualified type | `resolve_namespaced_type_token`, at the member read | ~5366 |
   | bare type inside a namespace | `namespace_chain_datatype` | ~12709 |
   | nested enum / class scope | `canonical_nested_namespace`, miss arm | ~30873 |
   | callee of a loaded dialect body | the CIR fixpoint's `activate_forest_func`, symbol → record from a per-Program map over active units, built on the first miss | cir_builder.cpp ~36291 (index code ~36030-36046, extracted) |

4. **Forest side, inside the one owner `materialize_pass`.**
   `CirMaterializeFilter` (cir_freeze.h ~1729) gains `unseeded_units`
   (counted as bound by `name_verdict`, seed lists never visited) and
   `records` (record ids asked for directly, bucketed into Pass 0). Asking by
   name in exact mode cannot admit a namespaced function (`name_verdict`
   answers -1 there), so admission is by record id. A generation that only
   adds records visits only those records and their pulls.
5. **Registration mid-parse.** `flush_forest_pending_globals` (~28580) splits
   into restore-if-grown (~28593-28611) and apply-pending (~28613 on); the
   materializer runs `forest_restore_decls` over the DELTA (a record →
   restored-index map plus the generation's tail; the dedupe map ~27297-27311
   built over the delta only), then apply-pending. A dialect free function
   registers only if THIS Program demanded it (a sibling TU's demand in the
   compile group does not register here). Precedents: `activate_forest_func`
   from `lazy_resolve`, adoption at ~43455.

## Part A — per name (the compile-time win)

Rows measured per §11 (symbolized release). Each task: Tier 1 + Tier 2, the
static gate loop, the dialect differential; the batch lane per batch.

- **A0 — baselines, counters, the differential (no behaviour change).**
  `forest_bind_stats` (lexer.cpp ~4746) gains active dialect units, names
  demanded, records carried, functions registered. New parse-cost workloads:
  `dialect_one.mad` (one php name), `dialect_all.mad` (all 13 fragment heads,
  one name each), `dialect_cxx.mad` (`<vector>` + 20 dialect names).
  `scripts/test_differential.sh OLD NEW [globs]`: every `tests/*.mad` run
  under both binaries through `scripts/test_fixtures.sh`'s `fixture_load`
  (the one fixture reader), stdout / stderr / rc / created files compared —
  the per-task proof that a dialect program's behaviour did not move.
  Expected: dialect.mad shows 367 functions registered; the base binary kept
  as OLD.
- **A1 — every dialect decl entry has a record.** `cir_forest_build_seeds`
  (cir_pack_seeds.cpp ~1058-1149) fills the column (pack rebuild, no format
  change); census line "dialect decl entries without a record (non-namespace):
  N" in `forest_pack_gate.sh`, red while N > 0. Expected 0.
- **A2 — record admission in `materialize_pass` (not wired).** RED unit case
  (tests/unit/test_cir_freeze.cpp): a 2-unit forest materialized with
  `records={f}` restores f and its parameter type only; a records-only second
  generation visits one record; a name an unseeded unit declares gets verdict
  1. Suite unchanged.
- **A3 — completion and rollback read the index (before A4).**
  `ForestDeclNames::for_prefix`; `completion_scope_names`
  (madc_complete.cpp ~1282-1316) and the visible-namespace enumeration
  (~300-430) gain one arm, `forest_dialect_names(scope, prefix, offer)`;
  `forest_bind_mark` / `forest_bind_rewind` (~28003-28038) carry the demand
  log and per-unit maps. RED: `using namespace php;` then bare `strto`
  completes to php's names; an entry that demands `php::count` and fails
  rolls back and a later entry re-demands it. test_repl_session.cpp
  ~2112-2113 stays green.
- **A4 — functions per name.** Activation additions (lexer.cpp ~4834; a
  written `#include <ns_*>` under madc routes to the activation owner,
  ~7838-7858); the filter split, the registration gate, the family and display
  owners, `lazy_resolve`, the flush split, the materializer (parser.cpp); the
  CIR fixpoint (cir_builder.cpp). Types, enums and templates of an active unit
  are still demanded at activation in this task. RED tests (`.expect` from
  OLD): `testdialectpername_family` (overloads of one name, a
  using-directive, a bare call through the preference walk, a user
  `namespace php { int extra(int); }` beside `php::count`);
  `testdialectpername_body` (a dialect inline body calling a sibling inline
  body the program never names; JIT, exe, obj); testgit, testmarkdown,
  testlazymoduledirect; libmadc's `security_policy` case. Expected:
  dialect.mad registers 3 php functions plus anchors; `register_forest_func`
  6.4M → < 0.3M; `forest_restore_decls` 4.9M → ~1M; the row re-recorded
  downward (estimate ≤ ~33M Ir).
- **A5 — types, enums, templates per name.** Hooks at parser.cpp ~30873,
  5366, 12709, 26371, 43092; activation keeps only namespaces and anchors.
  RED `testdialectpername_types`: `madc::project_kind` (declaration,
  enumerator, `switch`), `ui::ui_host_ops` (declarator, `sizeof`, functional
  cast), bare `SysInfo`, an unscoped `madc::fk*`, `php::print_r(x)`,
  `php::print_r<int>(x)`, `php::var_dump(a, b)`. Expected: aa67a4fa6's +0.88M
  ≈ 0 for a program that never names the manifest faces.
- **A6 — demand cost under a large closure.** The `dialect_cxx` row ≈
  `rung_vector` + Σ per-name cost (records-only generations + delta
  registration); recorded.
- **A7 — gates, in `make gates`.** `check-pack-seed-parity.sh` gains the
  `demanded` label: a dialect record restores only as demanded, carried or
  anchor — `listed` fails (selftest: a synthetic listed dialect line is
  flagged; negative control: seeding dialect units fails). Parse-cost rows
  `dialect.{live,forest}`, `println`, `dialect_one`, `dialect_all`; the
  dialect.mad restored-record count held by the parity gate.

The only accepted output delta: the declaration order of dialect entities
registered mid-parse (named in the commit when it shows, e.g. in
`--emit=c11`, which the differential also diffs over the dialect corpus).

## Part B — the dialect out of the system-header pack

- **B1 — a raw dialect image with a role.** `forest_pack.sh --dialect-only`
  → `--dialect-image`, built in every product mode (src/Makefile
  ~1102-1117); every block raw; an optional ROLE segment at id 28 (reserved,
  optional like LEDGER — no format bump). RED: `--dump-forest` shows
  `role=dialect`; dialect.mad decodes 0 zstd frames from it.
- **B2 — discovery and a second forest.** `forest_probe_arm` (lexer.cpp
  ~4272-4330) filters by role (the system bind skips dialect-role profiles;
  the dialect probe takes only them); a dialect twin of the bind directory in
  the compile group under a role key; the dialect chain and active set apart
  from `forest_chain_set` (unit ids are container-local). RED: a strict
  compile never maps the dialect image; `--no-forest-bind` still binds it.
- **B3 — readers carry the record's forest.** FuncDef gains
  `forest_body_src` (as `frozen_src_forest` does for templates), a pending
  global's def forest likewise; the CIR builder's `bind_forest->` readers
  (~36021, 36041, 36218, 36236, 36271 …) and the parser's (~29440, 26957,
  restore / flush) go through one owner, `Program::forest_extern_node(sym)`
  (bind forest, then dialect forest). RED: the A4/A5 tests and testgit /
  testmarkdown on the split binary.
- **B4 — type identity.** A freeze census of dialect records whose pulls reach
  C-header units, held by the pack gate; the dialect forest's swizzle resolves
  such a type by name through the Program (`resolve_named_datadef`) before
  building its own copy. RED `testdialectimage_ctypes`: ns_ui_web's `stderr`
  body with the program including `<stdio.h>` and passing a `FILE *`, both
  orders — never two `struct _IO_FILE`.
- **B5 — the pack drops the dialect.** Pack list v9 without the 14 fragments;
  the seed dialect census (cir_pack_seeds.cpp ~328-359), the per-population
  relayout (cir_freeze.cpp ~1981-2059) and the touch-log thaw (parser.cpp
  ~28601-28610) deleted; the pack gate's leak check becomes "0 dialect units";
  `forest_index_oracle.sh`'s TU back to its own std; `forest_bind_gate`'s
  dialectbind case points at the image. Expected: a smaller pack, strict rows
  ≤ baseline, dialect rows unchanged.
- **B6 — the pay-per-use gate.** `scripts/check-dialect-pay-per-use.sh`:
  a dialect image built from a tmp/ copy of include/madc with 50 unused
  functions and one unused enum added to ns_madc; dialect.mad against both
  images. Red when the Ir delta exceeds 0.1% or the restored-record counts
  differ. In `make gates`.

## Thread safety

The images are immutable mapped bytes; DECL_NAMES and decl-index reads are
const. `materialize_for` mutates the compile group's shared
`CirFrozenForest` under `MadcCompileGroup`'s existing contract (group
mutation single-threaded). The active set, the demand memo, the per-unit name
maps and completion state are per Program. No new globals. F2 locks
`materialize_for` before compiling TUs in parallel.

## Risks, ranked

1. Registering on first touch instead of before the parse: a user declaration
   that collides with a dialect name now registers first, and
   `restored_aggregate_yields_bare_tag` (~27540) can rename a shared class by
   what registered before it. Mitigation: first-touch hooks, the differential,
   `--emit=c11` diffed.
2. A type or template lookup path with no hook (53 direct
   `namespace_datatype_map` readers): loud ("not a member" / unknown type),
   never silent; A5 apart from A4.
3. Re-entrant mid-parse registration of enums and classes
   (`scope_enumerators`, default-argument settle, member-template stubs that
   mutate shared classes ~28739).
4. A dialect body calling a body nobody registered: MIR "import of undefined
   item" (loud); `testdialectpername_body` pins it.
5. Part B type identity: two copies of a C type.
6. Compile-group leaks between sibling TUs; F2 locking.
7. Demand cost under large closures (A6 holds it).

## Gates this plan leaves

The parse-cost dialect rows (re-recorded downward at A4 and A5), the
`dialect_one` / `dialect_all` / `dialect_cxx` rows, the seed-parity
`demanded` label (A7), and `check-dialect-pay-per-use.sh` (B6): adding an
unused declaration to a dialect fragment cannot cost a dialect compile.

## Execution record

- **A0 (2026-10-16).** `scripts/test_differential.sh OLD NEW [GLOB...]`:
  the suite's programs under OLD twice and NEW once through `fixture_load`
  (stdout, stderr, rc); OLD against itself flags a test nondeterministic,
  never a difference. Checked: a copy of one binary against itself — 43
  dialect tests identical; negative control (a wrapper adding one line) —
  every test differs, exit 1; `check-one-fixture-reader.sh` green. Workloads
  `dialect_one`, `dialect_all`, `dialect_cxx` (live + forest). Baseline on
  madc-release at f7e7e5fa3 (Ir; functions registered up front, `--show-stats`
  "forest functions"):

  | row | live | forest | registered |
  |---|---:|---:|---:|
  | dialect | 43,698,552 | 43,689,570 | 335 |
  | dialect_one | 35,578,265 | 35,570,817 | 335 |
  | dialect_all | 51,473,813 | 51,471,727 | 533 |
  | dialect_cxx | 600,001,906 | 287,202,035 | 817 |

  Ruling: no new A0 counters — `--show-stats`' "forest functions N eager"
  is the baseline metric; the names-demanded / records-carried counters land
  with A4, when they first count something. Ruling: `dialect_all` takes the
  seven fragments with plain functions (madc + the six language namespaces);
  git, markdown and ui load modules at run time, which a cost row must not
  depend on. The new rows are NEW (red) in the gate until the re-record
  after A4/A5, with the sqlite3 rows; the dialect rows' +2.4% is not accepted
  into the baseline — part A closes it.
- **A1 (2026-10-17).** N was already 0: 1,291 non-namespace decl entries in
  22 dialect units (13 `ns_*`, 9 `bits/*`), every one with a record; the
  pack-wide record-less entries (10 of 12,386) are all C/C++ header entities
  (function-pointer typedefs, `money_base::pattern`,
  `std::__default_lock_policy`, three enumerators imported by
  using-declarations) — outside part A. Landed: `--dump-forest` renders the
  dialect census (`dialectrecords`, each `dialectunrecorded` entry; the unit
  decided by `embedded_dialect_fragment_p`) and `forest_pack_gate.sh` holds it
  at a hard zero (two selftest legs: one unrecorded entry, the census
  missing). Two corrections to the decisions above, carried into A4:
  - No decl entry is ever `pdkNamespace` (its only producer, `pack_tap_name`,
    is never called with it), so DECL_NAMES holds no namespaces. Ruling: A4
    takes a dialect unit's namespace scopes from what its records already
    say (its namespaced functions' and types' owning namespaces, or its
    namespace-bind records), decided against the code in A4 — never a new
    format column.
  - A namespaced dialect function is indexed by its flat funcdef id
    (`__ns_php_count`; an overload `__ns_php_str_repeat__o2`), not by
    `php::count`; only templates appear qualified. Ruling: A4's lookup asks
    the owner that spells the flat id for `ns::name`, then takes the
    DECL_NAMES range of that id and its `__oN` overloads (`for_prefix`, A3).
- **A2 (2026-10-17).** `CirMaterializeFilter` gains `unseeded_units` (bound
  for `name_verdict`, seed lists never visited) and `records` (asked record
  ids, Pass 0, kept whatever their verdict), with `binds(unit)`; the tagged
  `(kind << 30) | index` decode is one pair, `cir_seed_item_kind` /
  `cir_seed_item_index` (`add_dependents` migrated). `materialize_for` counts
  a new record as growth of its own: a generation over the added records
  only (`materialize_pass(&added)`), whose pulls stop at aggregates an
  earlier generation built whole; a new unseeded unit is a full generation.
  Counters `_stat_mat_generations` / `_stat_mat_visited`. Three doctest cases
  (tests/unit/test_cir_freeze.cpp), watched RED (missing members, then 14
  failed assertions with the fields present but unread): a records filter
  restores one function and its parameter type; a records-only second
  generation visits one record; an unseeded unit's names are bound, its list
  unvisited, its function restored alone when asked by record. Nothing sets
  the fields yet. Rulings:
  - An asked NAME's lookup skips unseeded units' seed lists too — else a
    closure filter naming one dialect name (the adoption probe's bare name)
    re-admits the whole unit.
  - `names_only` (the product-definers check) reads `binds()`: an unseeded
    unit counts as bound there.
  - An exact filter's `name_verdict` still ignores both unit sets, as it
    ignores `bound_units`.
  - Without a seed index a records-only generation is a full pass.
  - The parity probe labels an asked record `listed` (it is bucketed in Pass
    0); A7's `demanded` label separates them.
  - Cost (2026-10-17): the asked-record tests are guarded by an emptiness
    check and `binds()` reads `unseeded_units` only when it is non-empty, so
    a bind that asks no record pays a size read, never a lookup. Symbolized
    packed pair, cf05fd6f5 against cf05fd6f5 + A2: cxx_stl.forest +719K Ir
    of 1,396M, of which the forest phases (flush, restore, materialize) are
    +70K; the rest is MIR / c2mir code A2 does not touch moving between links
    (address-dependent hashing — `pattern_match_p` swings by 7M between
    builds of near-identical code); c_headers.forest −2K.
- **v0.105.0 → cf05fd6f5 (2026-10-17), what part A answers.** Shipped
  binaries, same container (Ir): dialect +1.55% forest, dialect_one +1.85%,
  dialect_cxx +0.64% forest; c_headers +0.07%, cxx_stl +0.18% forest,
  subscript +0.17% forest; the six rungs −0.2% to −2.7%; sqlite3 and
  dialect_all do not compile under v0.105.0. Symbolized (same recipe, both
  packed): the dialect row's +1.08M is `flush_forest_pending_globals` +742K
  (`register_forest_func` +562K, `forest_restore_decls` +175K) and
  `materialize_pass` +128K — the up-front registration of this arc's new
  faces, which A4 removes; C's +545K is the lexer (macro paint, +316K in
  `tokenize`); C++'s +2.97M is `CirBuilder::translate_module` (+3.34M),
  not yet attributed further.
