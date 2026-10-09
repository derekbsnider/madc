# madc vs madcide — the capability inventory (where every line falls)

Companion to `2026-10-09-madc-madcide-split-arc.md` (ROADMAP 8.10). Owner
(2026-10-09): "what we really need is an inventory about what currently
completely exists within madc itself vs. implemented in madcide so that we
know where all the lines are". Four read-only sweeps (2026-10-09, develop
after `044baa5ae`): madc's own surface; `madcide_core.inc` +
`editor_events.inc`; madcide's services (LSP, MCP, seats, serve); madcide's
feature files (registry, plugins, REPL pane, git, markdown, help, lang,
recent, profiles, default bundle).

## The rules the placement applies

1. **Hard line = UI level.** NONE and LINE are madc's; TUI and GUI are
   madcide's.
2. **Build and projects are madc's**, with every `:` / `%` command that
   drives them.
3. **Commands are extensible**: a module or madcide registers into madc's
   registry; the registry itself is madc's.
4. **Fine lines** (non-UI capabilities that exist only in madcide) are placed
   by what state they depend on — listed below for owner decisions.

**Clarification the inventory forces on rule 1.** madc already contains full
TUI and GUI *renderers*: the `ui::` engine (`tui_model.h` 2112 lines,
`web_model.h` 1375 lines, `page.js` / `page.css`, `ui_term.cpp`, the
`madcwebview` module, `<ns_ui_ws>`). They are a LIBRARY any madc program
uses — the engine ships no application (`check-engine-app-purity.sh`). The
hard line therefore applies to madc's own interactive APPLICATION (`madc -i`
and its ex core stop at LINE), not to the `ui::` library, which stays in madc
at every level. madcide is an application on that library; Chthonia is a
product on madcide.

## What madc already has (no move needed)

- **CLI:** compile/JIT, AOT (`-c -o -shared -r`), `.o` run/link, `--project`
  (the SAME `<base>.prj.json` madcide writes — `src/madc_project.cpp`;
  object form `tus/entry/output/kind/icon/libs/madc/std/stdlib/defines/
  include_dirs`, or a compile_commands.json array), `--emit=`, `-E`, forest
  flags, `--capabilities`, `madc.ini` (std, stdlib, forest, include,
  cpu-limit, mem-limit).
- **REPL** (`-i`): session core with transactional entries, the 11 `%`
  commands + aliases (`madc_session.cpp:58-99`), payloads, `%whos`,
  completion, `line_edit.h` (emacs keys, ^R, multi-line), history file per
  standard, D8 interrupt, crash-isolated backend (POSIX fork; Windows
  in-process).
- **Script APIs with no CLI/REPL face:** parse handles (`parse_open/refresh/
  check/outline/enclosing/spans`), diagnostics as rows, code graph L1–L3 +
  revision handles, `parse_build` / `parse_run` (live tree), `project_open/
  tus/build/run`, `build_native`, `library_*` / `code_*` (plugin loading),
  `fork_uri`, tasks/channels (`frame_headers` = LSP framing, `serve_web`,
  WebSocket, `conn_*`), the 20 `session_*` verbs, the whole `ui::` engine,
  `git::` / `markdown::` modules.

## The inventory

Lowest UI level the capability inherently needs; "→" = the proposed side.

### A. Editing core (today `editor_events.inc` + `madcide_core.inc`)

