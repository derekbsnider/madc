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
- **A3 (2026-10-17).** `ForestDeclNames::for_prefix(stem, begin, end)`: a
  binary search to the run's start, a walk while the stem matches; the run
  is read with `name_at(i)` / `units_at(i, n)`, bounds-checked readers
  defined in the class body (`units_of` keeps its own probe — see Cost
  below). `CirFrozenForest::decl_entry_scope(kind, record,
  ns, display)`: a decl entry's namespace from its record's own ns field
  (arena / template / global) and a free function's display name; false for
  a member (a function entry whose record is its owner class, a template
  record with an owner). The owner, `Program::forest_dialect_names(scope,
  prefix, offer)` (parser.cpp, beside the bind mark — A4's demand reads the
  same units): active units = `forest_chain` ∩ `embedded_dialect_fragment_p`
  (`forest_active_dialect_units`); two DECL_NAMES runs —
  `namespace_function_symbol(scope, prefix)` (flat ids and their `__oN`;
  at the global scope the bare names), kept when the entry's record names
  the scope, and `scope::prefix` (templates, types, objects, enumerators),
  direct members only. A unit's decl index decodes once per Program into
  `forest_unit_names` (name → kind + record). Completion's arms:
  `completion_scope_names` and `visit_top_level_names` (each visible
  namespace; its names visit as `dialect_word`); `visit_top_level_names`
  takes the word, which narrows the index run. Demand state's home:
  `Program::ForestDialectDemands` (memo name → hit, log of name + records;
  `record` refuses a second demand of one name), its mark in
  `ForestBindMark::dialect_demands`, rewound by `forest_bind_rewind`;
  nothing writes it yet. `forest_unit_names` holds no registration state
  and stays across a rewind. Thread contract: all of it is per Program, on
  the Program's thread; the pack read is the bind carrier's immutable bytes;
  no globals. Tests watched RED with the declarations stubbed: for_prefix
  (10 failed assertions), the owner over a session bound to a pack frozen
  in-process (forest_bind_gate's dialectbind recipe — the dev unit binaries
  carry no pack) failing at its bound-unit check, the rewind (the third
  demand kept, log size 3). The bare `using namespace php; strto`
  completion passed before the arms (registration answers it today) — an A4
  guard, as are the bound session's `php::strto` / `strto` checks; the
  owner's php names equal what the bind registers into php (53). Rulings:
  - The leaf is a free function's display name (its record carries
    function_display_name); `strip_overload_suffix` only for a record
    without one. The display name is the source's spelling where the flat id
    is not (an operator's mnemonics, an instance's `__i…` suffix).
  - A qualified spelling's qualifier is its scope, the record not consulted:
    a scoped enum's enumerators share their enum's record, whose namespace
    is the enclosing one.
  - The global scope's run keeps a name only when its record's namespace is
    the global one: a bare twin of a namespaced type (`SysInfo`, record ns
    `madc`) is not offered bare — today's `type_reachable` answer. ns_php's
    `extern "C"` shims (`__php_*`) are global names, offered only to a
    reserved-shaped word.
  - The parameter is `stem`, not `prefix`: check-one-path-within's regex
    reads `strncmp(…, prefix, …)` as a directory-containment test.
  - Cost (2026-10-17), symbolized packed builds against A2 (b2ada368d):
    the first cut put `units_of`'s probe on out-of-line `name_at` /
    `units_at` — an exported member of the -fPIC libmadc is interposable
    and gcc did not inline it (cxx_stl.forest +3.48M Ir, all in
    `forest_restore_decls` / `materialize_pass` through `name_verdict`);
    inlined in the class body it still cost +2.2M (~107 Ir per lookup over
    20,079 lookups); `units_of` keeps its own probe and the readers serve
    `for_prefix` alone. Final: cxx_stl.forest +44K of 1,396M (forest phases
    −25K), c_headers.forest +13K, dialect.forest +23K against cf05fd6f5.
