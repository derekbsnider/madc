# Cold JIT Startup — tinycc parity for C-shaped programs

**Directive (owner, 2026-08-22, verbatim):** "continue to optimize madc...
use this examples/adventure as a test case, and I'd like you to pursue
decreasing the cold JIT startup time PROPERLY." Refined same day: adventure
uses no C++ features, so the bar is **full parity with tinycc** for
C-shaped code — not merely beating Python.

**The bar (measured, container):** `tcc` compiles + links + runs the FULL
original open-adventure — 18,116 lines of C including the generated
dungeon.c — in **~22ms** (`tcc -DVERSION=... main.c init.c actions.c
score.c misc.c saveresume.c dungeon.c -ledit -o /tmp/advent_tcc` + a run).
Python's port starts in ~100ms.

## Session 117 results (2026-08-22, all landed on
feature/track7-hub-projections-claude)

| lane | before | after | factor |
|------|--------|-------|--------|
| packed (`bin/madc-release advent.cc.json`) | 760 ms | ~210–225 ms | ~3.5× |
| dev (`bin/madc advent.cc.json`) | 1910 ms | ~292 ms | ~6.5× |

The five commits, in causal order:

1. **@59b3f7a3 `--show-stats` reaches the project + frozen lanes** — the
   instrumentation everything below was found with. Per-TU front-end
   walls + shared link/gen/exec phases; printed BEFORE the entry call so
   a program that `exit()`s still yields the compile-side report.
2. **@f1fc27d5 auto-include position gates** — THE dominant cost.
   `G.player.set(...)`/`ui::set(...)` pulled `<set>`, `madc::getline(...)`
   pulled `<string>` into 8 of 11 TUs (~1MB libstdc++ each): 776K tokens
   lexed for a 4.4K-line program. An identifier after `.`/`->`, or after
   a `::` whose qualifier is not `std`, never matches the std surface.
   776K → 62.5K tokens; front-end 1.71s → 0.18s; dev 1.91s → 0.355s.
   Gate: `scripts/check-autoinclude-position.sh` (fulltest, two-sided).
3. **@f848c0fe template payload/tokens as process-shared spans** — the
   segments bypassed the S1 decoded-segment cache and were decoded +
   copied per TU forest. Now spans into the image/cache (one decode per
   process, zero per-forest copies). Packed forest line 96→79ms.
4. **@ff6de290 lazy first-call codegen at link** — `MIR_set_gen_interface`
   is EAGER (91% of the project link wall was MIR_gen of all 271 funcs
   before main). The project lane now links with
   `MIR_set_lazy_gen_interface`; entry + TU inits stay explicitly gen'd;
   `-g` stays eager (debug registration reads machine code). Link
   58→11ms; packed 265→208ms.
5. **@2581d1eb recordability fixpoint once per blob per process** —
   S1-doctrine cache of materialize's pass 0. Structurally right; NO
   measurable packed win (the -O2 fixpoint share is small — the -O0
   profile overweighted std::map/set machinery, as the banked profiling
   law predicts).

Validation at every step: adventure parity **3 fragments + 94 whole logs
byte-identical** (rerun after each slice), targeted subsets green (29/29
auto-include+value, 15/15 + 8/8 freeze/frozen/project, 35/35 + 22/22
packed C++ template surface), new gate green with negative control.
**Owed at the merge wave:** one full battery (JIT + packed + exe/obj +
units) per testing-fulltest.md.

## Where the remaining ~210ms lives (packed, --show-stats phase lines)

| phase | ms | notes |
|-------|----|-------|
| forest | 79 | first TU ~27 (S1 miss: 14-frame 11.6MB intern-spine decode); 3 stdio-binding TUs ~14 each (declidx sweep 3 + materialize 7 + register 8); 7 lean TUs ~2 each (open rebind) |
| lex | 32 | 53K tokens — includes per-TU lean-prelude live parse |
| cir | 29 | per-TU translate_module |
| parse | 23 | |
| c2mir | 20 | |
| link | 13 | lazy now; MIR_load + stdlib dlopen + import resolve |
| other | ~25 | engine/MIR init, manifest, Program ctors (measured trivial), exec of `quit` |

