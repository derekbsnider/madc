# Colossal Cave Adventure (430-point) in madc — implementation plan

**Status: APPROVED DESIGN (owner, 2026-08-20 session #109) — this doc is
the execution plan.** Track 7 Phase 2: the real game on the Phase-1 hub.
Branch: `feature/track7-hub-projections-claude` (continues; Phase 1 is
green and unmerged on it).

**⚠️ RELEASE BOUNDARY (owner verbatim, unchanged):** "the new release is
allowed when Colossal Cave Adventure is tested to be fully playable ...
and then we will also have a master promotion." Operationally: the
in-scope open-adventure regression corpus — `win430` included — passes
byte-identical through our engine (see "Oracle"). The release then
batches develop (v0.93 + v0.94 + Track 7) into a three-platform master
promotion.

## SETTLED (owner rulings, this session)

- **Target = Adventure 2.5 (1995), the last Crowther & Woods release =
  what Open Adventure is. It is the 430-POINT version** (pinned from
  source: history.adoc "also known as 430-point Adventure"; the win test
  is `win430`; score.c totals 430). The owner's earlier "450" was a
  misremember — corrected at recon, owner-acknowledged direction stands.
- **References are references only.** esr/open-adventure (C, BSD-2) is
  the DATA + BEHAVIOR oracle; anthay/advent.cpp (1976 Crowther-only) and
  udel advent.py (350-point FORTRAN transliteration) were mined for
  ideas. We are in no way committed to any of their implementations —
  we keep open-adventure's *data* and *observable behavior*, and none of
  the three's control flow (goto webs, phase codes, packed condition
  ints, parallel arrays, FORTRAN trampolines: all rejected).
- **Architecture = modern game programming standards**: data-oriented
  ECS + systems + events + views. NOT deep-OOP inheritance trees.
- **MUD/MOO/MUSH/MUCK shaping (owner directive)**: this version is
  pre-planned for future multi-player. Concretely (seams now, no
  multi-player semantics now): no player singleton — the player is an
  actor entity and every system takes (world, actor); events carry an
  observer SCOPE (Diku `act()` as data); the per-actor command path is
  separate from the world pulse; ui:: sessions bind to actors; the
  builder role is the wizard/immortal tier. Adventure 2.5 *semantics*
  stay single-player for parity.
- **Style: `var`-first** — the example showcases the dynamic carrier;
  strict types only where they genuinely help (madc-C++ classes for the
  RNG service / view are fine).
- **Home**: `examples/adventure/` ("adventure" was the original binary
  name). BSD-2 license + attribution (Crowther, Woods, ESR) ride along.
- Level-0 text rendering stays madc-internal (dis-like); external UI
  render libs are dat-side (prior ruling, unchanged).

## Oracle and corpus

Reference clones (recon workspace, not in-repo):
`/workspace/open-adventure` (gitlab.com/esr/open-adventure),
`/workspace/advent.cpp`, `/workspace/advent.py`.

- adventure.yaml (4,240 lines) = the world: 185 locations, 70 objects,
  76 motion groups, 58 action verbs, 623 travel rules, ~370 named
  messages, 10 hints, 3 obituaries, classes, turn thresholds. Only SIX
  travel rules invoke "special" code (3 cases: plover passage, emerald
  transport, troll bridge); everything else is data. Condition forms:
  `[pct N]`, `[carry OBJ]`, `[with OBJ]`, `[not OBJ STATE]`, `nodwarves`.
- Regression corpus: 107 `.log`/`.chk` pairs, run `advent < foo.log`,
  byte-diffed. ~12 exercise their binary save format (out of scope —
  our save is the `.world` round-trip, gated our way), 3 need CLI flags
  (out of scope, one line of reason each). **In scope: ~92 pure
  stdin→stdout logs, `win430.log` (355 commands → 2,108-line transcript)
  is the boundary gate.**
- Determinism: logs embed `seed N`; we implement the SAME LCG
  (A=1093, C=221587, M=1048576 — advent.h, Knuth-checked) as a seeded
  madc RNG service. Dwarf walks and pct-rolls then replay exactly.
