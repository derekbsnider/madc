# madcide — the local IDE polish for the master release (design + plan)

**Status:** ACTIVE 2026-09-08. Owner ruling (2026-09-08): *the next master
release is still the local-only version once we get it a little more
polished up; then full focus on the client-server + headless models.*
This doc is the plan for that polish. KG Decision
`next_master_release_is_local_ide_then_remote_arc`.

**Shape being converged on.** The best tried-and-true modern concepts of
CLion, VS Code and Xcode — a workbench with a sidebar, editor tabs, a bottom
tool window with tabs, native menus and dialogs, one theme — while the same
composer and the same core serve the terminal byte-identically (the JOE /
Turbo-C / RHIDE / Neovim / Emacs personalities, and the client-server +
headless arc, follow the master release:
`docs/plans/2026-08-31-madcide-gateway-and-code-server.md`).

Every slice is additive on the tree (hints the TUI ignores), one composer,
one client (`check-madcide-seam.sh`), its own merge wave.

## P1 — one theme, two renderers (colours) — BUILT 2026-09-08

**The defect.** The owner: *"why does the GUI have a different colour syntax
highlighting scheme than the TUI? they should be the same."* The TUI paints
a span's JOE style spec (`keyword bold`, `string cyan` — `profiles/*.theme`)
through the one spec parser; the web renderer ignored that spec and styled
the span's SEMANTIC class from a private palette in `page.css`
(`.c-keyword { color: var(--syn-keyword, #8fb4ff) }`). Two vocabularies:
`^K T` swapped the terminal's scheme and not the window's, and the `function`
class the default scheme leaves unthemed on purpose (JOE parity) coloured
in the window only.

**The fix, at the deepest layer.**
- The render style and its spec parser move out of `tui_model.h` into the
  renderer-neutral `include/madcdis/ui_style.h` (`ui_style`, `ui_style_of`,
  `ui_style_colour_name` — one colour-name table). Each renderer owns only
  its last step from the style: the VT100 target style → SGR (as before),
  the DOM model style → the page's classes.
- `web_model` reads the same `{s, e, c}` row the grid reads, parses `c` with
  the one parser (a malformed spec is refused whole, as there), and emits
  the style as classes: `st-bold st-dim st-italic st-underline st-blink
  st-inverse`, `fg-<colour>`, `bg-<colour>`.
- `page.css` loses its syntax vocabulary. It keeps only a PALETTE: the 8
  ANSI colours, 16 with bold-as-bright (`.st-bold.fg-cyan` reads the bright
  entry — what a terminal shows for `bold cyan`), each an `@gui` custom
  property (`@gui pal-cyan #56b6c2`, `@gui pal-cyan-bright #6fd2de`). Blink
  is not rendered.
- The converter `spans_to_hspans` emits `{s, e, c}` only for a class the
  scheme styles — the semantic class leaves the wire (one fact, one
  vocabulary); a class the scheme leaves plain is plain in both renderers.
- Gate: `scripts/check-one-style-vocabulary.sh` (fulltest) — one spec
  parser, both models include it, the page carries no `.c-*` / `--syn-*` /
  `' c-'`, no span row is read or written by class name. Negative controls
  for each rule.

**Evidence:** `tests/unit/test_web_model.cpp` (the class rendering per
attribute/colour; bad rows skip), `tests/gui/ui_web_spans.mad` (the window's
COMPUTED colours: `bold cyan` paints the bright palette entry the `@gui`
theme set, `underline bg_blue` the blue background), `tests/gui/
madcide_theme.mad` (a palette entry travels as a custom property),
`tests/testidespans.mad` + `testidespanshift.mad` (the one row shape). The
terminal is byte-identical (`testmadcide`, the tui_model unit battery).

## P2 — the list overlays as dialogs; Open Project…; the project kind

The TUI's text overlays (Build `^B`, Project, Options `^T`, Modes, Help)
are composed as `popup` panes today (`pane_slot` data in
`compose_ide_tree`). The S6 treatment: each gains an additive dialog shape
(`list`/`choice` with a `dialog` hint — title, a filterable list, buttons
that post the pane's actions by name), the terminal unchanged. File →
Open Project… beside Open File… (the native folder/file dialog; the project
manifest is the file). The manifest gains a project KIND `console | gui`
(data, `project.kind`): it drives the Windows PE subsystem on `-o`
(`/SUBSYSTEM:WINDOWS` for `gui`) and where Run sends output (P3).

## P3 — the bottom pane with tabs; Run routing

The `panel` slot becomes a tabbed tool window (VS Code's shape, our
palette): **Problems** (the diagnostics projection), **Output** (the build
stream that already docks as `region: panel`), **Terminal** (an embedded
shell: a pty behind a `term` node the grid renderer paints). Run today is a
SILENT NO-OP in the GUI — `client_service`: `if ( !ui::suspend(t) )
return;` — the web target refuses suspend and the request is dropped
(fix-what-you-find). Routing by the project kind: a console program runs in
the Terminal tab (pty → the grid renderer in a DOM node); a gui program gets
its own OS window with stdout/stderr captured into Output.

## P4 — editor tabs over the buffer ring

The `tabs` hint on the editor group renders the buffer ring as a tab strip
(`push_buffer_row` data; the active buffer marked; click selects, the
`^K` buffer chords unchanged). A tab can be a browser view (a `web` node
whose content is a URL — help pages, documentation).

Then the polished local IDE → master (the release-tier lanes: libc++,
darwin full suite both arches, genuine Windows).