Key structural facts found:

- Adventure's TUs bind almost nothing: 5 grove binds total
  (`<stdio.h>` ×4, `<stddef.h>` ×1 — auto-included for stderr use). A
  bound C header triggers the whole restore machinery: all-units
  decl-index demand sweep + arena materialize + per-Program register.
- The demand filter (rung 2a) works — only the stdio closure's DataDefs
  build — but the SWEEP walks all 339 units' decl indexes per TU, and
  `CirMaterializeFilter` copies the whole verdict map.
- `is_system_header_path` depends on per-Program include config, so a
  demand-verdict process cache needs an honest (blob, closure, config)
  key — deferred to R4-lite below rather than shipped with a dirty key.
- The per-forest `open` (~2ms/TU × 11) is intern-spine rebind + name
  indexes over already-cached bytes — per-instance state.

## R4-lite: one bound forest surface per multi-TU compile — LANDED (session 118, 2026-08-22)

**Results (packed adventure, container):** forest phase 79ms → **45ms**;
wall ~210–225ms → **~171–175ms** (5-run spread 171–175). Dev lane
unchanged (~0.3s — no carrier; the win is packed-lane-specific by
construction). The three commits:

1. **@b1141126 S1 shared pools** — `MadcCompileGroup` (madc.h): sibling
   TU Programs share ONE strpool + ONE project type-id table via
   shared_ptr owners behind unchanged reference members. Also fixes the
   latent multi-TU hazard where `madc_active_project_types`
   (last-binder-wins) resolved runtime type-id queries against
   whichever TU's table was ambient.
2. **@2fffddd3 S2 incremental materialization** — `materialize_for()`
   unions a later, WIDER demand filter in (per-name verdict OR,
   monotone) and re-runs the passes; done-guards (`_defs_by_tid`,
   fresh-set for pass 2, `_mat_done_slots`/`_mat_done_globals`/
   `_mat_done_templates`, persisted `_method_by_func_id`) make the
   generations append-only. `set_materialize_filter` retired.
3. **@5078dbe0 S3 shared instance** — group map keyed (config word,
   -D hash); ensure_bind_forest adopts before probing; ownership via
   `_bind_forest_holder` shared_ptr (raw delete removed); declidx
   verdict cache keyed (instance, closure). Gate:
   `tests/testprojectwiden` — probe-verified FILE bound=0 in TU 1's
   sweep, bound=1 + union materialization on the ADOPTED instance in
   TU 2 (forest 29ms → 11ms, no re-open), byte-equal to the C oracle.

**Forest residue (~45ms):** first TU 28ms = the S1-miss intern-spine
decode (14-frame 11.6MB zstd — a pack-format / lazy-spine lever);
adv_actions 10ms + adv_travel/adv_loop ~3ms each = DISTINCT closures
paying their own sweep + union increment + register (the verdict cache
hits only on identical closure sets); register stays per-Program (R4
full shape). Validation: units 7/7, dev subsets 8/8, packed 42/42,
testprojectwiden both lanes, adventure parity 3+94 byte-identical
after every slice.

### The design as landed (was: DESIGN LOCKED)

The remaining forest cost is per-TU repetition over one immutable blob.
Coupling audit (session 118, code-verified):

- `CirFrozenForest::live_str_id` memoizes frozen→live ids into the
  AMBIENT `TokenBase::_active_strpool` — a shared instance is only
  valid across Programs that share ONE string pool.
- `materialize_from_arena` (cir_freeze.cpp:1979) references ZERO
  Program state — its products (DataDef/FuncDef/Variable in
  `_mat_storage`/`_defs_by_tid`/`_restored*`) are Program-independent.