- **A4 (2026-10-17).** Functions per name. What existed: verdict-0 (derived)
  free functions already deferred — `forest_deferred_funcs` /
  `forest_deferred_func_families`, promoted through `register_forest_func`
  (the one registration owner) by `activate_forest_func` (`lazy_resolve`, the
  CIR fixpoint) and the family / display owners. A1 indexes a dialect
  function by its flat id, so every free function of a bound fragment was
  verdict 1 and registered up front: dialect.mad's 335 (ns_madc through
  `queue_fragment_prerequisites`), which `forest_restore_decls` walked and
  `register_forest_func` registered — A0's 4.9M and 6.4M. Landed:
  - Activation: `forest_activate_dialect_unit`, called by the one bind walk
    (`forest_bind_include`) for a fragment unit under `--std=madc` — direct,
    prerequisite, child or written `#include <ns_*>`; nothing activates under
    `--forest-touch-log` (the layout pass reads units whole).
    `forest_dialect_activation` (once per Program) reads the unit's decl
    entries' kinds and records — never an entry's name — through
    `CirFrozenForest::decl_entry_scope`, which gains the record's
    registration key: `funcs` (key, record) strcmp-sorted, `withheld` (those
    records), `namespaces` (made scopes at the restore).
  - Restore (`forest_restore_decls`): an active unit binds and is seeded as
    before; `CirMaterializeFilter::withheld` (new) keeps its free functions
    out of every generation (`materialize_pass`' bucket) unless asked by
    record, and registration skips a withheld record this Program did not ask
    (`ForestDialectDemands::asked`) — a sibling TU's demand restores into the
    shared forest, never registers here. The filter is kept
    (`forest_restore_filter`) for delta restores.
  - The materializer `forest_dialect_demand(ns, name)` (parser.cpp, beside
    `forest_adopt_declared_function`): the run of
    `namespace_function_symbol(ns, name)` and its `__oN` in each active
    unit's `funcs`, an entry kept when its record names `ns` and the display
    name; memoized hits and misses; one records-only generation
    (`materialize_for` with `records`); `forest_restore_decls(forest, &delta)`
    over what the generation restored plus the asked functions an earlier
    generation restored (`CirFrozenForest::asked_restored_func`, else the
    restored list), staged in record order; `forest_apply_pending(&delta)`.
    The flush is `forest_restore_if_grown` + `forest_apply_pending(NULL)`;
    a delta apply walks the namespace-surface and default lists from the
    generation's start.
  - Hooks, each an existing owner: `activate_forest_member` (the family owner
    renamed and widened) at the top of `find_namespace_member` and
    `find_namespace_function_overload`, at the expression `ns::m` site ahead
    of its template / type probes, and at a namespaced declaration
    (`parseDeclaration`: the dialect's set registers before a program's own
    overload); `activate_forest_function_display` (every dialect namespace
    and the global scope); `lazy_resolve` (the global scope); `using namespace`
    (`forest_dialect_demand_namespace`, the namespace's names through
    `forest_dialect_names`); the CIR fixpoint (`register_loaded_refs`: every
    callee and address / initializer reference of a loaded body or frozen
    global is demanded before the registry judgements, and new
    registrations join `forest_lazy` / `forest_funcdef_syms` at once and at
    each round's top; tsubst's `emittable` asks too).
  - `--show-stats`: "forest dialect ...... N units; N names demanded (N hit),
    N records, N functions registered".
  Rulings:
  - Withheld, not unseeded: an unseeded unit's restore needs its other
    records asked, and an asked record bypasses every verdict — the seed
    lists carry records other units declare (ns_ui's list holds
    `ui_ws::host`, verdict −1 without <ns_ui_ws>), and an asked anchor's
    initializer loaded node unit 0 (402,678 records). Seeded, the rest of
    the unit restores under its verdicts exactly as before.
  - The demand reads the record's own key, not DECL_NAMES + the unit's
    decoded decl index: DECL_NAMES gives a name's units, not its records, and
    decoding a unit's entry names fills cold container-pool blocks (13 fills,
    2.2M Ir on dialect.mad; `forest_unit_decl_names` 3.0M for six units),
    while a global-scope prefix run walked 2,370 entries. Checked against
    the replaced lookup on 4,267 demands over the dialect corpus (974 hits,
    overload sets up to 5): identical.
  - Symbol → name is a DECODE, not a map over records: a loaded body calls a
    bodied dialect function by its record's emit symbol, but a declaration-
    only one by the Itanium alias registration computes
    (`namespace_cpp_function_symbol`), which no record carries.
    `itanium_function_name` (madc_mangle.cpp, the encoders' decoder twin)
    reads a namespace-scope function's own symbol back to scope + name —
    false for every other shape (template-id, substitution, member, special);
    a non-Itanium symbol is a global-scope name.
  - `register_forest_func` makes its program-scope Variable without
    `addVariable`'s reads of the parse state (namespace binding, `_inst_depth`,
    `parsing_extern_decl`): identical at the flush, correct mid-parse — for
    the verdict-0 activations too.
  - Re-entrancy: `forest_apply_pending` takes the staging lists off before
    applying (a default / constructor-argument re-parse may demand), and
    under dialect demand `rebuild_forest_param_defaults` copies its record.
    A demand made after a unit bound runs the whole flush first.
  - A DK_DEFBODY whose function a demand registers (none in today's
    fragments): the delta apply walks the deferred bodies whole when a
    staged function's body is one.
  - A rolled-back entry rewinds the active units and the registered list with
    the bind mark; `asked` is rebuilt from the kept demands.
  Thread contract: all of it per Program on its thread; the forest's
  `materialize_for` under `MadcCompileGroup`'s single-threaded group
  mutation; the activation keys are the bound forest's immutable bytes; the
  decoder is pure; no globals.
  Tests (`.expect` from OLD, v0.105.0's head_pack): `testdialectpername_family`
  (overloads of one name, a using-directive, a bare call through the
  preference walk, a user `namespace php { int extra(int); }` beside
  `php::count`) and `testdialectpername_body` (a dialect inline body calling a
  sibling the program never names; JIT, exe, obj). Watched RED with the
  namespaced-declaration and loaded-body demands off: "use of undeclared
  identifier 'register_host'" (family, line 34) and eleven "undeclared
  identifier _ZN6ui_web4openEPKcS1_Pv" → "cir_compile failed" (body).
  test_repl_session: a bound compile registers only what it names (no eager
  function, `php::count`'s family and its `__php_*` shims present,
  `php::trim` absent, 8 registered) and a rolled-back entry re-demands
  `php::count` — watched RED before the code; the pack path is unique per
  case (the bound carrier image is cached per path). test_mangle: the
  decoder's shapes, round-tripped through the encoders.
  Cost (2026-10-17), symbolized packed builds against A3 (0172efd76), Ir:

  | row | A3 | A4 | registered (A0 → A4) |
  |---|---:|---:|---:|
  | dialect | 43,305,443 | 32,781,345 | 335 → 8 |
  | dialect_one | 35,570,817 (A0) | 24,511,883 | 335 → 4 |
  | dialect_all | 51,471,727 (A0) | 36,678,393 | 533 → 19 |
  | dialect_cxx | 287,202,035 (A0) | 272,267,487 | 817 → 66 (+284 C++ eager) |
  | c_headers | 245,118,024 | 245,123,975 | — |
  | cxx_stl | 1,396,609,999 | 1,395,176,711 | — |

  dialect: `register_forest_func` 6,426,518 → 137,608;
  `flush_forest_pending_globals` 11,848,948 → 3,470,210;
  `forest_restore_decls` 4,994,537 → 3,043,980 (`materialize_for` 1.13M, the
  six activations 0.98M — decl-index segments 0.29M, `decl_entry_scope`
  0.41M); `materialize_pass` 3,643,031 → 1,137,482. cxx_stl: −1.43M, of which
  `register_forest_func` −1.04M (the context-free Variable);
  `forest_restore_decls` +313K is `name_verdict` +264K over the same 15,688
  calls, inside `__strcmp_avx2` under `units_of` (+270K over the same 257,829
  calls) — no call added. The first cut (whole seed lists asked, each unit's
  entry names decoded) measured dialect 40.19M, its activation 3.87M.
  Validation: fix lane GREEN (Tier 1 69/69, EXE/OBJ 30/30; Tier 2's seven
  lanes at baseline); test_repl_session 81/81, test_cir_freeze 44/44,
  test_mangle 88/88, test_libmadc_program `security_policy` 2/2;
  forest_pack_gate OK. Differential OLD (v0.105.0 head_pack) against A4
  packed: 88/88 identical over the plan's globs with `testfragment*` and
  `testui*`; the whole suite 2,082 of 2,084 identical, `testfreezerun`
  nondeterministic under OLD, `testgccvectorcmpresult` a SIGSEGV in the
  JIT-run program 5 runs in 10 under OLD and A4 alike (0 in 10 on the -O0
  binary, 0 in 5 without ASLR). `--emit=c11` identical on 69 dialect programs.
- **A5 (2026-10-17).** Types, enums, templates and storage-less objects per
  name. What existed after A4: an active unit's types, enums, enumerator
  constants, extern references and owner-less templates restored and
  registered at the bind — dialect.mad: 11 enums, `madc::SysInfo` and
  `madc::channel` (with channel's 32 method imports), 68 enumerator
  constants, `madc::sys`, four templates (two of them `php::print_r` /
  `php::var_dump`). Landed:
  - `CirFrozenForest::decl_record_facts(kind, record, CirDeclRecordFacts &)`:
    a decl entry's record read once — shape (free function, type, template,
    object), namespace, name, leaf (a namespace-scope template's key past
    `ns::`), enum flag, a global's flags, an enumerator constant's enum
    record. `decl_entry_scope` is an inline reader over it.
  - Activation: beside `funcs`, `objects` — each type, owner-less template
    and storage-less global (an enumerator constant, an extern reference) as
    (leaf, namespace, record, group), sorted by leaf; `groups` (an enum's
    record → its constants); `withheld` = the funcs' and objects' records.
  - Filter: `CirMaterializeFilter::withheld` holds a record of any kind out of
    a generation's candidates (the aggregate / typedef / enum buckets, the
    global and template seeds and lists) unless asked; a withheld type an
    admitted record's chain pulls still restores. `demand_restored(record,
    idx)` replaces `asked_restored_func`: an asked or withheld record's index
    in its restored list, every kind; `CirRestoredType` / `Global` /
    `Template` carry `record`.
  - Restore: a withheld global or template registers only when this Program
    asked it (`withheld_here`, A4's function gate generalized); a restored
    type registers (a pulled one included). `ForestRestoreDelta::rewalk`: an
    asked record an earlier generation restored and this Program never
    registered (a rolled-back entry, a sibling TU's demand) re-walks the
    type, global and template lists from 0 under their registered flags;
    `delta.objects` counts the asked non-function registrations.
  - Demand: `forest_dialect_object_records(ns, name)` — objects whose leaf is
    the name in that namespace; at the global scope also a type whose unit's
    DECL_NAMES carries the bare name (the bare twin, `SysInfo`); an enum and
    its constants expand as one group. `forest_dialect_ask` appends them to
    the function records — one materializer.
  - Hooks: `resolve_namespaced_type_token` (the member read, ahead of the
    namespace's type map), `namespace_chain_datatype` (each enclosing dialect
    namespace), `lazy_resolve_type` (global scope), `canonical_nested_namespace`
    (miss arm: `madc::project_kind::gui`'s qualifier); the expression `ns::m`
    site through A4's `activate_forest_member` (enumerator constants,
    `madc::sys`, `php::print_r<int>`); the CIR fixpoint
    (`translate_module`): a loaded dialect body's struct tags not in
    `struct_map` are demanded before the body emits (`ui_web`'s `host *h =
    (host *)arg;`), and Pass 1.97 defines them.
  - `--show-stats`: "N functions + N other declarations registered"
    (`ForestBindStats::dialect_objects`, carried by the bind mark).
  Rulings:
  - An anchor is a global WITH storage; it restores at the bind: a dynamic
    initializer runs before main (`__madc_run_child`, the ui hosts'
    `register_host` flags) and a loaded body reaches storage by symbol, while
    `translate_module` emits frozen global definitions from the bind's list.
    Storage-less globals restore by name. A loaded body naming a withheld
    extern reference by symbol fails loudly at c2mir; no fragment body names
    `madc::sys` today.
  - A sibling-renamed aggregate (ns_ui_ws's `ui_ws__host`, renamed at freeze
    by the sibling-namespace rule) carries no namespace and no decl entry
    names its record: no activation object, restored at the bind as before;
    `ui_ws::host` resolves through its namespace typedef record, an object.
  - The registration gate covers functions, globals and templates, never
    types: a pulled type registers with what pulled it.
  Thread contract: unchanged from A4 — per Program on its thread; the
  forest's `_withheld_sorted` / `_demand_restored` change only inside
  `materialize_for` under `MadcCompileGroup`'s single-threaded group
  mutation; the activation reads the bound forest's immutable bytes; no
  globals.
  Tests: `testdialectpername_types` (`.expect` from OLD; JIT, exe and obj
  identical) — RED with the five type hooks off and withholding on: "error:
  'project_kind' is not a member of namespace 'madc'" (line 13), "2 parse
  error(s) — compilation refused". test_repl_session: types, enums,
  templates and objects register as named; a rolled-back entry's type
  registers again. RED on A4: "no member named 'dialect_objects'", then 9
  failed assertions with that check removed (project_kind, file_kind,
  SysInfo, channel, fkMC11, sys and `php::print_r` registered at the bind;
  file_kind after the project_kind entry, channel after the `madc::sys`
  entry); the rollback case a guard (green on A4).
  Cost (2026-10-17), symbolized packed builds in one clone (tmp/cost_sweep2),
  A4 (5e1451416) against A5, Ir:

  | row | A4 | A5 | delta |
  |---|---:|---:|---:|
  | dialect | 32,104,537 | 29,483,964 | −2,620,573 |
  | dialect_one | 23,983,182 | 21,353,166 | −2,630,016 |
  | dialect_all | 36,065,257 | 33,390,263 | −2,674,994 |
  | dialect_cxx | 270,201,290 | 268,923,078 | −1,278,212 |
  | c_headers | 242,716,383 | 242,687,950 | −28,433 (−0.012%) |
  | cxx_stl | 1,388,504,035 | 1,388,716,573 | +212,538 (+0.015%) |

  dialect: `flush_forest_pending_globals` 3,442,360 → 1,817,063;
  `forest_restore_decls` 3,027,539 → 1,811,677; `materialize_pass`
  1,129,955 → 448,392; `register_forest_func` 132,759 → 44,966; MIR_gen
  −309K and c2mir −104K (SysInfo and its two destructors no longer
  emitted). What dialect.mad's bind restores: `__madc_va_list_tag`,
  `__madc_run_child`'s frozen definition and two C++ builtin templates — no
  enum, class, constant or extern reference; aa67a4fa6's manifest faces
  restore nothing (bits/project_enums: one withheld record; ns_madc's
  faces: unnamed functions). cxx_stl: the forest phases +291K —
  `materialize_pass`' own code −416K, `FrozenDefArena::get_def_at` +814K
  (gcc no longer inlines it into one `materialize_pass` lambda: 8,839
  out-of-line calls, that lambda's call count); a bind without dialect units
  adds no call (`_withheld_sorted` empty, `note_demand_restored` returns at
  its first test). The A4 table above was measured in another clone (the
  same-clone A4 dialect row is 32.10M, not 32.78M).
  Validation: test_repl_session 83/83, test_cir_freeze 44/44, test_mangle
  88/88, test_libmadc_program `security_policy` 2/2; fix lane GREEN (dev
  binary: Tier 1 70/70, EXE/OBJ 31/31; Tier 2's seven lanes at baseline);
  the plan's globs on the packed binary 106/106, EXE/OBJ 65/65;
  forest_pack_gate OK; the static gate loop 0 red of 103. Differential OLD
  (cf05fd6f5 head_pack) against A5 packed: 63/63 identical over the plan's
  globs with `testfragment*` and `testui*`; the whole suite (both binaries
  beside the same module libraries) 2,083 of 2,085 identical, 0 differ,
  `testfreezerun` and `testgccvectorcmpresult` nondeterministic under OLD.
  `--emit=c11` over those 63: 31 identical, 32 differ by removed
  lines only — `struct SysInfo`, its D1/D2 destructors and their
  `madarray_destruct` prototype; in testfragmentinit also ns_ui_ws's unused
  `typedef struct ui_ws__host host;` and `struct dialog_wait` emitted later
  (the accepted declaration-order delta).
- **A6 (2026-10-17).** Demand cost under a large closure. Symbolized packed
  build of 67c270415 (A5) in tmp/cost_sweep2, forest rows, Ir. Controls
  beside the workloads (tmp/, not gate rows): `cxx_none` — dialect_cxx with
  its twenty dialect calls removed; `cxx_one` / `cxx_php10` — cxx_none plus
  one / ten php names; `small20` — dialect_cxx without `<vector>` and its
  vector; `small_var` — small20 with its dialect calls removed; `small_none`
  — dialect_one without its php call.

  | row | Ir |
  |---|---:|
  | rung_vector | 74,805,426 |
  | cxx_none | 230,616,027 |
  | cxx_one | 240,175,280 |
  | cxx_php10 | 251,660,645 |
  | dialect_cxx | 269,213,479 |
  | small_none | 11,731,176 |
  | small_var | 12,659,442 |
  | dialect_one | 21,341,566 |
  | small20 | 48,499,818 |

  - dialect_cxx = rung_vector 74.8M + the C++ body 155.8M
    (`std::vector<int>` from an initializer list, `size()`, its destructor:
    parse +57.3M, `translate_module` +67.3M, `load_and_link` +23.9M of which
    `MIR_gen` +20.2M) + the twenty dialect calls 38.6M (19 names in six
    fragments, 11 units active, 40 demands registering 66 functions).
  - The same twenty calls with no closure (small20 − small_var): 35.8M. The
    large closure adds 2.76M (+7.7%). One php name with its fragment's
    activation: 9.56M (cxx_one − cxx_none) against 9.61M (dialect_one −
    small_none). Each further php name: 1.28M against 1.21M — the name's
    dialect body translated, compiled and linked, and its `println`
    statement; backend work, not the demand.
  - The demand (`forest_dialect_demand`, inclusive), dialect_cxx against
    small20: 2.87M against 2.09M. Per registering demand the same: the
    records-only generations (`materialize_for`, 40 calls) 636K against
    630K, the delta `forest_restore_decls` 186K against 184K; the delta
    `forest_apply_pending` 686K against 525K. The rest is misses: 104 names
    asked against 52, 40 hits in each — names of the C++ program reaching
    the hooks (`activate_forest_member` 580 calls against 199,
    `lazy_resolve_type` 230 against 80, `canonical_nested_namespace` 89
    against 0); `forest_dialect_ask` 1.22M against 0.65M, ~11K Ir per miss
    (the memo, then each active unit's `funcs` and `objects` searched).
  - Outside the demand: the bind's withheld probe over the larger closure —
    `std::binary_search` 6,570 calls, 651K, against 765 calls, 75K (an
    out-of-line instance of the -fPIC libmadc, called through the PLT); MIR
    codegen of a larger `main` (`reg_alloc`, `process_inlines`);
    `translate_module` +0.90M; the forest's teardown +0.94M.
  - Verdict: the per-name cost under the large closure is in proportion to
    dialect_one's; no task follows from A6. The two closure-dependent costs,
    should a later row need them: a miss is searched once per active unit
    for every C++ name the hooks see, and the withheld probe pays a PLT call
    per candidate.
- **A7 (2026-10-17).** The gates.
  - The probe (`MADC_SEED_PARITY_PROBE=1`, `materialize_pass`): its label
    owner (`par_label` / `par_label_idx`) answers DEMANDED for a record the
    generation was asked for (`asked_slots` / `asked_tmpl` / `asked_glob` —
    `CirMaterializeFilter::records`, or a records-only generation's added
    records), ahead of listed and carried; the printer gives a record a
    dialect fragment's decl index names (`embedded_dialect_fragment_p` over
    every unit, `decl_record_facts`) a role before its label: `anchor` or
    `dialect`. `CirDeclRecordFacts::anchor()` (a global with storage, A5's
    ruling) is the one predicate; the activation reads it too.
  - `check-pack-seed-parity.sh`: a `dialect listed` line fails; a workload's
    `<name>.restored` file caps its restored count (red above it, a hint to
    lower it below it) — `scripts/parse_cost/dialect.restored` = 12. Controls:
    the synthetic block flags exactly one unreached line and one listed
    dialect line; a one-name php program compiled with the withholding off
    must show `dialect listed` lines. RED: the A5 release failed the
    withholding-off control (its probe marks no record); the gate with its
    reader blinded failed the synthetic leg; a cap of 11 failed. GREEN on the
    new release: withholding off restores 455 records, 419 `dialect listed`;
    dialect.mad restores 12 — `php::count`, `php::explode`, `php::strtoupper`
    and its `__o2`, their three `__php_*` shims, `__ns_madc_serve_run_child`
    (demanded); `madc::__madc_run_child` (anchor, listed);
    `__madc_va_list_tag`, `__make_integer_seq`, `__type_pack_element`
    (listed, not dialect). dialect_one 8 (4 demanded), dialect_all 23 (19),
    dialect_cxx 2,556 (66; rung_vector 2,489).
  - Parse cost: `sqlite3.modes` = `forest` (owner 2026-10-17: its live row
    costs the same and doubles the gate's time); `<name>.flags` with
    `{scratch}`; a symlinked source missing on the host is RED, never
    skipped. No `println` workload exists; none added. Re-recorded on the
    packed release of this tree (no `--accept-increase`; check mode then 0
    red of 21):

    | row | baseline 2026-10-15 | 2026-10-17 | delta |
    |---|---:|---:|---:|
    | cxx_stl.live | 2,686,766,558 | 2,664,202,829 | −0.84% |
    | cxx_stl.forest | 1,394,786,846 | 1,392,069,287 | −0.19% |
    | c_headers.live | 244,938,187 | 243,120,917 | −0.74% |
    | c_headers.forest | 245,040,160 | 243,214,986 | −0.74% |
    | sqlite3.forest | new | 18,764,301,397 | |
    | dialect.live | 42,662,429 | 29,958,254 | −29.78% |
    | dialect.forest | 42,661,783 | 29,953,917 | −29.79% |
    | dialect_all.live | new (A0 51,473,813) | 33,821,789 | |
    | dialect_all.forest | new (A0 51,471,727) | 33,822,568 | |
    | dialect_cxx.live | new (A0 600,001,906) | 575,827,557 | |
    | dialect_cxx.forest | new (A0 287,202,035) | 269,886,167 | |
    | dialect_one.live | new (A0 35,578,265) | 21,785,232 | |
    | dialect_one.forest | new (A0 35,570,817) | 21,765,224 | |
    | rung_c8.forest | 132,744,889 | 130,934,223 | −1.36% |
    | rung_limits.forest | 23,349,340 | 23,007,047 | −1.47% |
    | rung_stddef.forest | 12,727,028 | 12,447,593 | −2.20% |
    | rung_stdio.forest | 19,757,493 | 19,421,983 | −1.70% |
    | rung_type_traits.forest | 28,368,823 | 27,989,683 | −1.34% |
    | rung_vector.forest | 75,687,073 | 75,108,530 | −0.76% |
    | subscript.live | 3,459,050,884 | 3,434,289,652 | −0.72% |
    | subscript.forest | 2,194,396,538 | 2,191,342,923 | −0.14% |

  Rulings:
  - The negative control is the withholding OFF, not dialect units seeded
    (A4: they stay seeded). `--forest-touch-log` turns it off — the pack's
    layout pass activates no unit (`forest_activate_dialect_unit`) — so no
    new switch.
  - A dialect record is any record a dialect fragment's decl index names,
    read apart from the activation, so a whole-unit restore shows; the rule
    is checked against those records, not against the withheld set.
  - Both gates read the packed release: the seed-parity gate runs in the
    per-commit static gate loop and the seam's static-gates stage (as
    `check-system-headers-from-libmadc.sh` does), the parse-cost gate in the
    seam's `parse-cost` stage. `make gates` builds the dev binary, which
    carries no pack, and names neither.
  Thread contract: the probe's role map is a local of the print, under the
  probe only; no state, no globals.
