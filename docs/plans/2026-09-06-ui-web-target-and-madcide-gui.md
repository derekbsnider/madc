# Level 3 is the web target: madc::ui over the platform webview, madcide GUI mode as the first customer, and `import` as the module binding

**Owner brainstorm + recon, 2026-09-06 (session s157, the morning after the
v0.98.0 master promotion).** This document captures what the owner RULED,
what stands as a default unless vetoed (STANDING), and what is OPEN. It is
the execution input for the arc; the slices in §5 each get their own plan
when they start.

It supersedes the split between rendering-abstraction.md's Phase 5 (web
backend) and Phase 6 (native GUI), and ROADMAP rows 7.5 and 7.6: they are
ONE target. The gateway design
([2026-08-31-madcide-gateway-and-code-server.md](2026-08-31-madcide-gateway-and-code-server.md))
is unchanged; the GUI is a client of the session it describes.

## 1. Rulings (owner, 2026-09-06)

- **GUI mode for madcide is the next natural extension of madc::ui, and the
  IDE is its first customer.**
- **HTML/CSS through the platform's own webview**, because every platform
  ships one: Windows → WebView2, macOS → WKWebView, Linux → WebKitGTK, iOS →
  WKWebView, Android → WebView. The browser and the desktop GUI are the same
  render target; the DOM mapping is written once.
- **Look and feel: familiar, not a clone.** VS Code dominates the market and
  sets expectations; take inspiration from VS Code, CLion, Atom and Qt Creator.
  It must not look like a forked VS Code either.
- **The library binding is platform-agnostic through ONE mechanism**, and it
  must work in compiled (AOT) form as well as under the JIT: Linux, Windows,
  macOS now; Android and iOS eventually, "at least in compiled form".
- **Adopt `import`, not an improved `#load`.** `#load` was already obsoleted
  by `-l` on the command line. The owner asked whether the C++20 Modules
  `import` or a madc-specific convention fits; the answer adopted is §3.1:
  `import` as the C++20 module import made whole (interface AND binding), and
  `#load` retires. **Amended by the owner during slice 0 (2026-09-06):
  `#load` stays as the LOW-LEVEL directive underneath the alias form (like
  `#pragma`, for tooling and fixtures — the file spelled verbatim, the author
  owning the platform); `import` is the language form. It is three lines over
  the shared binder.**

## 2. Recon

### 2.1 What the repository already has

- **The projection contract is a value tree** (`docs/language/ns-ui.md`
  §Projection-as-data): sparse objects `{ role, label, content, hints,
  states[], actions[], subject, children[] }`. Roles the renderers know:
  `group heading content status item action separator list choice edit`.
  Hints in use: `caret sel_start sel_end spans rows tabwidth view` plus the
  lens map. Unknown roles are structure; unknown hints are ignored — the
  documented extension seam.
- **Level 1 is two halves.** `madcdis/tui_model.h` (engine: layout, focus,
  chords, coalescing, diffing) and the `tui_target` provider seam
  (`madcdis/tui_provider.h`: bytes and key reads only; `src/ui_term.cpp` is
  the hand-rolled VT100 target, registered by name as `term`). The seam is at
  the CELL-GRID level: a webview plugged in there would be a terminal
  emulator in HTML, not a GUI. **The Level 3 renderer sits beside
  tui_model**, consuming the same tree `tui_model::compose` consumes.
- **madcide is already split for this.** `class IdeSession`
  (`tools/madcide/madcide_core.inc`, terminal-free, gated by
  `scripts/check-madcide-seam.sh`) and the TUI client
  (`tools/madcide/madcide_client.inc`, 90 lines): push facts (viewport,
  pending chord) → `compose_ide_tree` → `tui_render` → `tui_event` →
  `apply_ide_event` → rebind keys on a generation counter → service one
  parked terminal request. The client file itself says: "A remote client
  would do the same over a transport." A GUI client is that loop with a
  different target.
