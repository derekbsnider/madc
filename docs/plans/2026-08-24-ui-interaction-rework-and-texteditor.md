# UI Interaction Rework + examples/texteditor (Track 7.2 re-cut)

**Status:** decided with owner 2026-08-24 — execution plan.
**2026-08-24 (later, owner-directed): R1+R3 MERGED AND EXECUTED as THE
EVICTION** — see "R1+R3 as executed" below. The owner found the Phase 1
compiled adventure catalog living in `include/madcdis/adventure.h` and
hard-bound by `src/ns_ui.cpp` — a Rule #7 violation shipped as "pilot
reference code" with a migration IOU. Ruling: fix ASAP, eviction not
relocation. The engine now ships ZERO verbs; the pilot game is madc
source; `scripts/check-engine-app-purity.sh` (negative-controlled, in
fulltest) keeps it that way.
**Design:** [universal-application-interaction-rendering-abstraction.md](universal-application-interaction-rendering-abstraction.md)
(the owner's interaction-layer design, successor to
[rendering-abstraction.md](rendering-abstraction.md)) over the APPROVED
[2026-08-20-data-hub-projection-rendering.md](2026-08-20-data-hub-projection-rendering.md)
(hub layers, demands, access model, pilots — all its Decided items carry
forward unchanged).
**Goal chain:** rework the shipped `ui::` surface to the interaction model
(Context / Affordance / Invocation / Projection-as-data) → `examples/texteditor`
→ **madcide** consumes the result (hub doc Phase 2).

## The decided fork (owner review, 2026-08-24)

**ONE action registry, TWO binding kinds — both first-class and permanent.**