- BUT `dd->type_id` is a memo stamped ON the DataDef
  (`madc_type_id_for`, parser.cpp:17010) pointing INTO the stamping
  Program's `project_types` id_table — shared DataDefs therefore force
  a shared `project_types` too (semantically right: one type-id
  universe per program, the ODR shape).
- Node segments materialize into `_c2m`; the project lane already uses
  ONE c2m for ALL TUs (madc_cir.cpp:5898, created after all parsing) —
  the set_c2m constraint is satisfied by construction.
- The demand filter (`set_materialize_filter`) is first-caller-wins on
  the memoized materialization; sibling TUs have DIFFERENT closures
  (adventure: stdio ×4, stddef ×1) — a shared instance requires
  monotone filter-union + INCREMENTAL materialization.
- The closure is complete only at each TU's parse start (includes
  discovered during its lex) — sharing keyed by closure is impossible
  at open time; incremental is the only honest shape.
- `intern_keyed_map._slot` = 4 bytes × max-key-id — shared-pool slot
  growth is tens of KB per map at adventure scale (measure, non-issue).

### The design: `MadcCompileGroup`

A group object owning the substrate sibling TU Programs share, created
by the project lane (`project_parse_all`), passed to
`MadcEngine::create_program` per TU. NOT engine-scoped: a long-lived
embedding engine creating many independent Programs must not
accumulate one ever-growing pool. Default (no group) = private pools =
today's behavior; single-TU lanes and unit tests untouched.

- `std::shared_ptr<madc::dis::intern_table> strpool`
- `std::shared_ptr<madc::dis::id_table<DataDef>> project_types`
- `std::shared_ptr<CirFrozenForest> bind_forest` + the probe key it
  was opened under (config word + defines hash) — adopt only on exact
  match, else private probe (loud gate, never silent divergence)
- declidx verdict cache: closure-set → (declared_bound,
  declared_system) verdict maps (repeat closures skip the all-units
  decl-index sweep)

`Program::strpool` / `Program::project_types` become reference members
backed by shared_ptr owners injected at construction (default ctor arg
= make own). Forest ownership moves from raw `delete bind_forest` in
~Program to shared_ptr holders (kills the alias-aware delete dance for
ledger/source too).

### Slices (each its own commit, targeted tests per change)

1. **S1 shared pools:** Program ctor injection + MadcCompileGroup +
   project lane wiring. No behavior change anywhere (group unused
   until S3); project lane gains cross-TU spelling dedup.
2. **S2 incremental materialize:** persist the admitted-tid set
   (`_mat_admitted`); on a LATER, WIDER filter (union of verdict maps,
   monotone), re-run the pass sequence gated on newly-admitted tids
   (aggregate alloc/fill) and on verdict-flip (name-keyed surface
   loops: typedef/enum/func/global/template/nslink/defbody/defaults —
   admitted-now && denied-before, evaluated against old + new maps).
   Per-TU instances see one filter, one generation — behavior
   preserved; the suite is the oracle.
3. **S3 shared forest instance:** ensure_bind_forest consults the
   group first (config-key match), parks its probe result for
   siblings; set_materialize_filter on a materialized shared forest
   unions + increments (S2); declidx verdict cache keyed by
   closure-set. Register stays per-Program (R4-full territory).
4. **S4 measure:** packed/dev adventure `--show-stats`; expect the
   79ms forest line → ~30–45ms (opens ×10 ≈ 20ms, materialize ×3 ≈
   21ms, declidx ×3 ≈ 9ms reclaimed; register 8ms × binding TUs
   stays). Record honestly either way.

### Thread contract (thread-safety.md — STATED)

