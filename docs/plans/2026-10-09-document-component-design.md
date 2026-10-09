# The document component — file buffers in the engine (split arc, Phase 1A.1)

Owner (2026-10-09): file buffers are madc's. This is the design of the
component, grounded in a read of the substrate (`include/madcdis/hub.h`,
`text_buffer.h`, `src/ns_ui.cpp`) and of every consumer it replaces
(`tools/texteditor/editor_events.inc`, `lined_core.inc`,
`tools/madcide/madcide_core.inc`, the parse-handle API, the `%load` wire) —
recon 2026-10-09 at `b89162493`. Sequencing: `2026-10-09-madc-madcide-split-
arc.md` Phase 1A.1, the critical path of the arc. The practice survey
(IPython/Jupyter, IDLE, Julia, clang-repl/Cling, Neovim, Emacs, Kakoune,
Helix, Zed, xi, clangd, rust-analyzer) is summarized in
`2026-10-09-madc-madcide-inventory.md`; its rules are applied below.

## 1. What exists, and what is missing

- **The text component exists.** `world::_texts` is a sparse
  `map<entity_id, text_buffer>`; every mutation goes through a world method
  (`text_load/insert/erase/replace/checkpoint/undo/redo`), which `ui::text_*`
  reach through `mutation_context` (`verbs.h:54-114`, a forwarding wrapper —
  "the journaling seam", with no journal behind it). `text_buffer` is a
  piece table with a lazy newline index and a snapshot undo stack whose
  entries carry an opaque payload; its contract is already "the application
  checkpoints BEFORE mutating; one semantic edit = one step".
- **Nothing observes a mutation.** Searched the concept "change observer /
  subscriber / listener / notify" over `include/madcdis/*.h` and
  `src/ns_ui.cpp`: only channel listeners and the webview host callback.
  The notification is new.
