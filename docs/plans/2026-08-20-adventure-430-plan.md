# Colossal Cave Adventure (430-point) in madc — implementation plan

**HANDOFF (2026-08-21, session #111). READ THIS BLOCK FULLY, then the
SETTLED section, before any work.**

**⚠️ THE RELEASE BOUNDARY IS MET (@4b5d6080, pushed, branch
feature/track7-hub-projections-claude): ALL 94 IN-SCOPE REFERENCE LOGS
— win430 INCLUDED — REPLAY 100% BYTE-IDENTICAL, and the A10 corpus
gate enforces it in fulltest** (adventure_parity.sh: 3 fragments + 94
whole logs + negative controls, 99s; the 13 excluded logs are
documented by class in examples/adventure/tests/corpus/EXCLUDED.md —
11 binary save/resume + 2 reference-CLI-flag logs; exclusions shrink
only by file removal, no runner skip logic). Slices A5–A10 are ALL
LANDED: bear chain, cave closing (clock1/clock2 snapshots, stash
semantics via a `stashed` bag flag with obj_state returning the
encoded negative, obj_state_equals for the one transparent site),
lampcheck, the gem/cavity law (IS_FOUND is literally prop==0), the
null location (LOC_NOWHERE = entity 0; converter no longer emits it),
the hover pin, verbs-with-objects defaults, the empty-word motion
path. OWNER-DIRECTED ELEGANCE PASS also landed: value.index() carrier
feeder (+ php::array_search adopts the core), two more cir receiver
fixes (chained-subscript slot address; empty() exemption unwraps
value&), quip()/verb_quip()/one_of()/fixed_quip() shapes,
data tables over ladders — adv_actions.inc (1637) is now smaller than
actions.c (1677); game total 4297 vs the reference's 4681 in-scope.

**THE MODERNIZATION PASS (owner directives, 2026-08-21, verbatim
intent: "we should be using this example project as a way to show off
as many features of how madc makes coding easier, and more compact,
and more like Python/PHP/etc than plain old C... we want to modernize
this ancient game, not make the code look like it was ported from
Fortran"). M1+M2+M3 ARE DONE; M4 awaits the owner's shape call:**

M1. **DONE @7b3deb43 (with M2).** All six mergeable `var x; x = <expr>;`
    pairs merged. FINDING (probed, tmp/advprobes/probe_walrus.mad):
    `x := "<literal>"` / `x := y.c_str()` infer **const char\*** (the
    string-literal law), NOT the carrier — on the four mutated
    accumulators += would be a compile error and on the two c_str()
    sites the pointer would dangle into the ring — so all six took the
    owner's stated fallback `var x = <expr>;`. The walrus stays
    preferred wherever inference picks the right kind; no such site
    exists in the game today. The remaining ~110 bare `var x;` are
    out-param fills and stay.
M2. **DONE @7b3deb43.** All 90 `value` spellings are `var` (params
    `var &x` and range-for heads included). Gate: 94/94 byte-identical.
M3. **DONE @6fc67e0c** (+ two enablers): 11 real TUs (adv_*.mad) +
    manifest examples/adventure/advent.cc.json; the ONE include left is
    adv_decls.inc — the cross-TU declaration surface (66 extern globals
    + 78 prototypes; 58 helpers + 7 globals stay module-private).
    adventure_parity.sh drives the project invocation; 94/94
    byte-identical on the first post-restructure run. Enablers:
    - **@9c1329de cir: `extern var` was a silent cross-TU bug** — the
      carrier lane ignored vfEXTERN, every TU owned a private copy of
      an extern var (writes elsewhere read back EMPTY, exit 0); block
      scope even placement-newed over the shared global. Fixed at
      obj_storage_decl/var_decl/translate_block; reducer
      tests/testprojectvalue* pins both scopes.
    - **@c2988ee2 tests: the .helper fixture** — marks a .mad as a
      compilation unit of another test; replaced SIX hard-coded
      include_helper name-skips across the script fleet.
    - **MEASURED RESIDUE:** dev (unpacked) binary live-parses the
      embedded namespace headers PER TU → game invocation 0.7s→5.0s,
      the fulltest A10 gate 99s→8m23s. The forest-packed release binary
      runs the 2-TU ns probe in 0.10s (vs 1.08s dev) — the pack is the
      designed remedy; the follow-up work item is sharing the
      embedded-header parse across TU Programs under --project
      (startup-latency family, R4). Not a per-test timeout; recorded,
      not a blocker.
M4. **DONE @ea488a03 (classes) + @b9c5b1cb (the compiler fixes it
    surfaced) — the OWNER dictated the shape mid-session (2026-08-21):**
    "a player class and a game class... the player object should likely
    be part of the game object, so in the end there's probably only one
    single extern global"; "all these bools should be a single
    bitvector using an enum of bit values... or grouped across a few
    enums... especially if some are player-relative vs. game relative."
    - `class Player` INSIDE `class Game`; `extern Game G;` is the
      program's ONE extern global (M3's 66-extern block deleted).
      NSDMI defaults in the class body (limit = 330, ...); constants
      (M_*/T_*/G_*/P_*) are real enums — no storage.
    - Nine bools → TWO grouped bitvectors: PF_* on Player::flags,
      GF_* on Game::flags, via has/set/clear/put helpers.
    - Module-private state stayed private (ENT, STATE_E, SEQ_NEXT,
      MXSCOR, MOTION_QUIPS, LCG_X).
    - COMPILER FIXES SURFACED (@b9c5b1cb, own commit + reducers):
      (1) file-scope ctorless-class NSDMI never applied
      (tests/testnsdmiglobal.mad); (2) --project non-entry-TU dynamic
      initializers NEVER RAN — one __madc_global_init export, last
      module won (adventure SIGFPE'd on abbnum==0 -> `% 0`); fixed by
      adopting the object-mode per-TU init model, the engine plays
      ld.so's .init_array role (tests/testprojectinit*).
    - Gate: 94/94 byte-identical on the class-based project build.
      Game total 4447 lines vs reference 4681.
    - REMAINING M4 TAIL (open): curated UFCS `.method()` spellings
      where they read better, and (owner taste) whether the adv_state
      accessors (toting/at_obj/obj_state/...) become Game/Player
      methods — the systems (h_*, playermove, dwarfmove) stay free
      functions either way.

**REMAINING before the owner ceremonies:**
1. Merge-wave battery: the @c73a3a4d rerun came back GREEN on every
   stage (rb-20260821-041339.log: fulltest/exe/obj/release/packed/
   headerless all rc=0; 1113 passed / 0 failed — the two earlier RED
   logs at 02:57/03:35 predate the @c73a3a4d fix, don't re-diagnose
   them). ⚠️ BUT new src commits landed AFTER that battery (the
   brace-list literal wave below), so the merge gate needs ONE fresh
   `scripts/remote_build.sh battery` at the actual merge wave — not
   per commit. Targeted validation for the new wave is green: the
   18-test value/array/foreach/php blast-radius subset, the new
   testvalueinit in BOTH the JIT and native-exe lanes, and the 94-log
   parity gate (byte-identical).
   ⚠️ Lesson re-learned: a test's raw exit code is NOT the runner's
   verdict — diff against the .expect fixture.
1c. OWNER-DIRECTED wave 4 — THE ZERO-INCLUDE CONTRACT (@414e5637..@79d6e10f):
   advent.mad + modules now carry ZERO includes, ZERO using, ZERO std::
   (owner: "it's like you completely forgot what madc is"). Compiler
   fixes at the owners: the auto-include scan serves QUOTED USER
   MODULES (system headers stay inert; classify by resolved path); the
   prelude inserts before the first USER-CODE token (module or main —
   auto_include_user_units); print/println gained the C++23 STREAM
   forms (println(stderr, ...) — pass-through FILE sink in rt_dump's
   one writer); madc::getline(value&)->bool line input (ns_madc
   public); php::file_exists (directories count). value-first.md now
   STATES the law (bare print/println/format; a spelling gap indicts
   the compiler). ⚠️ RECORDED LESSON: a std::getline(istream&, value&)
   plain-decl fragment STOLE the namespace-map name slot from
   libstdc++'s template — string getline recursed to stack overflow
   (testofstreamwrite/testmanipview, fulltest 1111/2) — REVERTED; a
   std-side value overload needs overload-import design first.
   Adventure now 4264 lines vs reference 4681; 94/94 byte-identical;
   fulltest 1116/0. REMAINING OWNER ISSUE #2 (NEXT): replace the
   `#include "adv_*.inc"` textual modules with MULTI-FILE PROJECT
   support (--project) — restructure adv_*.inc -> .mad TUs + manifest.
1b. OWNER-DIRECTED ELEGANCE, wave 3 (@bf04d070 + @a6776e1c, pushed):
   brace-list value literals `var ds = { a, b, c };` (+ `var ds{...}`,
   `{}` = empty ARRAY, one-element law, file-scope literals) and the
   chainable `.push(x)` carrier method — one registered row set
   (madarray_push_*) serves both, adv_io's four php::array_push lines
   retired. Three defects fixed en route (all pinned): file-scope
   `value g(7);` silently read null (pre-existing — carrier global
   lane dropped ctor_args); a ref-returning carrier call as method
   RECEIVER loaded the payload word (chain SIGSEGV — class_this_arg
   carrier arm now re-wraps it like keyed subscripts); push externs
   declared with TWO shapes (void* vs long long*) hit
   need_output_extern's first-shape-wins dedup → MIR "mov: wrong type
   memory" (the list lowering now reads emit_symbol_ret_specs, the one
   return-shape owner). Residues: nested brace lists = loud error
   (future lowering); keyed literals `{ k: v }` not spelled yet; the
   carrier surface (size/count/index/push/at/substr/keyed slots/
   literals) still has NO docs/language page.
2. Release + master promotion = OWNER CALLS (release batches
   v0.93+v0.94+Track 7 into the three-platform promotion; VERSION
   stays 0.94.0 until the owner cuts it).
3. Recorded residues (not blockers): L3 value-by-value returns;
   auto-include in user modules; h_listen's positional walk duplicates
   pspeak_prop's (no int-subscript var read yet); php::array_unique's
   inner scan could adopt the index core; python::format vs
   std::format = deliberate spec twins (dupaudit verdict recorded).

**THE WORKING METHOD (keep for any regression):** the frontier sweep —
tmp/frontier.sh on the CONTAINER byte-prefix-scores all 107 logs;
`advent -d` RNG-trace diff vs an instrumented adv_rng.inc randrange;
regenerate the world on the CONTAINER after every converter edit
(rsync clobbers it — regen after every sync, verify the resave fixed
point via tmp/advprobes/probe_world_roundtrip.mad + cmp against
tmp/adventure.world.resaved, scp back before committing).

Where things stand — all pushed on
`feature/track7-hub-projections-claude` @75205e50, working tree clean,
VERSION 0.94.0 (⚠️ NO release until the boundary below; merge timing =
open owner call):

- **A0 plan approved** (this doc). **A1 Wave-L LANDED @163f6908**:
  `bag["k"]` + `bag.k` keyed slots (madarray_key_slot; Perl-model
  vivification — access creates, php::array_key_exists never does),
  `value&` param mutation, object-kind range-for (values, key order),
  php::array_key_exists (one overloaded script name; `_int` only on the
  extern-C shim, the array_push pattern). Contracts pinned in
  tests/testvaluekeys.mad; 147/147 blast-radius subset green.
  **L3 (value BY-VALUE returns) is NOT done** — its recon is in the A1
  STATUS block below (one admission point, but the carrier needs a
  C-VISIBLE slot type; decide with both the script lane and the future
  mangled-direct `madc::value ui::get()` lane in view). Until L3, ui::
  uses `value &out` params (the <ns_madc> convention).
- **A2 LANDED @a1d36959**: .world props carry full value trees as
  single-line JSON (json.hpp bridge in world_text.h detail::); fixed a
  real pre-existing round-trip hole (ambiguous strings now JSON-quote).
  Unit battery extended (test_world_text 7 cases / 111 assertions).
- **A4 LANDED @8b8cc392**: examples/adventure/ = vendored
  data/adventure.yaml (BSD-2 + COPYING) + tools/advent_yaml_to_world.py
  (shape-checked 185/70/623/76/58/10; run with python3 on the
  CONTAINER) + adventure.world CHECKED IN (1717 lines). Verified:
  ui::world_open + ui::world_save re-emits it BYTE-IDENTICALLY.
- **A3 LANDED @917fd1f2**: ui::get/name_of/contents (reads, never
  vivify) + ui::set (one overloaded name) / ui::move routed through
  mutation_context (G4 gate holds). tests/testuibags.mad walks the real
  dungeon from script (travel-rule bags iterate via range-for +
  keyed reads — the game loop's core access pattern, proven).
- **A5 LANDED (2026-08-20, session #110)** — the game runs and is
  BYTE-PARITY GATED. examples/adventure/advent.mad + adv_{state,rng,io,
  vocab,describe,travel,actions,loop}.inc, value-first. Gate G-A
  (fragment stage) = scripts/adventure_parity.sh in fulltest, negative-
  controlled: `opening` (win430.chk's first 579 bytes: welcome/seed/
  east/listings/>>Foof!<</dark Y2) + `overworld` (30-command sweep vs
  the reference binary: motions, BACK both branches, LOOK/CAVE, misses,
  DONT_KNOW/TWO_WORDS, enter-stream, GO-shift, pct forest wandering =
  RNG parity). Converter addenda landed: per-object seq/seq_fixed +
  seq_next (listing-order law), noaction list, travel verbs RESOLVED to
  motion names (⚠️ the YAML rule verbs are WORDS; two id spaces only
  coincided on compass names — the win was invisible until 'building'
  failed). Feeders landed en route: carrier string surface
  (==/!=/+=/substr/length/empty/at + as_integer/as_boolean/as_real/
  is_null; value-first.md RULE minted by owner directive),
  php::strtolower/strtoupper/ucfirst/ctype_digit/intval, ui::create,
  cstr catch-var typing, keyed-subscript method receivers, value&
  unwraps (method receivers + format args). A7/A8/A9 walls are LOUD
  exits (dwarfmove dflag>=1, special travel, croak) — never silent
  wrong output. Reference oracle binary builds on the CONTAINER at
  /workspace/madc/tmp/open-adventure (libedit-dev installed; rebuild
  with `make advent` after container rebuilds).
- **NEXT = A6, verb systems wave 1**: take/drop/open/lock/on/off/
  inventory/drink/eat + the object-word analysis (the reference's
  action() front half: HERE checks, liquids, GO_UNKNOWN paths) — then
  extend the fragment corpus with verb-using walks (keys/lamp/grate =
  the cave entry sequence) and start whole shallow logs.
- Working facts: reference clones live at /workspace/open-adventure
  (data + tests/*.log/.chk oracle + the C source), /workspace/advent.cpp,
  /workspace/advent.py. Probes live in tmp/advprobes/ (run_probes.sh;
  probe_ui_bags.mad shows the E2 patterns). Build/test ONLY via
  scripts/remote_build.sh (sync build / unittest; probes travel by scp
  — tmp/ and tests/ additions are NOT rsynced back; adventure.world was
  generated on the container and pulled back by scp). Targeted tests
  per slice; fulltest ONCE at the merge wave. Commits touching src/ or
  include/ carry the four rule trailers; commit messages with backticks
  go via `git commit -F <file>`.
- Deferred residues (recorded, not blocking): L3; implicit numeric
  coercion from a keyed read stays undefined BY DESIGN (`long n =
  bag["k"].as_integer()` / php::intval are the defined routes);
  `cout << bag["k"]` streaming admission unverified (printf/println
  coercion IS covered); integer-subscript value elements keep
  string-first typing (use range-for for value elements); the missing
  %s-vs-string format warning (pre-arc residue); the std::print
  auto-include scan does not fire for identifiers inside user #include
  modules (suppress_auto_include_scan conflates "system header serving"
  with "any include in flight" — advent.mad carries the explicit
  <bits/std_format> include); the streaming format lowering emits
  literal segments BEFORE evaluating args (a side-effecting format arg
  interleaves; std::format evaluates args first).

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

**A1 STATUS: L1+L2+L4+L5+L6 LANDED @163f6908** (probe matrix green,
tests/testvaluekeys.mad pins the contracts, 147/147 blast-radius subset
green; testvaluecount's iterate-zero pin superseded by L5 — the
count-vs-bound lesson now pins on the string kind). **L3 (value
by-value returns) remains** — recon complete (2026-08-20): ONE decision
point (class_return_via_retbuf, user-class-gated; every consumer
funnels through function_retbuf_class), but the carrier lacks a
C-VISIBLE POINTEE TYPE for the slot: values lower to aligned long[]
buffers with NO emitted struct tag, while retbuf_param/class_ptr_type/
ExternParam all render `struct <tag> *`. Design options: (a) synthesize
a one-time `struct __madc_value { _Alignas(16) long long w[4]; }`
definition + a carrier arm in append_decl_type_specs (uniform: all
lanes + arm64 sret ABI classification get the right size — preferred);
(b) void*-shaped retbuf special-cases per call lane (x86-64-honest for
script-emitted callees only). Copy-in = madarray_construct_value
(registered external ctor — class_ctor_call's emit_symbol arm); slot
temps already declare via the array model with cleanup; locals
auto-destruct via the cleanup attribute. NOTE the external lane is L3's
OTHER half: a mangled-direct host `madc::value ui::get(...)` needs the
same admission and then rides the existing external-sret machinery
(the std::string return path) — decide (a)/(b) with BOTH lanes in
view. E2's ui:: API shape depends on this; if L3 slips past A3, design
E2 with `value &out` and add the by-value overloads when L3 lands. Deferred residues
recorded: numeric extraction from a keyed read (`long n = bag["k"]`)
is not a defined coercion — spell php:: converters/var destinations;
`cout << bag["k"]` streaming admission unverified (printf coercion IS
covered).

**A1 PROBE RESULTS (2026-08-20, container, tmp/advprobes/, L0 control
green):** ALL SIX CONFIRMED. L1 member access = PARSER error
("Unidentified member 'name' in 'array'" — `.name` resolves against the
carrier class's method registry, no object-kind lowering). L2 subscript
= CIR lowering feeds the string key to the INTEGER-index getter
("using pointer without cast for integer type parameter") and the
result is not an lvalue — needs kind-dispatched get + a write-through
form. L3a script `var f()` return = the banked lowers-to-int defect
("returning pointer without cast for integer result"); C++ analogue is
class-by-value return (gcc: hidden sret pointer; in-tree precedent:
`__retbuf`). L3b CONTRAST CONTROL PASSES (php::print_r capture) — the
gap is script-defined/mangled-direct returns only. L4 `value&` param
assignment does not route through the retag/assign runtime family
("assignment of incompatible value" at the callee). L5 blocked by L2,
retest after. L6 `php::array_key_exists` not in ns_php (php-parity law:
mirror PHP exactly — key first, `bool` return).

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
  **A5 recon banked (2026-08-20, from the reference's I/O layer):** the
  transcript rhythm is `\n` + text + `\n` for EVERY message (vspeak's
  leading blank; empty/null messages print NOTHING) and `\n` + `> line`
  for every input (prompt echoed with the line under non-tty — the
  transcripts START with a newline byte). speak() carries the
  floor→ground rewrite when outside, %d/%s/%S(pluralize-by-previous-%d)/
  %V substitution. Vocab: first two whitespace words, 5-char
  case-insensitive prefix match, lookup order motions→objects→actions→
  zzword→numeric. describe: short unless abbrev%abbnum==0 or no short;
  dark→PITCH_DARK (unless forced); TAME_BEAR prefix; LOC_Y2 25%
  SAYS_PLUGH. listobjects: only when lit; increments abbrev; treasure
  first-sight tally + RUG/CHAIN/EGGS specials; STEPS suppressed when
  toting nugget, STEPS state by fixed-side. preprocess: enter water/
  stream; OV→VO swap; bare GRATE→DEPRESSION/ENTRANCE by region;
  water/oil plant/door→pour; cage bird→carry; typeless word[0]→motion.
  init: abbnum=5, limit=330, clock1=30 clock2=50, chloc=LOC_MAZEEND12
  chloc2=LOC_DEADEND13, dwarves from dwarflocs, tally=treasure count,
  treasures start NOTFOUND (state -1 semantics beside the state LABELS
  — model as a per-object `found` flag reading the C prop=-1/stashify
  contract in bag terms).
  **⚠️ OBJECT LISTING ORDER (A6 converter addendum):** C's atloc lists
  PREPEND on drop — listing order == placement timestamp DESCENDING.
  Init's effective order: plain objects ascending objnum, then
  two-placed ascending (their drop loops run high→low, two-placed loop
  first). The converter must emit a per-object `seq` prop replaying C's
  init drop order (two-placed desc objnum first, then plain desc); the
  game bumps `seq` from a world counter on every move and lists
  contents by seq descending. ui::contents order alone is append-order
  and diverges after the first mid-game drop.
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
