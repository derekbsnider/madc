# Forest Phase 2: Full Evaluation + Lazy Materialization — execution plan

**Date:** 2026-07-09
**Status:** ACTIVE — rung 1 in flight
**Owner directive:** the shipped madc binary carries the FULL pre-compiled
header forest embedded and lazy-defrosts exactly what each compile's
`#include`s require. **General purpose — no use-case-shaped warm lists, no
pre-designing the binary for specific programs.**
**Parent:** `2026-06-22-embedded-header-forest-execution-plan.md` (CLOSED).
This plan finishes the two settled decisions that close-out measurement
showed unrealized: settled #2's cost model ("save post-parse state — token
form leaves the real cost on the table") applied to DEFERRED BODIES, and
settled #7 ("only *used* nodes materialize").

## The measured gap this plan closes

testsubscript, -O2 release binary, byte-identical output in all modes:
live parse **1.006s** → cold corpus **0.665s** → (diagnostic) fully-warmed
corpus **0.399s**. Cold-corpus decomposition (-O0 stats): instantiate 0.708s
/ 3,983 calls, **207 `parse_deferred_lazy_body` derivations** (the consumer
re-parses every inline method body it touches from DK_DEFBODY token runs —
the producer never ODR-used them, so nothing translated froze), CIR build
0.642s (lowers everything the derivations produced), lex-bucket ≈0.45s
(dominated by whole-container `materialize_from_arena`). The warm diagnostic
proved the v22/v26 restore machinery delivers when the corpus carries
evaluated state: 3,983 → 194 instantiate calls, 207 → 0 body parses.

**No-reparse policy status:** DEFBODY on-use derivation is NOT a policy
violation — live defers inline bodies to first ODR-use too, so LOADED ==
PARSED holds. But it is once-per-COMPILE work; the parent plan's own verdict
called frozen instantiations "the secondary prize" and rejected token-form
state. Pack time is free — evaluate there, once ever.

## What can and cannot be pre-baked (the generality line)

- **CAN (general):** every deferred body of everything *the headers
  themselves define or name* — including the products libstdc++ itself
  pre-instantiates (`extern template basic_string<char>`, the iostream
  instantiations). Draining these at pack time is finishing the parse, not
  curation.
- **CANNOT (semantics):** products only the user names (`vector<int>`,
  `map<string,int>`). They instantiate on demand through the parse-once
  spine, exactly like g++ pays per TU. Their cost is attacked by
  instantiation SPEED (rung 4 = the existing front-end-performance track),
  and optionally later by a per-project side cache (learned locally, never
  shipped) — recorded, not scheduled.

## Rungs

### Rung 1 — pack-time deferred-body drain (fixpoint) [IN FLIGHT]

At the end of a `--freeze` / `--freeze-append` parse, before the arena
freeze: repeatedly run the existing `parse_deferred_lazy_body` derivation
over every entry in `deferred_lazy_bodies` until the map is empty or only
failed entries remain (parsing a body can defer more bodies and instantiate
more header-named products — run to fixpoint). Drained bodies become
translated func-defs and freeze through the EXISTING v22/v26 machinery; the
DEFBODY record for a drained body no longer freezes.

- **Error tolerance (no silent caps):** a body that fails to parse at drain
  time is RE-INSERTED as DEFBODY (falls back to today's on-use derivation
  in consumers — a pack must never be blocked by a parser gap in a method
  nobody calls) and the drain logs a per-freeze summary line
  (`drained N bodies, M left deferred`) so the fallback count is visible
  and ratchetable.
- **TU-root fence:** root-origin bodies may drain (harmless); the fence
  already excludes root-origin records at freeze. Header-origin bodies
  keep their pattern-file origin (`funcdef_files` classification).
- **Risk to watch:** emission-set parity. Restored translated bodies must
  emit exactly when a live parse would emit them (on reference), or the
  bind gate's whole-TU byte-identity breaks (the v15 synth-dtor overshoot
  precedent). The gate is the detector; if it trips, the fix is
  reachability parity on the emission side, not weakening the gate.
- **Gates:** forest_bind_gate 18/18 byte-identity, fulltest 680/0/0/16,
  packed suite 680/680 (re-pack), testsubscript phase table re-measured.
