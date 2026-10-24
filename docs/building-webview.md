# Building and testing the ui_web module

`ui_web` is the madc module (`include/madc_module.h`) that serves the web UI
level: a program says `import ui_web;` and its `ui::open(ui::WEB)` gets a
platform window. Its source is `src/modules/ui_web/` — the host
(`ui_web.cpp`), madc's native chrome (`madcwebview_chrome.{cc,h}`) and the
face (`ui_web.madi`) — linked into ONE image with webview/webview **0.12.0**,
whose source is a Git subtree at `third_party/webview` (upstream commit
`3ab4b5d722438fc8a13e6ca830c5e2372d19a01d`, history preserved). The MIT
notice remains at `third_party/webview/LICENSE`. The only local patch fixes
GTK's `set_size_impl` returning an error after successfully applying a valid
hint (the host sets its window size through it).

The build is optional: ordinary `make -C src` and non-GUI programs do not
depend on GTK or load a browser engine. Build the module explicitly:

| Build host/target | Command | Artifact |
|---|---|---|
| Linux | `make -C src ui_web` | `lib/madc/modules/ui_web.so` |
| Linux → Windows x86_64 | `make -C src ui_web-windows` | `lib/madc/modules/ui_web.dll`, linked against libmadc's import lib |
| Linux → both macOS architectures | `make -C src ui_web-macos` | `lib/madc/modules/{arm64,x86-64}-macos/ui_web.dylib` plus a flat copy |
| Single macOS architecture | `make -C src ui_web-arm64-macos` or `make -C src ui_web-x86-64-macos` | The corresponding artifact above |

Build on the desktop container, not the NAS. `src/ui_web.mk` owns these
recipes and reuses the main Makefile's SDK and UCRT toolchain settings.
Products and downloaded SDK headers stay outside the vendor subtree.

Linux uses **WebKitGTK 6.0 / GTK4** through `pkg-config`; the Make variable
`WEBVIEW_WEBKITGTK_API` defaults to `6.0`, and other values are rejected.
`scripts/provision_container.sh` provides `libwebkitgtk-6.0-dev`, Xvfb,
xauth, curl, and unzip. A missing dependency fails with an actionable message.

Windows uses the existing **UCRT** C++ runtime stage. The built-in webview
loader needs **WebView2 SDK headers**, but no `WebView2Loader.dll`.
`scripts/fetch_webview2_sdk.sh` fetches NuGet `Microsoft.Web.WebView2`
**1.0.1150.38**, verifies SHA-256
`921c004bd1764b585496b2eb3eec0a59a9a98e698246f1d9a3f1c08d1d84ebd5`,
and extracts the headers and their license into
`obj/webview/webview2-sdk`. `WEBVIEW2_SDK_DIR` overrides that location.
The actual Windows machine must have the Evergreen WebView2 runtime.

Darwin uses `MACOS_SDK`, `DARWIN_CLANGXX`, `DARWIN_CXX_ISYS`, and
`DARWIN_LD_FLAGS` from the existing Makefile. The **module alone** targets
macOS **13.3**, via `WEBVIEW_MACOS_MINOS`; madc's own minimum stays unchanged.
The deployment version must appear in clang's target triple: a separate
`-mmacosx-version-min` cannot override an older explicit target triple.
The 13.3 choice avoids the cross toolchain's missing
`__isPlatformVersionAtLeast` helper. Each dylib has install name
`@rpath/ui_web.dylib`.

## The module's surface

A program imports the module and names the level; the module registers its
host (target "web", level ui::WEB) as it loads, and an imported module opens
at program start in every lane (JIT, executable, object):

```c
import ui_web;
long t = ui::open(ui::WEB);
```

The face exports `ui_web::menu_activate(id)` (the menu test seam) and nothing
else. The webview C API is the module's own: the host calls it, and a
program reaches the window through `ui::`. A module carries no macros (as a
C++20 named module carries none), and upstream's header is mostly macros and
global C types.

The webview library's own threading contract, which the host keeps: use the
UI thread for creation, window access, rendering, binding and
destruction. `webview_dispatch` schedules work onto that thread and
`webview_terminate` may be called from a background thread. Callback ID and
request strings are borrowed for the duration of the callback. The caller
owns callback state and must keep it alive until callbacks have finished.
This is upstream's low-level library contract; the engine owns higher-level
state and synchronization. Handles remain pointers, never boxed `var` values.

## Native chrome — madc's extension of the library

`src/modules/ui_web/madcwebview_chrome.h` declares madc's own C API beside
upstream's — a native menu bar (`madcwebview_menu_begin` / `_add` /
`_separator` / `_end` / `_on_action` / `_activate`) and the platform's file
dialogs (`madcwebview_dialog_open` / `_save`) — and `madcwebview_chrome.cc`
implements it inside the module on all three platforms; the host is its
caller.

