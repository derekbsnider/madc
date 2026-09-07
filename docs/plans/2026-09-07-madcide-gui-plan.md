# madcide GUI mode (web target) — Implementation Plan

> **For agentic workers:** one trailer'd commit per task
> (`Hypothesis:`/`Layer:`/`Searched:`/`Oracle:`), targeted tests per task
> on the container via `scripts/remote_build.sh`, the full battery ONCE at
> the merge wave (Task 9). Branch `feature/madcide-gui-claude` off
> `develop`.

**Goal:** madcide runs in a window through the Level-3 web target — ONE
client loop and ONE composer serve both the terminal and the workbench, the
buffer/sidebar/panel/status-bar laid out by layout HINTS the TUI ignores and
the DOM honours.

**Architecture:** `compose_ide_tree` stays THE composer; it gains additive
layout hints (`region`, `tabs`, `popup`) that `tui_model` never reads (its
"unknown hints are ignored" rule keeps the terminal byte-identical) and
`web_model` renders as a workbench. `madcide_client.inc` becomes
target-generic (`ui::open(target)` / `render` / `event` / `close`, the vised
precedent), picking the target from `--gui`. Terminal requests in GUI mode
capture child output into a panel instead of suspending. Themes gain a
`@gui` scoped section (the `parse_keys` `@scope` rule) rendered as CSS custom
properties.

**Tech Stack:** C++11 header-only models (`include/madcdis/`), the embedded
page (`include/madc/ui_web/page.{js,css}`), madc-dialect tools
(`tools/madcide/`), the `<ns_ui_web>` fragment. No new dependency.

**Spec:** `docs/plans/2026-09-06-ui-web-target-and-madcide-gui.md` (§3.4
madcide GUI mode, §4 standing defaults, §5 item 3). Slice 2 (the engine)
landed `feature/web-provider-engine-claude` → develop `f50dca64`.

## Global Constraints

- **One composer, one client.** No madcide-GUI fork of `compose_ide_tree`
  or `IdeSession`; the target is a flag, never a second code path
  (`check-madcide-seam.sh` stays green).
- **The TUI stays byte-identical.** `tests/testmadcide.mad`'s composition
  clauses (bindings-swap identity, diagnostics/outline projections) MUST
  pass unchanged — layout hints are additive keys `tui_model` does not read.