| Capability | Lives now | Level | → |
|---|---|---|---|
| Line addressing (offset/line/column, phantom last line) | E:43-75 etc. | NONE | **madc** — ex addresses |
| Text mutation owner `ed_text_insert/erase` (splice + anchors + journal) | E:722-739 | NONE | **split**: the splice + edit notification → engine `text_buffer`; anchors and journal subscribe |
| Anchor registry (shifted marks/carets) | E | NONE | **madc** (engine) — ex marks `'a` need them; madcide's carets use the same |
| Change log (`clog_*`, `<base>.prj.events`) | E, C:5127-5170 | NONE | madcide — it is the Nexus event stream (MCP), a subscriber |
| Undo / redo | engine `text_buffer` (+ C checkpoints) | NONE | **madc** (already engine) |
| Line-wise edits, block copy/move/delete | C | NONE | **madc** — ex `:d :t :m` over ranges |
| Find / replace walk (`:s///c`-like) | C | LINE | **madc** — ex `:s :g :v /re/` on PCRE2 |
| Insert file (`:r`) | C `colon_command` | LINE | **madc** |
| Buffer ring (`:e :b`), save / save-as / close / quit | C + lined verbs `w q q! c` | LINE | **madc** — ex `:e :ls :b :w :q :x`; lined retires |
| `:N`, `!cmd` | `colon_command` C:7733-7837 | LINE | **madc** — `:!cmd` runs a shell; the pty Terminal pane stays madcide |
| `view*` colon verbs (8) | `colon_command` | TUI/GUI | madcide — registered into madc's registry |
| Clipboard core | C | NONE→GUI | madcide (system clipboard is a window capability) |
| Caret/selection editing, vi grammar, motions, modal scopes | C, enums | TUI | madcide |

### B. Command registries

| Capability | Lives now | Level | → |
|---|---|---|---|
| Session command table (11 + aliases) | `madc_session.cpp:58` | LINE | **madc** (stays the one owner) |
| Script-visible registration (`%name` → handler) | ONLY madcide: `plugin_repl_command` `plugins.inc:858-893`, intercept `repl.inc:573-589`, `%help` addendum `repl.inc:545-566` | LINE | **madc** — the missing extension point; `madc -i` and `BackendSession::honor` have no hook today |
| IDE command table (141 names) + contributed codes ≥4096 | `madcide_enums.inc:31-176`, `plugins.inc:670-923` | mixed | madcide — its LINE-level rows (below) become madc commands; the rest stay |
| Engine `verb_table` (zero built-ins) | `include/madcdis/verbs.h:188` | NONE | madc (unchanged) |

madcide's 141 commands by level: **LINE-capable and → madc** — colon (ex),
`shell`, `check`, `build`, `progargs`, the `bld-*` builds/runs (not the
terminal mode), the project manifest ops, `w`/`q`, undo/redo, and the
argument forms of `editfile`, `insertfile`, `save`, `saveas`, `close`,
`quit`, `bufsel`, `gotoline`, `find`, `replace`, `language`. **TUI/GUI →
madcide** — every no-argument picker or prompt, caret editing, motions,
vi grammar, windows/panes/views, menus, fonts/fullscreen, themes, key
styles, profiles, help panes, markdown preview, choice rows.

### C. Check, build, run, projects (owner rule 2)

