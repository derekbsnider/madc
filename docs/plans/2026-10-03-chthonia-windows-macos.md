# Chthonia on Windows and macOS, self-contained — plan (2026-10-03)

## 1. The owner's direction (2026-10-03, queue item 5)

"we need to be able to have the chthonia self-contained binary, and make sure
it works fully under Windows and MacOS without any weird errors". The owner
put it after the REPL fixes, `--std=` and the toolbar icons (all done), and
before the TUI (last).

The plugin design settled what the product is (`docs/plans/2026-09-30-madcide-plugins.md`
§9.3 and §9.4, owner 2026-09-30):

- chthonia is its own binary: the chthonia plugin linked in, its bundle's
  data baked in, its own name and configuration directory, GUI by default.
- The standalone package ships libmadc, madc and the `chthonia` binary.
- It ships with "the Windows REPL backend (a child of self: chthonia's shell
  on Windows)".

## 2. What the Windows hands-on showed (v0.101.0)

| Symptom | Cause | State |
|---|---|---|
| Run in `madcide.exe` printed madcide's usage (exit 2) | The Windows Run spawned the running executable with `--run-frozen=` on its command line; only the madc CLI reads that option (from 7225fa035, 2026-09-08). Another AOT program re-ran itself without end. | **Fixed** 6a8a692b4 |
| The REPL tab: "the backend process is POSIX-only for now (plan 41.9a)" | `SessionClient::start` forks; its `_WIN32` arm refuses | **Fixed** (Step A) |
| 12 s before the window appears | Not reproduced: the same v0.101.0 zip shows its window in 0.6 s (§5). The owner puts it down to load on the box | **Closed** (Step B) |
| The macOS tarball has no madcide | An AOT madcide binds libmadc's value runtime; on macOS that is `libmadc-0.dylib`, the darwin port's D5, not built yet | Step C |

## 3. Done: the run child (6a8a692b4)

`include/madc_run_child.h` owns a Run in a child of self where there is no
fork. The parent sets the request in the child's environment
(`MADC_RUN_CHILD=<kind> <path>`) and spawns its own executable
(`madc_run_child_process`). The child serves the request before its own `main`
(`madc_serve_run_child`). Two places call it: the madc CLI's `main`, and a
namespace-scope initializer in `<ns_madc>` (the `<iostream>` `ios_base::Init`
idiom). Any program that can call the run verbs includes `<ns_madc>`, so it
serves its own child: madcide.exe, chthonia, or a user's program. Precedent:
Python's multiprocessing spawn of a frozen executable.

Kinds today: `frozen` (a live parse frozen to a snapshot: `parse_run`,
`madcrun://`) and `project` (`project_run`, `madcproj://`). Gates:
`testparserunfrozen` and `testprojectrun` on every domain; their win64
`--exe` pass is the one that was red. No lane runs that pass (B166).

## 4. Step A — the Windows REPL backend: a third kind of the same owner

**Built (2026-10-04).** As designed below, with these specifics:

- The session's request stream is a DataChannel on both platforms: POSIX
  adopts the socketpair's ends (`detail::socket_channel_over`, the socket
  channel's own constructor); Windows accepts the child's `tcp://` connection
  on a `listen://127.0.0.1:0` listener. One protocol text serves both, and the
  client's waits go through one helper (`poll()` on POSIX, the reactor's
  probe `taskio::poll_readable` on a 5 ms cadence on Windows).
- The token is 128 bits from `rand_s` (the OS generator). A connection whose
  first line is not the token is closed and the listener keeps waiting.
- A run child of any kind ends quietly on a fault (`madc_crash_quiet_child`):
  a top-level filter ends the process with the exception code, after madc's
  report when the CLI installed it, so no crash dialog (Windows) and no
  debugger (wine, whose default filter ignores `SEM_NOGPFAULTERRORBOX`). The
  client reports the status. madcide spells a Windows status the way Windows
  tools do, `0xC0000005`.
