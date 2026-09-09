# Nexus, sessions and clients — the client-server design for madcide

**Date:** 2026-09-09 · **Status:** DESIGN — for owner review before the first
code slice (owner sequencing 2026-09-08: "design doc BEFORE code, thread-safety
contracts spelled out"). Standing defaults are marked; veto welcome.

**Inputs:** the RULED layering, state tiers, concurrency and permission model
([the gateway design](2026-08-31-madcide-gateway-and-code-server.md)); the
owner's [Nexus vision](2026-09-08-madc-ide-nexus-vision.md) and
[Multi-View architecture](2026-09-08-madc-ide-multi-view-architecture.md);
the seven rulings of [the cross-reference](2026-09-09-nexus-and-multiview-cross-reference.md)
§5–§5b (KG Decisions `vocabulary_nexus_session_client`,
`nexus_event_log_from_slice_one`, `nexus_native_records_when_no_external_owner`,
`tui_panes_and_tabs_user_opened`, `debugger_profiler_track_after_nexus_axes`,
`view_pairs_order`, `emitted_views_indent_and_colour`); the hub design's
demands ([data hub](2026-08-20-data-hub-projection-rendering.md)); the web
target's thread contract ([Level 3](2026-09-06-ui-web-target-and-madcide-gui.md)
§3.5, §3.7); `.claude/rules/thread-safety.md`.

## 0. Vocabulary (RULED) and scope

- **madc** and **the IDE** are not the same thing (owner, 2026-09-09): madc
  is the language, its compiler and its JIT — the engine. madcide is an
  APPLICATION that includes madc and extends it into an IDE (the running
  madc IS its compiler; the session lives in the same process). Everything
  below is the IDE's design over the engine's substrate.
- **Hub** — the running process as the madcdis substrate where clients,
  Views, tools and data meet (owner vocabulary 2026-09-09: the larger
  container). Today it is the madcide process itself; the headless server is
  the same object without a window.
- **Nexus** — the temporal meeting point the hub serves: the project's past
  (git, the change-event log), its present (the live AST, the session) and
  its future (plans, proposals). Every View is a projection of the nexus at
  one point in that time; "Nexus §N" below cites the owner's vision doc.
- **Session** — one live editing context on the hub: documents, the
  project, diagnostics, builds, the Views over them, the change-event log.
- **Client** — a window, a TUI, an MCP seat, a remote page: something that
  shows a layout of the session's Views and posts commands. **A window is a
  client.** Two windows on one process are two clients of one session.

In scope: the View object, containers (pane / tab / window) and layouts as
data, the multi-client loop (several windows on one UI thread), the change
event log, correlation maps (source ↔ MC11 first), presence, permission
tiers, the remote transports and the headless shape, and the
thread-safety contract of every piece. Out of scope (design-for only):
federation between Nexus nodes (Nexus §17–§24), the debugger/profiler tier
(ruled: a track after the nexus axes).

## 1. Where the code stands (facts, 2026-09-09)

- **One loop, one target, one session.** `run_ide` (tools/madcide/
  madcide_client.inc) opens ONE frontend (`ui::open(target)`: the grid
  `tui_model` for `term`, one `ui_dom_frontend` — one window, one
  `webview_t` host — for `web`), then loops compose → `ui::render` →
  `ui::event(t, w)` → `apply_ide_event`. `ui::event` blocks on ONE
  frontend's queue; the DOM frontend's `read_events` is the cooperative
  scheduler's bounded wait (fire due tasks, yield runnable, `tick`, then
  `ops->run(host)` — the PLATFORM loop, which is process-global on GTK,
  Cocoa and Win32).
