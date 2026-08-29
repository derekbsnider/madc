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
| `{event:"choose", option, action}` | 1-based index + action name | Enter on the selected option — the same number the line-mode menu prints |
| `{event:"action", action, seq}` | bound name + the sequence | A bound key sequence completed (empty action = unbound miss) |
| `{event:"focus"}` / `{event:"resize"}` | — | Recompose and re-render |

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

## Thread contract

Per `.claude/rules/thread-safety.md`: a session and every world
reached through it are confined to one thread (the script's).
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
