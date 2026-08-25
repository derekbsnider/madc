# madcide — hub Phase 2 (Track 7.2 consumer)

**Status:** execution plan, cut 2026-08-25 (session 130). Consumes Track 7.2
R1–R5 as shipped (see
[2026-08-24-ui-interaction-rework-and-texteditor.md](2026-08-24-ui-interaction-rework-and-texteditor.md));
the phase gate is the hub doc's, unchanged:

> Gate: one buffer edited through an entry lens with undo; diagnostics pane
> and outline are projections of live compiler data; a keybinding profile
> swap re-renders without touching projections.
> ([2026-08-20-data-hub-projection-rendering.md](2026-08-20-data-hub-projection-rendering.md), Phase 2)

**Owner directive (2026-08-25, post-R5 review):** the IDE uses JOE/WordStar
key bindings (`^K`-prefix chords: `^K S` save, `^K Q` / `^K X` quit forms,
`^K B` mark, …) and bindings MUST be configurable — the hub doc's
keybinding-profiles-as-wants seat. The TUI key adapter grows chord-prefix
support **as a bindings table mapping key sequences to action names — data,
per profile — never a second hardcoded map**. vised's Pico-style single
chords stay as they are (fine for an example editor).

**Folded in (owner-directed):** the R3 residue — the script-verb sibling
design (re-entrancy formalized, code-entity key-gating, compile-once
bodies) — designed here (§ IDE-5) and implemented inside this arc's verb
work.

## Standing constraints (inherited, not re-decided)

- Adventure oracle 94/94 + 3 fragments byte-identical in every merge wave.
- testlineed / testvised transcripts stay byte-identical — the texteditor
  is a shipped example; madcide REUSES its machinery, never forks it.
- Hub write path (check-hub-write-path), engine purity
  (check-engine-app-purity), dialect-lean, value-first: all gates stand.
- Seam law: every new binding surface takes values / entity handles and
  returns value-shaped results.
- Thread contracts stated per addition (each slice below names its own).
- Targeted tests per slice; ONE battery at the merge wave. No version bump
  (merges of slices; releases mark complete features).

## The slices

### IDE-1 — bindings-as-data: the chord key adapter

The owner-directed centerpiece. All in the model/provider seam R5 built:

- **One key-spelling owner, both directions.** `ui_key_name()`
  (src/ns_ui.cpp) moves into `tui_model.h` as `tui_key_name(tui_keyev)` and
  gains the reverse `tui_key_from_name(text, tui_keyev&)`; ns_ui adopts it.
  The spelling vocabulary is the one testvised already pins: `"^s"`,
  `"up"`, `"enter"`, printable characters as themselves.
- **`tui_bindings`** (tui_model.h): a table of key SEQUENCES → action
  names. A sequence is space-separated key spellings (`"^k s"`, `"^k b"`,
  `"^s"`); generic length, not hardcoded two. Built from a value object
  (`{"^k s": "save", ...}`) — data, per profile. Validation is loud and
  at build time: a sequence must start with a NON-printable key (printable
  heads would swallow typing), and no bound sequence may be a proper
  prefix of another (deterministic resolution; JOE's `^K` is only ever a
  prefix). An invalid table is refused whole.