- **Everything file-facing is application bag keys** (by design, 2026-08-24:
  "Document properties (path, modified, read_only) are application bag keys
  — never touched here", `ns_ui:189-191`). lined_core's `doc_set_path` /
  `doc_kind` / `doc_untitled` / `doc_name` / `new_document` /
  `setup_document` write `path`, `kind`, `modified`, `read_only`; the `w`
  verb saves; `checks/editable.madv` reads `read_only`.
- **The one mutation path is madcide-side**: `ed_text_insert` /
  `ed_text_erase` (`editor_events.inc:722-735`) = `ui::text_*` + anchor
  shift + journal splice. lined's verbs bypass it. Undo is
  `edit_checkpoint` with payload `{caret, modified}`, one per coalesced
  input event or command; undo restores the caret from the payload and
  `modified` by hand; undo/redo are not journaled.
- **The parse handle is a bag key** (`phandle`), opened on the document's
  NAME and the session standard, refreshed by a FULL re-parse
  (`internal_program_parse_refresh`: a fresh child Program), and kept honest
  by `phandle_text` — a copy of the whole text the handle was parsed from,
  compared by string equality before an edit verb trusts the tree's offsets.
  `textrev` / `cparsed_rev` are a second, parallel version counter.
- **`%load` sends the whole text with the path as its only identity**
  (`{seq, op:"load", path, text}`); no version crosses the wire.

## 2. The design

### 2.1 Three layers, one publish point

| Layer | Lives in | Owns |
|---|---|---|
| **text component** (exists) | `madcdis/text_buffer.h`, `hub.h` | the piece table, undo snapshots, **+ a version counter** |
| **document component** (new) | `madcdis/document.h`, `hub.h` | path, file kind, read-only, saved version (⇒ modified), the parse-handle SLOT, anchors, the document-level checkpoint |
| **edit notification** (new) | `hub.h` (`world`) | one publish per text mutation, to every subscriber of the world |
| **the parse face** (new, one function) | `src/ns_madc.cpp` / `<ns_madc>` | `madc::doc_parse(w, e)`: opens or refreshes the document's parse handle to its current version |

The notification hangs on the TEXT component, not the document: every
`world::text_*` mutator publishes, for any entity with text (a document, the
change log's own buffer, a terminal screen, the REPL transcript). With no
subscriber the cost is one empty-vector check. There is exactly one
publish point because there is exactly one mutation path — the world's
methods — and a gate keeps it that way (§5).

The document is a separate sparse component (`world::_docs`), so an entity
with text need not be a file (today's `[changelog]`, `[replay-buffer]`,
`[terminal]`, `[repl]`, panel and help buffers keep working unchanged), and
a document is an entity with BOTH components. `madcdis` is the data
substrate below the compiler, so the document stores the parse handle as an
opaque `int64` plus the version it was parsed from; the compiler-facing
public one layer up does the opening and refreshing. Likewise the file
KIND is stamped by the public (`madc::file_kind_of_path` is compiler-side),
and stored by the component.

### 2.2 The change record and its delivery

```cpp
struct text_change
{
    entity_id   entity;
    uint64_t    version;     // the text's version AFTER this change
    enum kind : unsigned char { splice, load, undo, redo } kind;
    size_t      off;         // splice: start byte; load/undo/redo: 0
    size_t      old_len;     // splice: bytes erased; others: the old size
    size_t      new_len;     // splice: bytes inserted; others: the new size
    const std::string *inserted;   // splice only; NULL otherwise
};
```

- **A splice is a byte-precise range replacement** — `[off, off+old_len)`
  became `inserted` (`new_len` bytes). An `insert` is `old_len == 0`, an
  `erase` is `new_len == 0`, `replace` is one record (not erase + insert:
  an anchor inside a replaced range must land once, at `off`). Rows and
  columns are derived by the consumer from `line_of` — one event shape,
  never a second line-granular one (Neovim's `on_bytes` is the model; its
  `on_lines` is a convenience over it).
- **`load`, `undo` and `redo` are whole-buffer events** (Neovim's
  `on_reload`, LSP's Full sync): the undo stack restores a pieces snapshot
  and a delta is not tracked; a subscriber that must stay exact resyncs from
  the text. The change log does not journal undo today either.
- **Delivered synchronously, after the mutation is complete, in
  subscription order.** The world sets `_publishing` for the duration; a
  text mutator called inside a callback is REFUSED (returns false) — the
  textlock rule. A subscriber that must edit in response schedules it.
- **Version**: `text_buffer::_version`, `uint64_t`, +1 on every load /
  insert / erase / replace / undo / redo (an undo is a new version, as in
  LSP, never a rewind of the counter). `world::text_version(e)`.

Subscribers: `world::text_subscribe(text_listener *)` — a C++ interface
with one method `on_text_change(world &, const text_change &)`. The script
face registers a dialect function the same way the hub executes verbs:
`ui::doc_subscribe(w, handler)` with `void handler(long w, long entity,
value &change)` — a JIT'd function is a C-ABI pointer, as `ide_api`
handlers already are. Per world, session-thread, released with the world.

### 2.3 Anchors

An anchor is a byte position that follows edits — a caret, a mark, a
selection end, a highlight span end. Today they are es bag integers shifted
by `shift_es_anchors` with the rule in `shift_offset` (`editor_events.inc:
100-114`). The document owns them:

- `anchor_id anchor_add(e, off, gravity)` → an id stable for the document's
  life; `anchor_offset(e, id)`, `anchor_move(e, id, off)`,
  `anchor_remove(e, id)`. `gravity` is `left` (stays before an insert at
  its offset — a selection start, a span start) or `right` (moves after it
  — a caret, a span end). Both the ex core's marks (`'a`, `.` as a line
  anchor) and madcide's carets are these.
- Shift rule, applied by the publish step BEFORE subscribers run (so a
  subscriber reads shifted anchors): for a splice at `off` with `old_len`,
  `new_len` — an anchor `< off` is unchanged; one inside `[off,
  off+old_len)` collapses to `off`; one `>= off+old_len` moves by
  `new_len − old_len`; one `== off` with `old_len == 0` moves iff gravity is
  right. On `undo`/`redo` anchors are restored from the checkpoint; on
  `load` they clamp to the new size.
- madcide may keep shifting its bag integers from the notification during
  Phase 1D and move to anchors later; both are correct, and the second is
  the end state.

### 2.4 Undo groups and the modified flag

- The boundary primitive stays `text_buffer::checkpoint` — the command
  layer decides the group: one ex command, one coalesced key run, one MCP
  transaction. The document's `checkpoint(e, app_meta)` wraps it: the entry's
  payload is `{app: app_meta, anchors: [[id, off, gravity]…], saved:
  saved_version}`, so undo restores the document AND its anchors AND
  whether it was clean, and hands `app_meta` back (madcide's `{caret}` keeps
  working; the caret will become an anchor).
- **`modified` is derived, never set**: `modified(e) = version(e) !=
  saved_version(e)`. `doc_mark_saved(e)` sets `saved_version = version`
  (save, or "the file on disk IS this"). Undoing back to the saved state
  reads clean again for free — today madcide carries `modified` in the
  payload to approximate this.
- `load` (a reload from disk) clears history as `text_buffer::load` does
  today, and sets `saved_version = version`.

### 2.5 The parse face

`int64_t madc::doc_parse(long w, long e)` (in `src/ns_madc.cpp`, declared in
`<ns_madc>`): if the document's kind does not reach the compiler → 0; if
the slot is 0 → `parse_open(text, name, standard)`; if `parse_version !=
version` → `parse_refresh(handle, text)`; then `parse_version = version`;
returns the handle. Standard = the document's kind resolved by Phase 1A.4's
precedence (`--std` > extension > ini > madc). `doc_close` closes the slot.
This replaces `ensure_phandle` + `reparse_buffer` + `phandle_text` +
`textrev`/`cparsed_rev` with one version compare. The checked form
(`parse_refresh_checked`, which keeps the old tree when the new text has
more errors) is exposed as `madc::doc_parse_checked(out_diags, w, e)`.
The handle stays a handle; `parse_*` / `graph_*` take it as today. Refresh
cost is unchanged (a full re-parse — a later measured change, not this
slice's).

### 2.6 Persistence and identity

- `doc_open(w, path)` → entity: reads the file (0 on failure), creates the
  entity, attaches text + document, stamps kind from the path, `saved =
  version`. `doc_new(w)` → an untitled document (`kind = fkMADC`, matching
  `edit_lang_kind`'s rule for untitled). `doc_set_path(w, e, path)` restamps
  the kind (save-as). `doc_save(w, e)` writes the text to the path, marks
  saved; false with no path or on an I/O failure. `doc_reload(w, e)`. The
  component never looks at the disk by itself: the publics do the I/O.
- Identity for comparison is `madc::canonical_path` (the existing owner);
  `doc_find(w, path)` answers the entity already open on that file (today's
  `same_file` scan in `edit_file_as`).
- The buffer ring is the set of entities with a document component, in
  creation order: `ui::docs(out, w)`. "Active" is the application's (es
  `adoc`; the ex core's current buffer) — not the component's.
- `world_save` still does not carry text or documents (runtime-only, as
  today).

### 2.7 Where a document lives — the process rule

A document lives in the process that EDITS it, never in the one that runs
user code: under `madc -i` the prompt process (the `SessionClient` side)
holds a ui world with the documents; the forked backend receives TEXT by
`%load` / `%run` / `%build` exactly as today (`{op, path, text}`) — the
backend is a restartable, document-blind mirror (Jupyter's kernel, IDLE's
subprocess). Phase 1A.5 gives Windows the same backend. madcide holds its
own world with the same component. The wire gains nothing in this slice; a
later slice may add the version to the load so a backend can skip a
redundant re-parse.

### 2.8 Script surface (`<ns_ui>`, namespace `ui`)

Existing `text_*` publics are unchanged in name and arguments; the four
mutators and the three history verbs now return `bool` (false: refused —
inside a callback, or a read-only document). New:

```
long  doc_new(long w);                        // untitled, fkMADC
long  doc_open(long w, const char *path);     // 0 on failure
long  doc_find(long w, const char *path);     // the entity on that file, else 0
bool  doc_load_file(long w, long e, const char *path);   // reload
bool  doc_save(long w, long e);
bool  doc_close(long w, long e);              // closes the parse slot too
value &doc_path(value &out, long w, long e);  // "" when untitled
bool  doc_set_path(long w, long e, const char *path);    // restamps kind
int64_t doc_kind(long w, long e);
bool  doc_modified(long w, long e);
void  doc_mark_saved(long w, long e);
bool  doc_read_only(long w, long e);
void  doc_set_read_only(long w, long e, bool ro);
int64_t doc_version(long w, long e);          // the text's version
value &docs(value &out, long w);              // the ring, creation order
bool  doc_checkpoint(long w, long e, value &app_meta);
bool  doc_undo(value &app_meta_out, long w, long e);
bool  doc_redo(value &app_meta_out, long w, long e);
long  anchor_add(long w, long e, long off, bool right_gravity);
long  anchor_offset(long w, long e, long id);          // -1 when absent
bool  anchor_move(long w, long e, long id, long off);
bool  anchor_remove(long w, long e, long id);
bool  doc_subscribe(long w, void (*fn)(long, long, value &));
```
and in `<ns_madc>`: `int64_t doc_parse(long w, long e)`,
`int64_t doc_parse_checked(value &diags, long w, long e)`.

The change value a script subscriber receives: `{entity, version, kind
("splice"|"load"|"undo"|"redo"), off, old_len, new_len, inserted}`.

## 3. What this replaces (Phase 1D's migration list)

| madcide today | becomes |
|---|---|
| `doc_set_path`, `doc_kind`, `doc_untitled`, `doc_name`, `new_document`, `setup_document` (lined_core) | `ui::doc_*` publics |
| `ed_text_insert` / `ed_text_erase` + `shift_anchors` | `ui::text_*`; anchors shift in the engine, or madcide shifts its bag integers from the notification |
| `journal_splice` inside the mutation owner | the change log subscribes (`doc_subscribe`) — Phase 1C.15's module |
| `edit_checkpoint` / `undo_edit` / `redo_edit` with `{caret, modified}` | `doc_checkpoint` / `doc_undo` / `doc_redo` with `{caret}`; `modified` derived |
| `ensure_phandle`, `reparse_buffer`, `phandle_text`, `textrev`, `cparsed_rev`, `doc_forget_parse` | `madc::doc_parse`, `doc_version`, `doc_close` |
| `w.madv` (`file_put_contents` + `modified=false`), `checks/editable.madv` | `ui::doc_save`, `ui::doc_read_only` |
| `same_file` scan in `edit_file_as` | `ui::doc_find` |
| lined's `c/i/a/d` verbs (bypass the owner) | retired; the ex core edits through `ui::text_*` |

Not moved: carets/marks/selections as es state (madcide's; may become
anchors), the buffer-row table with its per-row caret (`es.buffers`),
Views, lenses, `cparsing` (the background-parse lock is the application's
task discipline), git keys, terminal keys.

## 4. Thread-safety contract

Per-world, confined to the session thread like every hub component.
Subscribers are called on that thread, synchronously, with mutation locked.
No globals: the subscriber list and `_publishing` are world members. A
document's mirrors (the backend, an LSP client) hold a version, never a
pointer.

## 5. Tests and gates

- `tests/unit/test_document.cpp` (doctest, beside `test_text_buffer`): the
  record for insert / erase / replace / load / undo / redo with versions;
  anchor shifts for every gravity × position case including the collapse
  and the `== off` insert; undo restoring anchors and the saved state;
  `modified` derived; read-only refused; a mutation inside a callback
  refused and the outer mutation intact; subscribers called in order; a
  subscriber on a non-document text (the change log's own buffer).
- `tests/testdocument.mad`: the script surface end to end — open / edit /
  save round trip on a `tmp/` file (removed before exit), `doc_find`,
  `doc_parse` returning the same handle across a refresh with
  `parse_generation` advancing exactly when the version changed, a dialect
  subscriber receiving the record. The suite is the oracle (an engine
  feature, no gcc analogue).
- Gate `scripts/check-one-text-publish.sh` (fulltest): `text_buffer`'s
  mutators (`load`, `insert`, `erase`, `replace`, `undo`, `redo`,
  `checkpoint`) are called ONLY from `include/madcdis/hub.h` (the publish
  point) and the unit tests — no path mutates text without publishing.
  Two-way self-test.
- Existing: `test_text_buffer`, `test_verbs`, `test_interaction`,
  `testeditcheck`, every `testmadcide*` (madcide's `ui::text_*` calls keep
  their meaning; the bool return is ignored by existing callers).

## 6. Size

`document.h` ~220 lines; `hub.h` +~140 (docs map, anchors, subscribe,
publish, version); `text_buffer.h` +~15 (version); `ns_ui.cpp` +~320;
`<ns_ui>` +~70; `ns_madc.cpp` + `<ns_madc>` +~80 (`doc_parse`); unit test
~300; integration test ~90; gate ~60. No madcide change in this slice
(1D re-plumbs); madcide keeps compiling because nothing it calls changes
signature except the bool returns.