- **native** — compiled host functions (the shipped verb catalog's kind).
- **script-entity** — a verb whose body is madc source stored as a code
  entity, executed through the eval machinery.

This is not an interim: every surveyed mature system converged on exactly
this shape (Emacs C primitives + elisp through one `funcall` — "equal
dignity"; Neovim's one `nvim_*` dispatch for C/Lua/RPC; VS Code's one
command registry + when-clauses; Unreal's one reflection seam with hybrid
authoring; even LPMud keeps driver primitives native). Hot,
semantically-stable primitives stay native (`insert_text` at typing
cadence); turn-cadence domain/mod logic is the script kind's home.

**The anti-drift mechanism is a tracer, not a promise:** the texteditor
phase gate REQUIRES at least one verb executing from madc source through
the same registry and the same structured `Invocation` as the native kind.
A seam with a single implementation is unproven by construction; the
tracer proves it while adjustments are cheap.

**The seam law (applies from R1):** every action binding — `execute` and
availability `check` — takes a structured `Invocation` whose arguments are
`madc::value`s and resource/entity handles, and returns a value-shaped
result. No binding signature may accept or return anything a madc script
could not. Violations are the shim this plan exists to prevent.

## Standing constraints (inherited, not re-decided)

- **Adventure oracle:** `examples/adventure` 94/94 whole logs + 3 fragments
  byte-identical is the standing regression gate for EVERY phase. The
  rework changes internals; observable behavior of the shipped surface
  does not move without a decision.
- **Hub write path:** all mutation through the hub's one write surface
  (`scripts/check-hub-write-path.sh` gates it).
- **Access:** keys + levels (hub doc Decided). `Availability`
  (visible/enabled/reason) evaluates the SAME condition machinery that
  gates projections; a frontend never grants permission (design doc
  invariant 1/10).
- **Renderer dependency model:** level 0 internal and dependency-free;
  curses and richer renderers are optional dat-style providers.
- **Value-first:** content = `madc::value`; classification = interned ids;
  no std::string on the surface (dialect-lean).
- **Thread contracts stated per addition** (thread-safety law). Phase 1
  shape stays session-confined single-thread; contracts stay
  concurrency-ready (hub doc demand 15).
- **Testing:** targeted tests per change; battery once per merge wave;
  texteditor tests ride the `.input`/`.argv`/`.expect` conventions.

## Phases

### R0 — doc merge (with this plan)

Fold the hub doc's Decided items into the design doc; mark §15 Q2 settled
(actions ARE first-class data; two binding kinds); record the fork
resolution; chain the doc pointers. No code.

### R1 — interaction core rework

- `Context` (actor + focus + scope + mode + interaction state) as an
  explicit object; the adventure driver's implicit context re-expressed
  over it.
- Structured `Invocation` {actor, action, target, arguments(values),
  context}; `ui::act()` becomes interpret(text) → invoke(Invocation) —
  same observable results.
- `resolve_affordances(Context)` — application + actor + focus + related +
  mode − prohibited; `Affordance` carries provider, bound args, label,
  `Availability{visible, enabled, reason}`.
- The seam law lands here: registry bindings take Invocation-of-values.
- Gate: adventure oracle green; a probe test enumerates the affordances of
  a known adventure context and matches a pinned list.

### R2 — projection-as-data — **EXECUTED (2026-08-24)**

As-built (the eviction changed the ground: `render_look` is gone — the
application composes its own look; `render_inspect` was ALREADY
projection + typeset from Phase 1 S3, so R2's delta was the data form):

- The value tree already existed as `uinode` (S3) — role/states as
  interned ids, label/content/hints as `value`, actions, subject,
  children. R2 adds the **`choice` role** (a menu: the node's children
  are its OPTIONS) to the standard vocabulary and the numbered-menu arm
  to the level-0 renderer (`render_text.h`) — options are consumed by
  the menu; detail under an option still renders generically.
- **The tree IS hub data now** (demand 3): `uinode_to_value` /
  `value_to_uinode` (uinode.h) — sparse `{ role, label, content, hints,
  states[], actions[], subject, children[] }` objects, names not ids;
  the reader is tolerant (bare text = a content leaf; wrong-kind fields
  skipped). Script surface: `ui::inspect_tree` (the inspect projection
  as data; a gate refusal arrives as a status-role node) and
  `ui::render_tree` (typeset ANY value-shaped tree an application
  composes). One projection owner inside ns_ui
  (`ui_inspect_projection`) feeds both render_inspect and inspect_tree.
- Gate MET: `tests/testuitree.mad` — a script-composed choice tree
  renders as a numbered menu; the refusal and granted inspect trees
  walk as data; `render_tree(inspect_tree(...))` is byte-identical to
  `render_inspect` (`roundtrip=identical`, `.expect_quiet`); unit
  battery test_projection grew the choice + bridge/roundtrip cases
  (67 asserts); adventure oracle green in the wave battery.
- (Plan-name mapping: ProjectionNode = uinode; Collection ≈ list;
  Value-role and EditValue/EditText enter in R4 as planned.)

### R3 — script-entity binding kind (the tracer)

- Code entity + the MINIMAL verb-execution context API (world/session/
  actor handles + value args in, value result out).
- ONE madc-source verb executes through the registry — same Invocation
  path as native. Candidate: the texteditor's `filter_range` (or a
  turn-cadence adventure verb if sequencing favors it).
- This phase is also the probe of the eval substrate (B+A0+A + C.1
  shipped): what remains of eval-track C / A0.2 that the tracer needs
  surfaces HERE, while cheap.
- Deliverable alongside: the script-verb sibling design section — code-
  entity key-gating, re-entrancy policy (Phase-1: verbs do not re-enter
  `ui::act`), thread contract, and the deferred full arc (script verbs in
  stored worlds, moddable deployments).
- Gate: the tracer verb runs; killing its code entity's key makes it
  unavailable through the SAME availability machinery as native verbs.

### R4 — examples/texteditor, line mode — **EXECUTED (2026-08-25, line-mode scope)**

As-built (the eviction re-based this too — a pure-script example cannot
register native-kind verbs, so the "native kind" action list became the
GENERIC ENGINE PRIMITIVES those verbs call; native-kind editor bindings
remain the libmadc-host form, exercised at the unit level):

- **Buffer = piece-table component, LANDED** (`madcdis/text_buffer.h`;
  Track 8.1 pulled forward): immutable snapshot + append-only adds +
  piece splits; the ed line model (1-based, '\n' excluded, trailing
  span counts, empty = zero lines); mirrored-oracle unit battery
  (every mutation in lockstep with std::string, 128 asserts). The hub's
  SECOND component kind: a sparse column on `world`, writes mirrored
  through `mutation_context` (one write surface), const reads. RUNTIME-
  ONLY: world_save does not carry it — the w verb persists the document
  to its own file.
- **Session surface:** `ui::world_new` (empty session — no world file)
  and the `ui::text_*` family (load/insert/erase/replace + text/size/
  line_count/line/line_start/line_len/find). Document properties
  (path/modified/read_only) are APPLICATION bag keys.
- **Resources as entities:** the document and editor-state are ordinary
  entities; interaction state (quit today; caret/selection/mode join
  with R5's TUI) lives on the editor-state bag — the interaction-state
  category has its home.
- **The application** (`examples/texteditor/`): lined.mad +
  lined_core.inc + verbs/*.madv — p/c/i/a/d/f/w/q/q! are NINE
  madc-source verb bodies through the one registry; every edit is a
  range op composed from line spans (§7.7's "this still produces
  replace_range"). `php::file_get_contents`/`file_put_contents` landed
  as the whole-file pair (PHP parity, pre-L3 union mapping).
- **Gate MET (line-mode form):** `tests/testlineed.mad` drives THE
  example (one copy) through a pinned two-phase transcript
  (`.input`/`.expect`/`.expect_quiet`): writable phase (edit cycle,
  find, unknown-verb status, modified-quit refusal, w writes 23 bytes,
  clean quit) then a read-only phase (c/d/w refusals). Adventure oracle
  green in the wave battery.
- **Named residues (R5 / the sibling design):** §7.3's rules surface
  today at the REFUSAL level inside bodies; their affordance-ENUMERATION
  form (read-only removes edit affordances from `ui::affordances`)
  needs state-conditional gatherers scripts can feed — design that with
  the script-verb sibling section. `filter_range` as a named tracer is
  SUPERSEDED (every editor verb is script). caret/selection/search
  state + insert_text-at-caret are R5's TUI arrival. The pasted
  read-only check across five bodies and the range parse across two are
  recorded DupFamilies (consolidation point = compile-once script
  bodies, or the gatherer form moving the rule to one availability
  seat).

### R5 — level-1 TUI provider — **EXECUTED (2026-08-25)**

Planned as "curses TUI provider"; the library choice was never actually
decided, and the owner ruled 2026-08-25: **roll our own VT100/xterm
target** (recon ideas from ncurses/termbox2/notcurses as needed) rather
than vendor a dependency — deps come later, behind the same seam.

As built:

- **The model/target split** is what made a hand-rolled target cheap and
  the whole level honest to test: `madcdis/tui_model.h` holds everything
  that is not terminal I/O — tree→grid layout (heading/status bars,
  wrapped content, the flexible `edit` window with caret/scroll/h-shift/
  selection, the `choice` menu bar), focus + selection (the SAME tree
  line mode numbers is navigable here — criterion 4), byte→key escape
  parsing (CSI/SS3, bare-ESC flush), key→semantic-event adaptation with
  printable-run coalescing (§7.5), and dirty-row differencing — all
  dependency-free, unit-pinned (test_tui_model, 100 asserts).
  Presentation state (scroll/shift/focus/selection) lives in the model
  per §7.2; interaction state arrives as `edit`-node hints
  ({caret, sel_start, sel_end} byte offsets). Roles gained `edit`;
  `node_text()` became the one content-else-label spelling rule.
- **The target seam** (`madcdis/tui_provider.h`, the dat-style registry —
  register/create, madcdat's driver shape): `src/ui_term.cpp` is the
  built-in target — raw termios (IXON off: ^S/^Q are keys), alternate
  screen, CUP/SGR dirty-row repaint, poll-batched reads (the batch is
  what coalescing rides) with a 25ms grace poll only when a sequence is
  split (`tui_keyparse::pending`; the termbox2/ESCDELAY idea), SIGWINCH →
  resize event, atexit terminal recovery. POSIX-gated; on _WIN32 nothing
  registers and `ui::tui_open` refuses loudly (a Console target is a
  later provider). Script surface: `ui::tui_open/close/rows/cols/render/
  event` — compose-as-data in, semantic event objects out (`text`/`key`/
  `choose` with the 1-BASED option number line mode prints/`focus`/
  `resize`).
- **The availability check binding landed first** (the R4-named §7.3
  residue): verbs carry an optional state-conditional check, BOTH kinds
  (native fn / madc source via `ui::bind_check`), evaluated by the ONE
  `availability_of` that answers `ui::affordances` (probe invocations)
  and gates `invoke` — enumeration and dispatch can never disagree
  (invariant 5). Script protocol: "ok" = available, text = the disabled
  reason, empty (an eval failure included) = loudly disabled. DupFamily
  lineed_readonly_gate CONSOLIDATED: `checks/editable.madv` is the one
  read-only rule, bound to c/i/a/d/w; the five body copies are gone and
  the testlineed transcript stayed byte-identical.
- **The editor pair** (criterion 3): `examples/texteditor/vised.mad` +
  `vised_core.inc` — the semantic core (`apply_event`) is terminal-free
  and drives the SAME document actions as lined: engine range primitives
  at the caret for typing cadence; the SAME w/q/q! script verbs through
  `ui::act` for turn cadence (^S/^Q/^X and the menu's choose land on
  them). Typing consults the affordance-DERIVED editable verdict — one
  rule, one seat. Caret/mark/clipboard/search live on the editor-state
  bag; `setup_document()` is the shared open path.
- **Gates MET:** `tests/testvised` (headless: the exact event objects
  `ui::tui_event` produces, pinning inserts/movement/mark-cut-paste/
  modified-quit refusal/search/"Wrote 15 bytes." through the w verb/
  read-only refusals); `tests/testeditcheck` (one world-state rule flips
  enumeration and dispatch together); `scripts/tui_smoke_gate.sh` in
  fulltest (bin/madc on a REAL pty: alt-screen discipline, drawing,
  attributes, cursor, the exact semantic event stream incl. resize —
  plus a NEGATIVE-CONTROL program that must fail the harness); a real
  pty run of vised (type, ^S, ^Q → the file gains the text); adventure
  oracle green in the wave battery; every editor verb still madc source
  (the tracer clause, exceeded as before).
- **Named residues:** byte-oriented cells (UTF-8 glyph width);
  focusable identity is discovery order (stable-shape contract);
  Windows Console target; per-edit-node scroll keyed by slot; tab is
  focus-cycling only (no literal tab insertion); the editable verdict is
  derived at open (a mid-session read_only flip needs re-derivation);
  page size fixed at 10 lines (the app does not know the region height).

### R1+R3 as executed (2026-08-24, the eviction)

The owner's Rule #7 ruling merged R1 and R3 into one wave:

- **Interaction core (R1 as planned):** `madcdis/interaction.h` —
  `interaction_context` (actor/focus/scope/mode/interaction_state,
  built by `containment_context`), structured `invocation`
  (actor/action/target/value-arguments/context), `availability`,
  `affordance`; `verb_table` gained `availability_of()` (the keys+levels
  evaluator, surfaced) and invocation dispatch; `resolve_affordances` =
  registry actions + application gatherers − (mode prohibitions, seat
  held). `ui::act` = interpret → invoke. Seam law throughout:
  `action_env` (mutation context + credentials + host session handle) +
  invocation-of-values → value-shaped result.
- **Script-entity binding kind (R3's core, pulled forward):** the
  registry stores SOURCE next to native fn pointers; execution delegates
  to an injected `script_executor` (ns_ui injects the eval seam:
  `madc::eval_string_ctx`, ctx = the invocation as typed globals —
  w/actor/target = int64, arg/verb = const char*). `ui::bind_verb`
  attaches bodies; `%verb` lines stay the gating DATA. Gating, refusal,
  and availability are identical across binding kinds (unit-pinned).
- **The eviction:** `include/madcdis/adventure.h` DELETED (catalog,
  room_view, tick, noun-resolver, the register_catalog name ladder).
  `ui::render_look`/`ui::turn_count` removed (application projections/
  vocabulary). Generic replacements, vocabulary-as-data: `ui::links`
  (rel as argument), `ui::resolve` (alias property as argument),
  `ui::has_key` (the credential evaluator, surfaced). The `in`/`grants`
  spellings remain as DOCUMENTED session-layer substrate conventions.
- **The pilot is now the application it always claimed to be:**
  `tests/adventure_verbs/*.madv` (eight madc-source verb bodies) +
  `tests/adventure_bind.inc` + the re-cut driver composing its own look
  from generic reads. Transcripts (`testadventure`,
  `testadventurebuilder`) reproduce the reference shape with EMPTY
  stderr — the tracer requirement is exceeded: every pilot verb is
  script.
- **Gates:** `check-engine-app-purity.sh` (canary vocabulary + no
  engine-side verb registration + the header must not return; negative-
  controlled) joined fulltest; `check-hub-write-path.sh` re-pointed at
  the engine headers; `tests/testaffordances.mad` pins the affordance
  enumeration (player vs builder availability flip).

**Eval feeder gaps discovered by the tracer (R3's probe purpose):**

1. **FIXED (2026-08-24, the session after the eviction).** The gap was
   never eval-specific: ANY computed carrier text returned as `char *`
   (a plain `const char *f() { var a = ...; return a.c_str(); }` too)
   read freed memory — the value's cleanup dtor runs between the return
   expression's evaluation and the caller/shim-side copy, so only
   literal and ring text survived. Two language-wide fixes (an owning
   `value` wrapper was probed and rejected: value-by-value returns are
   L3, not yet implemented — `value f()` does not compile):
   - `madarray_cstr` string kind now COPIES the payload into the
     thread-local ring instead of lending the payload pointer — c_str()
     on the carrier is uniformly the ring-lifetime text contract
     (value-first.md's pre-L3 return convention; substr/format already
     honored it). Deliberate, documented divergence from
     std::string::c_str().
   - `translate_return` lowers `return v;` (carrier operand,
     char*-returning function) through `object_cstr_arg` — the one
     class-to-cstr owner. Previously an incompatible struct return:
     c2mir warning + garbage pointer. Scoped to the carrier; a
     std::string operand keeps g++'s type error.
   Also consolidated: `eval_body_wrapper_return_type` is the ONE owner
   of the wrapper-type-per-form spelling (parser.cpp's typed
   runtime-eval entries route through it, no more raw strings).
   Pinned by `tests/testevalreturn.mad` (all six body-return shapes,
   `.expect_quiet`) and `tests/testcstrreturn.mad` (the plain-function
   twin). Eval bodies may now return `a.c_str()`, a bare `var`, and use
   `+=` — the "literals or format() only" idiom restriction is lifted.
2. **FIXED (same session as gap 1).** A ctx binding's plain reads FOLD
   to a string literal (host memory has no module-referenceable symbol;
   the binding is a read-only snapshot), but `TokenSubscript` and
   `TokenDeref` embed the Variable in their own tokens and bypassed the
   fold — the first non-folding use was the first undeclared reference.
   One fold owner now: `CirBuilder::baked_cstr_constant`, applied in the
   plain-read, subscript-base (`arg[0]` → `"text"[0]`), and deref
   (`*arg` → `*"text"`) arms. `&arg` stays a loud undeclared-identifier
   error (host memory has no address the module can name). Pinned by
   `tests/testevalctxderef.mad` (`.expect_quiet`). ALL eval feeder gaps
   are now closed — eval bodies have no idiom restrictions left.
3. **RESOLVED by gap 1's fix:** `+=` accumulation inside eval bodies
   works (it was masked by the return gap); pinned by the `pluseq`
   shape in `tests/testevalreturn.mad`.

### Then: madcide (hub doc Phase 2, unchanged)

Buffers = texteditor machinery; diagnostics pane and outline = projections
over live compiler data; keybinding/theme profiles as wants. Starts as a
CONSUMER of R1–R5; its gate stays as written in the hub doc.

## Deferred (seats held, per the hub doc)

Levels 2–4 and web renderers; per-connection multi-client serving (7.3
reactivity/diff wire); full script-verb arc (stored-world verbs, CRDT
text); NLG prose; `render {}` syntax (design doc Phase 8 — only after the
library surface stabilizes).

## Open questions still open (design doc §15)

Q1 (Actor trait vs wrapper — R1 decides by implementation), Q3–Q10 as
written. Q2 is SETTLED (above). None block R1.
