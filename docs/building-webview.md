# Building and testing the platform webview library

`madcwebview` binds the C API of webview/webview **0.12.0**. Its source is a
Git subtree at `third_party/webview` (upstream commit
`3ab4b5d722438fc8a13e6ca830c5e2372d19a01d`, history preserved). The MIT
notice remains at `third_party/webview/LICENSE`. The only local patch fixes
GTK's `set_size_impl` returning an error after successfully applying a valid
hint; `tests/gui/webview_size.mad` covers all four hints.

The build is optional: ordinary `make -C src` and non-GUI programs do not
depend on GTK or load a browser engine. Build the library explicitly:

| Build host/target | Command | Artifact |
|---|---|---|
| Linux | `make -C src libmadcwebview` | `lib/libmadcwebview.so` |
| Linux → Windows x86_64 | `make -C src webview-windows` | `bin/madcwebview.dll`, with the existing UCRT C++/pthread DLLs beside it |
| Linux → both macOS architectures | `make -C src webview-macos` | `lib/webview/{arm64,x86-64}-macos/libmadcwebview.dylib` |
| Single macOS architecture | `make -C src webview-arm64-macos` or `make -C src webview-x86-64-macos` | The corresponding artifact above |

Build on the desktop container, not the NAS. `src/webview.mk` owns these
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
`DARWIN_LD_FLAGS` from the existing Makefile. The **library alone** targets
macOS **13.3**, via `WEBVIEW_MACOS_MINOS`; madc's own minimum stays unchanged.
The deployment version must appear in clang's target triple: a separate
`-mmacosx-version-min` cannot override an older explicit target triple.
The 13.3 choice avoids the cross toolchain's missing
`__isPlatformVersionAtLeast` helper. Each dylib has install name
`@rpath/libmadcwebview.dylib`; stage the selected architecture as
`lib/libmadcwebview.dylib` next to the target madc installation. The host
posture also supports the existing `DARWIN_HOST=1` toolchain configuration.

## Typed interface

```c
import madcwebview;
webview_t w = webview_create(0, 0);
```

The module row points at embedded `webview.h`. Its **global C declarations**
come directly from the upstream C section: `typedef void *webview_t`, all
three enums, the version structs, all **17** functions, pointer returns,
`const webview_version_info_t *`, and the original callback signatures.
There are no provider wrappers or namespace aliases in this header.
`scripts/gen_webview_header.py` regenerates it; `--check` verifies exact
agreement and runs in `fulltest` without needing GUI dependencies. It omits
upstream's C++ implementation and selects external rather than inline linkage.

Use the UI thread for creation, window access, rendering, binding and
destruction. `webview_dispatch` schedules work onto that thread and
`webview_terminate` may be called from a background thread. Callback ID and
request strings are borrowed for the duration of the callback. The caller
owns callback state and must keep it alive until callbacks have finished.
This is upstream's low-level library contract; the engine owns higher-level
state and synchronization. Handles remain pointers, never boxed `var` values.

## GUI validation and memory policy

```sh
scripts/remote_build.sh sync build gui
```

The `gui` stage builds the library, checks the generated header, then runs
`tests/gui/*.mad` through the existing fixture runner under `xvfb-run -a`.
It covers JIT and native executables. The DOM reducer verifies version data,
native handles, page text/layout, initialization, both callback signatures,
the promise returned by `webview_return`, and destruction. The size reducer
checks each defined size hint. Neither requires Internet page content.

The stage sets **`MADC_MEM_LIMIT=0` only for its test process**: WebKit's
virtual address reservations exceed madc's ordinary 4096 MB guard. Set
`MADC_GUI_MEM_LIMIT=<MB>` to choose a finite GUI limit; invalid values fail
before testing. The stage prints the selected policy, caps CPU time at
30 seconds and wall time at 120 seconds; individual tests retain runner
timeouts. Ordinary madc invocations retain their default memory guard.

`MADC_TEST_DIR` is a generic fixture-runner input; it defaults to `tests`.
GUI tests do not change the default suite's inventory or baseline. A
relocatable `.o` has no shared-library dependency table: to run one directly,
load its dependency explicitly, for example with `-lmadcwebview` and
`LD_LIBRARY_PATH` pointing at this build's `lib`. Direct `import` discovery
is tested by the GUI stage without those extra flags.

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