| Platform | Menu bar | File dialogs | Extra build inputs |
|---|---|---|---|
| GTK4 | `GtkPopoverMenuBar` over a `GMenu`; the webview re-parented under it | `GtkFileDialog` (GTK 4.10+; `GtkFileChooserNative` before) | — |
| Cocoa | the application's main menu (`NSMenu` on `NSApp`); the first submenu is the application menu, its Quit the window's `performClose:` | `NSOpenPanel` / `NSSavePanel` as a sheet on the window | `-fblocks` (the completion handler is a block; the code stays C++ over the ObjC runtime like the library) |
| Win32 | an `HMENU` bar (`SetMenu`); `WM_COMMAND` through a comctl32 subclass of the library's window procedure | `IFileOpenDialog` / `IFileSaveDialog` (COM), run from a message the subclass posts to itself | `-lcomctl32 -luuid` |

One reading of the madc key spelling serves the three: a control key
(`^s`) becomes the item's accelerator; a chord (`^k d`) or a bare key
(`pgdn`, a letter) is shown beside the title and never bound (bound, it
would fire on every such keystroke typed into the page). Every dialog call
is asynchronous on every platform — it returns 0 once the dialog is up and
the callback fires later, on the UI thread, inside the platform loop the
host is already running (`webview_run`). `madcwebview_menu_activate(id)`
fires an item as the user would — the test seam `tests/gui/ui_web_menu.mad`
drives it under Xvfb, and natively on the Windows and macOS staging sets.

The release targets build the module beside the binaries
(`modules-windows` runs `ui_web-windows`; `release-<arch>-macos` runs
`ui_web-<arch>-macos`; `package_release.sh` runs `ui_web`), and the packagers
ship it in madc's module directory, where an import looks — `lib/madc/modules/`
in the tarball, the macOS bundle and the Windows zip; beside libmadc in the
deb/rpm (`/usr/lib/x86_64-linux-gnu/madc/modules/`, `/usr/lib64/madc/modules/`). On Linux it is a WEAK dependency (`Recommends:
libwebkitgtk-6.0-4, libgtk-4-1`; the rpm excludes the module's own requires) —
madc never loads it, only a program that imports it does. The webview/webview
MIT notice ships beside it.

## GUI validation and memory policy

```sh
scripts/remote_build.sh sync build gui
```

The `gui` stage (`scripts/gui_lane.sh`) builds the module, then runs
`tests/gui/*.mad` through the existing fixture runner under `xvfb-run -a`, in
the JIT, native executable and object lanes. None requires Internet page
content.

**Memory policy (owner ruling 2026-09-07):** madc arms no resource guard by
default; the test runner asks for the memory guard's `auto` (4096 MB + 128 MB
per `--project` TU) for every test process. WebKit's virtual address
reservations exceed that, so the module's description carries the GUI flag
and a program that imports it lifts the (soft) guard at run start — the
GUI stage needs no memory override of its own. It still caps CPU time at 30
seconds and wall time at 120 seconds; individual tests retain runner
timeouts.

`MADC_TEST_DIR` is a generic fixture-runner input; it defaults to `tests`.
GUI tests do not change the default suite's inventory or baseline. An
object carries its module list (`__madc_module_deps`), so the single-object
lane opens the module before load with no extra flags.

## Separate worktrees

`remote_build.sh` derives its local root from its own location and defaults
the remote root to the same absolute path. `MADC_LOCAL_ROOT` and
`MADC_REMOTE_ROOT` override them. Paths must be absolute and contain no
whitespace or shell syntax. A linked worktree's NAS-specific `.git` file is
excluded from sync; keep an independent Git checkout at the remote root for
history-based gates. Build configuration and generated host probes also stay
on the build host. A fresh build stage bootstraps configure with `autoreconf`.

For Astra's branch, both build trees are `/workspace/madc-astra`. The shared
`/workspace/madc` checkout belongs to Claude. The busy guard still prevents
simultaneous build/test batteries on the container.

## Validation recorded 2026-09-07

- Linux library, Windows/UCRT DLL, and both Darwin dylibs build through Make.
  PE imports contain UCRT and no MSVCRT or external WebView2 loader; both
  Mach-O deployment commands report minimum **13.3**. Repeated unchanged
  builds do not recompile the libraries.
- GUI: **2/2 JIT, 2/2 executable**, no failures/timeouts. Callback object run
  with explicit library loading also succeeds.
- Module unit tests: **8/8**, **47 assertions**. Default-directory import
  neighbors: **3/3 JIT, 2/2 executable, 2/2 object**.
- GCC and Clang C controls reproduce the GTK size status bug (**−2**) and
  confirm the fix (**0**). The generated header check and provisioning
  `--check` pass. Invalid GUI memory settings are rejected.

Logs are under `tmp/logs` in Astra's worktree. These are targeted checks;
the combined slice-2 merge wave must run the normal full battery. Claude
owns the compiler binding fixes, `web_model`, and the provider engine.