- **Layout is DATA, not a name test** (Rule #7): a region/popup is a hint on
  the node's bag; `web_model` maps the hint value to a class, never a test
  against a role or a title string.
- **Keybindings are DATA** (owner law): the `@gui` theme section and any new
  workbench affordance route through `parse_keys` / the bindings table, never
  a hardcoded key→action arm.
- **Value-first dialect** in every `.mad`/`.inc`: `var`, bare
  `println`/`format`, zero includes.
- **GUI tests live in `tests/gui/`** (outside the default suite), run by
  `remote_build.sh gui` under Xvfb in JIT, exe and obj.

---

## Facts the executor needs (recon 2026-09-07, verified in source)

- **The composer** `IdeSession::compose_ide_tree` (`tools/madcide/
  madcide_core.inc:3665`) builds a `role:"group"` root of a status line +
  edit node (+ inactive-window snapshots + the bottom overlay row). The
  shared helpers `compose_heading/edit/status/menu` live in
  `tools/texteditor/editor_events.inc:446+`; `compose_joe_statusline`
  (`madcide_core.inc:899`) renders the JOE `%n %m %r %c %x %k` seats into ONE
  status string.
- **web_model** (`include/madcdis/web_model.h`) — `compose()` walks the tree
  and emits keyed DOM ops; `web_class_of(roles, role)` (line 69) maps a role
  to a class string; `hint_of(n.hints, key, dflt)` (`uinode.h:67`) reads a
  hint. The page's applier `madcApply(ops)` (`page.js:88`, `applyNode`)
  reconciles by node-path key; `page.css` carries the class styles;
  `madcSnapshot()` returns one line per text-bearing node.
- **The client** `run_ide` (`tools/madcide/madcide_client.inc:72`) is the
  ONLY place a UI handle lives; it uses `ui::tui_open/render/event/close`,
  `tui_cols/rows/pending/bind_keys`, and `tui_suspend/resume/refresh` for the
  terminal-request servicing (`client_service`). `vised`'s `run_visual`
  (`tools/texteditor/vised_core.inc`) is the target-generic precedent:
  `ui::open(target)` / `render` / `event` / `close`.
- **The target-generic surface** already exists (slice 2): `ui::open(const
  char *target)`, `ui::render/event/rows/cols/bind_keys/pending/eval_page`,
  and `ui_web::target()`. `ui::suspend/resume/refresh` exist as publics; on
  the DOM frontend suspend/resume are no-ops (there is no terminal to yield).
- **Themes**: `load_theme` (`madcide_core.inc:388`) reads `class spec` lines
  into the bag's `theme`; `parse_keys` (`:149`) already parses `@scope key
  action` lines (later lines win) into a `scopes` map — the `@gui` section
  reuses that exact rule. `load_status` (`:945+`) reads `lmsg`/`rmsg`.
- **Terminal requests**: `IdeSession::take_request` parks a `{kind}` request
  (`run`/`projrun`/`cmd`/`shell`/`refresh`); `term_exec` (`:4153`) runs the
  guest. `client_service` suspends the grid around it. GUI mode has no grid
  to suspend.

---

### Task 1: layout-hint vocabulary in the composer (`region`, `tabs`, `popup`)

**Files:**
- Modify: `tools/madcide/madcide_core.inc` (`compose_ide_tree`, the group
  nodes it builds), `tools/texteditor/editor_events.inc` (the shared helpers
  gain the region hint on their group/edit nodes where madcide places them)
- Test: `tests/testmadcide.mad` (unchanged — the gate is that it still
  passes), plus a new headless assertion `tests/testidehints.mad`

**What:** the composer stamps additive hints on its nodes:
`region` on each container group (`"rail"`/`"sidebar"`/`"editor"`/`"panel"`/
`"statusbar"`), `tabs:1` on the editor group when >1 buffer is open, and
`popup:1` on the palette/prompt/project float. These are new keys on the
existing `hints` bag; the composed ROLES and text are unchanged, so
`tui_model` (which reads only `caret`/`sel`/`focus`/`list`/`tabwidth`/`spans`)
renders identically.

- [ ] **Step 1:** add `tests/testidehints.mad` — compose the IDE tree
  headless, assert the editor group carries `region == "editor"` and the
  status carries `region == "statusbar"`, and that a node's role/text is
  unchanged from the pre-hint shape. Oracle: the assertion is the spec (the
  design's role→region table).
- [ ] **Step 2:** run it, watch it FAIL (no region hint yet).
- [ ] **Step 3:** stamp the region/tabs/popup hints in `compose_ide_tree`.
- [ ] **Step 4:** run `tests/testidehints.mad` (pass) AND `tests/testmadcide.mad`
  (still pass — byte-identical composition clauses).
- [ ] **Step 5:** commit.

### Task 2: `web_model` renders regions, tabs and popups

**Files:**
- Modify: `include/madcdis/web_model.h` (`compose`: emit `region` on a group
  op from the hint; a `tabs` marker; a `popup` marker), `tests/unit/
  test_web_model.cpp` (+cases)

**What:** when a group node carries `region`, the op gains `"region":"editor"`
(etc.); `tabs` and `popup` become boolean op fields. `web_class_of` is
unchanged (role→class); the region is a SEPARATE data attribute the page uses
for placement — never a new role. Doctest: a tree with region hints produces
ops carrying the region fields; a tree without them is byte-identical to
today (negative control).

- [ ] Steps: failing doctest → emit region/tabs/popup fields → doctest green.
- [ ] Commit.

### Task 3: the workbench layout in the embedded page

**Files:**
- Modify: `include/madc/ui_web/page.js` (`applyNode`: place a region group in
  its workbench slot via a `data-region` attribute / grid area), `include/
  madc/ui_web/page.css` (the rail/sidebar/editor/panel/statusbar grid, tab
  strip, popup overlay)
- Test: `tests/gui/madcide_workbench.mad` (+`.expect`, +`.timeout`)

**What:** the page lays the top-level regions into a CSS grid (rail left,
sidebar next, editor center with an optional tab strip, panel bottom, status
bar at the very bottom); a `popup` group renders as a centered overlay. The
GUI test opens the window, renders an IDE-shaped tree with regions, and
snapshots the workbench text through `madcSnapshot()`.

- [ ] Steps: failing GUI snapshot fixture → workbench CSS + placement →
  green under Xvfb (JIT/exe/obj).
- [ ] Commit.

### Task 4: `@gui` theme sections → CSS custom properties

**Files:**
- Modify: `tools/madcide/madcide_core.inc` (`load_theme` collects `@gui`
  scoped lines into a `gui_theme` bag alongside the unscoped JOE-vocabulary
  lines), the composer (attach the `gui_theme` as a hint on the root),
  `include/madcdis/web_model.h` (emit the root's theme vars),
  `include/madc/ui_web/page.{js,css}` (apply them as `--` custom properties),
  `tools/madcide/profiles/*.theme` (a `@gui` block in default + one more)
- Test: `tests/testidetheme.mad` (headless: `@gui` lines parse into the
  `gui_theme` bag; unscoped lines still reach `theme`), `tests/gui/
  madcide_theme.mad`

**What:** `load_theme` already ignores lines it does not recognise; the
`@gui accent #rrggbb` shape reuses the `parse_keys` `@scope` rule (scope
`gui`, later lines win). The TUI reads the unscoped lines exactly as today
(gate: `testmadcide` theme behaviour unchanged); the web target reads the
`gui` scope as CSS variables. ONE file per scheme, two renderings.

- [ ] Steps: failing headless parse test → `@gui` parse + attach + page
  apply → green; add the GUI theme snapshot.
- [ ] Commit.

### Task 5: the status bar as items

**Files:**
- Modify: `include/madcdis/web_model.h` (a `status` node whose hints carry
  `items` renders a left/right item bar instead of one pre-filled string),
  `tools/madcide/madcide_core.inc` (`compose_joe_statusline` also emits the
  seats as structured `items` hints when composing; the TUI keeps reading the
  flattened string), `page.{js,css}`
- Test: `tests/testidestatus.mad`, `tests/gui/madcide_status.mad`

**What:** the JOE seats (`%n`/`%m`/`%r`/`%c`/`%x`/`%k`) already expand to
text; the composer additionally publishes them as `items` (left group, right
group) on the status node's hints. The TUI ignores `items` and renders the
existing single string; the web target renders discrete status-bar items
(VS Code shape). No new seat logic — the SAME expansion feeds both.

- [ ] Steps: failing test → structured items hint + web render → green.
- [ ] Commit.

### Task 6: terminal requests capture into an output panel

**Files:**
- Modify: `tools/madcide/madcide_client.inc` (`client_service`: on the web
  target, `run`/`projrun`/`cmd` capture the child's output into a panel view
  on the session bag instead of suspend→term_exec→resume; `shell` opens the
  platform terminal or refuses with a reason until the embedded terminal
  lands), `tools/madcide/madcide_core.inc` (a `panel` region view fed from
  the captured output; `term_exec` gains a capture path)
- Test: `tests/testidepanel.mad` (headless: a `run` request's output lands on
  the panel bag; the composer projects it as a `region:"panel"` node)

**What:** GUI mode has no grid to suspend, so a terminal request runs with
its output captured and shown in the bottom panel (the design's Output
shape). The capture path is target-agnostic data on the bag; the TUI keeps
its suspend path. Gate: the headless panel test; `testmadcide` unchanged.

- [ ] Steps: failing panel test → capture path + panel projection → green.
- [ ] Commit.

### Task 7: the `--gui` flag; the client loop goes target-generic

**Files:**
- Modify: `tools/madcide/madcide.mad` (parse `--gui`, pick
  `ui_web::target()` else `"term"`), `tools/madcide/madcide_client.inc`
  (`run_ide(path, ro, target)`: `ui::open(target)` / `render` / `event` /
  `close`, `ui::cols/rows/pending/bind_keys`; `client_service` calls the
  Task-6 capture path on a non-terminal target), `scripts/check-madcide-seam.sh`
  (the seam gate still holds: the handle stays in the client)
- Test: `tests/gui/madcide_edit.mad` (open madcide in a window, type through
  the page's real key path, snapshot the workbench)

**What:** the one behavioural switch of the slice. `run_ide` stops naming the
`tui_*` wrappers and speaks the target-generic surface (vised's precedent);
`--gui` selects the web target, a machine without the library refuses at
`ui::open` with the reason (the lazy-row contract). The terminal default is
unchanged (gate: `testmadcide` + the pty smoke test).

- [ ] Steps: failing GUI edit fixture → target-generic client + `--gui` →
  green under Xvfb (JIT/exe/obj); `testmadcide` + seam gate still green.
- [ ] Commit.

### Task 8: `/dupaudit` scoped to madcide + the GUI inventory

**Files:**
- Modify: `docs/` (record any `DupFamily` found), `tests/gui/` (ensure each
  new fixture runs in all three lanes)

**What:** run `/dupaudit` scoped to the madcide/web-model subsystem the slice
touched (region-hint readers, status-item readers) before the merge; a family
it reports as divergent is a live bug. Any consolidation leaves a
`fulltest` gate.

- [ ] Steps: dupaudit recon → record/consolidate → commit (gate if any).

### Task 9: docs, mirrors, merge wave

**Files:**
- Modify: `docs/language/ns-ui.md` (the workbench section), the design doc §5
  item 3 landing note, `CHANGELOG.md` Unreleased, `docs/plans/ROADMAP.md`
  8.6, `claude_status.json`, `docs/test-status.md`, `docs/lane-status.tsv`,
  the KG `Feature madcide_gui`
- Validate: `make -C src fulltest` + the GUI stage + wine + c-testsuite +
  macOS cross on the container (the s16x wave), record the four develop
  lanes, merge `--no-ff` to develop, push.

- [ ] Steps: docs/mirrors → wave on the final content → ledger rows → merge.

## Self-review (2026-09-07)

- **Spec coverage:** §3.4's role→workbench table = Tasks 1–3, 5; `@gui`
  themes = Task 4; terminal-requests-into-a-panel = Task 6; the `--gui` flag
  and one client = Task 7; the gate (headless `testmadcide` unchanged + GUI
  DOM snapshots) is pinned in every task.
- **One composer / one client:** Task 7 is the only behavioural switch and it
  is target-selection, not a fork; `check-madcide-seam.sh` gates it.
- **TUI byte-identical:** every hint added (Tasks 1, 4, 5) is a key
  `tui_model` does not read; the gate is `testmadcide` passing unchanged.
- **Deferred (not this slice):** the embedded terminal panel (tui_grid →
  DOM grid), semantic-diff/virtualized editor windows, mobile bodies — §5
  item 5, by demand.
