# ui:: Namespace

The data-hub projection and interaction surface (Track 7). `ui::` is
how a madc program opens a **world** (a tagged data file), reads and
writes its entities through the hub's one write path, renders
**projections** (text today; richer render levels later), dispatches
**verbs**, and talks to the person at the other end (`ui::prompt`).

The working example is Colossal Cave Adventure:
`examples/adventure/` — an 11-translation-unit madc `--project`
program whose entire game state lives in a `ui::` world and whose
output is byte-identical to the original C `open-adventure` across the
94-log reference corpus.

Everything below is available in every madc TU with zero includes
(the auto-include scan serves `ui::` bare). Typed in madc's carriers:
`value`, `int64_t` handles, and `const char *` — never `std::string`.

## Interaction

| Function | Description |
|----------|-------------|
| `prompt(out, text)` | Write `text`, flush, read a line into `out`; `false` at EOF |

`ui::prompt` is the one prompt handler: it writes the prompt, flushes
(an unflushed partial line is invisible — stdio never flushes it for
you), reads a line, and returns it. When stdin is a pipe or file it
additionally echoes the returned line after the read — no terminal
exists to echo it — so a piped transcript reads exactly like an
interactive session (and EOF leaves the trailing prompt, matching the
adventure reference logs). Lines beginning `#` are script comments:
consumed silently in both modes. Reading delegates to `madc::getline`.

```c
var line;
while ( ui::prompt(line, "> ") )
	println("you said {}", line);
```

## World sessions

| Function | Description |
|----------|-------------|
| `world_open(path)` | Open a `%world` tagged file; session handle (>0), 0 on error |
| `world_new()` | An EMPTY session — no world file, no declarations |
| `world_save(w, path)` | Save the session's world (authored + runtime state) |
| `world_close(w)` | Release one session |

Sessions are independent; handles are never reused within a run. The
save is an **export**: runtime entities (actors, singletons) ride the
same file as the authored world, and saving again is a fixed point.

## Entities

| Function | Description |
|----------|-------------|
| `entity_by_name(w, name)` | Canonical-name lookup (0 = absent) |
| `create(w, name)` | New entity through the hub's mutation context |
| `location(w, e)` | Containment target (0 = nowhere) |
| `name_of(out, w, e)` | Canonical name (string kind; empty when absent) |
| `contents(out, w, container)` | Array of the NAMES held by `container`, link order |
| `links(out, w, from, rel)` | Array of `{key, target}` objects for the `rel` links from `from` |
| `resolve(w, actor, word, alias_prop)` | Word → entity over the actor's scope; matches canonical name or the `alias_prop` bag property (0 = no match) |