- **Chord resolution in `apply_keys`**, ahead of everything else: a key
  that heads a bound sequence enters pending-chord state; subsequent keys
  extend it; a completed match emits the new event kind
  **`action`** `{event:"action", action:<name>, seq:<spelling>}`; a
  completed miss emits `action` with an EMPTY action and the seq (the app
  may show "Unbound: ^K z"); `esc` cancels a pending chord silently;
  `resize` passes through without disturbing it. Bindings win over the
  built-in interpretations (a profile that binds `up` owns `up` — profiles
  simply shouldn't); unbound keys fall through to the existing behavior
  unchanged, so a session with no table is byte-identical to R5.
- **Surface:** `ui::tui_bind_keys(t, table)` — installs (replacing) the
  session's whole table; an empty object clears it. A profile swap is one
  call. `false` + stderr reason on an invalid table.
- Pending-chord state is adapter state and lives in the model beside
  focus (same thread-confinement contract as tui_model — one session, one
  thread).

Gate: test_tui_model grows the chord battery (round-trip spelling, chord
match, multi-key pending, unbound miss, esc cancel, resize passthrough,
printable-head and prefix-conflict rejection, precedence over choice
navigation, table swap); testvised stays byte-identical (no table bound).

### IDE-2 — undo: buffer history on the text component

The entry-lens edit path (edit-node hints in, `text_insert`/`erase`
back) exists since R4/R5; the gate's missing word is **undo**.

- `text_buffer` grows history exactly as its header anticipated: a
  checkpoint is a copy of the pieces vector (the add buffer is append-only,
  so old snapshots stay valid); undo restores the previous snapshot.
  Each checkpoint carries an OPAQUE `madc::value` meta payload — the
  application's caret (and anything else) rides with the state it belongs
  to, and the engine never learns what it means.
- Surface: `ui::text_checkpoint(w, doc, meta)` and
  `bool ui::text_undo(meta_out, w, doc)` (false on empty history). Writes
  ride `mutation_context` like every text_* verb. Redo is a named seat,
  not in this slice. History is runtime-only, like the component itself
  (world_save does not carry it; the w verb persists the file).
- Checkpoint CADENCE is the application's: madcide checkpoints per
  semantic event batch (one text event = one undo step; one verb = one
  step), which is what §7.5 coalescing already shaped the events for.
- Thread contract: history is component data under the same
  session-confined single-thread contract as text_buffer (stated in the
  header).

Gate: test_text_buffer grows checkpoint/undo asserts (mirrored against the
std::string oracle); the madcide headless test pins undo through the app.

### IDE-3 — compiler data as data: `madc::diagnostics` + `madc::outline`

The meta-level demand (hub demand 3): the IDE's panes are projections over
the compiler's OWN data — which already exists structured
(`Program::diagnostics`, severity/phase/message/file/line/column; the
parse tree carries file/line/col everywhere). What is missing is only the
script-side surface, and its home is `madc::` (the runtime eval API — the
compiler surface):

- `madc::diagnostics(out, source, filename)` — compile (never execute)
  `source` in a child Program under the runtime-eval policy machinery,
  CAPTURING the diagnostics vector as a value array of
  `{severity, phase, message, file, line, column}` objects instead of
  printing them. Empty array = clean.
- `madc::outline(out, source, filename)` — same compile, walking the
  parsed program for its top-level shape: a value array of
  `{kind, name, line, column}` (kind ∈ function / class / global — from
  the parse structures, names not ids at this boundary, per the uinode
  bridge precedent).
- Both are read-only over their own child Program; thread contract =
  the eval machinery's existing confinement, restated at the declaration.
- These serve ANY tool, not just madcide — a linter, a doc generator,
  a test harness probing compile errors as data.

Gate: reducer tests with `.expect` — a source with a known error yields
the structured diagnostic (message/line/column pinned); a clean source
yields an empty array; a two-function+class source yields the pinned
outline. Negative control: the error test also pins that NOTHING was
executed.

### IDE-4 — examples/madcide: the IDE over all of it

A consumer, as the hub doc frames it — buffers are the texteditor
machinery, panes are projections, profiles are wants:

- **Reuse, never fork:** vised_core.inc splits into the shared editor
  event core (`examples/texteditor/editor_events.inc`: es helpers,
  line_of/caret_to_line, derive_access/refuse_edit, setup_editor, do_verb,
  search state, cut/paste, the modeless edit-event application — text,
  movement, backspace/del/enter) and the vised-specific remainder
  (the Pico `^`-key dispatch, compose_tree, run_visual). vised includes
  both and stays transcript-identical (testvised is the refactor's gate).
  madcide includes the shared core from `examples/madcide/` and layers its
  OWN dispatch: `action` events (from the bindings table) map to the same
  verbs and primitives; unhandled events delegate to the shared core.
- **Panes as projections:** heading / edit window (flexible) / a lower
  pane that is either the DIAGNOSTICS list (items composed from the
  stored `madc::diagnostics` result; choosing one moves the caret to its
  line) or the OUTLINE (a choice over `madc::outline` symbols; choosing
  one moves the caret) / status / menu. The lower pane's content is a
  projection of stored compiler data on the world — composing it never
  calls the compiler; a `check` action refreshes the data (and save
  refreshes it too). Choose events carry the 1-based option; the app maps
  option → its stored array row (no per-symbol action names).
- **Profiles as data:** `examples/madcide/profiles/joe.keys` (default) and
  `pico.keys` — plain data files, one binding per line (`^k s save`),
  loaded by the app into the value table `ui::tui_bind_keys` takes.
  Starting JOE profile: `^k s`/`^k d` save, `^k x` save-and-exit, `^k q`
  quit (the modified-quit refusal stands), `^c` discard-quit, `^k b` mark,
  `^k y` cut, `^k c` paste, `^k f` find, `^k r` check (refresh
  diagnostics), `^k o` toggle outline/diagnostics pane, `^k p` swap
  profile. The pico profile maps the same ACTIONS onto vised-style single
  chords. Actions are the stable vocabulary; profiles only respell keys.
- **Profile swap = gate 3:** `^k p` rebinds and repaints; the projection
  composition path is untouched by construction (the headless test pins
  the same tree before/after a swap).
- Verbs: the texteditor's w/q/q! through the same registry, plus
  madcide's own script verbs where turn-cadence logic wants them (x =
  save-and-exit; check wraps the diagnostics refresh) — every one madc
  source (the tracer clause stands).

Gate: `tests/testmadcide` (headless, `.expect_quiet`) drives the app's
event application with the exact value shapes `ui::tui_event` produces —
action events from chords included — pinning: an edit + undo restoring
text AND caret (gate 1); a broken buffer's diagnostics pane rows and a
choose moving the caret (gate 2, plus outline); a profile swap changing
key dispatch while the composed tree stays identical (gate 3). A real-pty
vised-style smoke run of madcide itself. Adventure oracle + full battery
green at the merge wave.

### IDE-5 — the script-verb sibling (R3 residue, designed here)

Three parts, implemented inside this arc's verb work:

1. **Compile-once bodies.** Today every `invoke` of a script-kind verb
   (and every script check) hands `verb_def::source` to the injected
   executor, which re-lexes/re-parses/re-compiles it as a fresh eval unit.
   The fix lives at the DEEPEST seam that owns compilation — the
   runtime-eval machinery behind `Program::runtime_eval_source` — as a
   compiled-unit cache keyed by (source text, wrapper form, policy):
   the verb registry stays compilation-ignorant (verbs.h carries source
   text as identity — rebinding with new text is a new key), ns_ui stays
   a thin injector, and every `madc::eval_*` user benefits, not just ui.
   Eviction: cache entries are per-Program and die with it; a rebound
   verb's old entry ages out by keying on the text itself.
   Thread contract: the cache shares the eval machinery's confinement.
   Gate: a counter (`runtime_eval_compiles` in the stats surface) pinned
   by a test that invokes one verb twice and sees ONE compile.
   - This is also the recorded consolidation point for the app-level
     DupFamilies (adventure_pilot_tick ×8, prose_enumerate_rule ×3,
     lineed_arg_parse ×3): cheap bodies make a shared SCRIPT PRELUDE
     honest — a world-level prelude source bound once
     (`ui::bind_prelude(w, source)`), compiled once, visible to every
     body's unit, so shared helpers live in ONE .madv-style file instead
     of being pasted per body. The prelude is part of the cache key.
     Consolidating those three families is follow-up work that lands on
     this mechanism (tracked in the KG), not part of this arc's gate.
2. **Re-entrancy, formalized.** The Phase-1 contract (verbs do not
   re-enter `ui::act`) becomes ENFORCED, not stated: the world session
   marks verb-execution-in-progress; `ui::act`/`invoke` entered again on
   the same world refuses loudly with a stable reason string. The
   deferred full arc (queued follow-up invocations) keeps its seat.
   Gate: a reducer verb that tries to re-enter and pins the refusal.
3. **Code-entity key-gating.** The hub's Decided text: "defining or
   editing code entities is itself key-gated." Phase-1 shape: a world may
   carry a binding REQUIREMENT (the existing keys+levels requirement
   machinery — no new condition kind); when set, `ui::bind_verb` /
   `ui::bind_check` / `ui::bind_prelude` evaluate it against the
   session's credentials and refuse loudly without them. Unset = open
   (today's behavior, unchanged for every existing caller).
   Gate: a reducer that sets the requirement, fails to bind keyless,
   grants the key, binds.

### Sequencing

IDE-1 → IDE-3 → IDE-2 → IDE-4 → IDE-5 (parts 2–3 may land beside IDE-4's
verb work; part 1 whenever ready — it gates nothing in IDE-4 but makes it
better). Battery ONCE at the merge wave; dupaudit scoped to
tui/texteditor/madcide/eval before the merge.

## Deferred (seats held)

Redo; syntax highlighting (a projection-hints question — spans as data,
never renderer-side parsing); multiple buffers/windows; the build/run
integration (live build diagnostics as reactive propagation — hub demand
8's fuller form); mouse; UTF-8 cell width and the other R5 residues where
they stand; stored-world script verbs / moddable deployments (the full R3
arc); per-edit-node scroll keyed by subject (kills DupFamily
tui_line_scan).
