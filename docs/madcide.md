# madcide — the madc IDE

madcide is the IDE built on the running compiler: the editor holds a live
parse of your file, so diagnostics, the outline, syntax colour and the
status line's enclosing-function seat come from the compiler's own retained
state — nothing is re-parsed or exec'd to show them. One IDE, two faces:
the **terminal** (a JOE/WordStar-style TUI by default) and the **window**
(a native window on Windows, macOS and Linux through the platform webview,
with the native menu bar and file dialogs). Both run the same composer and
the same commands; the window adds a workbench around the editor.

```sh
madcide file.mad            # in the terminal
madcide file.mad --gui      # in a window
madcide file.mad -c check   # no surface: one command, the projection, a verdict
madc tools/madcide/madcide.mad file.mad   # from a source checkout
```

`-c "<command> [arg]"` runs ONE command of the registry (the names are the
`.menu` / `.keys` files' — `check`, `outline`, `gotoline 3`, `find add`,
`colon w`) against the opened file with no terminal or window at all,
prints what the IDE would have shown — the status line, the text, the
problems rows or the outline — and exits with the verdict: `0` clean, `1`
the file could not be read or the problems projection carries errors
(`-c check` on a broken file), `2` an unknown command or an argument the
command does not take. The argument is what you would have typed at the
prompt the command opens.

The packaged `madcide` binary ships with the Linux, Windows and macOS
releases (`bin/madcide`, `bin\madcide.exe`); the window needs the platform
webview library the packages install beside it (WebView2 on Windows,
WebKitGTK 6 / GTK 4 on Linux, WKWebView on macOS).

## Keys are profiles

Every key binding is data in `tools/madcide/profiles/*.keys`: `joe` (the
default — the full JOE/WordStar set), `pico`, `emacs`, `neovim` (a modal
personality that starts in normal mode). `^T` opens Options; its Keymap
row cycles profiles; `^K H` shows the loaded profile's own bindings.
The JOE defaults most worth knowing:

| Keys | Command |
|---|---|
| `^K S` / `^K D` · `^K X` · `^K Q` · `^C` | save · save and exit · quit · discard changes |
| `^K E` · `^K R` | edit (open or switch to) a file · insert a file |
| `^K B` `^K K` · `^K C` `^K M` `^K Y` | block begin/end · copy / move / delete the block |
| `^K F` · `^L` · `^K L` | find · find next · go to line |
| `^_` · `^^` · `^Y` · `^W` | undo · redo · delete line · delete word |
| `^K ;` · `^B` · `^K I` · `^P` | check · Build… · outline · the Project window |
| `^K O` `^K N` `^K P` · `^K 0` `^K 1` | split / next / previous window · close / only window |
| `^K A` | cycle the code view: the source, its MC11 lowering, C11, C++ (read-only lenses, indented and syntax-coloured like the source) |
| `^N` · `^K Z` | the Modes palette (`:` = the vi colon line, `v` = vi modal editing) · a shell |

A binding's last word is a command name from the IDE's one vocabulary
(`tools/madcide/madcide_enums.inc`, the `ide_cmd` enum and its name table).
The profile resolves every word to its code when it LOADS — a misspelt word
refuses the whole profile on the status line, naming its line (`Profile 'x'
line 12: unknown action 'svae'.`), so a typo is a load-time error, never a
dead key. The same rule covers the `@scope` lines (a prompt's, the project
window's, the vi `@normal` alphabet's actions).

Menus are data too (`profiles/default.menu`): the window's menu bar, and
any command palette, read the same command registry the profiles bind; a
menu row's command id resolves the same way and an unknown id refuses the
menu naming its line.

## Colour schemes

`profiles/*.theme` map the compiler's classifications (`keyword`, `type`,
`string`, `number`, `comment`, `function`) to JOE-vocabulary styles
(`bold`, `cyan`, `bold yellow`, `bg_blue`…); `default` is JOE parity,
`classic` and `vivid` are alternatives (the palette's Theme… command, or
the Scheme row of Options). One scheme, both faces: the terminal paints
the style through its ANSI colours and the window renders the same style
over a sixteen-colour palette — a `@gui` section in the theme sets the
window's palette and chrome colours (`@gui pal-cyan #56b6c2`,
`@gui accent #7aa2f7`).

## Projects

The Project window (`^P`, File → Project…) lists the project's translation
units, the open buffers and — while you type a filter — files in the
directory; `ins` adds a file, `del` removes one, Enter opens the row. The
first file is an implicit project; adding a second materializes the
manifest `<base>.prj.json` beside the launch file, written at once on
every change. File → Open Project… loads another manifest (the native
open dialog in the window, a prompt in the terminal).