- Gates: `testsession_*` (5) and `testmadcide_repl` lost their win64/wine64
  skips and pass under wine, as scripts and as Windows executables (an exe
  serves its own backend through `<ns_madc>`'s initializer).
  `testsession_pump` has a win64 twin: a crashed backend's status is
  -1073741819 there (oracle: a mingw-gcc program `_spawnl`'s a faulting child
  under wine and prints that status).
- Residue: the CLI REPL (`madc`, `madc -i`) still runs its session in-process
  on Windows. Its backend copies the configured Program (the command line,
  madc.ini), which cannot cross a process boundary. `testrepl_crash` keeps
  its win64 skip.

The POSIX backend (`src/madc_session_client.cpp`) is a fork child with three
channels:

1. the requests and replies: JSON lines on a `socketpair` (Jupyter's message
   shape);
2. the program's stdout and stderr, merged: the output pipe;
3. the program's stdin: the child's stdin (`session_input`).

On Windows the child is a run child of kind `session` (`MADC_RUN_CHILD=session
<port> <token> <std>`). Channels 2 and 3 are the child's stdio pipes, which
the Process owner already gives a Windows child. Channel 1 cannot be a
`socketpair` (Windows has none) and should not ride inherited handles. Here
the child connects to a loopback listener the parent opened (127.0.0.1, an
ephemeral port) and sends the token as its first line. The parent accepts one
connection and checks the token. That is Jupyter's kernel transport: ports
plus a key, in its connection file. madc's session already speaks Jupyter's
message shape. Python's `socket.socketpair()` on Windows is the same loopback
emulation.

- `serve_session(fd, …)` reads its requests with `::read` and writes with
  `::send`. On Windows a SOCKET is not a CRT descriptor, so both sides use
  `recv` / `send`, behind one socket I/O helper that both arms share.
- The client waits on the reply socket and the output pipe. The task reactor
  already has the Windows arms (`WSAPoll` for sockets, `PeekNamedPipe` for
  pipes, `src/madc_task_chan.cpp`). The client's `poll` and `session_readable`
  case adopt them; no second readiness owner.
- The child builds its Program itself (the POSIX child inherits the host's
  `ProgramFactory` by fork). Its options are the request's standard plus
  the engine's defaults, the same as the `project` kind's engine.
- The CLI REPL (`madc`, `madc -i`) runs its session in the CLI process on
  Windows today, so a crashing entry ends madc. With the backend it runs
  there as on POSIX, and a crash restarts the session.
- Gate: the five `testsession_*` tests and `testrepl_crash` lose their
  `win64` / `wine64` skips (all say "no fork on Windows"), plus
  `gui/madcide_repl` under wine.
- Thread contract: unchanged from §41.9a. One client per thread; the backend
  is single-threaded.

## 5. Step B — the 12 s Windows start

**Measured (2026-10-04): no defect.** Genuine Windows 11 (the owner's box,
over the `win_run.sh` channel). A PowerShell timer starts the process,
samples its top-level windows and child processes every 20 ms, then kills it.

| Build | Launch | WebView2 child | `webview` window | page title set | madcide CPU |
|---|---|---|---|---|---|
| v0.101.0 (the published zip, freshly unpacked) | `--gui hello.c` | 0.10 s | 0.62 s | 1.04 s | 0.25 s |
| v0.101.0 | `--gui` (untitled) | 0.07 s | 0.61 s | 1.06 s | 0.14 s |
| this branch (`tmp/winstage`, release-built `madcide.exe`) | `--gui hello.c` | 0.07 s | 0.62 s | 1.05 s | 0.22 s |

Without a window: `--help` takes 0.03–0.06 s, and `hello.c -c check` (a
whole session) 0.07–0.08 s. madcide spends under 0.3 s of CPU before the page
is up; the rest is WebView2's own start. The owner judged the 12 s to be load
on the box. Nothing to fix.

Found on the way, for D3: `madcide.exe` is a console-subsystem image, so a
launch from Explorer or a shortcut also opens a console window beside the
GUI one. The chthonia product, GUI by default, needs a GUI-subsystem image,
or to detach the console when it opens its window.

## 6. Step C — macOS

- D5: `libmadc-0.dylib`, built per arch beside `forest.bin`
  (`docs/plans/2026-09-01-darwin-host-port.md`). An AOT madcide and every
  plugin library bind it as `@rpath/libmadc-0.dylib`.
- The macOS tarball then ships `bin/madcide` and the plugins, as Linux and
  Windows do. The REPL backend and Run fork there (POSIX), so steps 3 and 4
  are not needed on macOS.
- Gate: the darwin lane's full suite on both Mac runner arches, plus the
  packaged madcide's `--help` smoke and a Run on the owner's Mac (the
  release tier).

## 7. Step D — the chthonia product

Plugin design §9.3 and §9.4, in its own order:

1. The two-TU link probe: split a small piece of madcide into two translation
   units and link them. Multi-object madc builds are tested only at fixture
   scale, and madcide is one unit.

   **Done (2026-10-04): it links and behaves identically.** The piece is the
   command line, `madcide_args.inc`. Its unit holds that file after a one-line
   prototype of `lang_kind_of`. The main unit is `madcide.mad` with that one
   include replaced by a header holding `struct ide_args` and the three
   prototypes. The two units link through a `--project` manifest of the two
   `.mad` files. Calls cross both ways: main to `ide_args_parse` /
   `ide_usage` / `ide_face_needs_file`, and the args unit back to
   `lang_kind_of`. A struct with a `ui::level` member crosses by reference
   beside a `var &`. Results against the one-unit build:
   - Linux AOT (`madc --project m.json -o madcide-split`): `--bogus` and
     `--std=c99x` refused with the same text and exit 2; `--help` exits 0.
     `-c language` (under `--std=c11`), `-c outline`, `-c check` and
     `--std=c++20 … -c check` match byte for byte.
   - Linux JIT (`madc --project m.json madcide <args>`, the program name
     first, as `--project` passes argv): `-c language` matches byte for byte.
   - Windows AOT (the hosted PE under wine): `-c language` matches once CR is
     stripped, and `--bogus` is refused.
   - Files: four in `tmp/madcide_split/` (not tracked); the recipe is above.
     D2 replaces it with the real split.
2. madcide's base as a linkable object, and `madcide_main(argc, argv,
   product)`.
   - `tools/madcide/madcide_base.inc` holds the include list that
     `madcide.mad` has today, plus `madcide_main`, which takes over `main`'s
     body.
   - `madcide.mad` is that include plus madcide's descriptor and a
     one-line `main`. It stays one unit, so every script and test that
     builds or runs it is unchanged.
   - `madcide_base.mad` is the include alone, with no `main`: the unit a
     product links, and the object madc-devel ships later.
   - `<madcide/product>` (beside `<madcide/plugin>`) declares the descriptor
     and `madcide_main`, and is all a product's own unit includes.
   - The descriptor reaches the base through one pointer, written once by
     `madcide_main` before any session or thread starts and read-only after
     that (thread contract).
3. The product descriptor, and the chthonia build.
   - `struct ide_product { name; bundle; level; plugin; activate; }` holds:
     - the program's name: the usage text, the window title, and the
       configuration directory (`~/.config/<name>`, `%APPDATA%/<name>`);
     - the bundle it opens when no flag, program name or setting names one;
     - the face it opens with no flag: `ui::TUI` for madcide, `ui::WEB` (a
       window) for chthonia; `--tui` asks for the console;
     - the plugin linked into it, and that plugin's activation.
   - A bundle whose plugin the product links activates the linked code (a
     `linked` transport) and never loads the plugin's library or source.
   - The chthonia build is a native project manifest,
     `tools/chthonia/chthonia.json`: madcide's base, the plugin's
     `chthonia.mad` and `chthonia_main.mad` (the descriptor and `main`).
     It has `"kind": "gui"`, so on Windows the image is a GUI-subsystem
     program: no console window (Step B).
   - Its `"icon": "chthonia.ico"` (the owner's artwork, six images from 16
     to 256 px) is the Windows icon. Done 2026-10-04: madc's PE writer lays
     the manifest's icon out as `.rsrc` (RT_ICON 1..6, RT_GROUP_ICON 32512),
     byte-identical to what windres + mingw-gcc produce (gated by
     `verify_pe_release.sh` check 8). On genuine Windows 11, Explorer's icon
     for chthonia.exe and its window's class icon (title bar, taskbar) are
     the emblem. Linux and macOS take the owner's `Chthonia.png` in
     packaging (a desktop file, the app bundle's `.icns`), D4.
   - The window's title is the product's name and the file, Thonny's
     `Thonny - <path> @ <line> : <col>`. Today the web target opens every
     window titled `madc` (`src/ns_ui.cpp`), and there is no op to change
     it. It needs a `title` op in `ui_host_ops`.
4. Packaging: chthonia ships in each platform's madc package, and as the
   standalone chthonia package (libmadc, madc, `chthonia`) for Windows and
   macOS, then the MSIX (PK6).
   **In each platform's madc package — done 2026-10-04 (Windows earlier).**
   - Linux: `package_release.sh` (and the Homebrew formula) builds
     `tmp/chthonia-pkg` with the release compiler. `stage_install.sh` stages:
     - `bin/chthonia`;
     - `share/applications/chthonia.desktop`;
     - `share/icons/hicolor/<n>x<n>/apps/chthonia.png`, written by
       `scripts/ico_png_images.py` losslessly from the six images of
       `chthonia.ico`, the one source of the artwork.
     The rpm lists them. The install gate's Linux legs add chthonia's usage
     line, `-c check` over a `<stdio.h>` program from `/tmp` (clean, rc 0), a
     syntax-error control (1 problem, rc 1), and the desktop entry and icon.
     A `stage_install.sh` tarball passed `package_install_gate.sh tar` on the
     container with every leg green. The deb and rpm builds themselves run
     with `package_release.sh` at the release tier.
   - macOS: on a darwin host of the tarball's arch, `package_release_macos.sh`
     builds madcide, chthonia and the plugins with that arch's release madc.
     It stages `bin/madcide`, `bin/chthonia`, `share/madcide` (the one
     staging script) and `madcide.1`, and the README describes them. A cross
     host packages the compiler alone and prints a stated SKIP. The mactar
     gate adds the same chthonia legs.
     The commands were proven by hand on the Intel Mac with the x86_64
     tarball's thin madc:
     - madcide and chthonia build;
     - `build_shipped_plugins.sh` builds `chthonia.dylib`;
     - the staging writes 32 files;
     - the installed chthonia, run from `/tmp`, prints its usage line, its
       `-c check` is clean (rc 0), and the control reports 1 problem (rc 1).
     The first scripted run is the release job's Mac step.
   - Open: an app bundle with an `.icns` for macOS (the Dock and Finder
     icon), the standalone chthonia packages, and the MSIX.
   - macOS: madcide and chthonia must be built by the native madc in the
     release job's Mac step (`package_release_macos.sh` on a darwin host).
     The container's cross madc only emits objects, and building a plugin
     runs the built madcide. The tarball takes the Linux layout:
     `bin/madcide`, `bin/chthonia`, and `share/madcide/{profiles,plugins,
     include,verbs,checks}`.
   - madcide's data staging is written twice today: `stage_install.sh`
     (`share/madcide`) and `package_release_windows.sh` (beside the exe). A
     third copy for the Mac must not be added: one staging script serves
     all three platforms.
     **Done 2026-10-04:** `scripts/stage_madcide_data.sh <data-dir>
     <plugins-dir>` is the one owner. Both callers adopted it with
     identical output: old and new staging were `diff -r` clean on the
     container, 32 files each for Linux `share/madcide` and Windows `bin/`.
     `check-one-madcide-staging.sh` (in `gates`) fails on an `install` or
     `cp` of madcide's data outside it, and has two negative controls.

## 7b. Step E — the forest in the library on Windows and macOS

Found 2026-10-04 on genuine Windows: chthonia.exe and madcide.exe report
`Failed to open include file: stdio.h` for hello.c (`-c check` exit 1),
where Linux reports clean. Their parse handles run in `libmadc-0.dll`, and
the forest, Windows' only source of the C headers, is packed into
`madc.exe` alone (`release-windows` → `forest_pack_windows.sh`). Wine hides
it: the container's mingw headers answer through `Z:`. So diagnostics,
colour and F5 fail in every program built on the engine, on every
Windows machine.

Owner, 2026-10-04: the forest was to move into the library on all three
platforms. Linux has it (the thin CLI, the forest in `libmadc.so`,
forest-carriers S4). The Makefile pins every hosted mode monolithic ("the
per-OS shared-library products land with the PK3 packaging twins"), and
they never did. The work:

1. Windows: the release set is its own directory, `bin/release-windows/`
   (PE binds by adjacency, so the exe and its DLLs travel together, and a
   dev rebuild of `bin/libmadc-0.dll` can never swap the release one).
   `madc.exe` there is thin: `madc.o` linked against `libmadc.dll.a`. Its
   `libmadc-0.dll` is the stripped engine carrying the forest (PE overlay),
   which every program finds through the library-image arm.
   `forest_pack_windows.sh` freezes with a copy of the set and appends to
   the release DLL. Every consumer of `bin/madc-release-x86-64-windows.exe`
   moves to the set: `verify_pe_release.sh`, `headerless_suite.sh`,
   `win_suite.sh`, `package_release_windows.sh`, `/promote`'s wine leg.
   **Done 2026-10-04.** Measured: `madc.exe` 142,848 bytes; `libmadc-0.dll`
   17,092,408 bytes (10,394,112 stripped engine + the 238-unit forest).
   `verify_pe_release.sh` asserts the exe carries no forest and that a `-v`
   run of an `#include <stdio.h>` program binds through
   `[library-image]`; its negative control (the same set with an unpacked
   DLL) prints no such line and live-parses, and under wine still prints
   the right answer, which is why the arm is asserted. headerless-win
   subset 23/0 (2 domain skips). On genuine Windows, from the staged zip
   layout: `madc.exe hello.c` prints 5; `testfreezerun.mad` (C++ through
   the groves) is right; a `madc -o` program runs; `madcide.exe` and
   `chthonia.exe hello.c -c check` are clean; chthonia's `replrun` over
   `--serve` fills Variables with `add`/`main`; the window shows syntax
   colour.