- Vocabulary parity: words match on their first 5 chars (TOKLEN 5).
- Known wrinkle: the `%V` version-string substitution (news text).
  Resolve at conversion time (pin the string or exclude that one log
  with a stated reason).
- Corpus rides in `examples/adventure/tests/` verbatim (BSD-2), driven
  by a parity gate script (G-A below), staged by capability wave.

## Architecture

**Hub / ui:: (C++ host) owns the generic substrate:** world file
load/save/round-trip, entities + value-bag components, containment
links, sessions + credentials, builder/inspect projections,
deterministic save. **The game (examples/adventure/*.mad) owns
Adventure:** command loop, parser, travel interpreter, verb systems,
NPC/environment systems, scoring/death/closing, and the transcript view.

Layers (game side):

1. **Model** — ECS over the hub. Entities: locations, objects, actors
   (player + 5 dwarves + pirate as a LIST with `is_pirate`/`is_player`
   flags — no magic index 6), one world-state singleton (clocks, tally,
   closing flags). Object state machines are data: named states with
   per-state `descriptions[]`, `changes[]`, `sounds[]`, `texts[]` —
   the YAML's exact shape, as bags.
2. **Input** — reader → tokenizer → 5-char-truncated vocabulary map →
   `Intent {verb, object, words}`. The intent is a value (command
   pattern; demand 15's serializable mutation unit).
3. **Simulation** — the turn is the frame: `handle_intent(world, actor,
   intent)` (travel system or action system) then `pulse(world)` (dwarf
   movement, pirate, lamp fuel, closing clock, hint triggers, monitors:
   death/score thresholds). Systems are self-contained madc units with
   narrow contracts. Pipeline ORDER is pinned by the oracle transcripts
   (their message ordering leaks their control flow — a constraint on
   ordering, not on structure).
4. **Events** — systems never print. They emit events (message-id +
   args, moves, state changes) tagged with scope: actor-private /
   room-visible / world-visible. Message substitution (%d/%s/%S plural)
   happens at render.
5. **View** — the transcript view consumes the one session's observer
   stream and prints byte-parity text (auto-look, abbreviated/long
   descriptions via the per-actor room-familiarity counter, object
   listing per state). ui:: projections power the builder/debug view;
   a future second client is a second view over the same stream
   (the Track 7 north star — post-parity, out of scope here).

Travel interpretation lives in the GAME as madc code reading ordered
rule bags (~60 lines). The hub's credential requirements are
access-shaped, not world-state-shaped; we promote a rule-list primitive
into the hub only when a second consumer wants it (rule of three).

## Data design

Per-entity bags mirror the YAML shape (this is the mined lesson: an
object is ONE bag, not six parallel arrays):

```
location:  { name, long, short, conditions:{lit,fluid,oily,deep,...},
             sound, loud, hints:[...], travel:[ {verbs:[...],
             cond:["not","GRATE","GRATE_CLOSED"], action:["goto",...]}... ] }
object:    { name, words:[...], inventory, place, fixed, immovable,
             treasure, state, states:[...], descriptions:[...],
             changes:[...], sounds:[...], texts:[...] }
actor:     { name, is_player, is_pirate, location, oldloc, oldlc2,
             inventory (containment links), seen:{loc:count}, deaths,
             hints_used, novice, ... }
world:     { turns, tally, clock1, clock2, closng, closed, dflag,
             lcg_x, zzword, ... }
vocab:     word(5ch) -> {type, id}   (synonyms = duplicate keys;
             "advice-only" is a word class)
messages:  name -> text (one pool; %-substitution at render)
```

Converter: `examples/adventure/tools/advent_yaml_to_world.py`
(Python + PyYAML — open-adventure's own make_dungeon.py precedent;
build-time tool, output CHECKED IN as `examples/adventure/adventure.world`).
A madc rewrite is later dogfood, not a gate.

## Capability waves (what madc must grow first)

### Wave L — language feeders (var ergonomics; parser/CIR lowering onto
the EXISTING C++ carrier — `include/libmadc/value.h` already has
make_object / vivifying object() / freeze law)

Probe matrix first (S0 discipline: measure before designing; probes in
`tmp/`, run on the container). Each confirmed gap = its own commit with
trailers + reducer + test. Candidate list, ordered by need:

- L1 object-kind MEMBER ACCESS in script: `o.name` read/write on a
  value whose kind is object/null (write vivifies). Dotted chains.
- L2 string-keyed SUBSCRIPT: `o["k"]` with RUNTIME keys (vocab and
  verb-dispatch maps need dynamic keys; member syntax can't).
- L3 `value f()` BY-VALUE RETURN — host fns and script fns (S0: loud
  error today). `var x = ui::get(...)` and `var lookup(string w)`.
- L4 `value &` PARAMETER retag/mutation (S0: loud error today) —
  systems mutate caller bags; carrier assignment deep-copies, so
  by-ref is the only mutating pass.
- L5 map-kind ITERATION: range-for over object kind (keys + values —
  save, inventory, debug dumps).
- L6 existence/kind predicates usable in script (has-key / isset-like;
  the is_*() family) — php:: parity (`php::array_key_exists`,
  `php::isset`-alikes) counts if it covers the map kind.
- (Recorded, not gating: `value = std::string` ingestion; the missing
  %s-vs-string -Wformat diagnostic — both stay queued residues.)

### Wave E — engine/ui:: feeders

- E1 nested values in `.world`: world_text serializes array/object
  props (today wt_value = flat int/bool/string). Round-trip law (G2)
  extends to nested shapes. A real hub feature (madcide needs it too).
- E2 script bag publics on ui::: get/set entity bag values, containment
  enumeration (contents list), entity-by-name already exists. Thread-
  safety contract stated (Phase-1 single-thread confinement carries).
- E3 sessions↔actor binding (session names an actor entity; wizard tier
  = existing builder key/levels).

## Slices

Targeted tests per slice; ONE fulltest at the merge wave (owner law).
Every src/include commit carries the four rule trailers.

- **A0** ✅ this plan (brainstorm approved).
- **A1** Wave-L probe matrix on container → confirmed-gap list recorded
  here; then L1–L6 as individual feeder slices (each: reducer, fix at
  the deepest layer, integration test; php::var_dump/print_r cover the
  map kind as they land).
- **A2** E1 nested `.world` values (+ unit tests + round-trip gate
  extension). **A3** E2/E3 ui:: publics.
- **A4** Converter + checked-in `adventure.world` + a spot-check gate
  (counts: 185/70/623; a known entity's bag deep-equals its YAML).
- **A5** Game skeleton: loop, vocab, intent, travel interpreter,
  describe/abbrev view, LCG + `seed` verb. Parity gate live on the
  motion-only log subset.
- **A6** Verb systems wave 1 (take/drop/open/lock/on/off/inventory/
  drink/eat...) — parity subset grows.
- **A7** Dwarves + pirate + combat (RNG parity proves here), knife,
  axe. **A8** Liquids/plant/troll/bear/specials (3), hints, listen,
  read, magic words, zzword.
- **A9** Lamp battery, closing/endgame, scoring, death/reincarnation,
  turn thresholds.
- **A10** Full corpus green incl. `win430`; save/load stitched-
  transcript gate (G2 pattern) over the game; `.timeout` fixtures as
  needed; corpus gate wired into fulltest.
- **A11** Scoped /dupaudit (madcdis + ui:: + the game) → merge wave
  (fulltest + EXE lane once) → **RELEASE + three-platform master
  promotion** (the boundary).

Gates: **G-A** parity gate script (runs each in-scope log through
`bin/madc examples/adventure/advent.mad`, byte-diff vs `.chk`,
capability-staged skiplist that only ever SHRINKS — the ratchet).
Phase-1 gates (round-trip, one-write-path) keep running.

## Out of scope (deliberate, not forgotten)

Multi-player serving (seams only), script-attached verbs (waits for
eval/exec), second-client renderers (post-parity north star), levels
1–4 rendering, binary-save parity logs, oldstyle mode.