| Capability | Lives now | Level | → |
|---|---|---|---|
| Check a buffer (diagnostics rows) | madc `parse_check`; madcide `check` cmd | NONE | **madc** — a `:check` / `%check` command (madcide documents `:check` at `madcide_line.inc:24` but never handled it) |
| Build from the live buffer | madcide `start_native_build` (C:6730) over `parse_build` / `build_native` | NONE | **madc** — `%build` gains a buffer source (today it takes a FILE); one build path |
| Run from the live buffer (+ `progargs`) | madcide F5 `replrun` (`repl.inc:803-835`) | NONE | **madc** — `%run` on a buffer, program args at the prompt |
| Project manifest read/write, add/del TU | madcide C:5011, 5242-5260, 5470-5482 | NONE | **madc** — madc already reads the format; writing moves with it (`%project`) |
| Project check / build / run (`bld-proj-*`, `proj_check_rows` C:7224) | madcide over `project_*` | NONE | **madc** |
| Build-row parsing into Problems (`build-<n>`) | madcide | TUI/GUI view | madcide (presents madc's rows) |
| `.prj.layout` sidecar | madcide | TUI/GUI | madcide |
| `--build-plugin DIR` | madcide (`plugin_code.inc:534-584`) | NONE | madcide face over `madc -shared` (already madc) |

### D. REPL

| Capability | Lives now | Level | → |
|---|---|---|---|
| Session, prompt, offer/complete, interrupt/EOF, payloads | madc (CLI) and duplicated in madcide's pane | LINE | **madc** (pane keeps only rendering) |
| History | madc file per std; madcide in-memory copy | LINE | **madc** (decided: `%history`, one store) |
| Completion insert rule | `line_edit::completed` vs `repl_completed` | LINE | **madc** (decided) |
| `%std`, `%reset` | nowhere; madcide restarts on language change | LINE | **madc** (decided) |
| Pane transcript, input field, @repl keys, Clear, Problems feed, Symbols | madcide | TUI/GUI | madcide |
| REPL events to plugins (reTAKEN/reRAN/…) | madcide | NONE | madcide — reads madc's reply rows |

### E. Language and configuration

| Capability | Lives now | Level | → |
|---|---|---|---|
| File-kind family predicates | `madcide_lang.inc:35-67` | NONE | **madc** — beside `src/file_kinds.cpp` |
| Per-language editing tables (comment opener, block delimiters, default indent) + indent detection | `madcide_lang.inc:71-218` | NONE | **madc** (file-kind attributes; the ex core's `>`/`<` and `:set` use them) |
| Session standard + change event | madcide world entity | NONE | **madc** = `%std` (decided) |
| Per-file-FAMILY default standard (`.c` → C17, `.cpp` → C++17) | madcide only | NONE | **owner decision F5** |
| settings.json (layered user/bundle/default) vs madc.ini | both | NONE | **owner decision F6** |
| Recent files (`recent.json`) | madcide | NONE | madcide (a window/terminal convenience) |
| Config dir / product name / profiles / bundles / keys / themes / menus / layout / status | madcide | TUI/GUI data | madcide |

### F. Git, markdown, help

| Capability | Lives now | Level | → |
|---|---|---|---|
| `%git log/show/blame/status/diff` + date/status formatting | `madcide_git.inc:93-613` | LINE | **madcgit module** (decided) |
| Changes / History / Blame views, `%G` status seat | `madcide_git.inc:618-781`, `madcide_repo.inc` | TUI/GUI | madcide |
| Markdown tree → plain text rows | `madcide_markdown.inc:73-249` | LINE | **madcmark module** (pure over `markdown::`) |
| Highlight spans, concealing lens, lens edit repair, preview | `madcide_markdown.inc:262-742`, `madcide_preview.inc` | TUI/GUI | madcide |
| Help browser (bundles, index, generated menus topic) | `madcide_help.inc` | TUI/GUI | madcide; madc's `%help` lists the one registry (built-ins + registered) |

### G. Plugins

| Capability | Lives now | Level | → |
|---|---|---|---|
| Code loading (library / source), activation symbol check | madcide over madc's `library_*` / `code_*` | NONE | madc already owns the primitives; madcide keeps its manifest |
| Manifest, search path, bundles, `contributes` (keys/layout/menu/theme/status/views) | `madcide_plugins.inc` | NONE / TUI-GUI data | madcide |
| Host child-process transport, seat tiers | `madcide_plugin_host.inc` | NONE | madcide |
| Session-command registration (`ide_api.repl_command`) | madcide | LINE | **madc** (B above) |

### H. Services — the fine lines (DECIDED 2026-10-09; see the arc plan's Phase 1C)

| Capability | Lives now | Level | Depended on (madcide-side) | → decided |
|---|---|---|---|---|
| LSP face `madcide --lsp` (1098 lines) | `madcide_lsp.inc` | NONE | document store | **madc core**: `madc --lsp` (C++ port) over the document component; madcide calls the language-service functions directly, never through the LSP |
| MCP protocol (JSON-RPC server, tiers) + MCP client (`madcide_mcpclient.inc`) | madcide | NONE | registry + tiers | **a madc module** (the MCP protocol module; per-tool tier is its data) |
| graph.* reads 24 + insert/replace/delete | `madcide_mcp.inc` | NONE | document store, undo, change log | **madc core** beside the LSP, over the one set of language-service functions and the document's undo |
| graph.* PAST 6 (`madcide_past.inc`) | madcide | NONE | git, change log | **the madcgit module**, registered when loaded |
| nexus.* 11, test.* 5, proposals, layers, the change log | madcide + `editor_events.inc` | NONE | change-log stream | **the project-memory module**: a subscriber to the edit notification, saved beside the project |
| discovery / advertisement (`madcide_discover.inc`, 345) | madcide | NONE | es `buffers` | **a madc module** (two consumers: the headless MCP and madcide); one owner per project |
| `madc --mcp --project X` | — | NONE | — | **madc**: the headless MCP server for a project, solely in madc |
| api seat, ws window seat, `--serve`, `--attach`, `-c` NONE client | `madcide_api/seat/serve/ws/once.inc` (~1,350) | NONE / GUI | IdeSession + registry | **madcide** (remote control of a running madcide session; its foundation `serve_web` / WebSocket / `conn_*` / `<ns_ui_ws>` is already madc's) |

## The three pieces of madcide-side state behind every fine line — all three resolved to madc

Every NONE-level service above is thin over madc APIs (`madc::parse_*`,
`graph_*`, `channel`, `frame_headers`, `serve_web`). What kept them in
madcide was three pieces of state, and the inventory round placed each:

1. **The document store** → madc's document component (Phase 1A.1).
2. **The change-log stream** → the project-memory module (Phase 1C.15), a
   subscriber to the document's edit notification.
3. **The command registry** → madc's (Phase 1A.2); IdeSession and the seats
   stay madcide's — the only service state that does.

## Fine-line decisions (owner, 2026-10-09)

- **F1. LSP** — madc's core: `madc --lsp` ("small enough"); madcide calls the
  same language-service functions directly (it holds no copy of madc's
  buffers; LSP's editor-owns-the-text model would invert ownership and add
  an async step per keystroke — the xi-editor mistake).
- **F2. MCP** — layered madc modules + core tools; a headless MCP server for
  a madc project exists solely in madc (`madc --mcp --project`).
- **F3. Seats / serve / attach** — madcide's ("unless it could be a reusable
  module beyond it"; its foundation already is). Discovery — a madc module.
- **F4. `madcide --line FILE`** — kept, = `madc -i FILE`.
- **F5. Standard by extension** — `--std` > extension > madc.ini/env `std` >
  madc; `.c` → C17 until C23 (51.4% measured, ROADMAP 2.13) nears C17's level.
- **F6. Config** — madc.ini owns the semantic keys (`std`, `modules`
  php.ini-style, `theme`); settings.json keeps UI preferences.
- **The module principle** (owner): modules are madc-language libraries any
  program can use; madcide is one consumer.

## Found on the way (Phase 1 fixes, each with a reducer)

- `:check` documented in `madcide_line.inc:24`, not handled — resolved by C's
  `%check` landing in madc.
- `--capabilities` `ui.levels` = `["tui","web"]` omits `"line"`, though the
  LINE frontend exists (`src/ns_ui.cpp:350`); `"c++"` is an emit target in the
  JSON but missing from `--help`.
- Stale comments: `ns_ui:381` ("arrives with slice V2.5"), `ns_madc:757-759`
  (Windows "no fork yet" — `session_open` runs a child over loopback TCP).
- Two `clog_head` definitions of different arity (`madcide_core.inc:4247`,
  `editor_events.inc:585`) — checked when the journal becomes a subscriber.
- madcide's REPL history is in-memory only (already covered by `%history`).
