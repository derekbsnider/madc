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

Data-derived credentials (grants carried by entities) are added per
actor at each use; `has_key` is how application verbs check
entity-attached conditions (a locked door's `requires` key) against
that same evaluator.

## Verbs and affordances

| Function | Description |
|----------|-------------|
| `bind_verb(w, name, source)` | Attach a madc-source BODY to a verb (the script-entity binding kind) |
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
close worlds. An empty `act` result means the verb is unknown (the
driver phrases that).

`affordances` enumerates what the actor can presently do, each entry
carrying its truthful visible/enabled/reason state from the SAME
keys+levels evaluator that gates execution — a frontend may hide or
disable from this, it never grants.

The pilot application (`tests/adventure_driver.inc` +
`tests/adventure_verbs/*.madv`) is the worked example: the whole game
is madc source bound through this surface.

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
