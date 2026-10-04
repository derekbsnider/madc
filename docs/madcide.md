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
madcide file.mad --line     # the ex / edlin line mode: stdin lines, text out
madcide file.mad -c check   # no surface: one command, the projection, a verdict
madc tools/madcide/madcide.mad file.mad   # from a source checkout
```

`--line` is the **ex / edlin client**: the same live-parse session driven
over stdin/stdout with no cursor addressing — it works over a pipe, in a
dumb terminal, and as an MCP seat's transcript. Each cycle typesets the
projection (the status line, the document, any message) to stdout and reads
one line: a `:` line is a colon command (the vi `:` mode — `w q wq x e r`,
`:N` to go to a line, the `viewsplit`/`viewfocus`/… verbs, `!cmd` for a
shell), and any other line is text inserted at the caret. It is the same
composer and the same commands as the terminal and the window; only the
rendering model (level `line`) is lower.

`-c "<command> [arg]"` runs ONE command of the registry (the names are the
`.menu` / `.keys` files' — `check`, `outline`, `gotoline 3`, `find add`,
`colon w`) against the opened file with no terminal or window at all,
prints what the IDE would have shown — the status line, the text, the
problems rows or the outline — and exits with the verdict: `0` clean, `1`
the file could not be read or the problems projection carries errors
(`-c check` on a broken file), `2` an unknown command or an argument the
command does not take. The argument is what you would have typed at the
prompt the command opens.

The packaged `madcide` binary ships with the Linux and Windows releases
(`bin/madcide`, `bin\madcide.exe`), and joins the macOS release once its
release job builds it beside `lib/libmadc-0.dylib`; the window needs the
platform webview library the packages install beside it (WebView2 on
Windows, WebKitGTK 6 / GTK 4 on Linux, WKWebView on macOS).

## Keys are profiles

Every key binding is data in `tools/madcide/profiles/*.keys`: `joe` (the
default — the full JOE/WordStar set), `pico`, `emacs`, `neovim` (a modal
personality that starts in normal mode), `chthonia` (Chthonia's keys,
after Thonny's: Ctrl+S, Ctrl+Z/Y, Ctrl+X/C/V, Ctrl+A, F5 to run, Ctrl+F2 to
stop; chthonia's default) and `vscode` (VS Code's default keymap; Ctrl+K opens its chords).
Each lists, in its header, the keys whose command madcide does not have
yet. `^T` opens Options; its Keymap
row cycles profiles for the session; `^K H` shows the loaded profile's own
bindings. View ▸ Key Bindings… (chthonia's Tools ▸ Key bindings…, the
`keystyle` command) lists the profiles by their display names and keeps the
one chosen for the bundle in use, in `settings.json`'s `"keys"` object
(`"keys": { "chthonia": "emacs" }`), so the next session of that bundle
opens with it. A profile's display name is its `@title NAME` line; a
`.keys` file dropped into the profile directory joins the list.
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

A binding's keys may carry modifiers: `ctrl+shift+s`, `ctrl+f2`,
`shift+right`, `alt+f4`, `ctrl+space`, `ctrl+plus`, and `primary+s` (Ctrl,
or Cmd on macOS). A plain Ctrl+letter is still written `^s`. A modified key
a profile does not bind acts as its key without Shift, then as its key
(`ctrl+shift+left` is `ctrl+left`'s binding, `shift+right` is `right`). A
terminal reports modifiers only where it sends xterm's modified sequences,
so Ctrl+Shift+S and Ctrl+digit are the window's (`--gui`, the browser).

Shift with a motion selects, as in Thonny and VS Code: the selection is the
same block the block keys light, extended from where the first shifted
motion started, and a motion without Shift drops it. Typing, Backspace,
Delete, Enter and Paste replace a selection made this way (or by Select all,
or by dragging the pointer); a block made with the block keys (JOE's `^K B`
/ `^K K`) keeps JOE's rules. Edit ▸ Cut, Copy, Paste and Select all are
commands (`cut`, `copy`, `paste`, `selectall`); `^K Y`'s delete fills the
same clipboard. Under `--gui` it is the system's clipboard; in a terminal
or a browser page it is madcide's own.

Edit ▸ Indent Lines, Dedent Lines and Toggle Comment (`indent`, `dedent`,
`togglecomment`) work on every line the selection touches, or on the
caret's line when nothing is selected. A selection that ends at the start
of a line leaves that line out. Indent puts a tab at the start of each
non-empty line. Dedent removes one leading tab, or up to a tab width of
leading spaces. Toggle Comment adds the language's line comment and a
space at column 0 of each non-blank line. When every non-blank line
already starts with the comment, it removes the comment instead. The
comment is `//` for C, C++ and madc (an untitled buffer counts as madc),
`#` for shell, Python, Perl, Ruby, Makefiles, YAML and TOML, `;` for INI
and `--` for Lua. A file with no line comment, such as plain text,
refuses. Each command is one undo step and leaves the selection covering
the whole lines. With a selection that crosses a line, Tab indents
instead of replacing the selection. The Chthonia keys bind Shift+Tab and
Ctrl+3, as Thonny does. The VS Code keys bind Ctrl+], Ctrl+[,
Shift+Tab and Ctrl+/.

Edit ▸ Replace… (`replace`; Ctrl+H in the VS Code keys, `^\` in the Pico
keys) works like nano's replace. It asks for the text to find, then for
the text to put in its place; an empty answer deletes each match. It then
stops at each match from the caret on, lights it, and asks "Replace this
one?": `y` replaces it, `n` skips it, `a` replaces it and every match after
it, and Esc or ^C stops. The search runs to the end of the buffer, wraps
once to the top, and ends where it began, so text a replacement put in is
never asked about. Each `y` is one undo step, and an `a` makes the rest one
undo step. The status line reports how many it replaced. In a window the
question is a dialog with Yes, No, All and Cancel buttons. The answer keys
are the `@replace` scope's, so a key profile can respell them (as with
`@confirm`).

File ▸ Save All (`saveall`; Ctrl+Alt+S in the Chthonia keys, Ctrl+K S in
the VS Code keys) writes every buffer that has unsaved changes. The
buffer you are in stays the active one. An untitled buffer has no file
to write, so Save All leaves it and counts it; Save names it. The status
line reports what happened, for example `Saved 2 files. 1 untitled (Save
As names it).`

File ▸ Close and Close All (`close`, `closeall`; Ctrl+W and Ctrl+Shift+W in
the Chthonia keys, Ctrl+W and Ctrl+K Ctrl+W in the VS Code keys) close the
current tab or every tab. A buffer with unsaved changes asks `Save changes
to NAME? (y)es (n)o (^C)`. `y` saves and closes, `n` closes without
saving, and Esc keeps the buffer open. In a window the question is a
dialog with Yes, No and Cancel buttons; the answer keys are the `@save`
scope's. Saying yes for an untitled buffer asks for its name (Save As),
and the buffer closes once it is written. Close All asks once for every
unsaved buffer: `y` runs Save All and closes every buffer it left clean,
so an untitled buffer stays open with its changes. The tab to the right
takes over, or the tab to the left when you close the last tab. madcide
always has a buffer, so closing the last file leaves the untitled buffer,
and closing the untitled buffer empties it. The document stays loaded in
case another client of the session is showing it.

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
menu naming its line. A row may name its picture, `{ICON}` after the
command (`new`, `open`, `save`, `run`, `debug`, `stop`, `step-over`,
`step-into`, `step-out`, `breakpoints`); an unknown icon refuses the menu
the same way. The `toolbar` menu places the window's toolbar buttons
(chthonia's: New, Open, Save, Run, Stop). A button with an icon shows the
picture, with its label and key as the tooltip. `{run Run}` adds an arrow
that drops the file's Run menu down as a list (the `menushow Run` command),
and a `toolbar -` row is a divider.

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
`output`, `kind`, `icon` and `commands`:

```json
{ "tus": ["main.mad", "util.mad"], "output": "app", "kind": "console",
  "commands": [{"name": "Docs", "cmd": "make -C {project} docs"}] }
```

`kind` is `console` (the default) or `gui`. Options gains a **Project
kind** row while a manifest is open; toggling it persists the manifest.
A gui project's Windows executable gets the GUI subsystem (no console
window at start — what `-mwindows` does for a single file), and Run sends
its output to the window's Output tab instead of the Terminal.

`icon` names the program's Windows icon file (`.ico`), relative to the
manifest. A project's Windows executable carries it as its icon
resources, the way `windres` would build them from `32512 ICON "app.ico"`.
Explorer shows it for the file, and a window the program opens shows it in
the title bar and taskbar. Linux and macOS executables carry no icon (a
desktop file or an app bundle supplies one there), but the file is read and
checked on every platform, so a bad `icon` fails the build everywhere.

## The language standard

A session has one language standard. Every buffer compiles under it (its
diagnostics, outline, colours, Build and Run), and the REPL runs it.
`--std=c17` (any standard `madc --std=` names canonically: `c11`, `c++20`,
`madc`, …) sets it on the command line; without that, settings.json's
`"std"` does. Unset, each file's family decides: `hello.c` is C17 and its
REPL a C one, `x.cpp` is C++17 and a `.mad` file is madc. Language… (the
`language` command, in the Build menu and chthonia's Run menu) changes it in
a running session. It re-parses every buffer, restarts the REPL and keeps
the choice in settings.json, and its **By file** row returns to each file's
own (`language file`).

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

## Sharing a session

One madcide session can carry more than one client: the editor you are typing
in, an LSP editor (VS Code through the extension in `tools/vscode-madcide`), an
agent's MCP client, and a browser window — all on the same buffers, the same
carets, the same undo history. Two processes on one file would be two sessions:
two carets, two undo histories, last save wins.

Every session **advertises itself** so nobody has to be told an address. A
running madcide writes a small JSON file — its endpoint, root, documents, pid
and what it serves — under `$XDG_STATE_HOME/madcide/sessions/` (or
`~/.local/state/madcide/sessions/`; `MADCIDE_SESSION_DIR` overrides). It is an
advertisement, not a lock: madcide never refuses a file that is already open,
and two sessions in one project both appear.

```sh
madcide --sessions                            # what is running, and how to join it
madcide file.mad --lsp --attach               # join whoever holds file.mad
madcide file.mad --mcp --attach               # the same, for an agent host
madcide file.mad --lsp --attach 127.0.0.1:7777  # or name one explicitly
```

The MCP seat's history verbs (`graph.history`, `graph.commits`,
`graph.revision`, `graph.diff`, blame provenance) read the repository through
the **`madcgit` module** — madc's read-only binding of the *system* libgit2
(`lib/libmadcgit.so`, built when `libgit2-dev` is present and loaded on first
use). libgit2 is a dependency of the IDE's nexus, never part of madc: without
the module those verbs answer exactly as for a file outside any repository.

An ordinary `madcide file.mad` listens on a **loopback ephemeral port** so it
can be joined; `--no-serve` opts out. `--serve <host:port>` runs headless on a
port you choose. A session that cannot bind, or cannot write the advertisement,
runs anyway — undiscoverable rather than broken. A crashed session's file is
removed by the next session that looks (the liveness test is a connection, not
a pid: pids get reused).

> **madcide has no authentication and no TLS.** A port is reachable by any
> local user. Bind loopback only, and reach a session on another machine
> through an ssh tunnel (`ssh -N -L 7777:127.0.0.1:7777 host`) — never by
> binding a public address.

The first api client to **speak** is granted **owner** and every later one
**observer**, so joining a session does not hand out edit rights; an owner
promotes one with `clienttier <id> editor`. "To speak", not "to connect": one
port carries api, ws and the page, and a connection is told apart by its first
byte, so it joins the roster — and can receive pushed events — when its first
message arrives.

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

## Plugins

A profile is a plugin: a directory `<name>/` holding its manifest
`<name>.plugin` (JSON) and the data files it carries (keys, layout, menu,
theme, status line). madcide looks in the user's `plugins/` directory first
(`~/.config/madcide/plugins`, `%APPDATA%\madcide\plugins` on Windows, or
`$MADCIDE_CONFIG_DIR/plugins`), then in the shipped one, so a user's plugin
overrides a shipped plugin of the same name.

A plugin can carry code. Its manifest names the source, one madc file in its
directory:

```json
{ "name": "hello", "api": 2, "code": "hello.mad" }
```

The source includes `<madcide/plugin>` and defines the activation madcide
calls when it loads the plugin, which registers its commands and views:

```cpp
#include <madcide/plugin>

bool hello_greet(long w, long es, long doc, const char *arg)
{
    ide::set(w, es, "msg", format("Hello, {}.", arg));
    return true;
}

extern "C" bool madcide_plugin_activate(const ide_api *api, long w)
{
    ide::bind(api);
    return ide::command(w, "greet", "Greet", hello_greet) != 0;
}
```

`madcide --build-plugin <dir>` builds it into its library beside the manifest
(`hello.so` on Linux, `hello.dll` on Windows, `hello.dylib` on macOS), the
running madcide compiling in-process; nothing is built automatically. The
plugin's code activates when the profile in use is the plugin, or when
`settings.json` lists it (`"plugins": ["hello"]`), before the keys and menus
load, so a key profile or a menu can name `greet`: from its library when it
has one this madcide accepts, else from its source, compiled into the
running madcide. A library loads in well under a millisecond; a source costs
a compile at every launch (about 16 ms for the first plugin and 7 ms for each
further one with the installed madcide), so a plugin ships fast with its
library and works without one. A handler calls madcide only through `ide::`
(`command`, `view`, `event` at activation; `run`, `command_id`, `get`, `set`,
`show` from a handler), so a plugin binds to nothing of madcide's own. The
code stays loaded until the session closes.

`ide::event` subscribes a handler to the REPL pane's events, whose kinds
`<madcide/plugin_api>` names: `reTAKEN` (an entry ran), `reRAN` (F5's program
returned), `reBINDINGS` (the session's names answered: `event["reply"]["rows"]`,
each row with `name`, `kind`, `type`, `value`, `file` and `line`) and
`reSTOPPED` (the session stopped). A view's rows are `{ "content", "file",
"line" }` objects on the key the view names; choosing a row with a line goes
there.

madcide's own REPL pane is a plugin compiled into madcide: its commands
(`repl`, `replrun`, `replstop`, `replclear`, `replbindings`, and the input's
`replenter`, `replcomplete`, `replolder`, `replnewer`, `replunfocus` in the
`@repl` scope) and its view `repl` register the way a plugin's do, so a key
profile, a menu or a layout names them as it names any plugin's. `replclear`
empties the transcript and keeps the session and its names (Thonny's Edit ▸
Clear shell, Ctrl+L in the `chthonia` keys).

View ▸ Program arguments… (`progargs`) sets the words F5 passes the program
after its path, as Thonny's does. The prompt starts with the current words.
They split the way Thonny splits them, with Python's `shlex.split`
(`python::shlex_split`), so a quoted word stays whole: `-n 3 "a file.txt"`
gives `main` an `argc` of 4. The `%run` line shows them as typed. An unclosed
quote or a trailing backslash is refused, and the old words stay; an empty
answer clears them. They last until the session ends. Build ▸ Run does not
pass them.

The shipped `chthonia` plugin carries code: the Variables view in chthonia's
right sidebar (View ▸ Variables), Thonny's; the sidebar's second tab is the
Outline (View ▸ Outline, which toggles it; toggled off, Variables shows
again). It lists the names the REPL session
defined, each with its type and value (`count int 3`, `square int (int)`),
refreshed after every entry and every F5 run and emptied when the session
stops; choosing a name the program's file defined goes to its line. The
packages ship it as its source plus its library, built by the packaged
madcide.

A library built against another plugin API version (it says both), or one
that cannot be loaded or has no `madcide_plugin_activate`, is refused, and
the plugin's source replaces it; the status line says so, so the library can
be rebuilt. A source that does not compile (its first error, positioned) or
has no activation, and an activation that returns false, are each reported on
the status line, and the editor opens without the plugin; what a refused
activation registered is taken back.
Plugin libraries are Linux shared objects, Windows DLLs and macOS dylibs; a
plugin binds the engine the editor loaded (`libmadc.so.0`, the
`libmadc-0.dll` beside `madcide.exe`, `lib/libmadc-0.dylib`).

A plugin's code can run in a process of its own, so a crash in it leaves the
editor running. The manifest asks for it beside its code:

```json
{ "name": "hello", "api": 2, "code": "hello.mad", "transport": "host" }
```

and `"plugins.isolate": true` in `settings.json` does it for every plugin.
madcide forks a child of itself that opens the plugin's code by the same rule
(its library, else its source) and activates it; the plugin's source is the
same either way. Its commands, views and events work as they do in madcide's
process, and its `ide::` calls reach the session as requests. A line the
child writes that is not a request (its own output, a crash report) shows on
madcide's stderr. When the child ends, the status line says so and madcide
starts it again, at most three times a session; then its commands say the
plugin is not running. Windows has no fork, so there a plugin that asks for
its own process runs in madcide's, and the status line says why.

The seat (`madcide --serve`, `--attach`) answers the same requests from any
client, under its tier: `{"plugin": "get", "key": K, "seq": N}` and
`{"plugin": "command_id", "name": NAME, "seq": N}` (an observer may),
`{"plugin": "set", "key": K, "value": V, "seq": N}` and
`{"plugin": "show", "kind": KIND, "seq": N}` (an editor), and `{"plugin":
"run", "code": C, "arg": A, "seq": N}` (the command's own tier); `scope`
names an entity of the world, the session's when absent.