- **Keys are data and stay client-private** (gateway design, RULED): profiles
  (`profiles/*.keys`, `@scope` sections, one `parse_keys`), validated
  handle-free by `ui::tui_validate_keys`, bound by the client. Themes are
  data too (`profiles/*.theme`, JOE's 8-colour spec vocabulary; `joe.status`
  the status-line seats).
- **JSON exists**: `js::parse` / `js::stringify` over the one JSON bridge.
- **Interaction bindings of either kind are first-class**
  (`madcdis/interaction.h`, the seam law): native or script-entity bodies
  attach to engine seams. A script-implemented render target is inside the
  design's own law, not an exception to it.

### 2.2 The library binding today (the `#load` / `-l` / madc_dl facts)

- **One loader seam already exists**: `include/madc_dl.h` / `src/madc_dl.cpp`
  — "one owner for every dlopen/dlsym-family use in the host"; POSIX body
  (dlopen) and Win32 body (LoadLibrary/GetProcAddress + recorded-module walk);
  "call sites never test the platform". `#load` and `-l` both go through it.
- **`#load` passes the library SPELLING verbatim** (`src/lexer.cpp` ~6456):
  `#load "libc.so.6" as libc;`. That is why `tests/testdlopen.mad` carries a
  `darwin_skip` and a `win64_skip`: the spelling is a Linux library identity.
- **`#load` is JIT-only.** `tests/testdlopen.exe_skip` states it: the
  synthesized `__dl_<ns>_<member>` import thunks resolve from the live handle;
  "Native #load support is a follow-on."
- **`-l<name>` already does the platform mapping, half way** (`src/madc.cpp`
  ~893): `lib<name>` + `MADC_DSO_SUFFIX`, opened RTLD_GLOBAL before compile
  under the JIT, forwarded as `-l<name>` to the host link line in AOT mode
  (`cc_link_args`; the exe lane records `DT_NEEDED` in `src/madc_cir.cpp`
  ~2080). **Found gap:** `MADC_DSO_SUFFIX` (`include/datadef.h` ~160) has
  only `.dylib` / `.so` branches — no `.dll`, and Windows has no `lib`
  prefix. The `-l` mapping is wrong on the win64 lane today (silent: nothing
  in the suite exercises it there). The resolver in §3.1 becomes the ONE
  owner of that mapping and fixes it.
- **`#load` live inventory**: `tests/testdlopen.mad` is the only source use.
  No embedded header carries it any more (`tests/testnoautoload.mad`'s
  comment about `<math.h>` is stale — verified against
  `src/embedded_headers.cpp`). Engine references: the lexer directive, the
  `dl_symbol_map` / `__dl_` thunk machinery in `cir_builder.cpp`, the
  `--no-auto-load` flag, `loaded_lib_paths` (the frozen-forest link closure),
  `sys.path` docs in `<ns_madc>`. Docs/rules mentioning it:
  `.claude/rules/embedded-headers.md`, `docs/language/preprocessor.md`,
  `docs/language/embedded-headers.md`, `docs/man/madc.1`,
  `docs/grammar/madc.ebnf`, `docs/architecture.md`.
- **`import` is unclaimed**: no handling in `src/` or `include/`; the only
  appearances in `tests/*.mad` are comments. The version-gated reserved
  keyword registry lives in `src/lexer.cpp` (~5464, "C++26 and earlier").
  `docs/plans/cpp-support.md` has no Modules item yet.

### 2.3 Platform facts (probed 2026-09-06)

