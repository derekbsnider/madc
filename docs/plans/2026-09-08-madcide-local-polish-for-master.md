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

## P2 — the list overlays as dialogs; Open Project…; the project kind — BUILT 2026-09-08

**P2a — the list panes as dialogs (additive, the S6 treatment).** Today the
TUI's list overlays — Build `^B`, Project `^P`, Options `^T`, Modes, Help —
are `choice` nodes the composer floats with a `popup` hint (Build docks into
the bottom panel), rendered as a bare list. Each gains a `dialog` hint the
TUI ignores:

```
hints.dialog = { "title": <the pane's label>,
                 "buttons": [ {label, choose: 1} | {label, action} ... ],
                 "filter": <the core's live filter text>   (only the panes that filter) }
```

The page renders a titled floating dialog (VS Code's quick-pick shape in our
palette): the title bar, an optional filter field showing the text the CORE
holds (typing still travels the one input path — the S6 quick-input rule),
the option rows with the live selection marked, and the buttons. Two button
kinds, both DATA the composer names: `{label, action}` posts an ACTION by
name (the scope's cancel — `paneclose` / `projclose` / `modesclose` — looked
up in the loaded profile's `@scope` table exactly as `confirm_choices` looks
up the confirm's answers; a scope that binds no cancel gets no Close button),
and `{label, choose: 1}` CHOOSES the selected row — the focus owner's own
enter contract, not a key. A click on a row chooses that row (one gesture,
as a quick pick picks). Both arrive through ONE new input kind,
`{"kind":"choose", key, index?}`, resolved by `focus_state::choose` in the one
focus owner (`ui_focus.h`; the gate `check-one-key-owner.sh` keeps the
selection map there) into the SAME `choose` event Enter produces — so every
pane's choose arm runs unchanged (Project: `@project enter projopen`;
Options: the row's own action; Build: the command row). A press outside the
dialog fires the scope's cancel (`dismiss`). Build moves from the bottom
panel to a dialog like its siblings; the live build OUTPUT stays in the
panel (P3's Output tab). Keys are never hard-coded: the buttons' actions
come from the scope tables (profile data), the primary button is the
choose contract. The terminal is byte-identical (unknown hints).

**P2b — File → Open Project….** `default.menu` gains `File openproject Open
Project…` beside Open File…. The command asks the host for the native open
dialog through the existing request/response verb (`dialog_request` mode
`openproject`; the terminal and a headless session get the prompt, mode
`openproject`), and the answer loads the manifest: `proj_startup`'s read is
factored into `proj_load(w, es, pf)` (validate the JSON object with `tus`,
set `proj` / `projfile`, bump the generation, announce) so the startup read
and the explicit open are ONE reader; Open Project… then shows the Project
dialog.

**P2c — the project KIND `console | gui`.** The manifest gains `"kind"`
(absent = `console`). Engine: `ProjectManifest.kind` (an enum, validated —
an unknown word refuses loud), handed to the native link as the executable's
SUBSYSTEM: `MIR_object_exec_params.gui_subsystem_p` → the PE writer stamps
`IMAGE_SUBSYSTEM_WINDOWS_GUI` (2) instead of CONSOLE (3) (`mir-pe.c`
hard-codes CONSOLE today; ELF and Mach-O ignore the flag — a GUI program is
an ordinary program there). The CLI spells it as gcc does on mingw:
`-mwindows` / `-mconsole`; the `--project` build lane and madcide's Build
project row pass the manifest's kind. madcide: the Options dialog (`^T`)
gains a row `P Project kind   console` while a manifest is open; its action
`opt-projkind` toggles console ↔ gui and persists the manifest at once
(`proj_write`, the immediate-persistence rule). P3's Run routing reads the
same field. Gate: `scripts/verify_pe_release.sh` gains the subsystem read
(console for madc.exe itself), and a wine-lane reducer emits a `-mwindows`
program and reads its header back.

## P3 — the bottom pane with tabs; Run routing — P3a + P3b-1 + P3b-2 BUILT 2026-09-08

**The shape.** VS Code's bottom tool window in our palette: a PANEL region
that is optionally visible and holds TABS — **Problems** (the diagnostics
projection: the rows the `diags` pane shows, `goto` on a row), **Output**
(the build stream: the `[build]` buffer the capture pump fills, live or
last), **Terminal** (an embedded console: P3b). GUI only — the terminal
keeps its panes byte-identical (the client pushes a `haspanel` fact for a
region-rendering target, as it pushes `hasterm` / `hasdialogs`; the
composer composes the panel only under it).

**Data, not names.** Panel state on the bag: `panel` (visible), `paneltab`
(the active tab). Commands in the one vocabulary (default.menu, View):
`panel` toggles visibility, `problems` / `output` / `terminal` show the
panel on that tab — key profiles may bind them (data). The panel node is a
`group` docked `region: panel` whose `tabs` hint carries the STRIP as data
— `[{title, action, active?}]` — so a tab click posts the tab's command by
name (the S1 rule), and whose one child is the active tab's content. Every
site that opened the diagnostics pane (`pane = "diags"`, six of them) goes
through ONE `show_diags` that, under `haspanel`, also shows the panel on
Problems — the terminal's behaviour unchanged.

**Run routing (P3b).** Run is a SILENT NO-OP in the GUI today
(`client_service`: `if ( !ui::suspend(t) ) return;` — the web target
refuses suspend, the request is dropped). The fix routes by the project
kind: a **console** program runs in the Terminal tab; a **gui** program
gets its own OS window and its stdout/stderr go to Output. Both keep the
owner ruling (the running madc IS the compiler): the live parse is still
FORKED (`parse_run` / `project_run`), only its stdio is wired — a new
engine verb runs the fork with its stdio on a channel: a PTY for the
Terminal tab (POSIX `forkpty`; the child gets a controlling terminal, so
prompts flush and `isatty` holds — the pty master is one more DataChannel
the `chan_select` pump reads, the `pty://` sibling of `exec://`), pipes for
Output. Windows: pipes first (ConPTY is the named residue). The Terminal
tab's node carries the pty's scrollback as lines plus a cursor line; while
the tab has focus, keys and text travel the one input path to the core,
which writes them to the pty (a small TUI-key → bytes table; `^c` is
0x03). Output parsing is a bounded terminal: `\r` `\n` `\b`, SGR colours
onto the one style (ui_style, the palette); other CSI sequences are
dropped. `bld-run-cmd` (a manifest command in terminal mode) and the
Shell request take the same route.

**Slices.** P3a (built): the panel, the strip, Problems + Output,
`show_diags`, the commands + menu rows, a GUI fixture (the strip renders; a
tab click posts its command) and the byte-identity check in testmadcide.

**P3b-1 — Run in the window (Output).** The fork of the live parse becomes a
DATA SOURCE: channel schemes `madcrun://<parse handle>` and
`madcproj://<manifest>` (registered beside `exec://`) run the tree /
the --project lane in a child — through the ONE spawn owner: `Process`
gains a `child_body` option (fork-as-isolation with the owner's pipes; the
body runs `madc_cir_execute` / `madc_project_execute` and `_exit`s;
POSIX only — Windows keeps the snapshot + `--run-frozen` child of self as
an ordinary `exec://` spawn), so the pipes, the reap and the cancel are
the owner's. madcide pumps the channel like `build_pump` into the Output
tab ([run] output → the `[build]` buffer, tagged) as a cooperative task
(the editor stays live; Stop cancels). `run_buffer` / the project Run
route there under `haspanel` — the silent no-op is gone; the terminal
keeps the suspend + inherited-stdio path byte-identical. `client_service`
reports a request the client cannot serve instead of dropping it.

**P3b-2 — the Terminal tab (pty).** `Process` gains a `pty` option
(`forkpty`; the master is ONE fd-backed channel that is both the stdin
and the stdout channel) so `madcrun://`, `madcproj://` and `pty://cmd`
(the Shell, manifest commands in terminal mode) run on a controlling
terminal — prompts flush, `isatty` holds. madcide keeps a `[terminal]`
buffer the pump fills through a bounded VT filter (`\r` `\n` `\b`, CSI /
OSC sequences dropped; SGR colours onto the one style later); the Terminal
tab's node is an `edit` over it (the line-DOM, caret at the end, a
`terminal` hint); a press on it sets `termfocus`, a press in a window
clears it; while focused, keys and text travel the one input path and the
core writes them to the child through a VALUE channel the pump selects on
(`termin`) — the bytes come from `tui_key_bytes`, the INVERSE of
`tui_keyparse` beside it in the one key-bytes owner (exposed as
`ui::key_bytes`); `@terminal esc termunfocus` is baked modal data. A
console program runs here by the project kind; a gui program keeps
P3b-1's Output route. Windows: pipes (ConPTY is the named residue).

## P4 — editor tabs over the buffer ring

The `tabs` hint on the editor group renders the buffer ring as a tab strip
(`push_buffer_row` data; the active buffer marked; click selects, the
`^K` buffer chords unchanged). A tab can be a browser view (a `web` node
whose content is a URL — help pages, documentation).

Then the polished local IDE → master (the release-tier lanes: libc++,
darwin full suite both arches, genuine Windows).
