# madcide GUI chrome & modular UI — design (recon + proposal)

**Status:** DESIGN PROPOSAL (2026-09-07). Not approved, nothing built. This
folds two 2026-industry-standard recon passes into a proposed architecture and
a slice decomposition, for owner review. It completes the web-target arc
(ROADMAP 7.5 / 8.6) beyond slice 3.1.

## The problem

`madc tools/madcide/madcide.mad <file> --gui` today renders the TUI's composed
tree into a webview, so it *looks like the terminal in a window*. The owner's
requirement: real **application chrome** — a native menu bar surfacing the
command set, real **file dialogs**, a status bar that is chrome (not a text
region), and additional **panes** — plus the parked resize-fill fix (#3). And a
forward-looking requirement: the client-server potential means file dialogs must
serve **both** a server-side (workspace) filesystem and a client-side (local)
one.

## The unifying insight — our model IS the proven one

madcide is an **API-gateway editor**: one headless core (the running madc
compiler holding the live parse handle) composes ONE client-agnostic `uinode`
tree (roles + `hints` bag + verb `actions`); thin clients (TUI grid, webview
DOM) render it. The recon's sharpest finding:

> Our `uinode` tree + additive layout hints (region/tabs/popup) is **Neovim's
> remote-UI grid + `ext_*` events, rediscovered.**

Neovim's headless core renders *everything* into a universal cell grid by
default; a client passes `ext_*` options at attach to receive **structured
events instead of grid cells** for specific widgets (`ext_popupmenu`,
`ext_tabline`, `ext_cmdline`, `ext_messages`, `ext_multigrid`) and draw them
natively — and if a client does NOT opt in, the core renders that widget into
the grid itself ([Neovim ui.txt](https://neo.vimhelp.org/ui.txt.html)). Emacs
(`--daemon` + `emacsclient -c`/`-t`) proves one core driving a GUI frame and a
TTY frame *simultaneously* is a decades-old pattern. LSP is the same shape at
the analysis layer.

**So the design is not "add GUI features"; it is "let a capable client take over
rendering specific composed roles natively, while the TUI keeps rendering the
same roles as the universal fallback."** That is exactly what slice 3's layout
hints already do for region/tabs/popup — we generalize it.

## Decision 1 — the native/DOM boundary is empirically fixed

Every modern editor draws the same line, whether web-tech (VS Code) or
custom-drawn native (Zed/GPUI, Fleet/Skia). The entire native camp still
delegates exactly three things to **native OS widgets** and custom-draws
everything else:

| Native OS widget | App-drawn (DOM, in our webview) |
|---|---|
| Menu bar (mandatory on macOS per HIG) | Status bar |
| File / open / save dialogs | Panels, side bars, docks |
| Window frame / controls / tabs | Command palette, context menus, tabs |

Nobody in 2024–2026 renders a status bar or panel as a native OS widget (Zed
custom-draws its status bar at 120fps; VS Code renders it as DOM). Zed even
uses the **native** macOS menu bar while GPU-drawing everything else, feeding it
from `app_menus() -> Vec<Menu>` **data** ([zed #914](https://github.com/zed-industries/zed/issues/914)).

**madcide's rule:** native OS widgets (via a new `madcwebview` native API) for
the **menu bar, file dialogs, and window controls**; **DOM-in-webview** for the
status bar, panels, command palette, tabs, and all content chrome. This is the
smallest native surface with the largest platform-fit payoff — and it confirms
the owner's "both are needed," while saying precisely *which* goes native.

## Decision 2 — menu / command / action as data (VS Code shape + Zed discipline)

Adopt VS Code's contribution shape as the description the session composes
([contribution points](https://code.visualstudio.com/api/references/contribution-points)):

- a **command registry**: `{ id, title, category, icon, enablement }`
- a **menu-location map**: location → `[{ command, when, group:"id@order", submenu }]`
  (locations: menu bar, command palette, editor context, view title, …)
- **context keys** + a `when`-clause expression language for enable/visibility.

Plus Zed's discipline: the **action id is the only vocabulary** that keymap
profiles, the menu bar, and the palette share; dispatch it up the focus tree,
gated by context expressions ([Zed actions](https://deepwiki.com/zed-industries/zed/3.3-actions-and-keybindings)).
This is a near-exact match to our existing rule — *keybindings/menus/actions are
DATA (profiles), never hard-coded to actions* — and to our verb `actions` on
`uinode`. One description; the TUI renders it as its menu line / `^K` palette,
the webview renders it as a native menu bar AND a DOM command palette, from the
identical data. **Do not invent a new schema** — this is the proven one.

## Decision 3 — the two-sided file model (authority-driven, URI-scheme'd, dialog-as-verb)

The file-access recon converged on: **the machine that owns the bytes owns the
picker, and files are addressed by URI scheme, not bare path.**

- **Pick the dialog by the active *authority*, not the button** (VS Code Remote's
  rule): default the workspace "Open" to a **server-side tree streamed over the
  API** (a composed `list` pane); reserve the **native client dialog** for
  choosing a connection/local target and for *explicit* import/export. **No
  silent cross-boundary open** — "reveal in local" is a deliberate non-feature.
- **Address every file `scheme://authority/path`** (LSP `DocumentUri`; VS Code
  `file:` vs `vscode-remote://ssh-remote+host/…`). Define a server-workspace
  authority (default) and a distinct client-local authority; the scheme decides
  the owner; show a persistent authority indicator.
- **A client-picked file reaches the core as BYTES, never a path.** The browser
  File System Access API returns *opaque handles with no path* and is
  Chromium-only (Safari/Firefox lack it); even the native client's local path is
  meaningless to the server process. "Open local into workspace" = read bytes on
  the client, stream to core, materialize under a server scheme. Crossing = copy.
- **File dialog = a core→client request/response VERB**, never DOM. The webview
  can't call OS dialogs directly (sandbox); we bind ONE tiny allowlisted host
  function (à la Electron `contextBridge` / Tauri capabilities) that pops
  `GtkFileChooserNative` / `NSOpenPanel` / the WebView2 picker and returns
  path+bytes. The vendored `webview/webview` provides no dialog — we implement +
  bind our own. The TUI satisfies the same verb with its inline picker.

## Decision 4 — modular layout is a constrained docking model, contributed as data

The mainstream chose constrained docking over free-form tiling. Named region
slots + content units declared **as data** (`{ id, title, icon, target_region,
when }`), each implementing a thin renderable contract, drag-to-move between
slots, layout **persisted per client**. Reference designs: VS Code
(viewsContainers/views + primary/secondary side bar + a *separate* splittable
editor grid) and Zed (a `Panel` trait + three docks). Our `region`/`tabs`/`popup`
hints are already the named-slot vocabulary; we formalize panes as contributed
data and keep the editor's splittable grid separate from the dock/panel system.

## Decision 5 — the attach handshake + intersection capability negotiation (forward-looking)

Adopt Neovim's model explicitly so the multi-client / presence north star stays
tractable: an **attach handshake** where each client declares capabilities; the
`uinode` tree is the universal fallback every client can render; each hint/role
is an `ext_*`-style externalization a capable client opts into rendering
natively. When multiple heterogeneous clients attach at once (TUI + webview, or
a presence observer), use Neovim's **intersection** rule — active capabilities
are the intersection of what clients requested — so a low-capability observer
never forces the rich client down to text. **The core always emits structure,
never pixels**, so a fourth client (MCP, a read-only presence viewer) is just
another renderer. This decision is *adopted as principle now, built later*: the
local milestone below just has to not violate it.

## Proposed slice decomposition

Foundation and native chrome first; the remote transport / multi-user is a
SEPARATE later arc (we only adopt its abstractions now so nothing has to be
undone).

- **S0 — resize-fill (#3).** The window must actually be an app window before
  chrome makes sense. Root-cause the window→ResizeObserver→viewport chain
  (systematic-debugging; reducer + GUI stage). Small.
- **S1 — command/menu/action as data.** Consolidate the existing menus/palettes
  (`^K`, `^T` options, `^N` modes, `^B` build) onto the VS Code contribution
  shape + one action vocabulary. TUI rendering unchanged (pure model
  consolidation, gated on testmadcide byte-identity). This is the spine.
- **S2 — native menu bar.** `madcwebview` native menu API (GTK first, then
  Win32/Cocoa); the web client renders the top-level menu data as a native menu
  bar; selections post action events back. TUI unchanged.
- **S3 — status bar as chrome.** Render the status region as real app chrome
  (DOM, but proper chrome — not a text line); left/right item model already
  exists from slice 3.
- **S4 — native file dialogs + the two-sided foundation.** The bound native
  client dialog verb (GtkFileChooserNative/NSOpenPanel/WebView2); open/save
  local files; introduce the URI scheme/authority and the server-side composed
  file-browser pane. (Client-server transport itself stays out — the server
  authority resolves locally for now, but through the scheme'd abstraction.)
- **S5 — panes as contributed data (docking).** The outline, diagnostics, and
  split windows as real panes in the docking model; per-client persisted layout.

Milestone boundary question for the owner: **is "the feature" (the merge wave)
S0–S5 — a locally-complete "madcide looks and works like a real app" — with the
remote transport / multi-user / presence as the NEXT arc?** Or is the boundary
drawn tighter/wider? The battery runs once, at whatever line is drawn.

## Recon sources

Full citations in the two recon syntheses (session 2026-09-07). Anchors: VS Code
[contribution points](https://code.visualstudio.com/api/references/contribution-points) ·
[when-clauses](https://code.visualstudio.com/api/references/when-clause-contexts) ·
[custom layout](https://code.visualstudio.com/docs/configure/custom-layout) ·
[remote extensions](https://code.visualstudio.com/api/advanced-topics/remote-extensions) ·
[vscode#109345 remote vs native picker](https://github.com/microsoft/vscode/issues/109345) ·
Neovim [ui.txt](https://neo.vimhelp.org/ui.txt.html) /
[api-ui-events](https://neovim.io/doc/user/api-ui-events/) ·
[Emacs server](https://www.gnu.org/software/emacs/manual/html_node/emacs/Emacs-Server.html) ·
Zed [panel system](https://zed.dev/blog/new-panel-system) /
[actions](https://deepwiki.com/zed-industries/zed/3.3-actions-and-keybindings) /
[native menu #914](https://github.com/zed-industries/zed/issues/914) ·
[Chrome File System Access API](https://developer.chrome.com/docs/capabilities/web-apis/file-system-access) ·
[Tauri dialog](https://v2.tauri.app/plugin/dialog/) ·
[GtkFileChooserNative](https://docs.gtk.org/gtk3/class.FileChooserNative.html) ·
[LSP 3.17 DocumentUri](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/).