2. macOS: `libmadc-0.dylib` carries the forest in `__MADC,__forest`
   (`-sectcreate`, then the ad-hoc signature), and the hosted CLI is thin.
   Proved on the owner's Mac.
   **Done 2026-10-04.** The dylib's link takes the same `-sectcreate` the
   hosted binary takes. The release CLI, `bin/madc-release-<arch>-macos`, is
   `madc.o` linked against the dylib (rpath `@loader_path/../lib`), stripped:
   170,656 bytes beside a 19,015,712-byte dylib (arm64). Before this, the pair
   was a 17,351,552-byte monolithic CLI holding the forest and a
   10,788,512-byte dylib holding none. The hosted dev binary stays
   monolithic and packed.
   `verify_macho_release.sh` takes the pair and checks:
   - the dylib's section bytes equal `forest.bin` and read back (837 units);
   - the CLI has no forest section, loads `@rpath/libmadc-0.dylib`, and has
     the `@loader_path/../lib` rpath;
   - both arm64 images are signed;
   - the prelude's provenance marker and C-linkage restore are in the dylib.

   The mactar install gate asserts `[library-image]` under `-v`, and that the
   CLI refuses to run with the dylib hidden.
   On the arm64 Mac (`madc-mac`, macOS 15.3.2, no `/usr/include`), from the
   tarball:
   - C, C++ (`<iostream>`, `<string>`) and dialect programs run.
   - `-v` prints `forest-bind: [library-image] opened container (837 units)`.
   - A `madc -o` program's `madc::diagnostics` over a C buffer
     (`<stdio.h>`, `<string.h>`) and a C++ buffer (`<iostream>`, `<string>`)
     reports 0 diagnostics for each.
   - The same program built from the 0.100.1 tarball (the old shape) reports
     `Failed to open include file: iostream` for the C++ buffer. C passed
     there because the C prelude is embedded in the engine's text. This was
     the macOS twin of the Windows `stdio.h` failure.
   - With the dylib hidden, dyld refuses to load the CLI.
   - chthonia, built on that Mac by the tarball's madc (`madc --project
     tools/chthonia/chthonia.json -o chthonia`, which loads
     `@rpath/libmadc-0.dylib`), run from its build directory:
     - `-c check` on `hello.c` (`<stdio.h>`) is clean, rc 0;
     - `-c check` on `hello.cpp` (`<iostream>`, `<string>`) is clean, rc 0;
     - the control, a C file missing a `;`, reports `2:47 error expected ';'
       before '}' token`, rc 1.

     Run from `/tmp`, the AOT chthonia cannot find the editor's verbs,
     whose path is baked relative to the build tree. The installed layout
     (`share/madcide`) is D4's to stage on the Mac.
   - The Intel Mac (`madc-mac-x86`, macOS 15.7.4) gave the same results
     from the x86_64 tarball: every probe and both controls, and chthonia
     built there with `-c check` clean on both files and the control
     reported.
   - Not yet done: the native-window check by driven input. Scripting keys
     over ssh (`osascript` → System Events) needs the owner to grant
     Automation/Accessibility on the laptop; the probe hung on that consent
     prompt and was killed.
3. Gate: a program built with `madc -o` that opens a parse handle over
   `#include <stdio.h>` reports no diagnostics where no headers are on
   disk: the genuine-Windows lane and the Mac. `chthonia.exe hello.c -c
   check` is clean on genuine Windows.

