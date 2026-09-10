# Slice V2 — containers and layouts (the client-server arc)

Design: `2026-09-09-nexus-client-server-design.md` §2.2 (containers; the two
owner rulings: layout flexibility, the editor split tree + fixed-slot chrome)
and §4 row V2. Branch: `feature/client-server-views-claude` (the arc branch;
targeted gates per sub-slice; the battery is the V5 seam,
`testing-fulltest.md`). Vocabulary: §0 (Session, Client, View, Pane, Tab,
Window, Layout).

**The owner's question that opened the slice (2026-09-09):** "which V# has
the side-by-side view?" — this one. A `split vertical` node in the editor
region holds two leaf panes side by side, each a tab over a View; source on
the left, its MC11 on the right. V5 adds the correlation (`viewsync`).

## 1. Where the code stands (facts, 2026-09-09, after V1.5)

- **State on the bag today.** `buffers` rows `{doc, path, caret, mark, bend,
  view}` + `bufidx` (the editor tabs, one per open document; the row is the
  tab's saved navigation); `windows` rows `{bufidx, caret, grow}` + `winat`
  (the S5 stack, fewer than two rows = no stack); `pane` (the terminal's
  ONE side pane: diags / outline / help / options / build / modes /
  project; `pane_slot_of` says where a window docks it: sidebar, panel or a
  popup); `panel` + `paneltab` (the window's bottom panel, composed only
  under the client fact `haspanel`); `views` rows + `fview` (V1).
- **The composer** (`IdeSession::compose_ide_tree`) builds one root group in
  tree order: [the buffer strip, window only] → status + edit (one window) or
  per-window status + edit with `rows` hints (the stack) → the `pane`
  choice box (`popup` or `region: sidebar|panel` by `pane_slot_of`) → the
  panel group (`region: panel`, a `tabs` strip, the active tab's content:
  Problems = `diag_items`, Output = the [build] text, Terminal = the
  [terminal] edit node) → the message / hint row → root hints (theme, menu).
- **The grid renderer** (`madcdis/tui_model.h`) is a LINEAR STREAM: fixed
  lines and edit slots top to bottom, edit heights `rows`-hinted or
  flexible (first unhinted flexes, later unhinted one row); a `choice` with
  `list` = label row + one option per row. It reads caret / sel / focus /
  list / tabwidth / spans / rows; it ignores `region`, `popup`, `tabs`,
  `dialog`. No columns: nothing can sit beside anything.
- **The DOM renderer** (`madcdis/web_model.h` + `include/madc/ui_web/page.js`
  + `page.css`, BAKED — `sync build` before `gui`): a `region` hint docks a
  node into a slot of the workbench CSS grid (rail / sidebar / editor /
  panel / statusbar / foot; a node without one goes to `foot`); a slot with
  2+ edit nodes is a STACK; a `tabs` array is a strip; `popup` floats;
  `rows` fixes a height; splitters resize the panel / sidebar and remember
  the sizes in localStorage (per viewer).
- **Profiles as data:** `parse_keys` (`.keys`, `@scope` sections, the baked
  modal defaults + the rescue set as INLINE profiles through the same
  parser), `load_menu` (`default.menu`, the registry), `load_theme`,
  `load_status`; `resolve_profile_dir` finds them beside the tool or in the
  install layout. Every word converts ONCE at load and a misspelling
  refuses with its line (V0.5).
- **Commands** are `enum ide_cmd` + `cmd_table()` + `default.menu` rows +
  the profiles' spellings, gated by `check-madcide-command-registry.sh`
  (five directions). The window verbs exist on JOE's seats: `splitw nextw
  prevw killwin onlywin groww shrinkw`; the lens cycle is `view`.
- **The one-shot** (V1.5) composes the same tree and typesets it with
  `ui::render_tree` — whatever V2 composes, the `-c` output follows.

## 2. The decided shape (standing defaults; owner veto welcome)

### 2.1 The layout is data on the client

A **layout** is a tree of containers holding Views, stored on the bag as
`layout` (one client today; V3 moves it onto the client record):

```
layout  { "window": "main", "editor": <node>, "chrome": [ <pane>, ... ] }
pane    { "kind": ctPANE, "slot": slotEDITOR | slotSIDEBAR | slotPANEL,
          "side": ui::side, "size": <percent, 0 = flexible>,
          "mode": lmTABS | lmSTACK, "tabs": [ <tab>... ], "active": <index>,
          "focus": 0|1, "hidden": 0|1 }
split   { "kind": ctSPLIT, "dir": ui::split, "size": <percent>, "children": [ <node>... ] }
tab     { "kind": ide_view, "view": <View id or 0>, "caret": n, "mark": -1, "bend": -1, "grow": 0 }
```

A **tab** is the CONTAINER of §2.1 / §6 Q1: navigation (caret, mark, bend;
`grow` for a stacked entry) lives on it, never on the View. A tool tab
(`problems`, `outline`, `output`, `terminal`) carries its `kind` and no View
row yet (its content is composed by kind through today's builders); a
source tab carries the buffer's View id.

**Enum homes.** Renderer-facing vocabulary in `include/madc/bits/ui_enums`
(the one text for engine + dialect): `ui::split { none, vertical,
horizontal }` and `ui::side { none, left, right, top, bottom }`; their
spellings are owned by the engine (`ui_split_name` / `ui_side_name` in
`madcdis/ui_events.h`, the `ui_level_name` pattern) and reach the dialect as
INPUT converters `ui::split_code(name)` / `ui::side_code(name)` in `<ns_ui>`
(the `ui::key_code` pattern — the word is spelled in ONE place). IDE-private
vocabulary in `madcide_enums.inc`: `ide_container { ctNONE, ctPANE,
ctSPLIT }`, `ide_pmode { lmTABS, lmSTACK }`, `ide_slot` gains `slotEDITOR`;
the words `pane split editor sidebar panel tabs stack views focus hidden` are
the layout file's and convert in `parse_layout` (`slot_of`, `pmode_of`,
`view_of` — existing pattern, new converters beside them).

### 2.2 The `.layout` grammar (the profile parser family)

```
# profiles/default.layout — the shipped workbench; the baked default is this
# same text as an inline profile (default_layout_text) through the same parser
@window main
pane editor tabs views source focus
pane sidebar left 20% tabs views outline hidden
pane panel bottom 25% tabs views problems output terminal hidden
```

- `@window <name>` — one window in V2 (a second refuses: "one window; V3
  adds windows").
- `pane <slot> [<side> <size>%] <mode> views <kind>... [focus] [hidden]`:
  `slot` ∈ `editor | sidebar | panel`; a sidebar names `left | right`, a
  panel `bottom | top`, the editor no side (a size only inside a split);
  `mode` ∈ `tabs | stack`; every kind an `ide_view` word; `focus` at most
  once per window.
- `split <vertical|horizontal> [<size>%]` — its children on the following
  lines, indented one level deeper (depth = leading whitespace count; a
  jump of more than one level, a child under a `pane`, or a split with
  fewer than two children refuses: "line N: ragged layout").
- The window's editor region is EXACTLY one top-level `pane editor` or
  `split`; a second refuses. Chrome panes are top-level only (never inside
  a split) and never split — the ruling.
- A misspelling refuses the profile with its line and word (`line 3:
  unknown view 'sourse'`), the `parse_keys` shape; the loader falls back to
  the baked default and says so on the status line.

The shipped default keeps the sidebar and the panel HIDDEN: the terminal
shows nothing it does not show today until the user opens a pane (the
ruled relaxation: "panes and tabs the user opens; popups for pick lists"),
and the window shows them exactly when it shows them today (a check
surfaces the panel; the outline surfaces the sidebar). Deviation from the
design's example, stated: the default sidebar lists `outline` only. The
`project` View is today's ^P modal PICK LIST (a popup with a filter and the
`@project` keys); a docked, non-modal project View (VS Code's explorer) is
a named follow-up of this slice, not its default.

### 2.3 What the layout re-expresses (the rules, one accessor each)

- **Chrome panes** (sidebar, panel) come FROM the layout: visibility is the
  pane's `hidden`, the shown tool its `active` tab, keyboard ownership its
  `focus`. `show_view(kind)` (one owner) finds the chrome pane hosting that
  kind, unhides it, activates the tab and focuses the pane; `hide` reverses
  it and focus returns to the editor region. It replaces `panel` /
  `paneltab` / `show_panel_tab` and the diags / outline arms of `pane` /
  `toggle_pane` / `goto_pane_row` (which read the focused chrome tab's
  kind). `pane` STAYS for the popups (help, options, build, modes,
  project): a pick list is not a layout pane. `haspanel` stops gating the
  panel: every client's layout has one; the terminal renders it (the
  ruling) — the check in the TUI shows the panel band with its strip.
- **The editor region.** Its leaf panes' containers are the tree's `tabs`.
  The primary leaf (the one the launch file opens in) hosts the buffer ring:
  in `tabs` mode its tabs ARE the `buffers` rows (one per document, `active`
  = `bufidx`) — read through ONE accessor, `pane_tabs(w, es, pane)`, so the
  ring keeps its owners (`push_buffer_row`, `switch_buffer`, `edit_file`)
  and the tree does not copy them; the strip is composed for BOTH renderers
  when the pane holds 2+ tabs (the terminal renders it as the pane's header
  line — new in the TUI, ruled). The S5 stack IS the primary leaf in
  `stack` mode: its stacked entries are the `windows` rows `{bufidx, caret,
  grow}` (each a container over the ring's View), `active` = `winat`; JOE's
  verbs keep their names and move onto the leaf: `splitw` = enter stack
  mode / add an entry, `killwin` / `onlywin` remove entries (one entry left
  = back to tabs mode — today's "table clears" rule), `nextw` / `prevw`
  cycle `active`, `groww` / `shrinkw` the entries' `grow`. One leaf, two
  modes, no second storage.
- **A real split** (`viewsplit right|bottom [<repr>]`) turns the focused
  leaf into a `split` node holding it and a NEW leaf whose one tab is a
  container over a View — the same View (a second window on the subject,
  JOE's ^K O feel side by side) or a code View of `<repr>` over the focused
  tab's subject (the owner's side-by-side: source left, MC11 right; V5 syncs
  them). The new leaf's tabs live IN the tree (they are not the ring). The
  focused leaf's edit keys go to its active tab's View; `viewfocus
  <left|right|up|down|next|prev>` moves the focus between leaves (and to a
  chrome pane); `viewclose` closes the focused leaf (its parent split
  collapses when one child remains; the primary leaf never closes — quit
  does that); `viewtab <repr>` adds a code-View tab to the focused leaf;
  `viewopen <repr>` replaces the focused tab's View representation (today's
  `view` cycle is `viewopen` with the next representation). `es.fview` =
  the focused leaf's active tab's View (V1's contract kept).
- **Chrome placement** (`viewdock <left|right|bottom|top>`) moves the
  focused chrome pane's slot/side — data the layout persists.
- **Persistence** (§5 standing default): the client's live layout is saved
  beside the manifest as `<base>.prj.layout` in the layout grammar
  (positions, sizes, modes, hidden flags — NOT the tabs' contents), read
  back at project open; the implicit single-file project keeps it in
  memory (the no-surprise-artifact law, §6). The window's splitter drag
  posts a `layout` event (`size` for a chrome pane) so the session's tree
  is the one truth and the terminal shares the sizes; localStorage stops
  being the owner (the page reads the layout's size when it has none).

### 2.4 The renderers

- **The grid** (`tui_model`): `compose` becomes rectangular. The root's
  children partition into CHROME (a node hinted `region: sidebar|panel`
  with `side` + `size`) and the FLOW (everything else, tree order — JOE's
  shape: status on top, the edit, the message line last). Chrome bands are
  carved first — a panel `bottom|top` takes `size`% of the rows (minimum 3),
  a sidebar `left|right` takes `size`% of the columns (minimum 12) — and
  each renders as a PANE: a header line from its `tabs` (the titles, the
  active one reverse) then its children as a flow inside the band. The
  centre rect renders the flow with TODAY's algorithm, painting at the
  rect's left with the rect's width; a group hinted `split` inside the flow
  is a nested layout: `vertical` divides the columns among its children by
  `size` (unsized share the rest evenly, one blank column between), `horizontal`
  the rows; each child renders as a pane or a flow recursively; a leaf
  group's `tabs` array renders as its header line. Scroll and focus state
  stay keyed by focusable slot. With no chrome shown and no split (the
  shipped default, one buffer) the grid is BYTE-IDENTICAL — the negative
  control. The paint plan (`tui_paint_plan`) is a grid diff; a partial-row
  scroll simply repaints — never corrupts.
- **The DOM** (`web_model` + the page): `split` (the direction's WORD at
  the wire, `ui_split_name`), `side` (`ui_side_name`) and `size` are
  additive op fields (like `region` / `tabs`; the negative control: no
  hint, no field). The page: a `split` group is a flex container (row for
  vertical, column for horizontal) whose children take `flex-basis:
  size%`; a leaf group's strip is the existing `tabStrip`; a chrome slot's
  `side` / `size` set the workbench's grid track (`--sidebar-w` /
  `--panel-h`, and the sidebar's side of the grid) when localStorage holds
  nothing (V2c makes the layout the owner).

### 2.5 Thread contract

Unchanged: everything on the UI thread; the layout is per-client state on
the session's bag; no new globals.

## 3. Sub-slices and gates (each banks on the arc branch with TARGETED gates)

| Sub-slice | Content | Gate |
|---|---|---|
| **V2a — the layout as data; chrome from it; the grid learns rectangles** | `ui::split` / `ui::side` in `bits/ui_enums` + name owners + `ui::split_code` / `side_code`; `ide_container` / `ide_pmode` / `slotEDITOR` + converters; `parse_layout` + `load_layout` + `default_layout_text()` + `profiles/default.layout`; `IdeSession::open` loads it; chrome panes composed FROM the layout (`show_view` replaces `panel`/`paneltab` and the diags/outline arms of `pane`; the `haspanel` gate on the panel and the strip goes); the editor strip for 2+ tabs on both renderers; `tui_model` rectangular compose (chrome bands, pane headers, `split` recursion); `web_model` op fields + the page's split flex + strip; gate rule 5 in `check-madcide-enums.sh` (no `"mode"/"side"/"slot"/"dir": "…"` literal) | `tests/testmadcide_layout` (the shipped file parses to the pinned tree; the baked default parses equal; refusals with their line: an unknown view, an unknown side, a ragged indent, a pane with children, two editor regions, a one-child split); `test_tui_model` cases (side-by-side split, bottom band + strip, left band, byte-identity with nothing hinted); `test_web_model` fields + negative control; `testmadcide` pins updated on the container FIRST (the check shows the panel band + strip in the TUI; the outline the sidebar; esc hides), `testmadcide_cli` follows; the GUI snapshots (`madcide_panel` / `madcide_workbench` / `madcide_split`) re-pinned only where the composition changed by design; `check-madcide-*` gates green |
| **V2b — the editor region re-expressed; the view* commands** | `pane_tabs` accessor (ring ↔ tabs mode, windows ↔ stack mode); JOE's window verbs over the leaf; `viewsplit right\|bottom [repr]`, `viewtab`, `viewopen`, `viewclose`, `viewfocus` as registry rows + enum + dispatch + `default.menu` titles + JOE/pico/emacs/neovim spellings where a seat exists; the focused leaf's tabs in the tree; the composer walks the editor tree (status header per leaf when 2+ leaves; the focus hint on the focused leaf's edit); the pointer arm maps a node's `tag` to a leaf; `es.fview` follows the focused tab | `testmadcide`: `viewsplit right mc11` → two leaves, the right one a code View over the same subject, both composed; `viewfocus` moves the caret's home; `viewclose` collapses; the S5 pins unchanged (stack mode); `tests/gui/madcide_layout` (a window: `viewsplit right` → two editors side by side with their sizes; the sidebar pane docked left; a `viewdock right` moves it) |
| **V2c — `viewdock`; persistence; the docs** | `viewdock`; `<base>.prj.layout` save/load under the manifest rule; the page's splitter → the `layout` event → the session's size; `docs/madcide.md` (layouts, the commands), the design doc §1 / §4 row V2 ✅, CHANGELOG, mirrors | `testmadcide`: a docked pane's side persists through a project reopen; the implicit project writes nothing; `testmadcide_layout` round-trips save → parse |

Order: V2a, V2b, V2c. Each sub-slice's commit carries the trailers (engine
files change in V2a); pins are VERIFIED on the container before they are
written (`tmp/logs/`); no battery until the V5 seam.

### Progress

- **V2a — DONE** (three commits on the arc branch): part 1 (`0dec6a0b`) the
  `.layout` grammar + `parse_layout` / `load_layout` + `ui::split` / `ui::side`
  enums and name owners; part 2 (`b0bdf2d6`) the tool Views from the layout's
  chrome via `show_view`, the panel persists; part 3 (this commit) the grid
  renderer learns rectangles (chrome bands, `split` recursion, pane headers),
  the DOM's `split` / `side` / `size` op fields and the page's split flex, the
  composer emits the `side` WORD on the wire. Notable: the madcide integration
  tests render through the LINE linearizer (`ui::render_tree`), not the grid,
  so the rectangular compose is exercised by `test_tui_model` alone and the
  `testmadcide` pins did NOT move; the 17 GUI snapshots are unchanged (the page
  does not consume `side` / `size` yet — that is V2c — and the composer emits no
  `split` yet — that is V2b). Targeted gates green on the container
  (`test_tui_model` 27, `test_web_model` 20, the five madcide tests, the gui
  stage JIT/exe/obj); no battery (the V5 seam owns it).
- **NEXT = V2b** — the editor region a real split tree: `pane_tabs` (ring ↔
  tabs mode, windows ↔ stack mode), JOE's window verbs over the leaf,
  `viewsplit right|bottom [repr]` / `viewfocus` / `viewtab` / `viewopen` /
  `viewclose` as registry rows + dispatch + profile spellings, the composer
  walks `layout["editor"]` emitting a `split` group, `es.fview` = the focused
  leaf's active tab's View. THE side-by-side the owner asked about.

## 4. What does not change

- The popups (help, options, build, modes, project) and their `@scope`
  data; the prompts; the colon line; the vi arm; the key profiles' bindings.
- The View table (V1) and its converters; the one-shot client (V1.5).
- JOE's single-buffer terminal screen: status on top, the edit, the message
  line — byte-identical while no chrome pane is shown and no split exists.
- The web target's slot grid: the chrome still docks by `region`; only the
  editor slot learns the split tree.

## 5. Risks and named residues

- The TUI look changes by RULING when a chrome pane is shown (the check's
  problems list becomes a bottom band with a strip; the outline a left band)
  and when 2+ buffers are open (a strip header). The pins move
  deliberately; `testmadcide`'s row-count assertions are re-read on the
  container, never guessed.
- `tui_model`'s rectangular compose is the engine's largest piece here;
  its unit tests carry the byte-identity negative control, and
  `tui_smoke` / `tui_scroll_gate` run as targeted gates.
- The docked non-modal project View; `viewwindow` (V3); `viewsync` (V5);
  a second `@window` in a layout file (V3).