- **The session** (`class IdeSession`, madcide_core.inc) keeps its state on
  entity bags in a `ui` world: the editor-state entity `es` (caret, mark,
  buffers, `windows` + `winat` (the S5 stack: rows `{bufidx, caret, grow}`),
  `panel` / `paneltab`, `hspans`, the parked terminal request, the key
  table generation) and per-document bags (`path`, `modified`, `phandle`).
  Client-pushed FACTS (viewport, terminal presence, dialogs, panel) arrive
  through methods; keys stay client-side.
- **The lens is session-level.** `enter_view` renders `madc::emit` over the
  live buffer into ONE view-buffer entity `es.vdoc` with `es.view`,
  `es.vmap` (EMPTY for a rendered view), `es.ocaret`; `nav_doc` routes
  navigation to it. One active lens per session, not per window — the gap
  that blocks "source beside its MC11".
- **Views already exist in all but name:** the edit node (a document's
  text), a lens (its render), Problems (`diag_items`), Output (the `[build]`
  buffer), the Terminal (`term_feed` into the `[terminal]` buffer through
  `madcdis/term_screen.h`), the Project window, the outline, the help pane.
  Each is composed by a `compose_*` function from bag state.
- **Profiles are the data family:** `parse_keys` (`.keys` with `@scope`
  sections; baked defaults as inline data through the same parser),
  `default.menu` (the ONE command registry + menu map, gated), `.theme`
  (`madcdis/ui_style.h`, one spec parser for both renderers, `@gui`
  section), `joe.status`.
- **Mutation is already funnelled.** `madcdis/verbs.h`: "EVERY MUTATION
  FLOWS THROUGH A VERB … the context's methods mirror the world's mutators
  one-to-one today; the indirection is the SEAM — demand 15's diff
  journaling and change propagation attach here later without touching a
  binding." Text edits flow through the one text-mutation owner
  (`ed_text_insert` / `ed_text_erase`, editor_events.inc), which already
  shifts the byte-anchored highlight spans. The piece table
  (`madcdis/text_buffer.h`) records undo as SNAPSHOTS (`history_entry
  {pieces, size, meta}`), not deltas; the web editor's patch protocol
  computes ONE splice `{at, del, ins}` per render from the common prefix —
  a text diff, not the mutation record.
- **Transports:** `madc::channel` (`madcdis/channel.h`) is client-side
  (`exec://`, `pty://`, `madcrun://`, `madcproj://`, TCP connect); NO
  listen/accept anywhere in `include/madcdis` or `src/` (grep 2026-09-09) —
  the server half is new. The `ws` target (design §3.5) is "the same page
  and the same DOM-op / event JSON over a WebSocket".
- **Access:** hub credentials = keys + levels per domain (`madcdis/hub.h`),
  `ui::session_level`, `ui::has_key`, `%require` gates from data; the
  `grants` bag convention.
- **Threads:** everything above is confined to the UI thread; concurrency
  is the cooperative task scheduler (`go`, `__madc_yield`, channels) — the
  `tick` host op makes the window's wait the scheduler's wait.
- **Landed this session (V0 of this arc):** every emitted code view is
  indented and coloured — the emitter (`src/cir_emit_c.cpp`) indents by
  block depth and renders precedence-aware parentheses; the lens buffer
  carries its own highlight spans (`enter_view` → `lex_spans` over the
  emitted text). Gate: `scripts/emit_layout_gate.sh`.

## 2. The model

### 2.1 The View — a projection with a provider

A **View** is data on the session:

```
View { id, subject, representation, revision, generator, runtime_context,
       vbuf, map, spans, caret, mark, scroll }
```

- `subject` — an ENTITY HANDLE (the gateway's day-one rule: never a byte
  offset): a document entity, the diagnostics set, a process stream entity,
  the project, a symbol (later).
- `representation` — `source | mc11 | c11 | c++ | problems | output |
  terminal | project | outline | help | …` (an enum in the engine, spelled
  as data at the surface; `enum-over-strings.md`).
- `revision` — `current` (the only value in the first slices); later an
  event sequence number (`event:1234`, the edit-history View) or a git
  revision (`git:a81f42`, the madcdat adapter).
