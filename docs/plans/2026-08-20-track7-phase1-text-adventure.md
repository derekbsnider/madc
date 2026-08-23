# Track 7 Phase 1 — the Text-Adventure Pilot (hub projections + Level 0)

**PROGRESS (2026-08-20): S0 ✅ (results below) · S1 ✅ hub.h 53/53 ·
S2 ✅ verbs.h 21/21 · S3 ✅ uinode/prose/render_text/projection 40/40 ·
S4a ✅ world_text.h 71/71 · S4b ✅ <ns_ui> + adventure catalog + the two
pilot fixture tests (runner 2/2, .expect_quiet clean) · S5 ✅ gates
(adventure_roundtrip_gate + check-hub-write-path in fulltest, both
negative-controlled). All four gates demonstrated: G1 player-refused /
builder-open inspect over one world; G2 stitched-transcript
byte-identity + save fixed point + corrupt-save refusal; G3 the
inventory-derived brass key, data refusal prose, leveled verbs; G4 one
write path, planted-violation control. Remaining: the merge wave
(fulltest + EXE lane once, scoped /dupaudit, merge WITH a release).**

**HANDOFF (2026-08-20, pre-compaction; session #108).** Where things
stand and what the owner ruled:

- All slices S0–S5 are green and COMMITTED on
  `feature/track7-hub-projections-claude` (pushed through @5d95a30a).
  The merge-wave battery passed on the container: fulltest rc=0 (all
  gates incl. the two new pilot gates + rule trailers), integration
  1106/0/0TO/9skip (+2 pilot tests), EXE 1065/0, release+packed
  1106/0/0/9.
- ⚠️ **OWNER LAW (second repeat): NO RELEASE — releases mark COMPLETE
  features, never slices.** The v0.95.0 ceremony drafts in tmp/ are
  DEAD; VERSION stays 0.94.0. Merge-to-develop timing is an OPEN OWNER
  CALL (they historically don't mind checkpoint merges; ask or await).
- ⚠️ **THE RELEASE BOUNDARY, owner-defined 2026-08-20:** "the new
  release is allowed when Colossal Cave Adventure is tested to be fully
  playable ... and then we will also have a master promotion."
  Operational reading: fully playable = the full 350-point walkthrough
  plays through our engine, gated by transcript parity against
  open-adventure's regression logs. That release then batches
  everything on develop (v0.93.0 + v0.94.0 + the Track 7 feature) into
  a MASTER PROMOTION — three-platform gated per promote.md step 5.
- **NEXT (owner-directed): the real game — BRAINSTORM DONE, PLAN
  APPROVED (session #109).** See
  [2026-08-20-adventure-430-plan.md](2026-08-20-adventure-430-plan.md)
  — the execution plan for Colossal Cave Adventure at
  `examples/adventure/`. Key rulings recorded there: target is
  Adventure 2.5 = the **430-point** version (pinned from source; "450"
  was a misremember), references are references only, architecture is
  modern data-oriented ECS + systems + events + views, MUD-shaped
  (actor-parameterized, Diku act()-as-data event scopes, command path
  vs world pulse, sessions bind actors), `var`-first style, oracle =
  open-adventure's ~92 in-scope .log/.chk transcripts with win430 as
  the release-boundary gate.
- **OWNER ARCHITECTURE RULING**: level-0 text rendering is INTERNAL
  (dis-like, dependency-free); external UI rendering libraries are
  dat-side providers (perhaps literally through madc::dat) — recorded
  in the design doc's Decided section.
- Engine gaps Colossal Cave will force (the brainstorm, to be planned
  next session): conditional/probabilistic travel (link-attached
  conditions — the brass-door pattern generalized onto links),
  vocabulary-as-data (synonyms, magic words, %vocab section),
  per-state object prose, autonomous NPC behavior on tick (dwarves,
  pirate — compiled catalog now, script verbs post-eval/exec), canned
  message tables, scoring/death/reincarnation, the lamp battery
  (already built in miniature). Data path: convert adventure.yaml →
  .world offline (checked-in output + converter tool); transcript
  parity against open-adventure's walkthrough logs is the "serious
  job" gate.

Status: implementation plan, 2026-08-20, executing the APPROVED design
[2026-08-20-data-hub-projection-rendering.md](2026-08-20-data-hub-projection-rendering.md).
Work happens on `feature/track7-hub-projections-claude` off `develop`;
merge comes WITH a release (release-cadence rule) and a pre-merge
`/dupaudit` scoped to madcdis + the new `ui::` surface.

## Objective

"You are in a maze of twisty little passages, all alike." One small world,
two roles, level 0 — and the entire hub exercised end-to-end in miniature:
ingest (SourceAdapter) → entities/components/relations → verbs → projection
→ level-0 render → export (save) → re-ingest (load).

## Gates (all four required; scripted, in the suite)

- **G1 — two projections, one world:** the same world data drives the PLAY
  projection and the BUILDER (debug) projection; the builder sees raw
  entity/component data the player never receives (project-then-transmit).
- **G2 — round-trip identity:** play N scripted turns → save → load in a
  fresh process → replay the remaining script: the combined transcript is
  byte-identical to an uninterrupted run. Negative control: a deliberately
  corrupted save must FAIL loudly, never half-load.
- **G3 — the access model works:** a door requires the brass key and opens
  only when the key ENTITY is in inventory (data-derived credential); a
  builder-only verb invoked by the player is refused with an exact,
  tested message; a leveled verb refuses below the required domain level.
- **G4 — one mutation path:** every world change flows through a verb
  handler's mutation context. Gate mechanism, not convention: the mutation
  context is the ONLY public write surface on the pilot's world API, and a
  suite check greps the pilot + ui:: sources for direct component-store
  writes (negative-controlled with a planted violation).

Oracle: scripted playthroughs through the standard fixture machinery —
`.input` = player commands, `.expect` = transcript lines. No new runner
capability needed; the text adventure is the fixture convention's ideal
customer.

## Decisions inherited from the design (do not re-litigate here)

- **Library surface only.** No new parser syntax; `render { }` is a later
  ergonomic layer. Script-facing publics go in a `ui::` namespace
  (`<ns_ui>` embedded header, declaration-only, mangled-direct per
  cpp-first-api); the real implementation is C++ in the host.
- **Value-first semantic IR**: content/hints = `madc::value`;
  classification (roles, states, action kinds) = registry-interned ids;
  platform facts native.
- **Access = keys + levels** over credentials; a role IS a key; key
  implication closed into a bitvector at credential build.
- **Prose**: projection library composes (authored / template behind one
  seam — NLG is post-Phase-1); level-0 renderer only typesets. Facts stay
  authoritative under any composed prose.
- **Verbs**: compiled registry bound by data (verb = named entity with a
  requirement); script-attached verb bodies wait for eval/exec.
- **Thread-safety contract** (first consumer of
  `.claude/rules/thread-safety.md`): the Phase 1 hub is **confined to one
  thread** — that is the stated contract. API shapes are concurrency-ready
  per demand 15: no raw component pointers cross a verb boundary, reads
  come from query/projection results, writes go through the mutation
  context. Going multi-threaded later changes no signatures.

## Slices

Targeted tests per slice; ONE fulltest at the merge wave. Every slice's
src/include commits carry the four rule trailers. Search-first standing
note: `id_table.h`, `intern_table.h`, `relation.h`, `schema.h`,
`source_adapter.h`, `cursor.h` already exist in `include/madcdis/` —
EXTEND those owners; state the grep + concept before any new named helper.

### S0 — Probe the value surface (measure before designing; ~half day)

Run the pilot's motivating `madc::value` shapes at live HEAD from a .mad
script: bag create/read/write through the carrier, value in containers,
value returned by reference from a host call, string content in and out.
Known opens that may bite: `value f()` by-value return lowers to `int f()`
(silent); `value` lacks std::string ingestion. Each failing shape the pilot
NEEDS becomes its own feeder-fix commit (fix-what-you-find) or a recorded
workaround here — never a silent detour.

**RESULTS (2026-08-20, container, 11 probes; tmp/track7probes/):**

Works — the pilot relies on these: scalar retag + printf coercion;
object-kind bags via `madc::context_set_*` with eval-path readback
including nested dotted paths (`lamp.fuel`); `.count()`; `php::print_r`
over nested bags; deep-copy isolation on carrier assignment; value egress
(`.c_str()` → std::string, `cout << value`); array kind push/subscript/
range-for (subscript yields `string` per the ruling — spell reads
`a[i].c_str()` / `string s = a[i]` / `cout << a[i]`).

Loud compile errors, confirmed, workarounds recorded — NO pilot blockers,
no feeder fixes required for Phase 1:
- `value f()` by-value return: compile error at HEAD (better than the
  banked "silent empty" symptom — it got LOUD somewhere in the
  v0.86–v0.94 arcs). Workaround: `value &` out-params.
- retagging a `value &` PARAMETER (`v = 5`): compile error (the documented
  sibling defect). Workaround: mutate via `context_set_*`.
- `value = std::string` ingestion: compile error (documented residue).
  Workaround: `.c_str()`.
- string-keyed subscript on the carrier (`bag["k"]`): unsupported, loud.
  Workaround: `context_set_*` + eval readback. (Ergonomic follow-up
  candidate AFTER the pilot: carrier subscript as sugar over the same
  entries — would retire the clunkiest spelling in the driver.)

Diagnostic-parity note (recorded, not pilot-blocking, matches canon
behavior but misses canon's warning): `printf("%s", a[0])` with the
string-typed subscript result prints garbage with NO -Wformat-style
warning, where g++/clang both warn on std::string-to-%s. Candidate small
diagnostics fix, own commit, own reducer — queued behind the pilot.

Also noted: the container dev `bin/madc` predates the v0.94.0 VERSION bump
(wave-validated code, stale label) — S1 starts with a sync + rebuild.

### S1 — Entity/component core + credentials (C++, madcdis; ~2-3 days)

- Entity handles over `id_table`; component attach/detach as record-family
  membership; a per-entity `value` property-bag component.
- Relations for geography (`exit`: room→room keyed by direction) and
  containment (`in`: thing→container). Inventory = containment on an
  actor.
- Credentials: key registry (interned names → ids), holder key-set as a
  bitvector, key→key implication relation closed at credential build
  (cycle-safe); per-domain numeric levels; requirement = {key set,
  optional (domain, min-level)}; `check(credentials, requirement)`.
- Unit tests (doctest): implication closure incl. a cycle, requirement
  matrix, bag round-trip, containment-derived key possession.

### S2 — Verbs (~1-2 days)

- Verb = named entity: id, name, requirement, handler (registry of
  compiled functions).
- `invoke(actor, verb, target…)`: build actor credentials (held keys +
  inventory-derived keys + levels) → requirement check → handler with a
  mutation context; refusals are loud and worded for the player.
- The mutation context is the only write path (G4's mechanism).

### S3 — Projection + semantic tree + Level 0 (~3-4 days)

- `UINode`: role/state ids interned; label/content/hints as `value`;
  actions = verb entity ids; children. The tree is hub data.
- Projection = query + mapping function; a projection INSTANCE binds
  (world, credentials) — access filtering happens here, before any
  renderer sees the tree.
- Level-0 renderer: typesetting only — linearize roles, lists, wrapping.
- Prose kit in the projection library: enumeration ("a lantern, a key and
  an apple"), pluralization, articles; templates as data.
- `ui::` script publics (minimal): open a world, projection for a role,
  render to a stream, invoke a verb, read pending output.

### S4 — World adapter + the game (~2-3 days)

- A minimal sectioned tagged-text world format (rooms, things, exits,
  verbs, requirements — SMAUG-flavored, deliberately small); a
  `SourceAdapter` implementation with locators; an export adapter writing
  the same format (save = export; G2's substrate).
- The game driver in madc (`tests/` world + `.mad` program): a two-word
  command loop (verb + object — deliberately dumb parser; parsing
  sophistication is not what this pilot tests), turn counter, lamp fuel
  that depletes into darkness (state evolution), maze rooms SHARING one
  authored description (the Q3 sharing case), the brass-key door (G3).
- Builder role: the `debug` projection — raw entity/component browse over
  the same world (the first, minimal naked-objects moment).

### S5 — Gates, fixtures, wave close (~1-2 days)

- `tests/testadventure*.mad` + `.input`/`.expect` playthroughs for both
  roles; the save/load transcript-identity test; the corrupted-save
  negative control; the access-refusal cases; the G4 write-path check
  wired into fulltest with its planted-violation negative control.
- Merge wave: fulltest once, EXE lane (shared runtime surface), scoped
  `/dupaudit`, release on develop.

## Out of scope (deliberately — each has a seat in the design doc)

Multi-threading (contract: single-thread confined); NLG (templates only);
stored-view descriptors (code-defined projections only); rendering levels
1+; multiplayer/live serving; script-attached verb bodies (eval/exec);
schema observation/hardening automation (5A.16 — the pilot uses dynamic
bags + declared components only); `render { }` syntax.

## Risks / honest unknowns

- The value-surface probes (S0) may surface feeder work that resequences
  S3/S4 — that is the probe's job; budget slack lives there.
- The `ui::` script-surface slice is the first NEW namespace since the
  mangled-direct migration; follow `ns_madc`'s registration pattern
  exactly (search first — the pattern owner exists).
- Save-format versioning is deferred (one format version in Phase 1); the
  export adapter writes a version line so Phase 2+ can evolve it.