Relation and property names are **arguments** — application vocabulary
is data, never engine spellings (Rule #7). Two substrate conventions
are fixed by the session layer: containment is the `in` relation, and a
carried entity confers a key via its `grants` bag property.

## Entity bags (keyed state)

Reads copy OUT of the world and never vivify; writes route through the
hub's mutation context — the one write surface
(`scripts/check-hub-write-path.sh` gates it).

| Function | Description |
|----------|-------------|
| `get(out, w, e, key)` | Value at `key` (null kind when absent) |
| `set(w, e, key, v)` | Bag write — one overloaded name for `value`, `const char*`, `int64_t`, `bool`, `double` |
| `move(w, e, dest)` | Containment move (dest 0 = out of the world) |

```c
long w = ui::world_open("examples/adventure/adventure.world");
long lamp = ui::entity_by_name(w, "LAMP");
var state;
ui::get(state, w, lamp, "prop");        // keyed read
ui::set(w, lamp, "prop", 1);            // keyed write (hub-routed)
ui::move(w, lamp, ui::location(w, ui::entity_by_name(w, "player")));
```

## Text component (piece-table buffers)

An entity may carry a text component — a piece-table buffer, the hub's
second component kind ("an editor buffer = entity with a piece-table
component"). Offsets and lengths are BYTES; lines are 1-based with
length excluding the `'\n'`; a trailing unterminated span is a line and
an empty buffer has zero lines. Writes route through the hub's mutation
context; size/line/find reads answer −1 when the entity has no
component (or the query is out of range). The component is RUNTIME-ONLY:
`world_save` does not carry it — a document persists to its own file
(the editor's `w` verb via `php::file_put_contents`). Document
properties (`path`, `modified`, `read_only`) are application bag keys.

| Function | Description |
|----------|-------------|
| `text_load(w, e, text)` | Reset the component to `text` (creates it) |
| `text_insert(w, e, off, text)` | Insert before byte `off` (clamped) |
| `text_erase(w, e, off, len)` | Erase the byte range (clamped) |
| `text_replace(w, e, off, len, text)` | Erase + insert |
| `text(out, w, e)` | The whole document (string kind) |
| `text_size(w, e)` | Byte size (−1 = no component) |
| `text_line_count(w, e)` | Lines (−1 = no component) |
| `text_line(out, w, e, n)` | Line `n`'s text (empty when absent) |
| `text_line_start(w, e, n)` | Line `n`'s byte offset (−1 when absent) |
| `text_line_len(w, e, n)` | Line `n`'s length sans `'\n'` (−1 when absent) |
| `text_line_of(w, e, off)` | The 1-based line containing byte `off` (indexed; `off == size` after a terminated last line answers `line_count + 1`, the phantom line; −1 = no component) |
| `text_find(w, e, from, needle)` | First occurrence at/after `from` (−1 = none) |
| `text_word_left(w, e, from)` / `text_word_right(w, e, from)` | Word motion (JOE `^Z`/`^X` duals over `[A-Za-z0-9_]`): the previous word's first byte / just past the next word's end (−1 = no component) |
| `text_checkpoint(w, e, meta)` | Snapshot the buffer BEFORE a mutation, with an opaque payload |
| `text_undo(meta_out, w, e)` | Legacy destructive undo: restore the newest snapshot, handing the payload back (`false` = nothing to undo); clears redo |
| `text_undo(meta_out, w, e, now_meta)` / `text_redo(meta_out, w, e, now_meta)` | The redo-preserving pair — see below |

**Undo/redo** is buffer history on the component: a checkpoint is a
pieces-vector snapshot plus an OPAQUE application payload — store the
caret (and the modified flag) there, so undo restores document and
interaction state together. Checkpoint cadence is the application's:
one semantic edit (a coalesced text event, a block op) = one
checkpoint = one undo step. History is runtime-only like the component;
`text_load` clears it. Redo is two stacks: the four-argument
`text_undo` and `text_redo` take `now_meta` — the payload live on the
document being LEFT — and capture it onto the opposite stack, so
walking either direction restores document + interaction state
together. A checkpoint is a new edit branch and clears redo; the
three-argument `text_undo` is the legacy destructive form (no capture,
so it clears redo too).

The line-mode editor (`tools/texteditor/` — nine script verbs through
the one registry, design doc §7.7) is the worked example: a line command
composes a range edit from `text_line_start`/`text_line_len`.

## The view seam — the document lens's coordinate map

An editor differentiates what is DISPLAYED from what is STORED (the
document lens: display = get(stored); an editable lens has a put). The
display↔stored byte correspondence rides as DATA — an array of
`{disp, stored, len}` copy-segment rows (`madcdis/doc_lens.h`'s codec).
Stored bytes outside every segment are CONCEALED (markdown formatting
characters, folds); display bytes outside every segment are SYNTHETIC
(rendered code views, decoration). Caret/selection TRUTH stays in stored
offsets; these publics are the ONE projection owner's dialect face —
never re-derive caret math per view.

| Function | Description |
|----------|-------------|
| `lens_to_display(map, stored)` | Project a stored caret offset into display space |
| `lens_to_stored(map, display)` | Project a display caret offset into stored space (the put direction's coordinate half) |

Contract (pinned by `tests/unit/test_doc_lens.cpp`): 1:1 inside a copy
segment, caret ends included; strictly inside a gap collapses FORWARD to
the next segment's start in the other space; a boundary shared by a copy
end and a gap belongs to the copy that ends there (the inverse of a
gap-adjacent caret lands at the EARLIER position — outside a concealed
run, the safe side for a put); past the last segment lands just after its
image; a valid EMPTY map (a wholly rendered view) answers 0; −1 =
malformed map (the strict codec refuses whole) or a negative offset.

The first consumers are madcide's read-only code views (`^K A` cycles
original → MC11 → C11 — and, on a C/C++ buffer, → C++ — over the one
document): the rendered text comes from `madc::emit` over the live
buffer, the lens data (view name + map) rides the edit node's hints, and
the identity lens — no view — adds nothing (the composed tree is
byte-identical to a plain editor's). The C++ view's render is the
retained source (the reverse render), so it applies only where the
buffer's own language is C/C++ — the app's document-kind rule.

## Access model and projections

Projection selection IS the access decision: what a session may see is
decided by which projections its credentials render, not by ad-hoc
checks in driver code.

| Function | Description |
|----------|-------------|
| `session_grant(w, key)` | Grant a role key to the session |
| `session_level(w, domain, level)` | Set a per-domain level |
| `has_key(w, actor, key)` | Does the actor's EFFECTIVE credential set hold this key? |
| `render_inspect(out, w, target)` | Generic entity inspector — gated by the world's `%require inspect` |
| `inspect_tree(out, w, target)` | The SAME inspect projection as walkable DATA (see below) |
| `render_tree(out, w, tree)` | Typeset any value-shaped projection tree through the level-0 renderer |

Data-derived credentials (grants carried by entities) are added per
actor at each use; `has_key` is how application verbs check
entity-attached conditions (a locked door's `requires` key) against
that same evaluator.

### Projection-as-data

A projection is an ordinary value tree: sparse objects of
`{ role, label, content, hints, states[], actions[], subject,
children[] }`, with `role`/`states`/`actions` spelled by NAME and
`subject` an entity handle. `inspect_tree` returns the inspector's tree
in that shape (a gate refusal arrives as a `status`-role node — the same
walk handles it), and `render_tree` typesets any such tree an
application composes, so `render_tree(inspect_tree(...))` reproduces
`render_inspect` byte-for-byte. Roles the level-0 renderer typesets:
`heading`, `content`, `status`, `item`, `action`, `separator`, `list`
(labeled), `choice` — a `choice` node's children are its OPTIONS,
numbered in line mode; a selection-capable renderer reads the SAME tree
— and `edit` (an editable text region bound to a document: content =
the text, hints carry `{caret, sel_start, sel_end}` byte offsets),
which level 0 linearizes as its text and the TUI presents as a scrolled
window. `group` and unknown roles are structure only.

```c
value menu;
menu["role"] = "choice";
menu["label"] = "Which way?";
value north;
north["role"] = "item";
north["content"] = "Go north";
var kids = {};
kids.push(north);
menu["children"] = kids;
value text;
ui::render_tree(text, w, menu);     // "Which way?\n  1. Go north\n"
```

## Verbs and affordances

| Function | Description |
|----------|-------------|
| `bind_verb(w, name, source)` | Attach a madc-source BODY to a verb (the script-entity binding kind) |
| `bind_check(w, name, source)` | Attach a madc-source availability CHECK to a bound verb (see below) |
| `bind_require_key(w, key)` | Arm code-entity key-gating: later binds require the session's credentials to hold `key` |
| `act(out, w, actor, verb, rest)` | Interpret + dispatch one invocation; result text is player-facing |
| `affordances(out, w, actor)` | Array of `{action, target, provider, label, visible, enabled, reason}` |

**The engine ships zero verbs** (Rule #7). A world's `%verb` lines
DECLARE actions and their gating (keys/levels/refusal — data); the
application attaches bodies as madc source via `bind_verb`. A body is
compiled as an eval unit per invocation with the invocation's fields as
top-level names — `w` (session handle), `actor`, `target` (entity
ids), `arg` (the raw argument text, `const char *`), `verb` (the
action's spelling) — and returns the player-facing text. Bodies run
inside `ui::act`: they must not re-enter `act` and must not open or
close worlds — ENFORCED: a nested `act` on the same world is refused
with `action re-entered the registry (verbs do not re-enter act)`,
which the outer body receives as that call's result. An empty `act`
result means the verb is unknown (the driver phrases that).

**Code-entity key-gating** (the hub's Decided rule: defining or editing
code entities is itself key-gated): after `bind_require_key`, every
`bind_verb`/`bind_check` on the session requires the armed key(s) in
the session's effective credentials — `session_grant` and the world's
key implications apply as everywhere else. A refused bind is loud on
stderr and binds nothing. Unset (the default) is open.

`affordances` enumerates what the actor can presently do, each entry
carrying its truthful visible/enabled/reason state from the SAME
evaluator that gates execution — a frontend may hide or disable from
this, it never grants.

**Availability checks** are the state-conditional half of that
evaluator ("a read-only document disables the edit verbs"). A check
bound via `bind_check` runs with the same context fields as a verb body
and answers `"ok"` for available or the refusal reason otherwise; any
other outcome — an eval failure included — disables the verb with a
loud generic reason, never a silent pass. The SAME evaluation answers
`affordances` (each verb probed with an argument-less invocation over
the actor's context) and gates `act`, so enumeration and dispatch can
never disagree; an unmet keys/levels requirement answers first. Check
bodies are READ-ONLY by contract: no mutation, no `ui::act`, no session
lifecycle. One body can serve many verbs (it receives `verb`) — the
texteditor binds `checks/editable.madv` to all five document-mutating
commands.

The pilot application (`tests/adventure_driver.inc` +
`tests/adventure_verbs/*.madv`) is the worked example: the whole game
is madc source bound through this surface.

## Level-1 TUI (the grid frontend)

| Function | Description |
|----------|-------------|
| `tui_open()` | Enter grid mode; TUI handle (>0), 0 with the reason on stderr |
| `tui_close(t)` | Restore the terminal and release the handle |
| `tui_rows(t)` / `tui_cols(t)` | Current surface size (−1 = bad handle) |
| `tui_render(t, w, tree)` | Compose a value-shaped projection tree onto the grid; only changed rows repaint |
| `tui_bind_keys(t, table)` | Install a keybinding PROFILE (key sequences → action names); a swap is one call |
| `tui_event(out, t, w)` | Block for the next SEMANTIC event object; `false` at end of input |
| `tui_suspend(t)` / `tui_resume(t)` | Hand the terminal to a child process (JOE `^K Z` shell) and re-enter: suspend restores the screen/modes as found; resume re-reads the size and forces the next render to repaint every row |

The loop is compose-as-data → `tui_render` → `tui_event` → apply. The
SAME tree `render_tree` typesets sequentially presents on the grid: a
`choice` menu becomes NAVIGABLE (arrows move the selection, enter
chooses, tab cycles focus between the tree's choice/edit nodes), and an
`edit` node becomes a scrolled document window whose caret and
selection ride its hints (`{caret, sel_start, sel_end}` — byte
offsets). Events arrive as value objects, names at the boundary:

| Event | Payload | Meaning |
|-------|---------|---------|
| `{event:"text", text}` | the run | Coalesced printable keys — ONE semantic insertion |
| `{event:"key", key}` | `"up"`, `"enter"`, `"^s"`, ... | A non-printable key for the application |
| `{event:"choose", option, action, action_code}` | 1-based index + action name + the option's `code` hint (0 = none) | Enter on the selected option — the same number the line-mode menu prints |
| `{event:"action", action, action_code, seq, arg?}` | the bound name, the bound CODE (an integer bound in the table, or the `code` hint the posting control carried; 0 = none), the chord's spelling, the argument a native control carried | a chord the bindings table resolved (`seq`), or a native control (a menu item, a dialog button, a tab) that fired the command by id — `arg` when the control carried the command's argument (a buffer tab's ring index); commands take arguments, the gateway shape |
| `{event:"action", action, action_code, seq}` | bound name + code + the sequence | A bound key sequence completed (empty action and code 0 = unbound miss) |
| `{event:"focus"}` / `{event:"resize"}` | — | Recompose and re-render |
| `{event:"wake"}` | — | Cooperative background tasks drained: recompose |
| `{event:"snapshot", text}` | the page's rendered text | A web target's page answered `madcSnapshot()` (the test seam) |
| `{event:"pointer", phase, offset, subject, tag}` | `"down"` / `"drag"` / `"up"`, a BYTE offset, the entity the node projects, the node's `tag` hint | A pointing-device gesture on an `edit` node, resolved by the engine to the offset in that node's text (the web target's hit test today; a terminal's mouse reporting takes the same shape). `subject` is absent when the node projects nothing — set it (the edit node's `subject` = its document) so a multi-window composer knows which document was hit; `tag` echoes the integer `tag` hint the composer stamped on the node (absent when it carried none) — the composer's OWN identity for the node, the one answer when two windows show the same document (madcide stamps the window index). `offset` is `-1` for a press on the node at no text position (a window's header): activate, place nothing. The shared editor core's `edit_pointer` is the one caret model for it: a press places the caret, a drag lights the selection through the same mark (and end, under a two-point personality), a release ends the gesture |

**The vocabularies are enums.** Every event object also carries the
enumerator values beside the names: `event_code` (`ui::event_kind`),
`key_code` on a key event (`ui::key`), `phase_code` on a pointer event
(`ui::pointer_phase`). Compare against those — `ev["key_code"] ==
ui::key::down` is checked by the compiler, where `k == "down"` was a
string a typo turned into a silent miss. The three enums are ONE text,
`include/madc/bits/ui_enums`, that the engine (`tui_key`,
`tui_event_kind`, `pointer_phase` alias it) and `<ns_ui>` both include, so
there is no second copy to drift. Control chords (`"^s"`) and printables
stay names: a chord is a key plus a letter, the bindings-table vocabulary.
`ui::key_code(name)` turns a key spelling into its enumerator
(`ui::key::none` when it is not one) for an application that synthesizes a
key event from an action name (the editor's "the action name IS the key
spelling" rule).

**Keybindings are data.** `tui_bind_keys` installs a whole profile: a
value object mapping key SEQUENCES to action names
(`{"^k s": "save", ...}`) — sequences are space-separated key
spellings (the same names key events carry), any length, letter-case
insensitive (JOE's `^K S` == `^K s`), and ctrl-insensitive in the
CONTINUATION position (JOE's `^K ^Z` == `^K Z` — users keep ctrl held;
ctrl+punctuation like `^_` has no letter form and stays itself). Bound
sequences resolve ahead of
the built-in key handling and arrive as `action` events; a chord's
prefix (`^k`) waits for its continuation (esc cancels). Validation is
loud and whole-table: unknown spellings, printable-HEADED sequences
(they would swallow typing), and a sequence shadowing a shorter binding
are refused, leaving the installed table unchanged. An empty object
clears; a profile swap is one call and never touches projections.

**Highlight spans, styles, and colour schemes (AST-2; owner: VT-102
ANSI / JOE parity).** An `edit` node's hints may also carry `spans`: an
array of `{s, e, c}` rows — document byte offsets plus a style SPEC in
JOE's vocabulary: the attribute words `bold dim italic underline blink
inverse` (JOE's `reverse` accepted), a foreground colour
`black red green yellow blue magenta cyan white`, and a `bg_<colour>`
background — e.g. `"bold yellow"`. Bold-as-bright gives the 16
effective foreground colours (the VT-102 / 8-colour-terminal model;
256/true-colour is a named later seat). A row whose spec contains any
unknown word is skipped whole — spans are presentation. The model
paints them through the same range-overlap rule as the selection, and
the selection paints LAST (it wins where they overlap). The VT100
target emits reset-then-SGR per style change; a grid with only
normal/reverse produces the byte-identical stream it always did. The
web target reads the SAME rows through the same spec parser
(`madcdis/ui_style.h`, the one owner) and renders the style as the
page's classes — `st-<attribute>`, `fg-<colour>`, `bg-<colour>` — over
one palette of sixteen CSS custom properties (`pal-<colour>` and
`pal-<colour>-bright`; `bold cyan` reads the bright entry, as a
terminal shows it), so the window shows the scheme the terminal shows
and a class the scheme leaves unstyled is plain in both (the gate
`scripts/check-one-style-vocabulary.sh` keeps the page free of a
syntax vocabulary of its own). The
renderer never classifies: `madc::parse_spans` (see `eval.md`) provides
classification rows from a parse handle's retained state, and a colour
SCHEME — app data, `profiles/*.theme`, one `class SPEC` pair per line
(class first; the same comment/blank line rule as the keybindings) —
maps class names to style specs. madcide swaps schemes by name at
runtime (the ^T Options pane's Scheme row; `default.theme` and
`classic.theme` ship).

The renderer behind this surface is a provider: the built-in target is
the dependency-free VT100/xterm one (raw mode, alternate screen,
differential row repaint; POSIX only — on Windows `tui_open` refuses
until a Console target registers). The model half — layout, focus, key
semantics, chords, coalescing, diffing — is engine code shared by every
target. `tools/texteditor/vised.mad` is the worked example of the
editor pair (Pico-style fixed chords); `tools/madcide/madcide.mad`
(a tool, not an example) is the worked case of profiles-as-data
(the full JOE/WordStar set by default — `profiles/joe.keys` is the
readable truth; the pico profile respells the single-chord subset) and
of the compiler-data panes (`madc::diagnostics` / `madc::outline` rows
composed as a navigable `choice` whose chosen row moves the caret; its
`^K H` help pane projects the loaded profile's own lines — help is data
like the bindings).

## Level-3 web target (the same tree in a window)

`ui::open(level)` names the RENDERING MODEL a program wants — `ui::level`
from `<bits/ui_enums>`, the ordered enum `ui::NONE < ui::LINE < ui::TUI < ui::WEB <
ui::GUI < ui::GFX2D < ui::GFX3D` (owner 2026-09-09: escalating requirement and
platform specificity; a program composes for the highest level it wants, a
lower level ignores the hints it cannot show, a target below the requested
level refuses with the reason). The loop is the same on every level —
compose-as-data → `ui::render` → `ui::event` → apply — and the event
objects are the ONE vocabulary tabled above. `ui::TUI` is the grid frontend:
the `tui_*` functions are its spellings over the same handles (`tui_open()`
IS `open(ui::TUI)`), kept as the level-1 API. `ui::WEB` is the web target: the
value tree rendered as a DOM through the platform's own webview (WebKitGTK /
WKWebView / WebView2) by `<ns_ui_web>`, a madc fragment that does `import
madcwebview;` and registers itself as the host serving `ui::WEB` — the engine
never names a platform library. `ui::level_of(t)` reports the opened
target's level; "a real terminal exists" is `level_of(t) <= ui::TUI`.
`open(name)` (`"term"`, a registered host's name) remains for name-addressed
opening — a test's fake host — never a program's spelling of what it wants. The window is
a second frontend over the same models: `web_model` composes keyed DOM
operations the embedded page applies, and turns the page's raw key
spellings and printable runs back into the same semantic events the
terminal emits — chords resolve in `key_resolver`, focus moves in
`focus_state`, the keys → events loop is `ui_apply_keys`; no key ever
means anything in JavaScript.

| Function | Description |
|----------|-------------|
| `open(level)` / `open(name)` / `level_of(t)` | Handle (>0), or 0 with the reason on stderr: no target serves the level here (`ui::NONE` / `ui::LINE` have no frontend yet), unknown name, cannot serve here (no tty; no display; the `madcwebview` library absent — its row is LAZY, so the program still compiles and runs without it); one already open. `level_of` = the opened target's `ui::level`, -1 for a bad handle |
| `close(t)` / `rows(t)` / `cols(t)` | As the `tui_*` twins; a web target's rows/cols are the viewport in text cells, reported by the page |
| `render(t, w, tree)` / `event(out, t, w)` | The one loop; `event` blocks until the page posts one event |
| `bind_keys(t, table)` / `validate_keys(table)` / `pending(out, t)` | Profiles are data on every target |
| `suspend(t)` / `resume(t)` / `refresh(t)` | Terminal capabilities — a window answers `false` / no-op |
| `eval_page(t, js)` | Script text into a page-hosted target (the test seam: `madcSnapshot()` posts the rendered text back as a `snapshot` event); `false` on the grid. Evals before the page has loaded are dropped by the platform view — the first `resize` event is the page's ready signal |
| `register_host(name, level, ops)` / `post_event(ctx, json)` | The script-hosted target seam `<ns_ui_web>` rides: a table of C function pointers (open / close / eval / run / menu / dialog / tick — the last three optional: `tick(host, ms)` arms a one-shot UI-thread timer that ends the host's next loop after `ms` when no event did, so the engine can hand its cooperative tasks the CPU while the window idles — asked only while tasks are live, so an idle window still blocks for free; `menu(host, json)` draws the native menu bar from the engine's menu JSON, `{"bar":[{"title","items":[{"id","title","key"?,"enabled"}\|{"sep"}]}]}`, sent only when it changed; `dialog(host, json)` shows the native file dialog a request describes, `{"mode":"open"\|"save","title","path"}`, answering later through the door) registered once from a fragment's static initializer, and the host's one inbound door for the page's event objects (`{"kind":"key","key":"^k"}`, `{"kind":"text","text":"abc"}`, `{"kind":"resize","rows","cols"}`, `{"kind":"snapshot","text"}`, `{"kind":"pointer","phase","key","line","col"}` — a gesture hit-tested by the page to an edit node's line index and UTF-16 column, which the engine resolves to a byte offset over the rows it emitted; `line` and `col` both omitted = a press on the node at no position (a window's header) — `{"kind":"action","action":id}`, a native menu selection, a dialog button or a popup's dismissal — the same action event a bound chord produces, and `{"kind":"dialog","mode","path"}`, a file dialog's answer, `""` = cancelled). `tests/testuihostfake.mad` is a display-free host that proves the seam in every lane; the host DECLARES the `ui::level` it serves (`ui::WEB` for a page) and `open(level)` picks the first registered host of that level |
| `dialogs(t)` / `dialog(t, json)` | Native file dialogs: can the target show one (a window whose host draws chrome; the terminal cannot), and show the one the request describes — true = up, the answer arrives as an `{event:"dialog", mode, mode_code, path}` event (`mode_code` = `ui::dialog_mode`); false = not here, the application falls back to its own prompt |

`madc::module_available("madcwebview")` answers whether the window can
exist on this system; `tools/texteditor/vised.mad <file> --web` is the
worked example (`tests/gui/ui_web_hello.mad` / `ui_web_edit.mad` are the
suite's, under Xvfb in JIT, exe and `.o`).

### The workbench (madcide GUI mode)

`madc tools/madcide/madcide.mad <file> --gui` is the web target's first
customer. ONE client loop and ONE composer serve the terminal and the
window alike; the target is the `--gui` flag. The composer stamps additive
LAYOUT HINTS the terminal ignores (its "unknown hints are ignored" rule)
and the window honours:

| Hint | On | The window |
|------|----|-----------|
| `region` (string) | any container / status / editor node | a workbench grid slot: `rail` · `sidebar` · `editor` · `panel` · `statusbar`; the nodes docked into one region STACK in the slot in tree order |
| `rows` (N) | an edit node | a fixed height of N text cells — the same hint the terminal reads (an inactive window of a ^K O split); without it the editor flexes |
| `tag` (N) | an edit node | the composer's own identity for the node, echoed as data on the pointer events it yields (madcide: every window's edit node carries its window index while split) — no renderer reads it for itself |
| `terminal` (1) | an edit node | the embedded Terminal's screen (madcide's Terminal tab: the `[terminal]` buffer a program's pty bytes fold into through `ui::term_feed`) — the page styles it as a terminal surface; a press on it gives the running program the keyboard |
| `tabs` (`[{title, action, arg?, active?}]`) | a group (madcide's bottom panel), a content node docked first into the editor region (madcide's buffer tabs) | a tab STRIP as data — the page draws it above the node's children and a click on a tab posts its `action` by name with its `arg` (madcide: Problems / Output / Terminal show the panel on that tab; a buffer tab posts `bufsel` with the buffer's ring index) |
| `popup` (1) | a palette / quick-pick / prompt | a centered floating overlay |
| `dismiss` (action id) | a popup | the action a press OUTSIDE the popup fires (a prompt's cancel), posted as `{"kind":"action"}` — data, never a key |
| `prompt` (`{label, input}`) | the prompt row (a content node) | the core's prompt as a QUICK INPUT: the label over a field showing the input text the CORE holds, a caret after it, the keys it answers to beneath; typing still travels the one input path — the page never edits the text (Neovim's `ext_cmdline` shape) |
| `confirm` (`{label, choices:[{label, action}]}`) | the confirm row | a question as a DIALOG: the label and one button per answer, each posting its action by name — what its key does in the terminal |
| `dialog` (`{title, filter?, buttons:[{label, choose:1}\|{label, action}]}`) | a list `choice` (madcide's Build / Project / Options / Modes / Help panes) | the list pane as a titled DIALOG (the quick-pick shape): the title bar, a filter field showing the text the CORE holds (typing still travels the one input path), the option rows as pick targets, and buttons — a `choose` button picks the selected row and a click on a row picks that row (both arrive as the `choose` input the one focus owner resolves into the SAME choose event Enter produces), an `action` button posts its action by name (the pane's cancel, from its `@scope` table). The terminal ignores it |
| `items` (`{left:[…],right:[…]}`) | the status node | the status bar as chrome: each side a row of SEGMENTS `{seat, label, text}` — one per JOE format seat that showed text (`%n` the name, `%r`/`%c` with their `Row`/`Col` labels, `%m` the modified badge, `%k` the pending chord …), laid as discrete themeable items (`.sb-seat.sb-<letter>`) in the proportional chrome font on the bar's own surface — the name leads, labels are small captions, the modified / read-only badges are accent pills, the pending chord a key cap — and while split each window's header is the same bar with the ACTIVE window's header marked; the terminal shows the same expansion as one string |
| `theme` (`{name:value}`) | the root | CSS custom properties (`--name`) — the `@gui` theme scope; the chrome reads `bg` `fg` `accent` `font` `chrome-font` and the status bar's `sb-bg` `sb-fg` `sb-line` `sb-label` `sb-name` `sb-active` (the seats' own: `sb-context`), and the sixteen-colour palette the highlight styles read: `pal-<colour>` / `pal-<colour>-bright` for `black red green yellow blue magenta cyan white` |
| `menu` (`{bar:[{title,items:[{id,title,enabled?}\|{sep}]}]}`) | the root | the command / menu contribution: the bar's menus in order, each item a command id + title, `enabled: 0` when its `[when]` fails now; a chrome-rendering client draws it natively, the terminal ignores it |

**Menus and commands are data.** `profiles/default.menu` is the one
description a menu bar, a command palette, or any client that renders
commands natively reads (the VS Code contribution shape: a command registry
plus a menu-location map, one action vocabulary): one line per item,
`MENU COMMAND TITLE… [WHEN]`, `MENU -` a separator, comments and blanks as
in every profile file. `COMMAND` is an action id from the vocabulary the
key profiles bind and the dispatcher understands — never a key spelling: a
renderer shows the chord the LOADED profile binds to the id, so the
composed tree stays profile-independent. `[WHEN]` names the context that
enables the item (`key`, `!key`, joined by `&&`; the keys: `editable`
`dirty` `selection` `split` `buffers` `project` `building` `modal`
`viewing`), judged against the session's live facts at every compose. The
menu named `palette` lists palette-only commands (a title for every
command the bar does not carry). `scripts/check-madcide-command-registry.sh`
(fulltest) keeps the three in agreement: every profile action is registered
or a key spelling, every dispatched action is registered, every registered
command is dispatched.

**The native menu bar.** The window draws that description as real
application chrome: the engine resolves each command's bound chord from the
installed key profile (`key`, the shortest sequence bound to the id — the
composed tree stays profile-independent, the host's view carries the keys)
and hands the host the whole menu whenever any title, key or enablement
changed. The host (`<ns_ui_web>`) walks it item by item into madc's
extension of the webview library (`madcwebview_menu_begin` / `_add` /
`_separator` / `_end`, declared in the embedded `webview.h` beside
upstream's API; `src/madcwebview_chrome.cc` implements them in
`libmadcwebview`) on every platform the library builds for: GTK4 draws a
`GtkPopoverMenuBar` over a `GMenu` model, the webview re-parented beneath
it, each item an action under the `menu.` prefix; Cocoa sets the
application's main menu (the bar at the top of the screen; its first,
application menu carries Quit as the window's own close — the close
button's path, never `terminate:`); Win32 sets an `HMENU` bar on the window
and reads `WM_COMMAND` through a subclass of the library's window
procedure. One reading of the key spelling serves all three: a CONTROL key
(`^s`) becomes the item's accelerator (a GTK accel, a Cocoa key equivalent,
Win32's accelerator column), while a chord (`^k d`) or a bare key (`pgdn`,
a letter) is shown beside the title and never bound — bound, it would fire
on every such keystroke typed into the page; unbound, the page still
delivers it to the engine, which resolves it as the terminal would.
Choosing an item posts `{"kind":"action","action":id}` back through the
host's one door, so the session dispatches it exactly as it dispatches the
chord. `ui_web::menu_activate(id)` fires an item by id — the test seam
`tests/gui/madcide_menu.mad` drives, under Xvfb on Linux and natively on
the owner's Windows 11 box and Intel Mac.

**Native file dialogs.** Open File and Save As are a request / response
VERB between the session and its client, never a widget the session
draws: a client that can show native dialogs pushes the fact
(`IdeSession::dialogs`, from `ui::dialogs(t)`), and then those commands
park a `filedialog` request — mode, title, the initial path — in the same
slot the terminal requests ride; the client shows it (`ui::dialog`, the
host's `dialog` op, `madcwebview_dialog_open` / `_save`: a `GtkFileDialog`
on GTK 4.10+, an `NSOpenPanel` / `NSSavePanel` sheet on the window on
Cocoa, `IFileOpenDialog` / `IFileSaveDialog` on Win32 — asynchronous on
each: the call returns once the dialog is up and the answer arrives later,
inside the platform loop the host is already in) and the answer comes
back as a `dialog` event the dispatcher applies — open edits the chosen
file, save writes the buffer under the new path (its buffer row follows),
`""` cancels. Without the fact (the terminal, a headless session) both
commands keep JOE's prompts exactly as before, and a host that refuses the
request (no window, a fake host) falls back to the same prompts through
`IdeSession::dialog_fallback`. Paths are the one local workspace's; the
URI scheme / authority addressing of the two-sided file model lands with
the remote-transport arc. `tests/testidedialog.mad` pins the verb
headless; `testuihostfake` the seam.

**The prompts as dialogs.** Every prompt the session runs on the terminal's
bottom line — find, go to line, insert file, theme, tab width, the vi colon
line, the project window's add — is the SAME row in the window, floated as a
quick input; the quit question on a dirty buffer (JOE's `(y,n,^C)?`) is a
confirm dialog. The composer stamps the data ADDITIVELY on the row
(`compose_overlay_row`: `popup`, `prompt` / `confirm`, `dismiss`), the
terminal keeps its one-line text, and the core keeps owning the input: a
key reaches the prompt exactly as before, the page only draws what the core
holds. A dialog BUTTON (and a press outside the quick input) posts an
ACTION by name — `pyes`, `pcancel` — and the prompt arms admit it through
`scope_action_named`: an action event is accepted when the modal scope
(`@confirm`, `@prompt`) binds that name to some key, so a button reaches
exactly what a key can and any other command mid-prompt stays what it was
(inert for a prompt, a cancel for a question). The confirm's buttons are the
scope's actions with titles (`confirm_choices`: Yes = `pyes`, No =
`pcancel`), so a profile that respells the keys keeps the dialog honest.
`tests/testidehints.mad` pins the hints and the action-by-name arms
headless; `tests/gui/madcide_prompt.mad` the popup, the typing, the
dismissal and the buttons in the real page.

The renderers read `region` through `hint_str` (the string twin of
`hint_of`), so a node without a hint carries none — the terminal tree is
byte-identical. The grid places SLOTS, one per region a parent's children
name, never a node: two nodes docked into one region stack inside its slot
(a grid area given two direct children would overlap them), and a child
that names no region flows to the `foot` slot under the status bar (the
message line) instead of the first empty grid cell. The ^K O split rides
this: with two-plus windows the composer docks EVERY window's status line
and edit node into `editor`, so the window stack reads as JOE's screen —
a status line heading each window, the inactive windows at their `rows`
height, the active one flexing and marked; the status bar slot is empty
while split, and the single window's status returns to it. A press picks
the window: in another window's text it activates that window (its `tag`,
the window index, names it even when both show the same document) and
places the caret there; on a window's header (its status line) the page
posts the window's edit node with NO position and the session activates it,
caret kept — `tests/gui/madcide_split.mad` drives both in the real page. `@gui prop value` lines in `profiles/*.theme` feed the
web colours/fonts through the ONE `@scope` rule the key tables use
(`scope_line_parts`, shared by `parse_keys` and `load_theme`); the
unscoped JOE-vocabulary lines still feed the terminal — one file, two
renderings. While a build streams, its output docks as the `panel` region
(the VS Code Output shape); interactive Run and the shell use the terminal
until the embedded terminal lands. `tests/gui/madcide_{workbench,theme,
render}.mad` are the suite's fixtures (Xvfb, JIT / exe / `.o`); the full
`--gui` loop is a manual gate.

**Remote X displays.** GTK4 paints the window through its GL renderer,
which on a software GL (any X server without a GPU) presents frames over
MIT-SHM. An X connection over TCP has no shared memory — `DISPLAY` names
a host, as a Docker container talking to a Windows or remote X server does
— and there the GL path paints a BLACK window, or never paints the region a
window grow exposes (measured 2026-09-08 against `Xvfb -listen tcp`). The
web target therefore defaults `GSK_RENDERER=cairo` (GTK's XPutImage
renderer) when `DISPLAY` names a host and no renderer was chosen; a local
display keeps GTK's default. Set `GSK_RENDERER` yourself to override. The
software-GL flags (`LIBGL_ALWAYS_SOFTWARE=1`,
`WEBKIT_DISABLE_COMPOSITING_MODE=1`) do not cure the black window on their
own. A relative `LD_LIBRARY_PATH` (e.g. `lib`) breaks WebKit's sandboxed
web-process launch (`bwrap: execvp … WebKitWebProcess: No such file`, exit
133) — use an absolute path or none (the module map finds `lib/` itself).

## Thread contract

Per `.claude/rules/thread-safety.md`: a session and every world
reached through it are confined to one thread (the script's).
A ui frontend (grid or window) and its host are confined to the thread
that opened them; a web target's page callback runs inside the host's
loop on that thread (`webview_dispatch` is the only cross-thread door and
is unused).
`ui::prompt` operates on the process-global stdio streams under
stdio's own locking — one prompting thread at a time is the supported
shape.

## See also

- `docs/plans/2026-08-20-data-hub-projection-rendering.md` — the
  Track 7 design (hub, projections, render levels, access model).
- `examples/adventure/` — the complete worked example.
- [sys-object.md](sys-object.md) — the `madc::` namespace surface
  (`madc::sys`, `madc::getline` — the raw line reader `ui::prompt`
  delegates to).