- `generator` — `madc` for everything the compiler renders; `gcc` / `clang`
  for external-assembly Views (§2.6); absent for stored text.
- `runtime_context` — absent until the debugger track.
- `vbuf` — the View's OWN text buffer entity when the representation is a
  render (today's `vdoc`, one per View instead of one per session); the
  subject's own buffer when it is stored text.
- `map` — the coordinate map to the subject's stored space (today's
  `vmap`; §2.6 fills it); `spans` — the View's highlight rows (landed for
  lenses); `caret / mark / scroll` — the View's navigation state (today
  on `es`, per View from V1).

A **provider** produces a View's text + spans + map for a (subject,
representation, revision, generator): the document provider (stored text +
`parse_spans`), the emit provider (`madc::emit` + `lex_spans` + the
emitter's map), the diagnostics provider (`diag_items`), the stream
provider (`term_feed`), the project provider, and later the external-asm
provider (runs the oracle compiler), the git provider (madcdat adapter),
the MIR provider (`MIR_output` per function through the parse handle).
Providers are the extension point (Multi-View §23); the composer knows only
the View interface.

What changes about today's code: `es.vdoc / view / vmap / ocaret` become
one row in `es.views`; `nav_doc` becomes "the focused View's buffer";
`cycle_view` becomes "replace the focused View's representation";
`compose_edit_node` composes a View by id. The identity lens (a source
View over the document) composes BYTE-IDENTICAL to today — the existing
gates (`testmadcide`, the GUI DOM snapshots) pin that.

### 2.2 Containers — window ⊃ pane ⊃ tab ⊃ View; layouts as data

A **layout** is a tree of containers holding Views, owned by the CLIENT
(the state tiers: "open panes" are client-private) and saved as data:

```
# profiles/default.layout — the shipped workbench (the baked default is this
# same text as an inline profile through the same parser — the rescue-keys
# pattern; the terminal reads the same file and renders panes and tabs)
@window main
pane editor            tabs   views source            focus
pane sidebar left 20%  stack  views project outline
pane panel bottom 25%  tabs   views problems output terminal   hidden
```

- **pane** — a region with a size hint and a stacking mode (`tabs`: one
  View visible at a time, a strip; `stack`: Views stacked vertically — the
  S5 window stack's shape). The bottom panel is a pane; the editor region
  is a pane whose tabs are today's editor tabs; the S5 window stack is a
  pane in `stack` mode.
- **window** — a container of panes that is ALSO a client (§2.3). "Give the
  Terminal its own window" = `viewwindow terminal`: move a View from the
  panel pane into a new `@window`.
- The TUI reads the same layout (RULED relaxation: panes and tabs the user
  opens; popups for pick lists): `pane … tabs` renders a strip of names in
  the pane's header line; `stack` renders the S5 stack; a hidden pane takes
  no rows. Post-master item 2 (TUI panes/tabs) is this slice, not a second
  one.
- Commands over containers are data-bound like every command: `viewopen
  <repr>`, `viewsplit right|bottom <repr>`, `viewtab <repr>`, `viewwindow
  <repr>`, `viewclose`, `viewfocus <dir>`, `viewsync on|off` (Multi-View
  §28's `view.open source / view.split_right mir`) — all through the one
  registry (`default.menu`) and the key profiles. No key is hard-coded.
- Saved layouts are workspaces (Multi-View §27): `compiler-dev.layout`,
  `codegen-compare.layout` ship as examples once the Views they name exist.

### 2.3 A window is a client — the multi-client loop

A **client record** lives on the session:

```
Client { id, level (uiLevel), transport (local|ws|api), name, colour, tier,
         layout, focus (view id), viewport {rows, cols}, keys_generation,
         pending_chord, flags {terminal, dialogs, panel, …}, last_seq }
```

`level` is the ordered UI-level enum (RULED 2026-09-09, KG Decision
`ui_level_enum_ordered_by_requirement`): `ui::NONE < ui::LINE < ui::TUI < ui::WEB <
ui::GUI < ui::GFX2D < ui::GFX3D` in `include/madc/bits/ui_enums`, escalating in
requirement and platform specificity. `ui::NONE` is a client with no
rendered surface that still posts commands (an `api` / MCP seat, the
headless gates); `ui::LINE` the ex/edlin line mode; `ui::TUI` the grid; `ui::WEB`
the webview page (local or over `ws`). The level is the rendering model; the
devices and chrome a target offers (keyboard, pointer, touch, menu bar,
dialogs, sixel) are feature FLAGS beside it. A target declares the level it
serves — `ui::open(ui::WEB)` replaces `ui::open("web")` — and the order has a
meaning: the session composes for the highest level a client wants, every
lower level ignores the hints it cannot show, and a target below the
requested level refuses with the reason or serves the lower rendering by the
program's choice, never silently.

The loop becomes: for every client whose input or view changed since its
last compose, `compose(client)` (that client's layout, that client's focus,
the presence of the OTHERS drawn as carets in their colours) → render on
that client's frontend; then WAIT ON ANY CLIENT; apply the event tagged
with its client; repeat. Concretely:

- `ui::event_any(out, targets[], w)` — the ONE blocking decision over N
  frontends. For N windows of the web target there is nothing to
  multiplex: `ops->run(host)` runs the process-global platform loop, so an
  event from ANY window ends it and lands in the frontend whose `ctx`
  posted (already keyed that way in `ui::post_event`). `event_any` walks
  the frontends for a non-empty queue after each `read_events` round,
  running the scheduler step (fire due, yield runnable, `tick`) ONCE per
  round, not once per frontend.
- A remote client (`ws`, `api`) is a frontend whose `read_events` parks a
  cooperative task on its channel (`wait_readable`) — the scheduler's
  bounded wait already pumps parked tasks between platform loops, so a
  socket and a window share one thread with the existing machinery.
- A TUI and a window in ONE process is NOT a first target: the tty read
  blocks in `read_keys` and the platform loop blocks in `run`; joining them
  means a non-blocking tty poll driven from the tick. Named, not blocking
  (the TUI's own multi-pane shape needs no second frontend).
- `IdeSession::compose_ide_tree(root, doc)` becomes `compose(root,
  client)`; `apply_ide_event(doc, ev)` becomes `apply(client, ev)`; the
  client-pushed facts move onto the client record. `run_ide` keeps its
  shape for one client (behaviour-identical) and grows the loop over
  `clients[]`.
- **Presence** rides the client record: caret, mark, selection, colour
  (dealt from the theme's presence palette at connect — a new `@presence`
  theme section, standing default: eight distinguishable colours over the
  sixteen-colour palette), display name. Byte-anchored presence shifts
  through the one text-mutation owner — the `shift_hspans` mechanism
  generalized to an ANCHOR REGISTRY: every anchored thing (client carets
  and marks, highlight spans, View maps, diagnostics rows) registers its
  offsets on the document and the mutation owner shifts them all in one
  pass. A highlight is a pointing gesture (RULED).

### 2.3b The IDE serves every level up to `ui::WEB` (OWNER 2026-09-09)

Owner requirement (2026-09-09, during the review of this document): "the IDE
should be able to work in all of those UI modes". The IDE is ONE session
behind clients of every level the enum names below `ui::GUI`; `ui::GUI` and above
are out of scope for this arc. The `ui::LINE` mode is the vi `:` command mode
the IDE already has, without the TUI part (owner) — no new command language.

| Level | The IDE client | Today | Slice |
|---|---|---|---|
| `ui::NONE` | (a) **one-shot**: `madcide <file> -c "<command> [arg]"` — the session opens, ONE registry command runs (its name resolved at the command-line boundary, the same table the profiles use), the resulting projection prints to stdout as text (problems rows, the outline, a lens) and the exit status is the verdict; (b) the **headless server** `madcide --serve` (no window; `api` / `ws` seats). | the headless harness (`testmadcide` drives the session with no frontend at all) IS (a) without a command line | V1.5 (a); V6 (b) |
| `ui::LINE` | the **ex / edlin client**: a line frontend over the colon interpreter — `:` verbs (`w q e r`, `goto`, `find`, and every registry command by name) read from stdin, the level-0 sequential renderer prints the projection to stdout (numbered choices; the edit node's lines by range, the way `ex` prints). No cursor addressing: it works over a pipe, in a dumb terminal, and as an MCP seat's transcript. | the colon line (`do_verb`) and `render_tree`'s level-0 print exist; no line frontend | V2.5 |
| `ui::TUI` | the grid client (the JOE personality and the other profiles) | landed | — |
| `ui::WEB` | the webview window | landed | — |
| `ui::GUI` and above | native chrome, 2D, 3D | out of scope | — |

Consequences for the design:

- `ui::open(ui::LINE)` needs a real frontend — `ui_line_frontend`: `render` = the
  sequential typesetter over the composed tree, `read_events` = one stdin line
  → the same semantic events (a `:` line is a colon command; a bare line is
  text; the key vocabulary's spellings name keys). Same session, same
  composer: a lower level ignores the hints it cannot show (the order's
  meaning, §2.3), so the composer changes for none of this.
- A `ui::NONE` client gets PROJECTIONS, never a layout: the client record's
  `level` gates what the session composes for it.
- The registry-command name → code table (slice V0.5) is the one converter
  every input boundary uses: profiles, menus, the `-c` command line and the
  `:` line.
- Thread contract: unchanged — every frontend runs on the UI thread; the
  line frontend's stdin read is the blocking decision, exactly the grid's
  `read_keys`.

### 2.4 The change event log (RULED: from slice one)

Every mutation produces one **change event**, appended by the session:

```
ChangeEvent { seq, session, actor (client id), ts, verb, object (entity
              handle), payload, causal_parent }
```

- `payload` for text is the splice `{at, del, ins}` — written by the ONE
  text-mutation owner (`ed_text_insert` / `ed_text_erase`), which is
  exactly where the anchor registry shifts (§2.3): one pass, one record.
  For every other verb it is the verb's arguments as posted (projadd,
  bufsel, viewopen …). The verb layer's `mutation_context` is the seam
  verbs.h already reserved for "diff journaling and change propagation".
- `causal_parent` is the `seq` the actor had seen when it posted (its
  `last_seq`): the "based on" that a rebase-on-receipt mirror needs later
  and an audit reads now.
- **Consumers, one record:** (a) change propagation — after a verb the
  session bumps `seq`; every client with `last_seq < seq` recomposes (the
  loop already recomposes per event; this makes it per-CHANGE for the
  clients that did not post); a remote client receives the events since
  its `last_seq` (the mirror's splice stream, Nexus §20 "exchange events
  they have not seen"); (b) the **edit-history View** (Multi-View §11):
  `View{doc, source, revision: event:N}` reconstructs the text at N by
  replaying splices from the loaded snapshot — "before agent change" is
  the View at the last seq whose actor was not that agent; (c)
  provenance (Nexus §10): who did what, when, based on what. Undo stays
  the piece table (unchanged, per client's own history); the log is the
  RECORD, not the undo mechanism.
- **Persistence (standing default, mirrors the manifest rule):** the log
  lives in memory for the implicit single-file project and persists as
  `<base>.prj.events` beside `<base>.prj.json` once a manifest exists —
  JSON lines, appended per event, no fsync (a crash loses the tail; the
  buffer's truth is its own file). A native nexus record (RULED: native
  when no external tool owns it), never a program cache: it records what
  PEOPLE and agents did, and it is the owner's to delete. Retention: the
  file is rewritten from its snapshots when it exceeds a size knob
  (config; default 16 MB), keeping the last N events whole.
- **Not a CRDT:** one session is the single authority; verbs apply in
  arrival order (RULED); the log is what makes that order durable.

### 2.5 Permission tiers

Tiers are LEVELS in one hub domain (`ide`) on the client's credentials —
no third mechanism: `observe` (0: presence + navigation + read Views),
`edit` (1: the mutation verbs), `admin` (2: project membership, builds,
settings, promoting another client). Verbs declare their requirement as
data (`%require ide >= 1` on the verb row, the existing gate shape); a
refused verb answers with the registered refusal prose, never silently.
Day-one trust is local: every local client connects at `admin`; a remote
or MCP client is born at `observe` and promoted by an admin verb
(`clienttier <id> <tier>`). An agent's changes as PROPOSALS (Nexus §16) is
a later tier between observe and edit: `propose` posts events into a
reviewable side log the admin lands or drops — the log shape already
carries it (actor + causal parent); design when an agent client exists.

### 2.6 Correlation maps — source ↔ MC11 first (RULED order)

The IR already holds the map: every `cir_node` carries its originating
tokens with file/line/col. The emitter (`CEmit`, the one layout owner)
counts the bytes it writes; a hook per statement/declaration node records
`{disp: start, len, stored: the node's first token offset}` into a side
table that `madc::emit` returns beside the text (a second `value` out, or
rows on the returned object). `enter_view` stores it as the View's `map`
— the `vmap` that is EMPTY today — and the existing `ui::lens_to_display /
lens_to_stored` functions (the markdown-conceal shape: 1:1 in copies,
forward collapse in gaps) become live. Then `viewsync` between a source
View and its MC11 View is cursor synchronization through two maps: the
first correlated pair, and the proof of the mechanism (approximate, many-
to-one: a source statement maps to the lowered statements it produced;
lowered machinery with no source token maps to nothing and says so —
Multi-View §24 "without pretending every transformation is reversible").
Second pair: **MAD C GEN-ASM beside gcc and clang** — the external-asm
provider runs `gcc -S -fverbose-asm -O0` / `clang -S -O0 -g` on the
emitted C (or the source when it is plain C) into a View with generator
`gcc|clang`; correlation by function symbol and `.loc` directives; the
MAD C side is the MIR text per function today (a MIR provider through the
parse handle) and GEN-ASM once MIR exposes code ranges (the debugger
track's first primitive). Third: git-revision Views through the madcdat
git adapter (the nexus axes slice).

### 2.7 Transports and the headless shape

- **`ws` — a remote WINDOW.** The same embedded page over a WebSocket: the
  DOM-op JSON the window's `eval` carries goes down the socket, the page's
  event JSON comes back into `post_event`. A remote window is a client of
  kind `ws` with the same record as a local window. Server half in
  madcdis: a `listen://host:port` channel scheme that yields ACCEPTED
  channels (each an ordinary `madc::channel`), served by cooperative tasks
  — no thread. A WebSocket framer over a byte channel (RFC 6455, no
  extensions) is small and lives beside `channel_stream.h`.
- **`api` — commands and events for non-page clients.** One JSON line per
  message: `{"cmd": name, "args": […], "seq": last_seq}` in, `{"event":
  …}` and `{"reply": …}` out — the SAME command vocabulary the profiles
  bind (the registry), the same events the log records, plus queries (the
  composed tree of a client, or raw projections a client declares it
  consumes — LSP's capability negotiation borrowed, as the gateway design
  says). The MCP seat is an adapter over `api`; the LSP endpoint is a
  second adapter (semanticTokens ← spans, publishDiagnostics ← diags rows,
  documentSymbol ← outline, hover ← `parse_enclosing`).
- **Headless** = a session with zero windows and one or more `api` / `ws`
  clients: `madcide --serve <addr> [file|manifest]`. The fake host
  (`register_host` / `post_event`, display-free) already proves the
  session runs without a display; the headless gates are its first
  formal clients.
- **Build topology** (owner 2026-09-08): a headless head coordinating
  headless servers on the build hosts, project mirrored, builds triggered
  under permission — `remote_build.sh` retires when it exists. This rides
  the `api` transport + the event log (the mirror's splice stream) + tiers;
  it is the far slice of this arc, after the MCP seat pays for itself.

## 3. Thread-safety contracts (the law: stated per piece)

| Piece | Contract |
|---|---|
| Session, world, verbs, event log (in memory) | CONFINED to the UI thread — the hub/world contract unchanged. All clients of a process share one session on one thread; verbs apply in arrival order. |
| Frontends (grid, DOM, ws, api) | Each confined to the thread that opened it (design §3.7); N windows on one thread; the platform's cross-thread door (`webview_dispatch`) stays unused. |
| Change-event persistence | Appended by the session thread only; readers (edit-history View, remote replay) read through session queries, never the file concurrently; rewrite-on-size runs on the session thread between events. |
| Remote connections | Accept and read in cooperative TASKS on the session thread (channels park, never block a thread); no shared mutation across threads exists in this arc. |
| Presence | Session-owned data written only through verbs (`presence caret <off>` IS a verb, cheap by construction); byte-anchored, shifted by the one mutation owner. |
| Providers that run a subprocess (external asm, git) | The subprocess is an `exec://` channel pumped by a task; the View's text lands through a verb when the pump completes. |
| The future multi-threaded hub (F2) | Changes contracts, not signatures: every write is already a verb, every anchor already registered, every event already ordered — demand 15's promise. |

## 4. Slices (each shippable, each with a gate; the merge wave is the whole arc's local half)

| Slice | Content | Gate |
|---|---|---|
| **V0** ✅ 2026-09-09 | Emitted views indented + coloured (the emitter's layout; lens spans) | `emit_layout_gate.sh`; `testmadcide` view rows |
| **V0.5 Enums, not strings** (OWNER LAW 2026-09-09) | The UI level enum in `bits/ui_enums` + a target declares its level (`ui::open(uiLevel)`); a dialect event carries the engine's key / kind codes and the handlers switch on them; profile action names resolve to registry ids at load (a misspelling refuses the profile with its line); the view / region / tab / pane discriminators become enums | a gate that fails a string compare against an event field or a discriminator in dialect dispatch (negative control); `testmadcide` byte-identical; a misspelled-action profile fixture refuses |
| **V1 Views** | `es.views` table; the lens = View{doc, mc11}; `nav_doc` → focused View; `compose_edit_node` by View id; per-View caret/mark/scroll | `testmadcide` byte-identical composition for the identity lens; GUI DOM snapshots unchanged |
| **V1.5 The `ui::NONE` client** (OWNER 2026-09-09, §2.3b) | `madcide <file> -c "<command> [arg]"`: one registry command against the session, the projection to stdout, the verdict as exit status — the headless harness with a command line | `tests/testmadcide_cli.*`: the `-c` output pinned against the headless harness for the same command; a misspelled command refuses with exit 2 |
| **V2 Containers + layouts** | pane/tab/window as client layout data; `default.layout` (inline baked default) through the profile parser family; the S5 stack, the panel and the editor tabs re-expressed; TUI panes + tabs; `view*` commands as registry data | new `.layout` parse gate with a negative control; TUI composition pinned; `tests/gui/madcide_layout` |
| **V2.5 The `ui::LINE` client** (OWNER 2026-09-09, §2.3b) | `ui_line_frontend` (stdin lines → events, the level-0 printer → stdout); `ui::open(ui::LINE)`; the colon interpreter is the command language ("the vi `:` mode without the TUI part") | `tests/testmadcide_line.*`: a scripted stdin transcript through the line frontend, output pinned; the same commands on the TUI twin agree on the document text |
| **V3 Clients + windows** | client records; `ui::event_any`; `viewwindow` opens a second window on the session; presence carets + `@presence` colours; the anchor registry replaces `shift_hspans` | `tests/gui/madcide_window2` (two windows, one edit seen in both); `check-one-anchor-owner.sh` |
| **V4 Event log** | ChangeEvent written by the mutation owner + verb seam; per-change propagation; `<base>.prj.events`; the edit-history View (`revision: event:N`) | headless replay test (log → text equality); size-rewrite test |
| **V5 Correlation** | the emitter's coordinate map → View `map`; `viewsync` source ↔ MC11 | `testmadcide` map rows > 0 for the MC11 lens; a sync round-trip pin |
| **V6 Transports + headless** | `listen://` + the WebSocket framer in madcdis; `ws` remote window; `api` line protocol; `--serve`; tiers born low; the MCP seat | a loopback ws test under the runner's caps; an api smoke test; a tier-refusal test |
| **V7 Pairs 2–3** | external-asm provider (gcc/clang Views); MIR provider; git-revision Views (madcdat adapter) | fixtures per provider; the parity method as a test |

The local half (V1–V5) is one feature ("any View in any container; a
window is a client") and gets ONE merge-wave battery when complete
(`testing-fulltest.md`). V6–V7 are the remote half, banked separately.

## 5. Standing defaults (owner veto welcome)

- Vocabulary in code and docs: `Nexus`, `Session`, `Client`, `View`,
  `Pane`, `Tab`, `Window`, `Layout`, `Provider`, `ChangeEvent`.
- Every discriminator in this arc is an enum in `bits/ui_enums` (UI level,
  transport, View representation, container kind, region, tab, tier); text
  spellings exist only in the profile files and the wire JSON and convert at
  load (`.claude/rules/enum-over-strings.md`, sharpened 2026-09-09).
- Layout files: `profiles/*.layout`, the baked default an inline profile;
  the client's live layout persists per project beside the manifest as
  `<base>.prj.layout` (the window remembers its sizes today in
  localStorage — that moves here so the TUI shares it).
- The event log file: `<base>.prj.events` (JSON lines), memory-only for
  the implicit project; a 16 MB rewrite knob in config.
- Presence colours: a `@presence` theme section, eight entries, dealt
  round-robin at connect.
- Tiers: `observe / edit / admin` as levels 0/1/2 of the `ide` domain;
  local clients `admin`, remote/MCP `observe`.
- The `api` protocol: JSON lines, one object per line, commands by
  registry name with argument arrays, replies keyed by the client's
  message id.
- Statement-level granularity for the emitter's coordinate map (V5);
  expression-level is a later refinement.

## 6. Open questions

- Should a View's `caret/mark/scroll` live on the View (shared by every
  client showing it) or on the client's container (each client its own
  place in the same View)? Recommendation: on the CONTAINER (client-private
  navigation, the state tiers) with presence publishing it — a shared View
  is one text seen from many places.
- The event log for a NON-project session that is edited by a remote
  client: memory-only means the record dies with the process. Acceptable
  for slice one? (The manifest rule says yes; the owner may want the log to
  force materialization.)
- Whether the `api` transport speaks the composed tree (trivial thin
  clients) or raw projections only (gateway open question): recommendation
  both, declared per client at connect.
- Entity identity across history (rename/move survival) — named hard
  problem, unchanged.

## 7. Traceability

- Cross-reference §6 (what the client-server step becomes) → §2 here.
- KG: Decision `post_master_order_after_gui_release` (item 1 = V2+V3 here);
  Feature `nexus_client_server_arc` to be created at the first code slice
  with this document as its plan.
- ROADMAP 8.6 post-master text and the Plan Index point here.