| Where | Fact |
|-------|------|
| Build container (WSL2 Ubuntu 24.04) | No webkit or gtk dev packages, no Xvfb, no DISPLAY. apt candidates present: `libwebkitgtk-6.0-dev` 2.52.3, `libwebkit2gtk-4.1-dev` 2.52.3, `libgtk-4-dev` 4.14.5, `xvfb` 21.1.12, `weston` 13; `sudo -n` works. A `scripts/provision_container.sh` line, not a blocker. mingw has no WebView2 header (the vendored library carries its own loader). |
| Windows box (the genuine-Win channel) | Windows 11 build 26200; Evergreen WebView2 runtime 152.0.4191.66 installed (`HKLM\...\EdgeUpdate\Clients\{F3017226-...}\pv`). The channel lands in **WSL** (`uname` = WSL2) and runs Windows binaries through interop, which places them in the logged-in desktop session (explorer.exe in `Console`, session 1). WebView2 is unsupported in Session 0 / non-interactive logins ([WebView2Feedback #4259](https://github.com/MicrosoftEdge/WebView2Feedback/issues/4259), [#2434](https://github.com/MicrosoftEdge/WebView2Feedback/issues/2434)); the interop path should avoid that — **to be proven by the spike**, never assumed. |
| Owner's Mac | macOS 15.3.2; WebKit.framework; CLT SDK MacOSX15.sdk; a console user is logged in (a GUI session exists for ssh-launched windows). |
| GitHub mac runners | Have a window server (Xcode UI tests run there) — the darwin suite lane can host the page. |
| webview/webview | MIT. C API of ~15 functions (`webview_create/destroy/run/terminate/dispatch/set_title/set_size/navigate/set_html/init/eval/bind/unbind/return/get_window/get_native_handle/version`). Backends: WebKitGTK 4.0 / 4.1 / 6.0, Cocoa+WebKit, Win32+WebView2 with a built-in loader. C++11 core (C++14 on Windows). CMake shared-library build. Latest tag 0.12.0 (2024-09-11); no GitHub releases. `webview_bind` gives JS `window.<name>(...)` returning a promise; the callback receives `(id, req_json, arg)` and answers via `webview_return`. No custom URI scheme API; content via `set_html` / data URLs. |
| WebKitGTK | 2.52.x stable; API 4.1 = GTK3 + libsoup3, 6.0 = GTK4 + libsoup3 (4.0 = GTK3 + libsoup2, legacy). |
| WebView2 distribution | Evergreen runtime is part of Windows 11 and on "the vast majority" of Windows 10 devices; detect via the registry key; the loader ships with the app (DLL or static). |

### 2.4 Look-and-feel anatomy (the inspiration set)

- **VS Code** (ux-guidelines): Activity Bar · Primary Sidebar (views) ·
  Secondary Sidebar · Editor (editor groups, tabs) · Panel (Terminal,
  Problems, Output) · Status Bar (workspace items left, file items right) ·
  Command Palette · Quick Pick.
- **Qt Creator**: mode selector (Welcome/Edit/Design/Debug/Projects/Help) on
  the left edge, kit selector + build/run/debug buttons under it, Locator
  (Ctrl+K), output panes along the bottom.
- **CLion**: tool windows docked on every edge with a strip of toggles,
  run configurations in the toolbar, gutter icons, a bottom Problems/Run/
  Debug area.
- **Atom**: tree view sidebar, tabbed editor, a command palette, "hackable"
  through data.
- **The common denominator** (what our workbench composes): a left rail of
  mode/view toggles · a primary sidebar (explorer/project, outline,
  diagnostics) · a tabbed editor group · a bottom panel (diagnostics table,
  build/run output) · one status bar · a palette/locator popup. Every one of
  these maps onto a role madc::ui already has (§3.4).

## 3. Design

### 3.1 `import` — the module binding (RULED direction; prerequisite slice)

**What it means.** `import name;` names a MODULE. C++20 says `import`
brings a module's interface into scope and leaves how the implementation is
bound to the build system. Every other modern language's `import` does both.
madc's does both: it is the C++20 import made whole, not a contradiction of
it — when real C++20 modules (`export module`, interface units) reach the
compliance roadmap, the SAME resolver serves them: module name → interface +
binding is the shape both need.

**The resolver (data, never hardcoded):**

- A **module map** maps a module name to (a) an INTERFACE — today an
  embedded header or a header on the module path (e.g. the vendored
  library's `wv` declarations shipped as an embedded fragment), later a
  module interface unit — and (b) a BINDING by NEUTRAL library name
  (`madcwebview`). No `.so`, `.dll` or `.dylib` appears in any source.
- **Platform spelling is the seam's job**: `lib<name>.so` / `lib<name>.dylib`
  / `<name>.dll`, searched on the madc library path (beside the running
  binary → the install libdir → `MADC_LIBRARY_PATH`). ONE mapping owner,
  shared with `-l` (which today maps in `madc.cpp` with the Windows-less
  `MADC_DSO_SUFFIX` — that code moves down, and the `.dll` case is fixed
  there). `-l` is the build-system spelling of the same binding; `import` is
  the source spelling.
- **Binding policy is per TARGET** (the enum, I3): under the JIT the seam
  opens the library (madc_dl, RTLD_GLOBAL — the `#load` semantics); a native
  artifact gets a LINK-TIME import (`-l<name>` on the host link line / a
  `DT_NEEDED` / `LC_LOAD_DYLIB` / PE import — the `--no-auto-load` shape),
  or a STARTUP-TIME open emitted into the artifact through the same seam
  (the runtime already ships madc_dl), chosen by the target profile. iOS has
  no JIT, so the compiled form is the only form there and its policy is
  "link"; Android's is "open the bundled library". The source is identical.
  This closes the `testdlopen.exe_skip` follow-on ("native #load support").
- **Namespaces come from the interface.** The module's declarations say
  `namespace wv { ... }`; `import madcwebview;` then `wv::webview_create(...)`.
  The interface-less convenience `#load` offered (call anything in the
  library with the variadic dlsym convention) survives as the SECONDARY form
  `import name as alias;` — Python's spelling — so `import libc as libc;`
  is today's testdlopen with a neutral name.
- **Gating** (I3, I4, I8): `import` is a keyword under `--std=madc` and under
  the C++20-and-later standards through the existing version-gated keyword
  registry; an ordinary identifier under C89–C17 and C++98–C++17. Under
  C++20 the standard's meaning (interface import) is exactly what madc does;
  the binding is what the standard leaves to CMake. Blast radius today: no
  identifier `import` anywhere in the suite.
- **`#load` becomes sugar over the same resolver on day one** (one
  implementation, no parallel path). Owner ruling 2026-09-06: it STAYS as the
  low-level verbatim-file directive (tooling / fixtures; `vector_abi_gate.sh`
  binds a freshly built `.so` by absolute path with it) — not retired.
  `--no-auto-load` is the JIT-side spelling of the "link" policy for both.

**Invariants check** (`docs/plans/madc-vision-and-invariants.md`): I3 —
no target or standard named outside the enum (the binding policy hangs off
the target profile, the keyword off the standard registry); I4 — `import`
earns a registry entry; I5/I1 — no IR change (a module binding is a link
fact, not a node); I6 — one resolver replaces `#load` + `-l` + the
`exe_skip` gap instead of patching three; I8 — C modes reject `import` as a
keyword exactly as they should.

### 3.2 The web provider — Level 3

The same two halves as Level 1, at the tree level instead of the grid level:

- **`web_model` (engine, `madcdis/web_model.h`, dependency-free):** turns the
  value tree into DOM OPERATIONS as JSON (create/replace/remove node, set
  text/attribute/class, keyed by node path so the applier reconciles), and
  turns page input back into the SAME semantic event objects the TUI emits
  (`{event:"text"}`, `{event:"key"}`, `{event:"choose"}`, `{event:"action"}`,
  `{event:"focus"}`, `{event:"resize"}`). **The chord/key-table machinery is
  factored OUT of `tui_model` into one shared owner** (`madcdis/keys.h`:
  bindings, chord pending state, validation, coalescing) so both models use
  one implementation — the dupaudit family this slice must not create. Key
  resolution happens HERE, in the native client half, never in JavaScript:
  the page sends raw key spellings in the TUI's vocabulary (`"^k"`, `"up"`,
  `"enter"`, printable runs) and the model resolves them to action names —
  so keys stay client-private exactly as the gateway design rules.
- **`web_target` (madc source — an embedded fragment, e.g.
  `include/madc/bits/ui_web`):** `import madcwebview;` then hosts the page:
  create the window, load the embedded page, bind the ONE native callback the
  page posts events to, push DOM ops with `eval`, run the loop / `dispatch`.
  The engine's C++ never names a platform library and never grows a webview
  dependency; a missing library makes `open` refuse with the reason, the
  same shape as `tui_open` without a tty. Registering a script-implemented
  target with the provider registry is the script-entity binding kind
  (`interaction.h`'s seam law).
- **The page (one embedded JS file + one CSS file, identical on every
  platform; generated into the binary like the embedded headers):** applies
  DOM ops, captures input (keydown → key spellings; `input` events →
  printable runs; IME composition later), reports viewport FACTS (rows/cols
  in text cells for the edit region, pixel size) back through the callback —
  the same "facts the client pushes" pattern as `S.viewport()`. It contains
  no editor knowledge and no key→action knowledge.
- **The editor surface is our own**: an `edit` node renders as a line-DOM
  (one element per visible line, `spans` from `madc::parse_spans` as class
  names the theme colours), our own caret and selection painting, a hidden
  input element for keystrokes. No Monaco, no CodeMirror, no contenteditable.
  First slice sends the document text as the tree carries it today
  (tui_model takes it the same way); virtualized windows ride the 7.3 diff
  work.
- **Rendering cadence**: full re-compose per cycle with keyed reconciliation
  on the page, exactly the compose+diff cadence the TUI runs today. Track
  7.3's semantic-diff wire refines it; nothing here precludes it.

### 3.3 The target-generic surface

`ui::open(const char *target)` returns a handle; `ui::render(t, w, tree)`,
`ui::event(out, t, w)`, `ui::bind_keys(t, table)`, `ui::validate_keys`,
`ui::close(t)`, `ui::rows/cols` work on any handle. The `tui_*` names stay
as the `term` target's spelling (`tui_open()` == `open("term")`); no
application changes to keep working. `suspend`/`resume` are a
terminal-only capability: the web target answers false, and the IDE's
terminal-request seam routes accordingly (§3.4).

### 3.4 madcide GUI mode — the first customer

- **One client loop.** `madcide_client.inc` stays the client; it picks the
  target from a flag (`madc tools/madcide/madcide.mad --gui <file>`, later a
  profile/config default). `IdeSession` is untouched by the target.
- **One composer.** `compose_ide_tree` stays THE composer for both targets.
  It gains LAYOUT HINTS the TUI ignores and the web target honours:
  `region` (`rail` · `sidebar` · `editor` · `panel` · `statusbar`), `tabs`
  (the buffer set as an editor-group tab strip), `popup` (palette / project
  window / prompts float; the TUI already treats them as popups). This is the
  rendering-abstraction doc's own promise — one semantic tree, appropriate
  presentation per target — and the TUI's "unknown hints are ignored" rule
  is what makes it a no-op there.
- **Role → workbench mapping** (no new roles needed for slice 3):

  | madcide today | role | web target |
  |---------------|------|------------|
  | status row (`joe.status` seats) | `status` | status bar items (bottom; `%n %m %r %c %x %k` become items) |
  | the buffer | `edit` | editor group, tabbed |
  | diagnostics / outline / project window / help | `choice` | sidebar views (docked) or panel tables |
  | ^N modes palette, ^P quick-open, prompts | `choice` / `edit` (+`popup`) | command palette / quick pick overlays |
  | pane titles | `heading` | view title bars |
  | containers | `group` (+`region`) | rail / sidebar / editor / panel / statusbar |

- **Themes**: `.theme` files gain a `@gui` scoped section (the `@scope` rule
  from `parse_keys`, later lines win) carrying hex colours and font specs as
  CSS custom properties; the TUI reads the unscoped JOE-vocabulary lines as
  today. One file per scheme, two renderings.
- **Terminal requests in GUI mode**: `run` / `projrun` / `cmd` capture the
  child's output into a panel view (VS Code's Output shape) — no suspend; the
  `shell` request opens the platform's terminal application until the
  embedded terminal lands. **Embedded terminal (later)**: a pty whose output
  paints through our own `tui_grid` into a DOM grid — Level 1 inside Level 3,
  reusing the model we have.
- **Look**: the common-denominator workbench of §2.4 in our own palette and
  typography; JOE's single top status line was a TUI real-estate ruling and
  does not bind the GUI (STANDING, §4).

### 3.5 The remote target (ROADMAP 7.5, absorbed)

The same page and the same DOM-op / event JSON over a WebSocket instead of
`webview_bind` / `webview_eval` is a second target (`ws`) differing only in
transport. Recon gap: `madcdis` channels are client-side today (no
listen/accept found in `include/madcdis/*.h`); the server half is this
slice's work, and it is also the gateway design's transport slice (5).

### 3.6 Testing and lanes

- `web_model` is pure engine code: doctest batteries (tree → ops, keys →
  events, the shared key machinery's existing tests move with it) — the bulk
  of the logic, headless, every lane.
- The real page runs under `xvfb-run` on the container (a `gui` stage in
  `remote_build.sh`; provisioning adds `libwebkitgtk-6.0-dev` + `xvfb`),
  natively on the mac runners, and through WSL interop on the Windows box. A
  test drives the target with synthetic events (`eval` a dispatch) and reads
  a DOM snapshot back through the callback as text for an `.expect` fixture.
  Each lane is a SPIKE to prove before it becomes a gate; a lane that cannot
  host a window is formally skipped with the reason, never silently green.
- `import` ships its own reducers: the testdlopen family goes
  cross-platform (its `darwin_skip` / `win64_skip` / `exe_skip` drop), a
  win64 `-l` mapping test, a native-artifact import test in the exe and obj
  lanes.

### 3.7 Thread-safety contract (the law)

A web target instance is confined to the thread that opened it — the
platform webview APIs demand the UI thread anyway (`webview_dispatch` is the
only cross-thread door and is used only to hand the loop a closure). The
session registry contract of ns_ui is unchanged. `web_model` is a plain value
object like `tui_model`. The remote target's server half states its own
contract when its slice is designed (per-connection state, verbs serialized
through the session — the gateway design's concurrency ruling).

### 3.8 Licences

webview/webview is MIT (compatible with MPL 2.0; vendored as a subtree with
its notice, like MIR). WebKitGTK is LGPL/BSD and loaded at runtime, never
linked into madc. WebView2's loader is redistributable; the runtime is
Evergreen. WKWebView is the OS. No GPL enters the tree.

### 3.9 What we do NOT build

No JS engine (the OS ships one). No editor component from anywhere (ours is
the line-DOM over our own spans). No Codicons, no VS Code theme JSON, no
Monaco. No second composer for the GUI. No key→action logic in JavaScript.
No per-platform code above the vendored library.

## 4. Standing defaults (owner veto welcome)

1. **Vendor webview/webview** (MIT) as `third_party/webview`, C API only,
   built by our Makefile into `libmadcwebview` per platform, behind our
   provider seam so it stays replaceable and fixes flow upstream. The VT100
   precedent (hand-rolled over ncurses) was about avoiding a dependency; here
   the platform webviews ARE the dependency by design and the wrapper is only
   glue (~3k lines).
2. **Linux backend: webkitgtk-6.0 / GTK4.** Both 6.0 and 4.1 ship on Ubuntu
   24.04 at 2.52.3; GTK3 is maintenance-only.
3. **Visual identity**: bottom status bar (every modern IDE), dark default
   plus a light theme, one accent colour that is not VS Code's blue, system
   UI font, our own icon set as inline SVG. The owner's taste rules here.
4. **Names**: target `web` (webview and browser alike), module `madcwebview`,
   the remote target `ws`.
5. **Where the module map lives**: an embedded registry for shipped modules
   plus per-target binding policy tables (data), and `MADC_MODULE_PATH` for
   user modules.

## 5. Sequencing (each slice shippable, each with a gate)

0. **`import` slice** (prerequisite): resolver + module map + the one name
   mapping (fixes the Windows `-l` gap) + keyword gating + `#load` as sugar +
   native lowering (link-time / startup-time) + reducers (§3.6). `#load`
   stays as the low-level directive (owner 2026-09-06); the rules/docs that
   name it present `import` as the language form.
   **LANDED (2026-09-06, `feature/import-module-binding-claude`):** module
   map + spelling owner (d1884c85), `-l` through it (66d0d4c2),
   `__madc_dl_member` (def479f5), alias-form lowering in every lane
   (00265630), the directive (b2b201bb), native link closure (7d63f2e5);
   docs at `docs/language/import.md`. `#load` kept as the low-level directive
   (owner ruling above).
   **PLAN (2026-09-06):** [2026-09-06-import-module-binding-plan.md](2026-09-06-import-module-binding-plan.md)
   — eight tasks with anchors, code and gates; branch
   `feature/import-module-binding-claude`.
1. **Spike** (throwaway, one day): the vendored library built on the
   container, a `.mad` that says `import madcwebview;` and shows a window
   with "hello" under `xvfb-run`; the mac and Windows twins (proves the
   interop-session assumption). Output = a go/no-go per lane, no kept code.
2. **The web provider**: `web_model` + the shared key owner factored out of
   `tui_model` (behaviour-identical, gate-pinned) + the `web_target` fragment
   + the embedded page + the target-generic surface. Reducer:
   `tools/texteditor/vised.mad` renders and edits in a window; doctest for
   `web_model`; the `gui` lane stage.
3. **madcide GUI mode**: layout hints in `compose_ide_tree`, the workbench
   CSS, `@gui` theme sections, status bar items, output panel for requests,
   the `--gui` flag. Gate: the headless `testmadcide` event battery unchanged
   (one composer), plus the GUI lane's DOM-snapshot fixtures.
4. **The remote target** (`ws`): server-side channel + the same JSON; a
   browser is a client. Presence colours join with gateway slice 4.
5. Later, by demand: embedded terminal panel (tui_grid → DOM grid), Track
   7.3 semantic diff + virtualized editor windows, Android/iOS bodies for the
   vendored library (C/C++ app shells — NDK `NativeActivity` + JNI-driven
   WebView with the embedded Java shim on Android, the Objective-C runtime
   from C++ on iOS; §6).

The ROADMAP rows: 7.5 = this target (absorbing 7.6); 8.6 = madcide GUI
mode; 1.8 = `import`.

## 6. Open questions

- The module interface unit syntax (`export module`) and BMI-free
  compilation model when C++20 Modules join the compliance roadmap — the
  resolver must not preclude them (it does not: name → interface + binding).
- Windows GUI testing through WSL interop from an ssh session (spike).
- Server-side sockets in madcdis for the `ws` target.
- Per-target binding policy for a program that mixes JIT and AOT (the
  frozen-project / `--run-frozen` twins).
- Mobile hosting (owner 2026-09-06: C/C++, not Java/Kotlin — RULED
  direction). **Android**: the NDK's `NativeActivity` (API 9+; the manifest
  names our `.so` through `android.app.lib_name`, entry
  `ANativeActivity_onCreate`) makes the app shell pure C/C++ — no Java or
  Kotlin SOURCE anywhere in madc or in an app. The one caveat: the Android
  WebView has no NDK API; it is a framework class driven from C++ through
  JNI (`activity->env` / `->clazz`, on the UI thread the activity callbacks
  already run on), and receiving the JS→native bridge requires a Java
  callback object (`addJavascriptInterface` / `WebMessageCallback` /
  `WebViewClient` are all Java types). So the Android body of
  `libmadcwebview` carries a few-dozen-line Java SHIM compiled ONCE to dex
  and embedded in the library artifact (loaded at runtime through the
  platform class loader) — a fixed part of the library, never something an
  application writes. JIT is permitted on Android (app processes may map
  executable memory), so both the JIT and the compiled form work there.
  **iOS**: pure C++ over the Objective-C runtime (`objc_msgSend` — the same
  technique the desktop wrapper's Cocoa backend uses for WKWebView on
  macOS; `UIApplicationMain` is a C function), no Swift; compiled form only
  (no JIT on iOS). Packaging for both (manifest / Info.plist, per-ABI
  libraries, signing) is a build-scripts concern driven from our Makefile,
  not Gradle or Xcode projects. Out of scope now; the requirement that
  nothing above the library names a platform is what keeps the door open.