The manifest is JSON — `tus` (the files, or objects with `file`,
`directory`, `defines`, `include_dirs`, `std`, `stdlib`), `entry`,
`output`, `kind` and `commands`:

```json
{ "tus": ["main.mad", "util.mad"], "output": "app", "kind": "console",
  "commands": [{"name": "Docs", "cmd": "make -C {project} docs"}] }
```

`kind` is `console` (the default) or `gui`. Options gains a **Project
kind** row while a manifest is open; toggling it persists the manifest.
A gui project's Windows executable gets the GUI subsystem (no console
window at start — what `-mwindows` does for a single file), and Run sends
its output to the window's Output tab instead of the Terminal.

## Build and Run

`^B` (Build…) lists the commands: Check (in-process, the live parse),
Build (a native executable or object from the tree), Run (the live parse
forked — nothing execs), Stop, and with a manifest open the project-wide
Check / Build / Run plus the manifest's own `commands`. Diagnostics land in
the Diagnostics pane (terminal) or the Problems tab (window); a failed
check or build opens it, and Enter on a row goes to the line.

In the terminal, Run hands the program the real terminal (JOE's `^K Z`
shape) and returns after a key press. In the window, the bottom panel
takes it: a console program runs on a pseudo-terminal in the **Terminal**
tab — prompts flush, what you type goes to the program, `^]` hands the
keyboard back to the editor (a click in an editor window does too) — and
a gui program opens its own window while its output streams into the
**Output** tab. Every stream ends with the program's exit status.

## The window's workbench

The window arranges the editor with the pieces an IDE user expects:

- **Editor tabs** — the open buffers as a strip above the editor; the
  active one marked, `*` while modified; a click switches (the palette's
  Switch to Buffer… does the same by name).
- **The bottom panel** (View → Toggle Panel) with **Problems**, **Output**
  and **Terminal** tabs; View → Problems / Output / Terminal show it on
  that tab. Drag the splitter over its top edge to resize it (the sidebar's
  right edge likewise); a double-click on the panel's splitter maximizes it
  and back; the window remembers the sizes.
- **The Build menu** lists every `^B` row directly (Check, Build, Run, Run
  native, Stop — the project's rows and a manifest's own commands when one
  is open); Build → Run runs with no overlay. `Build…` keeps the palette
  for the keyboard.
- **Dialogs** — Build, Project, Options, Modes and Help are titled dialogs
  with the rows as pick targets and buttons named after what the pane's
  keys do (Open / Run / Change / Select, Close); a click outside closes
  them. The prompts (find, go to line, file names) are quick inputs; the
  quit question is a confirm dialog.
- **Native chrome** — the menu bar and the platform's file dialogs (Open
  File…, Save As…, Open Project…).
- **The status bar** as chrome: the file name, row/column, the modified
  badge, the pending chord, the enclosing function.

Nothing the window shows is a second implementation: every dialog, tab
and menu item is the same command the terminal's keys run, composed once
and rendered by each face.

## Split views and layouts

The editor region is a split tree — one pane, or a `split` of panes side by
side — and the sidebar and bottom panel are fixed-slot chrome. The colon line
(`^N` then `:`, or a bare `:` in vi) drives it:

- `:viewsplit right [mc11|c11|cpp]` splits the editor: the focused pane on the
  left, a new pane on the right. With a representation the new pane is a
  read-only code View of the buffer's lowering — **source on the left, its
  MC11 on the right** (V5 will correlate their carets). `:viewsplit bottom …`
  splits horizontally instead.
- `:viewfocus next|prev` moves the focus between panes; `:viewopen
  mc11|c11|cpp|source` re-represents the focused pane in place; `:viewclose`
  closes it (the split collapses to its sibling; the first pane stays open —
  quit closes that).
- `:viewdock left|right|top|bottom` moves the focused chrome pane (the sidebar
  or the panel) to a slot and side; `:viewsize sidebar|panel <percent>` sizes a
  band — the same size the window's splitter drag sets.

The layout is client data — a `.layout` profile through the same parser family
as the keys, menu and theme. In a project it is saved beside the manifest as
`<base>.prj.layout` (positions, sizes, hidden flags — not the panes' contents)
and restored when the project reopens; a single-file session keeps it in memory
(no stray artifact). Because the splitter and `:viewsize` set the size the
session owns, the terminal and a fresh window share it.

Dedicated keys and menu items for the `view*` commands are a coming addition;
today they reach the colon line — and any client that speaks the registry.