## 7a. Thonny parity: what chthonia has and what it lacks

Thonny is the oracle (owner, 2026-09-30). Rows are features a Thonny user
reaches for, by menu.

| Area | chthonia has | chthonia lacks |
|---|---|---|
| File | New (Ctrl+N), Open, Save, Save As, Quit; toolbar New/Open/Save; Save All (2026-10-04: `saveall`, Ctrl+Alt+S); Close, Close all (2026-10-04: `close` / `closeall`, Ctrl+W / Ctrl+Shift+W, a Yes/No/Cancel question for unsaved buffers) | Recent files |
| Edit | Undo, Redo, Cut, Copy, Paste, Select all, Find, Go to line, Clear shell (2026-10-04: `replclear`, Ctrl+L), Indent/Dedent selected lines and Toggle comment (2026-10-04: `indent` / `dedent` / `togglecomment`; Tab over a multi-line selection, Shift+Tab, Ctrl+3), Replace (2026-10-04: `replace`, nano's flow — find, replace with, a y/n/a question per match as a Yes/No/All/Cancel dialog in a window) | — |
| View | Shell, Variables, Problems, Outline (2026-10-04: the sidebar's second tab; toggling it off returns Variables), Program arguments (2026-10-04: `progargs`, split by `python::shlex_split`; F5 passes them, Build ▸ Run does not) | font size, Full screen |
| Run | Run (F5), Stop; toolbar Run/Stop | Debug, Step over/into/out, Resume, Run to cursor, breakpoints, Interrupt, Send EOF; toolbar Debug and the steps |
| Tools / Help | Key bindings, Options (2026-10-04: tab width, scheme, keymap), Help, About | — |
| Window | a window titled `chthonia - <file> @ <line> : <col>`; no console window on Windows (D3) | `--tui`/`--help` from a console on Windows (the GUI-subsystem image does not attach to its parent's console) |

The debugger is the largest gap: there is no stepper. It is its own arc
(the JIT executes, `mir-interp-not-assumed`) and is designed separately.
Every other row is a command over machinery that exists.

## 7c. Projects and building in Chthonia (owner, 2026-10-04)

The owner: "Chthonia needs a way to handle projects and building
executables like madcide does … those things are inherently important to a
C/C++ IDE." Thonny has neither; the parity table (§7a) is not the limit.

Everything it needs exists in madcide's base; Chthonia's bundle leaves it
out. The change is the bundle's data:

1. `chthonia.menu` — File gains Project…, Open Project… and Add Current
   File to Project (`project`, `openproject`, `projaddcur`): the implicit
   one-file project becomes a manifest when a second file joins it
   (`proj_add`). A Build menu after Run carries `build` (Build…), so
   `compose_menu_bar` lists the ^B rows under it as direct items: Check,
   Build, Run, Stop, and with a manifest open the project-wide rows plus the
   manifest's own commands.
2. `chthonia.layout` — the bottom panel's views become `repl problems
   output terminal`: a build's output streams into Output, and Build ▸ Run
   runs a console program on a pseudo-terminal in Terminal.
3. Keys — `chthonia.keys` binds Build… to Ctrl+B (Xcode's and Sublime's
   build key; Thonny binds nothing there); `vscode.keys` binds Ctrl+Shift+B,
   VS Code's Run Build Task.
4. Help (§7d) says how Run ▸ Run (F5: the program in the Shell, its names
   in Variables) differs from Build ▸ Run (a native executable in the
   Terminal).

Gate: `testmadcide_chthonia` pins the menu bar (`[File Edit Run Build View
Tools Help]`), the File and Build ids, the panel's views, and a Build row
run from the Chthonia bundle producing an executable; the key pins move
with the bindings.

**Done 2026-10-04** as described. The test picks the Build row by its
action in the build rows' data and waits on `buildlive` (the REPL's session
is a task that never ends, so a task drain would not return). Not done: a
Build button on the toolbar — `ui::icon` has no build picture, and adding
one touches every renderer; the menu and Ctrl+B carry it.

## 7d. Help, and Markdown (owner, 2026-10-04)

The owner, on Help ▸ Help: "it's sort of cryptic, and doesn't read like a
real help page, and the scrollbars are a little off … it needs topics,
contents, information on all the menus and commands." And: "a markdown
render could be particularly useful within the context of an IDE, because
it would be good for the IDE to support reading, displaying, and editing
markdown files (i.e. README.md)." Today Help lists the loaded key profile's
binding lines in a choice pane, under the label `Help — <profile> profile`.

The name "Thonny" appears nowhere Chthonia shows text; the key style that
follows Thonny's defaults is titled Chthonia (`chthonia.keys`, owner
2026-10-04).

**One Markdown parser: cmark-gfm** (github/cmark-gfm, owner 2026-10-04 —
the CommonMark reference implementation plus GitHub's tables, task lists,
strikethrough and autolinks, which READMEs are written in). A dependency,
not a subtree: Debian/Ubuntu `libcmark-gfm-dev`, Homebrew `cmark-gfm`, and
the same upstream release built with mingw for Windows. It links
statically into a madc module beside `madcgit`, which every package ships,
because Help must work on every platform. Its COPYING (BSD-2 for cmark;
MIT for houdini, GitHub's buffer and the utf8proc-derived utf8.c) ships
verbatim with each package: `/usr/share/doc/madc/cmark-gfm-copyright` on
Linux, `THIRD_PARTY_NOTICES/cmark-gfm-COPYING.txt` on macOS and Windows.
The CommonMark spec (CC-BY-SA 4.0) is read from the dependency's source
tree by the conformance lane and never copied into the repository.

The module turns Markdown text into a value tree: each block and inline
with its kind (an enum) and its source range. Three consumers:

1. **A rendered page view** — headings, paragraphs, lists, code blocks,
   tables and links as ui-tree nodes (`heading`, `content`, `list`,
   `item`, `action`, `group`), which the web page, the TUI and
   `render_tree` already draw. A link to another topic or file is an
   action row. Two uses:
   - **Help**: each bundle contributes its topics as `.md` files (a new
     contribution kind beside keys, menu and layout), starting at a
     Contents topic. One topic, **Menus and commands**, is generated as
     Markdown from the loaded menus and the command registry: every menu,
     every item, what it does, and its key in the current key style. It
     cannot drift from the real menus. Command descriptions are data in
     the bundle's help, the base bundle's when the bundle has none.
   - **View ▸ Markdown Preview** of the `.md` buffer being edited, beside
     it, refreshed as it changes.
2. **Highlighting** of `.md` buffers through the existing span machinery
   (IDE-7).
3. **The editable concealing lens** on the view seam (AST-3's named seat:
   formatting characters hidden, caret and edits in stored offsets through
   the coordinate map). Block source positions from cmark are exact;
   inline positions are measured first, and where they are inexact the lens
   computes them inside the block's known range.

**Measured 2026-10-04** (cmark-gfm 0.29.0.gfm.6, Ubuntu's
`libcmark-gfm-dev` + `libcmark-gfm-extensions-dev`, which ship static
archives and a `libcmark-gfm.pc`; a probe printing every node's
`start/end line:column` with `CMARK_OPT_SOURCEPOS` and the table,
strikethrough, autolink and tasklist extensions attached, over a sample
with emphasis across a line break, a link, code spans, a nested list, a
table, a fenced block and a quote with an entity and an escape):
- exact, delimiters included: every block, emphasis and strong (across the
  line break too), links, strikethrough, table cells — so the concealed
  characters of an inline are its range minus its children's;
- a code span reports its content only (the backticks are just outside);
- a soft line break reports no position (0:0);
- a text node's literal is decoded and merged (`&amp;` reads `&`, `\*`
  reads `*`, one node), its range covering the source run — offsets inside
  it come from scanning that run;
- a list and its last item end at the following line, column 0.
The lens takes the ranges and scans for the backtick runs, the escapes and
the entities; nothing needs patching in the dependency. Still to check:
the help pane's scrolling on a rendered window.

Gates: a conformance lane over the CommonMark spec examples (a ratchet,
like the C lanes); the shipped-notices check in `package_install_gate.sh`
— a data list of the libraries each package links, each artifact carrying
every listed notice (webview, libgit2, zstd and cmark-gfm today); help
topic and Menus-and-commands pins in the madcide tests.

Thread contract: the parser is a pure function; a bundle's help topics are
read-only data loaded per session.

## 7e. Git in Chthonia (owner, 2026-10-04)

The owner: "since we have libgit support already for madcide, I think
Chthonia should include/reveal git support". What exists: the `madcgit`
module, madc's read-only view of a local repository over libgit2, which
madcide reaches only through its agent seat (the graph verbs `status`,
`history`, `commits`, `revision`, `diff`, and git provenance for blame). No
menu, view or status field shows any of it. The work is in madcide's base,
and Chthonia's menu and layout show it.

1. **Stage 1 — show what exists (read-only):** the branch and the file's
   state against the last commit on the status line and the tabs; View ▸
   Changes (the buffer's diff against the last commit); View ▸ History (the
   file's commits; choosing one shows that revision read-only through the
   view seam); blame for the caret's line.
2. **Stage 2 — writing (the owner's call):** init, stage and commit, which
   extends `madcgit` past its read-only design; push and pull last, since
   they need TLS and SSH in the Windows and macOS libgit2 builds (built
   today with the network backends off).

Packaging: every target links libgit2 statically into the module, Linux
included since the 1.8.7 / 1.9.7 floor (owner, 2026-10-04: Ubuntu's system
libgit2 is 1.7.2), so no package depends on a libgit2 runtime package; its
notice ships beside the module (`share/doc/madc/libgit2-copyright`,
`THIRD_PARTY_NOTICES/libgit2-COPYING.txt`).

## 7f. REPL commands, and the Variables row (owner, 2026-10-04)

The owner: Run writes `%run hello.c` in the REPL, but typing it is refused
("unknown command '%run'"); add `%run`, `%load` and maybe `%build`; then
"access to % commands for git operations". Today the engine session has
four commands (`InteractiveSession::command_rows`: help, type, pinfo, whos),
and a plugin can only observe REPL events (`ide::event`), not add a command.

1. **The Variables row** (owner: "why do we need the type twice? and we
   should support showing the content … when it is a char *"). The row
   form (`__madc_show_row`: `%whos` and the Variables view) prints a pointer
   through `__madc_dump_sh_ptr`, the entry show's re-enterable spelling
   `(const char *) 0x…`; both consumers already have a Type column. The row
   spells the value alone, as gdb does inside an aggregate and CodeLLDB's
   Variables view does: a pointer is its address, and a pointer to a
   character type is gdb's `0x… "Test"`. The text is read through a
   fault-safe, bounded read: a new runtime owner (`process_vm_readv` on
   self on Linux, `vm_read_overwrite` on macOS, `ReadProcessMemory` on
   Windows; the searched concept "read memory that may fault" has no owner
   in the tree), cut at the row's width with `…`; memory that cannot be read
   shows `0x… <unreadable>` (gdb: `<error: Cannot access memory …>`). The
   entry show (`p` at the prompt) keeps its re-enterable spelling.
2. **Engine commands** (madc's REPL, every host): `%run FILE [ARGS]` (D16:
   a fresh namespace, the file's `main` run with ARGS split by the shell's
   rules, its names left for the prompt), `%load FILE` (Julia's `include`
   into the current session), `%build FILE [-o OUT]` (the in-process native
   build). F5 submits the typed `%run` line, so the shown command and the
   run are one path.
3. **The IDE command layer:** `ide::repl_command`, a plugin's `%name`
   answered by the IDE before the session sees the entry; `%open FILE`,
   `%edit NAME` (the name's definition in the editor, from the bindings'
   file and line), and `%git`. `%help` lists both layers.
4. **`%git VERB`:** the calls the Git view makes (§7e), one implementation
   behind the menu and the command. The read verbs come with §7e stage 1:
   `log [FILE]`, `show REV[:FILE]`, `blame FILE[:LINE]`, `status`,
   `diff [FILE]`; `status` and `diff` are new `madcgit` reads (the module
   has open, head, refs, revparse, log, show, blame and dirty). The writing
   verbs (`add`, `commit -m`, `checkout`) come with stage 2.

## 8. Order

A (the REPL, which every chthonia user sees first; done) → B (measured, no
defect; done) → D1 (the probe; done) → D2 → D3 → D4 with C folded in (the
Mac build is chthonia's Mac build) → the parity rows (§7a) → Projects and
building (§7c) → the libgit2 floor (1.8.7 / 1.9.7, owner 2026-10-04) →
REPL commands and the Variables row (§7f) → Help and Markdown (§7d) → Git
(§7e) → Recent files and the rest of §7a → the debugger arc.
Owner, 2026-10-04: the chthonia binary comes first, GUI by default, working
on all three platforms with Thonny's functionality. Each step is its own
commit with its reducer, Tier 1 + Tier 2 per commit, and the batch after
each step.
