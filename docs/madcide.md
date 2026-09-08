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
madc tools/madcide/madcide.mad file.mad   # from a source checkout
```

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
| `^K A` | cycle the code view: the source, its MC11 lowering, C11, C++ (read-only lenses) |
| `^N` · `^K Z` | the Modes palette (`:` = the vi colon line, `v` = vi modal editing) · a shell |

Menus are data too (`profiles/default.menu`): the window's menu bar, and
any command palette, read the same command registry the profiles bind.

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
  that tab.
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