`MadcCompileGroup` and everything it shares (intern_table, id_table,
CirFrozenForest instance) follow the C++ standard-library convention:
concurrent READS are safe once the compile completes; MUTATION IS
SINGLE-THREADED. The project lane compiles TUs sequentially on one
thread — the group adds no locks. Parallel TU compilation (the F2
programs-use-cores arc) must synchronize the group's mutation points
before going wide: `intern_table::intern`, `id_table::add`
(`madc_type_id_for`), forest bind/unit-load/materialize entry, and the
group's own maps. The existing PROCESS-level caches (S1 decoded
segments, recordability fixpoint) keep their mutexes — unchanged.

R4-lite landed at ~171–175ms; the follow-on slice @da1ffcd3 (per-kind
slot buckets in the blob cache + the findVariable per-call getenv)
took it to **~163–166ms** (forest 45 → 38ms).

## The -O2 attribution (session 118 — the ranked lever list)

Method: relink the unstripped -O2 objects (`rm bin/madc-release; make
-C src MODE=release ../bin/madc-release; cp` → `bin/madc-O2sym`), pack
it as its own carrier (`forest_pack.sh bin/madc-O2sym`), then restore
the stripped release (strip + re-pack). callgrind on the O2sym packed
launch = 1.13B Ir total (~172ms pre-buckets). Ranked:

| cost | Ir share | status |
|------|---------|--------|
| zstd decodes (11 container-global segs, ~18MB raw: spine 4.1MB, arena 6.5MB, template payload+TOKENS 6.4MB, misc 0.9MB) | ~9.6% | PARKED: raw spine = +10MB binary (owner size-trade); template segs decode because the flush HYDRATES 364 derived member-template records (register_skipped_class_template_function) — lazy MEMBER-kind stubs are B2-entangled (parser.cpp:22046 kinds) — own slice |
| malloc/free family | ~12% | spread; the flat-structures territory |
| dynamic_cast (493K calls) | ~8.8% | the de-RTTI sweep: hot dispatch chains in parse/cir on C-shaped code (the old "0.36% negligible" finding was template-workload-specific) — own slice |
| materialize_pass self (full-arena rescans) | 4.1% | FIXED @da1ffcd3 (per-kind slot buckets) |
| flush_forest_pending_globals surcharge (findVariable×35K + per-call getenv) | ~4.5% | getenv FIXED @da1ffcd3; the ~9K pending funcs/TU are the verdict-0 derived surface — narrowing it = demand-driven derived-entity restore, own slice |
| inject_pending_auto_includes (11×, the per-TU lean-prelude/fragment lex) | 5.5% | the R4-full "share the prelude parse" target |
| strcmp/memcmp/strlen/string== | ~9% | spread string machinery |

NEXT, in measured order: (1) the de-RTTI dispatch sweep (~9ms+), (2)
lazy MEMBER-template hydration at the flush (kills the 6.4MB template
segment decodes + 364 hydrations for C-shaped TUs; B2 entanglement),
(3) R4-full shared prelude lex/parse (~9ms+ in
inject_pending_auto_includes alone), (4) demand-driven derived-entity
restore (the ~9K verdict-0 funcs per binding TU). The tcc bar (~22ms)
needs several of these plus the malloc/flat-structures arc.

## Traps for the next session

- Wall time drifts on the container across minutes (thermal/load) — the
  `--show-stats` phase lines are the stable comparator; interleave A/B
  when walls must be compared (feedback_interleave_ab_measurements).
- The -O0 callgrind profile overweights std::map/set/vector machinery
  that -O2 inlines away — rank -O2 phase lines first, use -O0 callgrind
  only for structure discovery (this session's fixpoint-cache lesson).
- The game exits via `exit()` inside JIT code: anything after the entry
  call in madc_project_execute does not run on a normal quit.
- `MADC_FOREST_CACHE_PROBE=1` prints S1 hit/miss per segment; the
  single-TU `--emit=c11 --show-stats` run prints the itemized forest
  lines (map+open / bind / restore / unit loads / decode).
- Release binary is stripped: profile the DEV binary with
  `--forest-bind=bin/madc-release` (config gate accepts it — the
  verify_macho_release.sh precedent).