- **Expected effect:** string/iostream-surface body parses go to zero in
  consumers (header-named products). User-named container products still
  derive on use — that share moves in rungs 2–4. Corpus record count grows
  (44,689 → six figures); rung 2 keeps that free for consumers.

### Rung 2 — lazy materialization (parent plan settled #7)

`materialize_from_arena` reconstructs the whole container eagerly at first
restore. Make materialization demand-keyed, in two steps:

- **2a (closure-filtered eager):** materialize only records whose defining
  unit is in the TU's bound-include closure — the SAME demand key item 5
  built for registration (`forest_chain_set` × B4a decl index). Cross-unit
  references stay safe because a unit's includes are in its own frozen edge
  closure by construction; any referenced-but-unmaterialized record
  materializes on swizzle miss (the guard path).
- **2b (on-touch, only if 2a's numbers demand):** lazy handles at the
  swizzle boundary — deref triggers materialize. Bigger surgery; do not
  start it before 2a is measured.
- **Gates:** same suite + gate set; the lex-bucket / total in the phase
  table is the metric. This rung is what makes rung 1's bigger corpus free.

### Rung 3 — emit only referenced surface (CIR)

The CIR builder walks and emits the whole registered surface (Pass 0/0.5
struct emission, Pass 0.75 protos). Gate emission on reachability from the
TU's own tree. NOTE: live emits the whole surface too — this rung changes
BOTH paths together so bind == live byte-identity is preserved; fidelity
gates are `--emit=c11` vs g++ (cirfidelity) + the full suite. This is the
recorded "auto-include / emit-only-referenced" roadmap item.

**DESIGN (2026-07-12, recon done — defer-and-filter, mirroring Pass 0.75's
existing referenced-only proto model).** Sizing: on testsubscript's emitted
C, 718/869 struct defs (83%) and 142/199 typedefs are dead (never
referenced beyond their own definition). Protos are ALREADY referenced-only
(Pass 0.75 keys on referenced_funcs, filled while bodies translate into a
temp list before the proto pass). Types get the same treatment:

1. **Collect** `referenced_types` (struct/union tags + typedef aliases) at
   the type-reference chokepoints during node construction
   (append_type_specs, the `N_STRUCT(id, IGNORE)` tag-ref builders,
   typedef_emit_name). Always-on set inserts — no behavior change.
2. **Defer** Pass 0 dkTypedef/dkStruct/dkUnion nodes and Pass 0.5 class
   defs into a pending vector (node, kind, name, dd) instead of appending
   to top_list; dkGlobalVar is already deferred. TU-ROOT-origin decls (the
   user's own file) emit UNCONDITIONALLY — the faithful mirror applies to
   the user's source, not the include surface.
3. **Emit** after bodies + protos + globals are built: closure = seeds
   (referenced_types ∪ TU-root decls) expanded through member/base/anon
   deps and typedef→underlying links; splice survivors at the ORIGINAL
   position (before protos) in ORIGINAL relative order — intra-segment dep
   order is preserved by construction, and C's def-before-use holds because
   the segment stays ahead of everything that references it.
4. Late-discovered structs (Pass 1.97 splice / emit_struct_with_deps
   during body translation) are by-construction referenced — unchanged.

Steps land individually gated: (1) collection only → byte-identical;
(2) defer + unconditional re-emit (filter off) → MADC_DUMP_MIR identical
on the bind gates; (3) filter on → cirfidelity + fulltest + bind 18/18 +
packed suite + bench row.

**FIRST CUT landed @ee8f0ac7 (post-build remove-in-place superseded
defer-and-filter: build whole-surface as today, then c2mir_op_remove dead
type decls before `append(module, top_list)`).** Removed 106 structs +
174 typedefs on the LIVE headline TU; wall-marginal.

**SECOND CUT — the UNIFIED referenced-surface filter (2026-07-12, this
branch). Two first-cut root causes found by comparing a fresh bound emit
against live:**

1. **The first cut NEVER fired on the bound compile** — the binary whose
   wall matters. Restored TopDecls carry no parse position (file=NULL →
   classified root); Pass-0.5 class struct defs are never position-stamped
   (root even LIVE — live's 106 removals were only the Pass-0-stamped
   plain C structs/typedefs).
2. **Type decls were the only conditional category.** Pass-1 protos for
   dead library functions, Pass-1.45 dtor protos, Pass-1.5 vtables/
   typeinfo, Pass-1.6/1.7/1.8 synth dtors (all swept per-class from
   struct_map, unconditionally), the Pass-1a globals (8 stream externs
   incl. the dead wide-char four, tag objects), and their
   __madc_global_init ctor groups each SEED the harvest — any one of them
   pins the dead class webs the type filter tried to remove.

Ground truth (gcc-parity): g++'s whole TU for `#include <iostream>` +
`cout << 42` is 58 lines — no tag globals, no hardware_* constants, no
vtables, no dtor bodies. Referenced-only across EVERY category is g++'s
COMDAT/ODR-use shape.

Design as landed:
- **Origin verdicts.** Live: TopDecl origin/file → is_system_header_path;
  classes: the exact `from_system_header` flag (serialized, restored).
  Bound: `forest_declared_system` (name → declared by a BOUND unit whose
  header path is a system include, built beside forest_declared_bound in
  forest_restore_decls) → new `TopDecl.forest_system` +
  `PendingForestGlobal.system`; restored free-function bodies classify by
  their `forest_body_unit`'s path. tmp/-grove binds stay non-system →
  roots → gate corpora keep the whole surface, like live.
- **One cond map (builder member m_cond_nodes), marked at the emission
  sites, consumed by ONE fixpoint** replacing the type-only block: TYPE
  nodes admit by tag (typedef-bearing also by identifier); every other
  category (protos, output-externs, globals, vtables/typeinfo/thunks,
  synth dtors, library/loaded bodies) admits iff its DECLARED SYMBOL is
  referenced by admitted content. Roots (anything unmarked) seed the
  harvest. A file-scope STATIC with dynamic init (the <iostream> __ioinit
  model) is never conditional — internal linkage is observable per-TU.
- **Ctor groups:** collect_global_ctors records each global's slice of
  __madc_global_init statements (m_ctor_groups); a group never seeds the
  harvest (it references its own global — every ctor'd global would
  self-admit) and is admitted/pruned with its storage decl.
- pack_recording stays EXEMPT (frozen corpus complete for arbitrary
  consumers); live and bound filter identically → bind == live by
  construction.

Measured (testsubscript, before gates): LIVE emitted C 6079 → 3623 lines,
struct defs 763 → 206, typedefs 131 → 88; BOUND 6893 → 4279 lines (the
first cut left bound at the FULL 864-struct surface), structs 196,
typedefs 133. Residual live/bind set diffs = the pre-existing live ⊃ bind
instantiation-product gap (37/38 verified present before this change) +
conservative root-keeps where a restored name is not in the B4a decl
index (stdin/stdout macros → FILE web). Gate updated: the strbind
consumer now REFERENCES hardware_destructive_interference_size and
in_place so the v14/v16 restoration asserts stay meaningful under
referenced-only emission.

**RUNG 3 CLOSED — merged to develop @154becbf (2026-07-12), bench row
@9411221b.** Two gate-caught fixes after the first fulltest (strbind's
new whole-TU MIR byte-identity assert was the only red): (1) the
forest_declared_system lookup must be QUALIFIED-then-bare (std:: tag
globals are indexed only as "std::X" — bind kept piecewise_construct/
allocator_arg/ignore as roots while live filtered them); (2) a combined
`typedef struct X {...} Y;` self-admitted through the any-id-in-subtree
rule (common member ids like size_t are always referenced) — typedef
admission is now by DECLARED ALIAS or tag only. Final numbers: live
3393 lines / 169 structs / 60 typedefs, bound 3965 / 155 / 100. Gates:
fulltest rc=0 (681/0/0/16, bind 18/18, oracles), cirfidelity rc=0
(ASM-DIVERGEs = pre-existing __madc_shim_* host-adapter class), packed
suite 681/0, bench bound 0.824 → **0.804** (clean, load 1.26).

**HONEST LADDER FINDING:** the ≤0.665 wall step did NOT close with the
emission surface — the wall moved only −2.4% while emitted decls dropped
~60%. The remaining bound cost is REGISTRATION breadth + body compile +
consumer-side derivations (MTI columns 217/187 unchanged), i.e. rung 4
territory (instantiation speed / front-end-perf track) plus whatever a
fresh bound callgrind now shows. Next sitting: profile the post-rung-3
bound binary (with -g, remember the 120s CPU cap → compare site counts)
before choosing the next cut.

### Rung 4 — instantiation speed (successor track, not this plan)

User-named products' remaining cost = the front-end-performance track
(`2026-06-22-front-end-perf-Onsquared-HANDOFF.md`: the two O(n²)
`c2mir_node_op` walks; the parent plan's deferral note measured madc ~7×
g++ at the same parse work). Re-measure after rungs 1–3 to size what's
left; g++ on the same source is the external bar (gcc-parity rule).

## Related queued work (not on this critical path)

- extern-array-global dims fidelity → format v28 → unlocks project-header
  (mud.h) corpora — the SMAUG remaining −53% per-TU input.
- Per-project product cache (general mechanism, learned locally) — idea
  only; revisit after rung 4 sizing.

## Measurement protocol (every rung)

`--show-stats` phase table on testsubscript (-O0 dev + -O2 release), cold
vs bound; packed suite 680/680; bind gate 18/18; fulltest. Headline metric:
bound testsubscript -O2 wall (baseline 0.665s; diagnostic floor with all
products warm was 0.399s; target = startup + user-product instantiation
only).

## Rung 1 CLOSED — close-out measurement (2026-07-11)

**Exit gates (all met, branch feature/forest-rung1-drain-claude @b635feea):**
fulltest rc=0 — suite 681/0/0/16 + hardcoding + call-emit + warn census +
tag-arith + tsubst flag-on + forest_selfexe + forest_bind 18/18 (incl. the
owner's bar: testsubscript freeze+bind == live == .expect) + forest_index
oracle + forest_dm oracle: the first fully-green fulltest of the rung-1
era. Packed suite **681/681** (0 failed, 0 timed out) after the span-carry
fix restored packed std::stoi/stod (teststod + teststdstringconv were the
last two packed failures; root cause and fix in commit b635feea).

**Phase re-measure (testsubscript; NAS load avg ~4–5.5 during measurement —
absolute walls are confounded; same-load relative comparisons hold):**

| metric                        | live      | bound (packed) |
|-------------------------------|-----------|----------------|
| -O0 wall (dev, 3-run range)   | 2.63–2.76s| 2.08s          |
| -O2 wall (release, range)     | 1.29–1.62s| 1.23–1.34s     |
| parse_deferred_lazy_body      | 217       | **187**        |
| instantiate calls (-O0 stats) | —         | 3882 (0.682s)  |
| cir build (-O0 stats)         | —         | 1.072s         |

**Verdict vs plan targets:** the drain machinery is correct end-to-end
(byte-identity everywhere), but the consumer-side derivation count moved
207 → 187, not → 0: the drained corpus reverts a large fallback tail to
DEFBODY/span carry (all logged per-freeze — ~32 span-carry reverts + the
local-class/instantiation-product families in a <string> freeze), and the
un-drained tail still derives on use. The -O2 bound wall did not reproduce
the 0.665s baseline under load; the plan-anticipated corpus growth
(44,689 → six-figure records, eagerly materialized) is the other headwind —
**rung 2 (lazy materialization) is where that cost is recovered**, as
planned. Headline re-measure should repeat on a quiet machine at rung-2
close.

**Residual (recorded, not blocking):** drain-failure families in the pack
log (parser gaps in bodies nobody calls — e.g. the spurious multi-return
`extract` misparse, `__cerb` iostream internals, chained-arrow) each revert
tolerantly; burning them down widens warm coverage and is rung-2+ fuel.

## END TARGET (made explicit 2026-07-12, owner confirmed): ~0.1s

0.399s is the floor of the CURRENT machinery only (it still pays whole-
container materialization + whole-surface CIR emission). The architectural
end state is "startup + user-product instantiation only" ≈ **~0.1s** on this
hardware. Ladder from here (bound testsubscript -O2, trend rows in
docs/perf/forest-timings.tsv):

- 0.913 (@dfd085a2 — raw segments, zlib inflate removed, −20% Ir)
- 0.829 (@ceff5bf7 — task #14 LANDED, diagnosis corrected: the flood was
  the stale-hit index force-rebuild (196,596 re-interns from 28 rebuilds
  of a ~7k-entry scope), triggered by the one untracked Variable rename
  (operator peer retag). Variable::rename() now zeroes name_sid; the
  rebuild re-interns only zeroed entries. Hashing 853M→301M Ir; helps
  live too (2.593→2.413). The queued "container-sid remap" design was
  wrong — kept here as a record.)
- ≤0.665 and onward to ~0.4 — rung 2 (closure-filtered materialization):
  post-#14 profile is FLAT (breadth-bound) — the remaining gap is
  whole-container materialize + register, exactly 2a's thesis.
  **2a MERGED @3f06188c (2026-07-12), gated 681/681 + bind 18/18 after two
  fixes (frozen-tree emission-name contract; two-surface ns_ok/flat_ok
  gating — see the commits). RESULT: wall-NEUTRAL on testsubscript (0.833
  — its bound closure spans the corpus, 682/718 aggregates admitted);
  small TUs cut 69% (teststat 220/718). The "2b only if 2a's numbers
  demand" condition IS MET for the headline test: the next testsubscript
  lever is 2b (on-touch materialization of the TOUCHED set, much smaller
  than the closure-DECLARED set) and/or rung 3 — size both against a
  post-2a profile before choosing.**
  **SIZED (2026-07-12, post-2a callgrind): 2b is DEAD — materialize self
  = 13M Ir (0.28%); the remaining bound breadth is registration +
  whole-surface CIR emission (set<string> inserts ~230M, intern 301M,
  despace 70M). RUNG 3 is the next step. 2b is closed WONT-DO.**
- <0.399 — rung 3 (emit only referenced surface; bound cir 0.542 vs
  TU-only fraction)
- ~0.1 — rung 4 (instantiation speed) + startup residue (~0.05–0.15)

## Rung 2a design (2026-07-12, recon done — implement next)

Filter `materialize_from_arena` (cir_freeze.cpp:1378) by the SAME two
demand predicates item 5 already applies at registration
(parser.cpp:12471-12511), moved before DataDef construction:

- **Demand key:** defrec has NO defining-unit field — the key is the
  name-keyed B4a decl index, as at registration. Build the permitted set
  as ARENA-pool ids to avoid per-record string hashing: for each decl-
  index name (container pool) mark bound/unbound, resolving into the
  arena's own frozen pool via `frozen_intern_table::find()` (one hash per
  indexed name, ~5k). Name in NO unit's index = derived entity = keep.
- **Products:** a `<`-bearing record is judged by its canonical-spelling
  HEAD (defrec.canon_id), the forest_product_permitted rule, so
  pre-instantiated products of unbound templates drop too.
- **Cross-unit safety:** forest_chain_set is the TRANSITIVE unit-edge
  closure (forest_bind_include is a post-order DFS over frozen edges), so
  a bound record's references resolve inside the filtered set by
  construction. Guard for residue: a chain-referenced but filter-excluded
  record is ADMITTED (reference-pull during the recordability fixpoint) —
  never cascade-drop a bound dependent because its referent was filtered.
  2b's on-touch handles are NOT needed for this (only if 2a's numbers
  disappoint).
- **Empty closure** (direct restore, unit tests): keep whole-container
  semantics, as registration does.
- **Gates:** unchanged suite + bind 18/18 + both oracles; registration
  filter stays (it consumes the same predicates — share the code, do not
  duplicate: hoist the predicate construction so both call one helper).

### Post-rung-3 bound profile (2026-07-12, tmp/cg_r3.out, -g packed debug binary)

Truncated 4.1B-Ir window (120s CPU cap; site counts comparable with the
post-2a profile). Top sites are ALL registration/name machinery:

| cluster | Ir | vs prior profile |
|---|---|---|
| intern hash_step + hash_bytes | ~300M | unchanged vs post-#14 |
| set<string> Rb-tree insert/compare | ~306M | unchanged vs post-2a |
| memcmp + string::compare | ~240M | unchanged |
| malloc / free / memcpy | ~172M | unchanged |
| despace_spelling | 45M | was 70M (shrank with emission) |
| forest arena / unit_segment walk | ~100M | — |

Confirms the rung-3 close-out finding: decl emission was not the bound
wall. Next-cut candidates: (a) pool-sid → session-sid mapping at load
(restored names already carry pool ids; re-hashing every string on load
is re-deriving), (b) sid-keyed hot name sets (uid-is-the-spine), (c)
rung 4 instantiation (MTI 217/187). (a)+(b) attack the profiled ~800M;
(c) the post-window body work.

**Recon correction (same day, raw callgraph):** the ~300M intern hashing
is QUERY-TIME, not load-time — the callers are findVariableThisScope
(every scope lookup hashes its query), the intern_keyed_map find/erase
paths, and TokenIdent construction. Candidate (a) is deprioritized; the
real lever is (b) properly scoped: SID-KEYED lookups end-to-end (tokens
already carry spelling_id from the str-drop track — re-hashing a string
built from a token is re-deriving). Same shape for the set<string>
Rb-tree cluster. That slice touches hot parser paths → scope it fresh
(which chokepoints, which callers already hold a token/sid) before
editing.

### DEEP RECON, second pass (2026-07-12 next sitting) — the churn is REBUILDS, not hashing

Full caller-attribution over tmp/cg_r3.out (callee call-counts + inclusive
Ir per caller→callee edge, awk over cfn=/calls= records). The ~800M
"intern + set<string>" cluster decomposes into three surgical targets,
and it REVISES the sid-keyed framing: the grand token-sid conversion has
LESS payoff than banked, because most walk-entry hashes come from callers
holding CONSTRUCTED strings (mangled names), not tokens.

| # | site | in-window Ir | calls | root cause |
|---|---|---|---|---|
| A | `CirBuilder::tsubst_method_body` → `set<string>::insert` | **501.7M** (93% of the whole Rb-tree cluster) | 170 calls × ~884 inserts = 150,266 | rebuilds `synth_dtor_syms` (whole struct_map scan, cir_builder.cpp ~16754) + `forest_body_syms` (struct_map×methods + funcdef_map, ~16766) FROM SCRATCH per call, then discards them. 774.6M Ir inclusive / 4.6M per call. |
| B | `findVariableThisScope` → `unordered_map::emplace` | **253.3M** | 204,655 emplaces (28 rebuilds × ~7k-entry program scope + 8k absorbs) | task #14 fixed the rebuild re-INTERNS (196k→28); the stale-hit rebuild still `clear()`s and re-EMPLACES every entry (parser.cpp ~10992). parseFunction's 1,204 lookups carry 324M inclusive — mostly this. |
| C | `findVariable(sp,string)` wrapper → `intern` | 212.0M | 38,479 walk-entry interns (~5.5k Ir each = LONG mangled names) | callers: Program::findVariable(string) 23,950 — of which flush_forest_pending_globals 13,471/79.7M (BOUND-ONLY, our v13 flush), unique_overload_symbol 4,162/54.9M (candidate-probe loop), TokenUSING 4,532; plus translate_module 7,297/37.6M, addVariable 3,168, parseFunction 1,204. |
| D | PCH `deserialize_tokens` → TokenIdent(string) ctor | 51.2M | 73,768 | every rehydrated token re-interns its spelling — the pool-sid→session-sid translation idea applies HERE (hash each DISTINCT spelling once per load), worth ~50M only. |
| E | TokenKeyword/TokenDataType ctor interns | ~35M | ~45k | per-occurrence token creation re-interns fixed spellings; ctor-overload taking the sid the lexer just used for the map hit. |

Slice order (this sitting): **Fix B** (one function, semantics must equal
the full rebuild: erase stale entries whose var's name_sid ≠ key,
re-intern zeroed vars, then ONE ordered pass over `variables` refilling
first-wins ONLY the affected keys) then **Fix A** (builder-level memo of
both sets, size-stamped on struct_map/funcdef_map; pre-edit verify no
in-place verdict flips during translate — MTI must register classes
complete, has_forest_body set at restore only; a false `emittable()`
verdict is a LOUD bail_covered error, so be conservative). C/D/E are
recorded follow-ons — C's biggest half is our own bound-only flush loop.
A+B ≈ 755M Ir ≈ 18% of the truncated window.
