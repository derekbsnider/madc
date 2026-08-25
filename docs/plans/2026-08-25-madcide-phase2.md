# madcide — hub Phase 2 (Track 7.2 consumer)

**Status: EXECUTED 2026-08-25 (session 130, same session as the cut).**
IDE-1 through IDE-4 landed in full; IDE-5 landed parts 2+3 (re-entrancy
enforced, code-entity key-gating), with part 1 (compile-once bodies)
re-scoped on a measured blocker — see its section. The hub-doc Phase-2
gate is MET, pinned headlessly by `tests/testmadcide` and re-verified on
a real pty (madcide AND vised). As-executed notes live inline in each
slice below; residues at the end.

**Was:** execution plan, cut 2026-08-25 (session 130). Consumes Track 7.2
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
   **MEASURED BLOCKER (found during this arc, 2026-08-25): a compiled
   unit cannot simply be cached, because the eval ctx is BAKED AT
   COMPILE** — the s128 ctx design folds every binding read to a
   literal (`CirBuilder::baked_cstr_constant`; "the binding is a
   read-only snapshot"), so a cached body would freeze its FIRST
   invocation's `arg`/`actor`/`target` forever. Compile-once therefore
   NEEDS runtime-bound ctx first: a stable per-Program ctx parameter
   block the module imports by symbol (the MIR import table binds host
   addresses; per-invoke the host re-fills the block), replacing the
   baked-constant fold for cached units. THEN the cache lives at the
   deepest seam that owns compilation — the machinery behind
   `Program::runtime_eval_source` — keyed by (source text, wrapper
   form, policy): the verb registry stays compilation-ignorant, ns_ui
   stays a thin injector, every `madc::eval_*` user benefits.
   Eviction: entries are per-Program and die with it; a rebound verb's
   old entry ages out by keying on the text itself. Thread contract:
   the eval machinery's confinement. Gate: a compile counter pinned by
   an invoke-twice test. Its own slice — it gates nothing in this arc.
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

## As executed (2026-08-25, session 130)

Landed in plan order, targeted-green per slice, one battery at the wave:

- **IDE-1** as designed, plus one design correction the unit battery
  forced: chord sequences are LETTER-CASE-INSENSITIVE (JOE's `^K S` ==
  `^K s`), owned by `tui_bindings::seq_spelling` — key EVENTS keep
  `tui_key_name`'s exact spelling. The pty smoke gate now binds
  `"^k s"` and pins the action event with the two chord bytes arriving
  in separate read batches. test_tui_model 174 asserts (was 100).
- **IDE-3** surfaced two pre-existing defects, both fixed at their
  layers: (a) a later tokenize session's tokens inherited the PREVIOUS
  unit's ambient file:line (TokenBase ctor statics; getRealToken's ==0
  backstop can't fire on a stale nonzero stamp) — `_tokenizer_init`
  now resets the ambient position; user-visible in `--project` TU2+
  error lines, reducer `tests/testprojecterrline`, own commit;
  (b) diagnostics RENDERED through the engine stream even with a
  per-Program sink (Program::error() prefers the engine by design —
  engine-owned IO) — capture is a thread-local `DiagnosticRenderMute`
  on the two render owners (print_diagnostic, throwbuf::sync);
  recording is never muted. Positions byte-match the file-based
  oracle. Reducer `tests/testcompilerdata` (`.expect_quiet` = the
  capture proof).
- **IDE-2** as designed (`text_buffer` history exactly as its header
  anticipated); the checkpoint payload is `{caret, modified}` so undo
  restores document + interaction state together. One dialect lesson:
  a bare `a && b` is int-typed (C), so the payload's flag is stored
  through a `bool` variable — strict `as_boolean()` reads it back.
  test_text_buffer 145 asserts (was 128).
- **IDE-4** as designed. The vised split (`editor_events.inc`) kept
  testvised/testlineed byte-identical; madcide's `x`-verb became
  `save_quit` in the DISPATCHER (two verbs from the app — zero
  duplicated verb logic, no re-entrancy). Headless gate
  `tests/testmadcide` pins all three gate clauses (the profile-swap
  tree comparison is a straight `value == value`); madcide AND vised
  re-verified on real ptys. Scoped dupaudit found ONE family born this
  session — vised/madcide composing the same four projection rules —
  consolidated same-session into shared compose helpers
  (`compose_heading/edit_node/status/menu` in editor_events.inc).
- **IDE-5** parts 2+3 as designed (`_invoking` latch in
  verb_table::invoke — checks at dispatch run BEFORE the latch;
  `ui::bind_require_key` arming the bind gate over the session's
  effective credentials). Reducers `tests/testreenter` (the refusal is
  the nested RESULT) and `tests/testbindgate`. test_verbs 69 asserts.
  Part 1 re-scoped: see the MEASURED BLOCKER note in its section (the
  baked eval ctx) — runtime-bound ctx is the prerequisite slice.

**Residues (named):** compile-once bodies behind the runtime-bound ctx
block (IDE-5 §1 — the DupFamilies adventure_pilot_tick /
prose_enumerate_rule / lineed_arg_parse still consolidate there via the
script prelude); the two editor LOOPS (run_visual / run_ide) stay
parallel structure — consolidation point = a loop driver taking
compose/apply callbacks once dialect fn-pointer ergonomics are proven
here; the ev_* event builders duplicated across testvised/testmadcide
(test-local, pinned by the events contract); outline kinds beyond
`function` (classes/globals — extend the same pending_funcs-style walk);
a deep-check tier for madc::diagnostics (CIR build errors surface only
at execution today); madcide editable re-derivation on mid-session
flips (inherited from R5); profile files carry no savequit binding in
pico (actions need not all be bound — by design).

## Owner review (2026-08-25, post-ship) — THE NEXT ARC (madcide v2)

The owner reviewed the shipped madcide and gave three directives. This
section is the execution input for the next session; nothing here is
implemented yet.

**1. madcide is a TOOL, not an example.** Relocate `examples/madcide/`
→ `tools/madcide/` (git mv; fix the include paths, the default
`profile_dir`, tests/testmadcide's includes, and the doc references —
grep `examples/madcide` across scripts/tests/docs). The texteditor
stays an example; madcide keeps including its shared
`editor_events.inc` (the libmadcedit packaging question, ROADMAP 8.1,
stands).

**2. Bind the FULL basic editing set.** The shipped joe profile only
bound `^K` chords — the plain-editor coverage regressed (arrows worked;
the WordStar diamond and the direct editing keys did not). The owner's
list (bindings stay data in `profiles/joe.keys`; actions the engine or
app must GROW are marked):

- File/exit: `^K X` save+exit ✓ · `^K D` and `^K S` save ✓ · `^K R`
  insert/include file (NEW action + a filename prompt; the shipped
  `check` moves off `^K R` — take `^K E`, a data choice) · `^C` abort
  without saving ✓ · `^K H` help window (NEW — compose the pane FROM
  the loaded bindings table: help is a projection of the profile) ·
  `^K Z` open shell (NEW — needs `ui::tui_suspend/tui_resume` publics
  + target support: leave alt screen/restore termios, run $SHELL,
  re-enter + full repaint).
- Movement: `^B`/`^F` left/right · `^P`/`^N` up/down · `^A`/`^E` line
  start/end · `^Z`/`^X` prev/next WORD (NEW: word motion — put
  `word_left/word_right` on text_buffer beside find, + `ui::text_word_*`)
  · `^U`/`^V` PgUp/PgDn · `^K U`/`^K V` top/end of file (NEW actions) ·
  `^K L` go to line (NEW — a numeric prompt). Motion actions should
  DELEGATE to the existing edit_key vocabulary (bind "^b" → action
  "left" etc.; the dispatcher synthesizes the key event) — no second
  movement implementation.
- Editing: `^D` delete char (= del) · `^Y` delete line (NEW) · `^W`
  delete word right (NEW) · `^_` undo / `^^` REDO — the key parser
  currently DROPS 0x1c–0x1f: extend tui_keyparse (0x1c..0x1f → ctrl
  '\\' ']' '^' '_'; tui_key_name already spells "^"+ch;
  tui_key_from_name must accept the non-letter ctrl spellings), and
  REDO must land in text_buffer history (undo stack + redo stack; a
  checkpoint clears redo; undo/redo need the CURRENT meta passed in so
  the opposite stack restores caret — extend
  `ui::text_undo(meta_out, w, e, now_meta)` + new `ui::text_redo`;
  keep the simple undo overload for the existing pins).
- Blocks: `^K B` begin · `^K K` end (NEW: es carries mark=begin +
  bend=end; the shared compose_edit_node's selection becomes
  [mark, bend-else-caret] — vised never sets bend, unchanged) · `^K C`
  copy block to caret · `^K M` move block to caret · `^K Y` delete
  block (clip = its text). These REPLACE the shipped mark/cut/paste
  actions in madcide (vised's Pico set untouched).
- Search: `^K F` find ✓ · `^L` find NEXT (NEW — repeat es "search"
  from caret+1 with wrap). Generalize madcide's prompt into one prompt
  mode (find / goto-line / insert-file); vised keeps the shared simple
  search.

testmadcide updates with the new vocabulary (its mark/cut/paste pins
change with the block model — it is this arc's own test). pico.keys
respells the subset that fits single chords (data).

**3. The AST-in-memory design (the owner: "the more important part").**
madcide, editing C/C++/madc sources (.c .cpp .cc .h .hh .mad .madc
.inc .madh .mc11, …), should PARSE the file into memory ON LOAD; open/
save PROJECT uses the same cc.json `--project` consumes; and the
PROJECT'S PROGRAM AST is MAINTAINED IN MEMORY — this is how colour
syntax highlighting works, and the status line's code info. Mapping to
machinery (the meta-level dogfood, hub demand 3 — madcide browsing the
compiler's own forest):

- **IDE-6 — persistent parse handles**: today madc::diagnostics/outline
  throw their child Program away. Grow a HANDLE surface (open/refresh/
  close a live child Program per TU; a MadcCompileGroup for a project)
  so the AST persists between queries; parse-on-load by extension;
  re-parse cadence = on save/idle first (true incrementality later).
  Status-line info = enclosing function/class at the caret (an
  outline-at-offset query over the retained tree — MC11-IR tokens carry
  file/line/col, the source of truth).
- **IDE-7 — colour**: tui_attr (normal/reverse today) grows a small
  colour palette; the VT100 target emits SGR; highlight SPANS arrive as
  edit-node PROJECTION HINTS (spans as data — classification from the
  retained tokens/AST; the renderer never parses).
- **IDE-8 — project + multi-buffer**: open cc.json → TU list pane,
  per-TU buffers/parse handles, save project writes the same json;
  this is where `^K E` becomes JOE's edit-file and check moves again.

Recon for the item-3 brainstorm (both passes — the repo record and
the Turbo-C→JetBrains IDE-architecture survey) is banked in
[2026-08-25-madcide-ast-brainstorm-recon.md](2026-08-25-madcide-ast-brainstorm-recon.md);
its Part C is the brainstorm agenda.

Open question noted for IDE-6: whether the handle API lives in madc::
(beside diagnostics/outline — likely) and how it composes with the
runtime-eval child policy; the `^K Z` shell needs `system`/`getenv`
resolution checked (no include/madc/stdlib.h exists — dlsym fallback
declares int returns per the embedded-headers rule).

### As executed — items 1 + 2 (2026-08-25, session 131)

Both directives landed on `feature/madcide-v2-claude`; item 3 (the AST
arc, IDE-6/7/8) is deliberately NOT started — the owner asked to
brainstorm it first.

- **Relocation** as specified (git mv + the reference sweep; testmadcide
  1/1 + a parse-and-usage smoke from the new path).
- **Engine feeders** (one commit, trailers): tui_keyparse accepts
  0x1c..0x1f as ctrl `\` `]` `^` `_` (tui_key_from_name takes the
  non-letter spellings); text_buffer REDO (two stacks; checkpoint
  clears redo; the meta-carrying undo/redo pair `now_meta` so the
  opposite stack restores caret+modified with the document — the
  one-argument undo stays for the old pins, destructive); word motion
  `word_left/word_right` beside find (`[A-Za-z0-9_]`, JOE ^Z/^X duals);
  `tui_target::suspend/resume` (default refuse) + the term_target
  implementation (open/close's mode switching factored into ONE
  enter/leave_grid_mode pair all four callers share; `_saved` stays the
  pre-open state; close-while-suspended skips the double restore).
  Publics: `ui::text_word_left/right`, `ui::text_undo(out,w,e,now)` +
  `ui::text_redo`, `ui::tui_suspend/tui_resume` (resume re-reads the
  size and resets the diff basis — the next render repaints fully).
- **A second chord convention, forced by the pty probe** (own commit,
  the v1 case-rule's sibling): ctrl-held CONTINUATIONS — JOE's `^K ^Z`
  == `^K Z`. New `tui_bindings::cont_spelling` (ctrl+letter → the bare
  letter; ctrl+punctuation stays itself); bind() canonicalization and
  the model's pending extension both ride it. Found live: the probe
  typed ^K ctrl-Z, got "Unbound: ^k ^z", and the typed `echo SHELLMARK`
  landed in the DOCUMENT — the smoke's marker check was fooled until
  the alt-screen counts (h=2/l=2) carried the verdict.
- **The app**: joe.keys = the owner's list verbatim (motion actions
  delegate to the shared edit_key BY KEY-NAME — the dispatcher
  synthesizes the key event; one movement implementation); block model
  mark+bend (shared selection = [mark, bend-else-caret]; vised never
  sets bend — byte-identical); blockcopy shift-adjusts the markers,
  blockmove = one undo step + refuses moving into itself, blockdel
  takes the clip (read-only copies); ONE prompt mode
  (find/goto-line/insert-file) projected by the shared status rule
  (a new arm ahead of vised's search prompt); find stores the pattern,
  `^L` repeats from caret+1 with wrap; undo/redo ride the ONE
  {caret,modified} payload seat (edit_meta/restore_meta); help (`^K H`)
  projects the loaded profile's own lines (profile_line = the one
  line-filter rule parse_keys shares); `^K Z` = tui_suspend +
  `system("exec \"${SHELL:-/bin/sh}\"")` + tui_resume (no getenv — the
  dlsym fallback would hand the pointer back as a long; the shell
  resolves its own fallback); check moved to `^K E`; pico.keys respells
  the single-chord subset (⚠️ ^h/^i/^j/^m can never arrive as ctrl
  chords — those bytes ARE backspace/tab/enter).
- **Gates**: testmadcide v2 pins (every edit undone — the tail's save
  byte-count proves the round trips; all values verified against
  hand-computed offsets before pinning); editor-family subset 6/6;
  test_tui_model/test_text_buffer grown; real-pty smoke of the full
  loop including the shell round trip.

## Deferred (seats held)

Redo; syntax highlighting (a projection-hints question — spans as data,
never renderer-side parsing); multiple buffers/windows; the build/run
integration (live build diagnostics as reactive propagation — hub demand
8's fuller form); mouse; UTF-8 cell width and the other R5 residues where
they stand; stored-world script verbs / moddable deployments (the full R3
arc); per-edit-node scroll keyed by subject (kills DupFamily
tui_line_scan).
