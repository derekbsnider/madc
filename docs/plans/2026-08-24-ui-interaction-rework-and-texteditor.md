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

### R2 — projection-as-data

- `ProjectionNode` as a value tree: role (interned id), binding, content
  (`value`), state, actions (AffordanceRefs), children, hints. Minimal
  role vocabulary first: Group, Heading, Content, Value, Collection,
  Choice, Status. (EditValue/EditText enter in R4.)
- `render_look` / `render_inspect` re-cut as projections over the tree;
  the level-0 renderer only typesets (headings, lists, wrap, numbered
  Choice).
- Gate: adventure oracle green; one Choice projection renders as a
  numbered menu in line mode from the same tree a future TUI will read;
  the projection tree itself is inspectable as hub data (demand 3).

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

### R4 — examples/texteditor, line mode

- Resources: `TextDocument` (path, buffer, modified, read_only) +
  `EditorSession` (document, caret, selection, mode, search) — the
  interaction-state category gets its home (caret/selection/mode are
  session state, never domain, never presentation).
- Buffer = piece-table component (Track 8.1 pulled forward, per the hub
  doc's Phase-2 note).
- Actions (native kind): insert_text, delete_range, replace_range,
  move_caret, save, search, quit; `filter_range` as the script-shaped
  verb (R3's tracer home if not already landed).
- Frontend: the line-mode range editor (design doc §7.7) — level 0,
  dependency-free, `.input`/`.expect` testable.
- Gate: scripted line-editing transcripts against a pinned oracle; the
  read-only / modified / selection affordance rules of design doc §7.3
  exercised; adventure oracle green.

### R5 — curses TUI provider

- First level-1 renderer as an optional dat-style provider: layout,
  focus, key input adapter, differential cell updates — all INSIDE the
  provider; key-run coalescing into one semantic `insert_text` (§7.5).
- Gate (= design doc success criteria 3 + 4 + the tracer clause): the TUI
  editor and the line editor drive the SAME TextDocument actions with no
  duplicated mutation logic; the same Choice projection renders numbered
  in line mode and navigable in the TUI; ≥1 verb still executing from
  madc source; adventure oracle green.

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

**Eval feeder gaps discovered by the tracer (R3's probe purpose —
banked, owed next):**

1. **SILENT:** an eval body returning a `var` or a `.c_str()` result
   through the `char *` wrapper returns empty (a var return also warns
   "incompatible pointer types"; a c_str() return is fully silent).
   Reducer shape: `ui::bind_verb(w, "t", "var a = \"x\"; return
   a.c_str();")` → act returns "". Workaround in the pilot bodies:
   return only literals or `format(...)`.
2. **LOUD:** a ctx-installed `const char *` global breaks under `[]` or
   unary `*` — c2mir check "undeclared identifier" (the subscript path
   emits the global without its declaration). Reducer: body `if (
   !arg[0] ) ...`. Workaround: coerce via `var a = format("{}", arg)`.
3. `+=` accumulation inside eval bodies is unproven (its probe was
   masked by gap 1); the pilot bodies rebuild strings via `format`.

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
