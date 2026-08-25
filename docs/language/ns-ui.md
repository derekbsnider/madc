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
| `text_checkpoint(w, e, meta)` | Snapshot the buffer BEFORE a mutation, with an opaque payload |
| `text_undo(meta_out, w, e)` | Restore the newest snapshot, handing the payload back (`false` = nothing to undo) |

**Undo** is buffer history on the component: a checkpoint is a
pieces-vector snapshot plus an OPAQUE application payload — store the
caret (and the modified flag) there, so undo restores document and
interaction state together. Checkpoint cadence is the application's:
one semantic edit (a coalesced text event, a cut, a paste) = one
checkpoint = one undo step. History is runtime-only like the component;
`text_load` clears it; redo is a named seat.

The line-mode editor (`examples/texteditor/` — nine script verbs through
the one registry, design doc §7.7) is the worked example: a line command
composes a range edit from `text_line_start`/`text_line_len`.

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
insensitive (JOE's `^K S` == `^K s`). Bound sequences resolve ahead of
the built-in key handling and arrive as `action` events; a chord's
prefix (`^k`) waits for its continuation (esc cancels). Validation is
loud and whole-table: unknown spellings, printable-HEADED sequences
(they would swallow typing), and a sequence shadowing a shorter binding
are refused, leaving the installed table unchanged. An empty object
clears; a profile swap is one call and never touches projections.

The renderer behind this surface is a provider: the built-in target is
the dependency-free VT100/xterm one (raw mode, alternate screen,
differential row repaint; POSIX only — on Windows `tui_open` refuses
until a Console target registers). The model half — layout, focus, key
semantics, chords, coalescing, diffing — is engine code shared by every
target. `examples/texteditor/vised.mad` is the worked example of the
editor pair (Pico-style fixed chords); `examples/madcide/madcide.mad`
is the worked example of profiles-as-data (JOE/WordStar `^K` chords
default, the pico profile respelling the same actions) and of the
compiler-data panes (`madc::diagnostics` / `madc::outline` rows
composed as a navigable `choice` whose chosen row moves the caret).

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
