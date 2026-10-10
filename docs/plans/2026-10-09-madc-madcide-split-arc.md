# The split arc — madc's interactive mode takes the ex core, madcide leaves

Owner (2026-10-09): the next arc decides the clean point between madc and
madcide. Part of madcide comes into madc's interactive mode; everything else
becomes a completely separate repository. This plan coordinates the plans that
feed the arc and fixes the boundary from a recon of the code (2026-10-09, HEAD
`3af2f7db0`, three read-only sweeps: the ex core and line mode, the REPL
commands by owner, the tests/gates/packaging/Chthonia inventory).

The plans it coordinates:

| Plan | Role in this arc |
|---|---|
| `madc-repl-thonny-plan-2026-09-24.md` D24 (+ its Layering bullet) | the ex core and the one command registry: the "into madc" half |
| `2026-10-08-madcide-own-repo.md` | the repository move: the "out of madc" half (its inventory is corrected below) |
| `2026-10-08-ide-plugins.md` | AFTER this arc, in the madcide repository |
| `2026-10-08-madcide-live-colour.md` | DONE — shipped in v0.103.0 (the own-repo plan's predecessor) |
| next-round list (Symbols, Outline kinds, GUI gutter + toggle, two C++14 defects) | placed into the phases below |

## The principle (owner, 2026-10-09)

madc on its own holds the ed/ex editing core and everything inherently part of
madc; madcide adds the visual layer, as vi adds visual mode to ex, without
being limited to vi. The REPL is both a C/C++ prompt and a line editor a
vi/vim user can work in. Prompt line editing stays bash/emacs by default.

The lines (owner, 2026-10-09):

- **The hard line is the UI level.** CLI-only and line mode are madc's; TUI
  and GUI are madcide's. The line used to sit between CLI-only and line
  mode; the interactive madc mode moved it to between line mode and TUI.
- **Fine lines** divide non-UI capabilities that exist only in madcide today
  (LSP, MCP, and others) — placed by the inventory, not by where the code
  happens to live.
- **The line-mode commands are extensible:** a module, or madcide itself,
  registers `:` / `%` commands into madc's registry.
- **Build and projects are madc's.** madc builds itself and supports
  multi-file projects (`--project`), so that functionality lives on the madc
  side, and every `:` / `%` command that drives it stays on the madc side
  too. madcide presents it (menus, the Problems pane, project views).

**The inventory** — every capability, its UI level, what it depends on and
its proposed side, plus the fine-line decisions F1–F6 — is
`2026-10-09-madc-madcide-inventory.md`.

## What the recon found

1. **madc already owns the REPL commands.** Every `%` / `.` / `:` / `?`
   command that exists works at the plain `madc -i` prompt from
   `src/madc_session.cpp` (`%help %type %pinfo %whos %load %run %build %call
   %open %edit %quit`, aliases at :93-99) — except `%git`, which is madcide
   dialect code (`tools/madcide/madcide_git.inc`) registered through
   `plugin_repl_command`. Not built anywhere yet: `%history`, `%std`,
   `%reset`, D24's wider recognition rule, and every ex buffer command.
2. **Three command registries, not one:** madc's session `command_rows`
   (`madc_session.cpp:58`, a static C++ array, enum codes); the engine's
   per-ui-session `verb_table` (`include/madcdis/verbs.h:188`, interned-name
   lookup, ZERO built-in verbs by Rule #7); madcide's `cmd_table`
   (`madcide_enums.inc:31`, ~141 rows). D24's "one registry" has no
   script-visible extension point yet (plan :2730) — the prerequisite for
   moving anything.
3. **lined's ed verbs bypass the one mutation path.** `tools/texteditor/
   verbs/{c,i,a,d}.madv` call `ui::text_*` directly — no anchor shift, no
   change-log record, no undo checkpoint. madcide dispatches only `w`, `q`,
   `q!` from that set (and reads `c`'s availability for read-only). The
   arguments are bare numbers: no `.`/`$`/`%`/`/re/`, no `s///`, no undo.
4. **The one text-mutation owner is madcide-side.** `ed_text_insert` /
   `ed_text_erase` (`tools/texteditor/editor_events.inc:722-739`) update the
   text, every client's anchors and the change journal (`clog_*`). The engine's
   `text_buffer` (`include/madcdis/text_buffer.h`, a piece table with
   insert/erase/replace, lines, literal `find`, checkpoint/undo/redo) knows
   nothing of anchors or journals, and has no regex and no marks.
5. **`colon_command` (`madcide_core.inc:7733-7837`) handles 15 forms:**
   editing/file — `:N`, `w`, `q`, `q!`, `wq`/`x`, `e FILE`, `r FILE` (and `!cmd`,
   whose IMPLEMENTATION is the IDE's pty Terminal); IDE — `viewsplit`,
   `viewfocus`, `viewclose`, `viewopen`, `viewdock`, `viewsize`, `viewwindow`,
   `viewsync`. `:check`, which `madcide_line.inc:24` documents, is not handled.
   The REPL pane cannot reach `colon_command` (`:w` there goes to the session
   and is refused).
6. **Line mode** (`madcide --line`, `madcide_line.inc`, 57 lines) is the full
   IDE composer printed whole every cycle through the engine's
   `ui_line_frontend` (`src/ns_ui.cpp:350-409`), with typed text inserted at
   the caret — no addressing. It is what the owner found "doesn't really work".
7. **Duplicated between madc and madcide:** the REPL history store (madc: file
   per standard, `madc_repl.cpp:52-161`; madcide: in memory,
   `madcide_repl.inc:124-200`) and the completion insert rule
   (`line_edit::completed` vs `repl_completed`).
8. **The engine carries no madcide code path** — only comments (and two
   strings: `--project` help text, a channel-cookie comment).
   `check-engine-app-purity.sh` already gates it.

## The boundary

**Into madc (interactive mode and the engine):**

- **One command registry with a script extension point** — `madc_session.cpp`'s
  table stays the one owner; a host (madcide, a plugin, a plain-prompt
  module) registers further names into it, resolved to an enum/code once at
  input (enum-over-strings). D24's recognition rule (any leading `. % : ?`
  except `.`-digit, `::`, `%:`) replaces `command_text`'s narrow rule, and
  the prefixes are MODES (owner 2026-10-09): `%` and `.` the REPL's
  commands, `?` the query, `:` ed/ex/vim commands ONLY — colon mode, the
  host's; one mode's command may alias another mode's.
- **The document component — file buffers are madc's** (owner 2026-10-09;
  Phase 1A.1). A `text_buffer` plus path / kind / modified / read-only /
  load-save, its parse handle, anchors and marks, a version, and the EDIT
  NOTIFICATION: one change record (a byte-precise range replacement, plus a
  reload event) that every observer consumes; madcide's anchor shift and
  change journal subscribe to it instead of being the mutation path. Without
  this, an ex edit made through madc's core would skip madcide's anchors and
  journal — the exact defect lined's verbs have today. This is the deepest
  layer for "one mutation path" (Rule #2). The documents live in the process
  that edits, never the one that runs user code.
- **The LSP** (`madc --lsp`), **the MCP as layered modules + the code-graph
  tools in core, the project-memory module, discovery, and `madc --mcp
  --project`** — Phase 1C; decided below.
- **The ex core** on `text_buffer`: buffers (`:e`, `:ls`, `:b N`, `:bn`,
  `:bp`), addresses and ranges, the ed core, the ex/vim additions, `:set`, and
  `u`/`redo` on `text_buffer`'s history — D24's list, unchanged. The regex
  engine is a design decision of its own (below).
- **`%history` (one store — madc's file per standard; madcide's pane displays
  it), `%std`, `%reset`; the completion insert rule's one owner
  (`line_edit::completed`).**
- **`%git` — a MODULE's command, never a built-in** (owner 2026-10-09: "%git
  depends on the git module, so it would only be present if perhaps the
  madc.ini or a command line option indicated to load that module"). The
  madcgit module registers `%git` into the session registry through the
  extension point when it is loaded; without it, `%git` is an unknown command
  like any other. Loading it: the existing `-lmadcgit` on the command line, a
  new repeatable madc.ini key naming modules to load (the schema in
  `include/madc_config.h` grows one key; precedence CLI > environment > ini as
  for the others), and an `import madcgit;` entry at the (owner 2026-10-09: yes)
  prompt. The verb bodies (`git_log_text`, `git_show_text`, `git_blame_text`,
  `git_status_text`, `git_diff_text`, `git_relative_date`) depend on no view;
  they move into the module's side over the madcgit API, with live text
  supplied by the host as `%load` already does. madcide's Changes / History /
  Blame VIEWS stay in madcide and load the module themselves. The same path
  serves any module that brings session commands.
- **Line mode** = `madc -i` with a buffer: the ex core at the prompt.
  `madcide --line` keeps no editing logic (it becomes that, or is retired —
  owner).
- **Engine twins for engine subjects tested only through madcide** (before
  anything moves): `ui::LINE` / `ui_line_frontend` (testmadcide_line),
  `madc::channel::serve_web` (testmadcide_serve_web), `ui::term_feed`
  (testmadcide_termcolour), `ui::key_sequence_for` (testmadcide_hint,
  testmadcide_emacs_keys); `tests/gui/madcide_theme.mad` and
  `madcide_workbench.mad` are already engine tests (no tools/ include) —
  renamed `ui_web_*`; `tests/testbuildcancel.mad` stops compiling
  `tests/testmadcide.mad`; lined's R4 and vised's R5 engine contracts are
  carried by the rewritten `examples/vised/`'s test (below), checked against
  `test_verbs` / `test_interaction` / `test_text_buffer` / `testeditcheck`
  for anything it does not reach.

**Moves to the madcide repository:** everything else the own-repo plan lists,
with the corrections below — `tools/madcide/` (56 files, ~28.9k lines),
`tools/texteditor/editor_events.inc` (madcide's editor core), `tools/vscode-madcide/`,
the IDE half of `colon_command` (the `view*` verbs, registered into madc's
registry), the views (Symbols, Changes/History/Blame, Help, Outline view,
Markdown preview), and the seats (the api seat, the ws window, `--serve`,
`--attach`, `-c`, permission tiers over madcide's commands) — NOT the LSP,
the MCP, the project memory or discovery, which Phase 1C moves into madc
(decided below; madcide loads them).

**Retired: lined** (owner 2026-10-09, "completely deprecated") —
`tools/texteditor/lined.mad`, `run_session`, its ed verbs
(`tools/texteditor/verbs/*.madv`, `checks/editable.madv`) and
`tests/testlineed.mad`, superseded by madc's ex core. madcide's `w`/`q`/`q!`
and its read-only verdict call the registry instead; `lined_core.inc`'s
document helpers (`doc_set_path`, `doc_kind`, `doc_untitled`, `doc_name`,
`new_document`, `setup_document`) become the document component's operations
in madc (Phase 1A.1 — corrected 2026-10-09; they were to move into madcide),
and `resolve_data_dir` moves into madcide.

**Stays in madc as an example, rewritten: vised** (owner 2026-10-09: keep it
as a simple example). `examples/vised/` — a self-contained visual editor on
madc's PUBLIC surface only: `ui::` (one tree, the terminal UI and `--web`),
`ui::text_*` (the buffer and its undo), the command registry for `w`/`q`. It
includes nothing from madcide (today's 197-line `vised_core.inc` leans on
`editor_events.inc`'s key/pointer handlers and composers, which move with
madcide; keeping `editor_events.inc` in madc for it would make madc ship
application code). Its test is the engine-level check of the R4/R5 contracts
lined's retirement leaves uncovered: edits through the one mutation path,
read-only refusing an edit, save/quit through the registry. Built in Phase 1D
after the registry and the edit notification, so it demonstrates them.
`tools/texteditor/vised.mad`, `vised_core.inc` and `tests/testvised.mad`
are replaced by it.

## Corrections to the own-repo plan's inventory

- Tests: 96 test programs (63 `testmadcide*`, 8 `testide*`, 17 + 2
  `tests/gui/madcide_*`, 7 others), not 93; plus `tests/tui_golden/` (37
  files) and `tests/mcp_fixture_server.mad`. testdatadir_install_first,
  testgraphpast, testmcpclient, testnexus_layers, testnexus_records are
  madcide behaviour (they move); testlineed and testvised are the R4/R5 gates
  (twin check above).
- Gates: 29 scripts move (the seven `check-madcide-*`, the anchor / blank-run
  / selection / subject-doc / event-builder / madcide-staging owners,
  `check-dialect-literals` — its scope is `tools/` only — the quit/save pty
  gates, `tui_golden*`, `tui_win_golden.py`, `conpty_host.c`, `vtscreen.py`,
  `tui_scroll_*`, `stage_madcide_data.sh`, `build_shipped_plugins.sh`,
  `install_gate_pty.py`). `check-dialect-lean` is ENGINE-only (include/madc)
  and stays. `chthonia_sanitize_check.sh` belongs in the Chthonia repository.
  Spanning both: `check-one-style-vocabulary`, `-library-spelling`,
  `-git-owner`, `gui_lane.sh` (it also runs the engine's `ui_web_*` /
  `webview*`), `run_tests.sh`'s MADCIDE_* hermeticity, `win_run.sh`,
  `win_suite.sh`, `remote_build.sh`, `lane_ledger.sh`'s APPLICATION set.
- Build: `src/Makefile` has NO madcide target — packaging is the only builder
  (`package_release*.sh`, `stage_install.sh`, `package_install_gate.sh`, the
  Homebrew `madc.rb.in`, the AppArmor heredoc in `package_release.sh`). There
  is no desktop entry in madc (Chthonia ships its own).
- Chthonia pins `"madc": "0.102.1"` with `"libs": ["madcide"]`, and uses
  madcide's headers (`<madcide/product>`, `<madcide/plugin>`,
  `<madcide/harness>`), libmadcide, `madcide --build-plugin`, madcide's
  `chthonia.keys` and `vscode.theme` profiles, MADCIDE_CONFIG_DIR /
  MADCIDE_PLUGIN_PATH. Its unpushed `dark-plus` branch (Dark+, line numbers,
  terminal layout) is Chthonia 0.0.4 on madc 0.103.0 (madcide inside it).

## Phases (reordered 2026-10-09, after the inventory round — dependency order)

The critical path is **1A.1, the document component**: every slice of 1B,
1C and 1D stands on it, and 1A.2 (the registry) is second. 1B (the prompt)
and 1C (the headless services) are independent of each other and run in
parallel once 1A is in. 1D (madcide re-plumbed) is last. Phase 0's parser
items are their own sessions, any time.

**Phase 0 — independent, any order:**
1. Chthonia 0.0.4 on madc 0.103.0 (the `dark-plus` branch; madcide still inside madc).
2. The two C++14 defects (`cpp1y/lambda-generic-ice2.C` abort,
   `cpp1y/var-templ24.C` > 30 s) — a parser session.
3. The Outline's kinds beyond functions (`madc::parse_outline`) — a parser
   session.
4. The GUI gutter in `page.js` / `page.css` (engine), with its gui test.
5. **Colour only when colour is available** (owner 2026-10-09) — a defect,
   its own commit with its test: `Program::print_diagnostic`
   (`src/parser.cpp:25140`) emits ANSI escapes unconditionally, so
   `madc file.c 2> log` writes them into the file. Rule, the gcc/clang one
   (`-fdiagnostics-color=auto`): colour only on a terminal, never when
   `NO_COLOR` is set; ONE predicate every coloured output asks (1B.9 reuses it).

**Phase 1 — into madc (one repository still). Four slices:**

*1A — the engine foundation (sequential; nothing else starts before 1A.1–1A.2):*
1. **The document component** (owner 2026-10-09: file buffers are madc's) — **LANDED 2026-10-09, `56f701aa2` on `feature/document-component-claude`** (design doc `2026-10-09-document-component-design.md`; unit battery + fix lane green; not pushed).
   On the hub beside `text_buffer` (`madcdis/text_buffer.h`), generic to any
   program that edits text — engine purity holds as it did for the text
   component. One document = the text + undo (already per document), path,
   file kind (stamped once at open / new / save-as), modified, read-only,
   load / save, its parse handle (refreshed lazily — today madcide's
   `ensure_phandle`), anchors / marks (shifted on every edit — today
   `editor_events.inc`'s registry), a version counter, and the EDIT
   NOTIFICATION. Rules from the practice survey (`2026-10-09-madc-madcide-
   inventory.md`): ONE authoritative copy per document — every other copy
   (the forked backend's input, an LSP client, a mirror) is keyed by the
   version and resyncs when it falls behind (clangd's DraftStore); the
   document owns the undo GROUPS, the command layer sets the boundaries
   (one ex command = one group; a key sequence = one group); the change
   event is a byte-precise range replacement (start row/col/byte, old end,
   new end, version) plus a separate whole-buffer RELOAD event, applied in
   order, delivered synchronously with mutation refused inside the callback
   (Neovim's textlock) — line-granular events are derived. Lives in the
   process that EDITS, never the one that runs user code (`madc -i`: the
   prompt process; madcide: its own, the same code). Supersedes the 08-24
   ruling "document properties are application bag keys"; lined_core's
   document helpers (`doc_set_path`, `doc_kind`, `doc_untitled`, `doc_name`,
   `new_document`, `setup_document`) become this component's operations
   (NOT madcide's — a correction to the retirement paragraph above). A
   buffer ring is a per-world entity set, not a global.
2. **The command registry's extension point** — **LANDED 2026-10-10,
   `886c7fa09`** (unit battery + fix lane green; not pushed) — + D24
   recognition with the modal prefixes (owner 2026-10-09: `:` is colon mode only; `%` `.` `?`
   the REPL's; cross-mode aliases) — design
   `2026-10-09-command-registry-design.md`. `madc_session.cpp`'s table stays
   the one owner of the REPL commands; a module (in the session) or a host
   (by payload) registers names, resolved to a code once at input; `%help`
   lists the one registry. The extension point exists today ONLY inside
   madcide's REPL pane (`plugin_repl_command`) — `madc -i` has no hook.
3. **PCRE2 into libmadc** — **LANDED 2026-10-10, `461773c96` (the engine
   `madc::regex` + build / link / packaging) and `2f44803b8` (the five script
   functions on it; gate `check-one-regex-engine.sh`)** (unit battery + fix
   lane green; Windows + macOS release builds and packages green; not pushed)
   (decided, below; design
   `2026-10-10-pcre2-design.md`) + the five regex functions'
   fixes (invalid pattern refused; `madc::` three leave `ns_perl.cpp`; lean
   mangled-direct forms) — own commits with reducers.
4. **The standard by extension**: `--std` > extension > madc.ini/env `std`
   > madc; `.c` → C17, `.cpp`/`.cc`/`.cxx` → C++17, `.mad` → madc; and
   **madc.ini's new keys**: `modules` (repeatable, php.ini-style; the
   `%git` enabler) and `theme`. The ini schema in `include/madc_config.h`.
5. **Windows `madc -i` gets the crash-isolated backend** (owner 2026-10-09:
   make Windows work like Linux and macOS). The child-of-self backend
   already exists (`SessionClient::spawn_backend`,
   `src/madc_session_client.cpp:611`, used by `madc::session_open`); only
   `madc -i` stays in-process (`src/madc.cpp:545`), because its configured
   Program cannot cross to the child (the `make_program` guard). Fix: the
   child receives the CONFIGURATION (the `--std`, `-I`, `-D`, `-l`,
   `--config` / `--no-config` arguments, never a mode or output flag) and
   builds the identical Program itself, on every restart; the in-process
   branch is deleted. INTERACTIVE ONLY (owner): the backend is reached only
   from `run_repl` and `session_open`; running a file, `-c`, `-o`,
   `--project` and `--emit` never start one. A Cygwin-style fork was weighed
   and refused (memory-copy emulation, fragile with JIT code, MIR and
   WebView2 DLLs). Test: a crashing entry under Windows `madc -i` restarts
   the session; a plain `madc file.mad` starts no child.

*1B — the interactive core at the prompt (after 1A; parallel with 1C):*
6. **The ex core and line mode**: buffers (`:e`, `:ls`, `:b N`, `:bn`,
   `:bp`), addresses and ranges, the ed core, the ex/vim additions (`:s`,
   `:g`, `:v`, `/re/`, `?re?` on PCRE2), `:r`, `:!cmd`, `:N`, `:set`,
   `u`/redo on the document's groups — D24's list. `madc -i FILE` IS line
   mode. The per-language editing tables (comment opener, block delimiters,
   default indent, indent detection — `madcide_lang.inc:71-218`) and the
   file-kind family predicates move beside `src/file_kinds.cpp` for `>`,
   `<` and `:set`.
7. **`%history` (one store), `%std`, `%reset`, the one completion insert
   rule** (`line_edit::completed`).
8. **Check, build, run and projects at the prompt** (owner: build and
   projects are madc's): `%check` (madcide documents `:check` and never
   handled it); `%build` / `%run` on a live BUFFER (today a file only —
   madcide's `start_native_build` and F5 `replrun` move down); program args
   at the prompt; `%project` — manifest write, add/remove a TU, check /
   build / run of the project (madc already reads the manifest; madcide's
   `proj_check_rows` and the `bld-proj-*` bodies move down).
9. **Syntax colour at the prompt** (owner 2026-10-09). The pieces exist:
   the token classifier (`include/tokens.h:145`, `madc::lex_spans`), the
   one style vocabulary (`include/madcdis/ui_style.h`, depth detection
   included), and `line_edit.h`'s `line_painter`, which repaints the entry
   every keystroke. The painter styles each span through the same
   style-to-SGR step the TUI uses; the echoed source line under an error
   and `%type` / `%pinfo` output use it too. madc ships ONE built-in
   default theme; **madc.ini's `theme` key overrides it** (owner);
   madcide's themes stay its own data. Phase 0.5's predicate gates it.
10. **`%git` as the madcgit module's command**, registered on load
    (`-lmadcgit`, the ini `modules` key, or `import madcgit;`); the verb
    bodies (`madcide_git.inc:93-613`, ~520 lines) move into the module.

*1C — the headless services (after 1A; parallel with 1B):*
11. **ONE set of language-service functions** over a document: diagnostics,
    outline, definition, references, hover, semantic spans — mostly the
    existing `madc::parse_*` / `graph_*`, now taking a document. The LSP,
    the MCP tools and madcide's direct calls all read it; nothing answers
    twice.
12. **The LSP into madc's core** (owner: "small enough"): `madc --lsp FILE`
    — a C++ port of `madcide_lsp.inc` (1,098 lines of dialect today; a
    core face must not recompile its own source at every launch) over 1A.1
    and 1C.11: document sync on madc's documents (a `didOpen` no longer
    switches an active buffer — there is none), `executeCommand` through
    the registry. madcide never talks to madc's buffers THROUGH the LSP
    (it owns no copy; it calls 1C.11 directly).
13. **The MCP protocol module** (a module by the module principle): the
    JSON-RPC server with tool registration (name, schema, handler,
    per-tool permission tier) and the MCP client (`madcide_mcpclient.inc`,
    459 lines). Any madc program exposes tools with it.
14. **The code-graph tools into core** beside the LSP (19 tools over 1C.11
    + the document's undo for insert / replace / delete), and **the
    git-history tools into the madcgit module** (6 tools, `madcide_past.inc`
    502 lines) — each registered into 1C.13 when present.
15. **The project-memory module**: the change log (`clog_*`, today
    `editor_events.inc` — a SUBSCRIBER to 1A.1's edit notification now),
    records and links (`madcide_nexus.inc` 514), proposals
    (`madcide_propose.inc` 242), test discovery / runs / results
    (`madcide_tests.inc` 546), asset layers (`madcide_layers.inc` 176);
    saved beside the project (`<base>.prj.events`). The 21 `nexus.*` /
    `test.*` tools register into 1C.13.
16. **Discovery as a module** (`madcide_discover.inc`, 345 lines) and
    **`madc --mcp --project X`** (a single file = the one-TU case): the
    headless MCP server for a project, solely in madc — 1C.13 + 1C.14 +
    1C.15, the history tools when madcgit is loaded. ONE OWNER PER PROJECT:
    the first process advertises; a second attaches or refuses with the
    owner's pid. Proposals persist with 1C.15, so a headless agent's
    proposal is decided later in madcide.

*1D — madcide re-plumbed (after 1B and 1C):*
17. **madcide on madc's documents**: its buffer ring, `ensure_phandle`,
    `ed_text_insert` / `ed_text_erase` and the anchor registry
    (`editor_events.inc`, 1,778 lines) are replaced by 1A.1; its views,
    carets, selections, panes and lenses point at madc documents. The
    change log is now 1C.15's; madcide loads that module.
18. **`colon_command` splits**: the editing verbs are madc's (1B.6), the
    `view*` verbs register into madc's registry; lined's verbs retired;
    `madcide --line FILE` = `madc -i FILE` (kept, decided); the pane's
    history and completion from madc (1B.7).
19. **madcide's service files deleted, madc's loaded**: `madcide_lsp.inc`,
    `madcide_mcp.inc`, `_nexus`, `_past`, `_propose`, `_tests`, `_layers`,
    `_mcpclient`, `_discover` (~5,600 lines) go; madcide imports 1C.13 /
    1C.15 / 1C.16, registers its commands as tools (1C.13) and keeps the
    seats (`madcide_api/seat/serve/ws/once.inc`, ~1,350 lines: the api
    seat, the ws window, `--serve`, `--attach`, `-c`, tiers over its
    commands). `tools/vscode-madcide` targets `madc --lsp`.
20. **madcide features that ride the move**: the Symbols view in madcide's
    base (Chthonia drops its plugin in its paired release), View ▸ Line
    Numbers (the gutter toggle, persisted through the layout).
21. **`examples/vised/`**: the self-contained rewrite on `ui::`, the
    document component and the registry, with its R4/R5 contract test;
    `tools/texteditor/vised*`, `lined*`, the `.madv` verbs, testlineed and
    testvised deleted.
22. **The engine twins** (above) land.

**Phase 2 — the repository split** (`2026-10-08-madcide-own-repo.md` steps
2-7, on the corrected inventory): the `git filter-repo`, madcide CI on the
pinned madc, the removal commit in madc, Chthonia repointed at a madcide
release, the three first releases, KG + mirrors. The ledger's APPLICATION
split is deleted with the paths it names.

**After the arc:** the IDE plugins (`2026-10-08-ide-plugins.md`), in the
madcide repository.

## Seams (merge waves)

ONE seam (owner 2026-10-09): the arc's release boundary is the end of
Phase 2. Phase 1 does not release on its own; its slices bank on the arc's
branch with their targeted gates.

- **The release:** madc **0.104.0** WITHOUT madcide; madcide **0.7.0**, its
  first standalone release, built on madc 0.104.0; Chthonia **0.1.0**, built
  on madcide 0.7.0 + madc 0.104.0. One battery on madc's content, then each
  repository's own lanes.
- **Phase 0's Chthonia release** is separate and earlier: Chthonia **0.0.4**,
  the current `dark-plus` build on madc 0.103.0 (madcide still inside madc).
- Phase 0's engine items ride the seam (each with its own targeted gate).

## Thread-safety contract

The document component and its edit notification are per-world,
session-thread state like the hub's components (`include/madcdis/hub.h`) —
a buffer ring is a per-world entity set, never a global; a document's
mirrors (the forked backend's input, an LSP client) are keyed by its version
and resync. The registry extension point is written at activation and read
at input on the session's thread. The MCP protocol module serves one
connection at a time per session thread; a per-tool permission tier is
plain data. No new globals.

## Owner decisions open

1. ~~lined and vised~~ — decided (above): lined retired, vised kept as a
   rewritten madc example.
2. ~~The ex regex flavour~~ — decided: PCRE2 (below).
3. ~~Symbols~~ — decided (below): madcide's built-in view; Chthonia uses it.
4. ~~`madcide --line`~~ — decided (below).
5. ~~madc releasing while madcide is red~~ — decided: yes.
6. ~~MCP~~ — decided (below).
7. ~~Seats~~ — decided (below).
8. ~~Where madc.ini's `std` sits~~ — decided (owner 2026-10-09): `--std` >
   file extension > madc.ini/env `std` > madc. The ini `std` sets the
   standard for a file with no recognised extension and for a bare `madc -i`.

## Decided (owner, 2026-10-09, inventory round)

- **File buffers are madc's** — in the process that edits, never the one that
  runs user code (`madc -i`: the prompt process; madcide: its own process,
  the same buffer code). Windows `madc -i` gets the child backend (Phase 1A
  step 5).
- **LSP is part of madc's core** ("small enough"): `madc --lsp`, its document
  sync on madc's buffers, `executeCommand` through the command registry.
- **`madcide --line FILE` = `madc -i FILE`** (kept; it costs nothing).
- **Standard precedence:** `--std`, then the file extension, then the madc
  dialect (a file with no recognised extension — a shebang script). `.c` →
  C17 until C23 nears the C17 level (ROADMAP 2.13: C23 measured 51.4%), then
  the table flips to C23.
- **madc.ini loads modules**, as php.ini loads extensions (a repeatable key).
- **A madc release may land while madcide is red against it** (madcide pins
  its madc).
- **The module principle** (owner): "modules are madc-language
  extensions/libraries that any madc program could benefit from... including
  madcide". A capability goes into a module when a program other than
  madcide could use it; madcide is one consumer.
- **A headless MCP server for a madc project exists solely in madc** (owner
  2026-10-09: "the ability to operate a headless MCP server for a madc
  project"). `madc --mcp --project app.prj.json` (a single file is the
  one-TU case); nothing in it needs madcide. Layers: (1) the MCP protocol —
  a module (the JSON-RPC server with tool registration: name, schema,
  handler, a per-tool permission tier; and the MCP client); (2) the
  code-graph tools — madc core, beside the LSP, over the one set of
  language-service functions (LSP, MCP and madcide's direct calls share it);
  (3) the git-history tools — the madcgit module, registered only when it
  is loaded; (4) project memory (the change log, records, links, proposals,
  test runs) — a module that subscribes to the buffers' edit notification
  and saves beside the project; (5) registered commands (madcide's among
  them) exposed as tools. madcide is an optional UI over the same project:
  it loads the same modules in-process and serves the same MCP, plus its
  commands. **One owner per project at a time:** the first process to open a
  project advertises itself, a second finds it (discovery — now a module:
  two consumers) and attaches or refuses with the owner's pid. Proposals
  persist with the project memory, so a headless agent's proposal is
  accepted or rejected later in madcide. Later: whether madcide can attach
  to a running headless server; what an edit tool does with no person
  watching (proposed: lands in the buffer, saved only by an explicit save
  tool).
- **Seats, `--serve`, `--attach`, `-c`, the ws window: madcide's** (owner:
  "unless it could be a reusable module beyond it"). Their reusable
  foundation is already madc's (`serve_web`, WebSocket, the `conn_*` roster,
  `<ns_ui_ws>`). Discovery (~350 lines) is NOT madcide's: it became a module
  with the headless MCP (its second consumer, above).

## Decided (owner, 2026-10-09)

- **Repository:** `derekbsnider/madcide` — the established name.
- **madcide's version numbers:** madcide was first released with madc 0.98.0,
  which is madcide 0.1.0, so madcide's minor = madc's minor − 97 (madc 0.103.0
  ↔ madcide 0.6.0). The next madc release is 0.104.0, so the first standalone
  madcide release is **0.7.0**.
- **Chthonia:** **0.0.4** = the current `dark-plus` build on madc 0.103.0
  (Phase 0, if released); **0.1.0** = the first Chthonia on madcide 0.7.0 +
  madc 0.104.0 (owner: it should have started at 0.1.0; it starts there now).
- **One release boundary:** madc 0.104.0 ships WITHOUT madcide; Phase 1 and
  Phase 2 both land before it (see Seams).
- **`%git`:** the madcgit module's command (above), `import madcgit;` at the
  prompt included.
- **Symbols** (owner 2026-10-09: "madcide should have the same symbols view as
  chthonia, and chthonia can just use it"): a BUILT-IN madcide view, the
  behaviour of Chthonia's plugin (`plugins/chthonia/chthonia.mad`, 83
  lines): the REPL session's names, each `name type value`, refreshed after
  every entry and every F5 run (`replbindings` on reTAKEN / reRAN), emptied
  when the session stops, a row the program's file defined going to its
  definition. In madcide's default layout's sidebar and View menu, with
  help. Chthonia deletes its plugin and lists `symbols` in its layout — in
  Chthonia 0.1.0 (on madcide 0.7.0): both registering `symbols` would make
  Chthonia's plugin fail to activate, so the two land together.
- **Regex: PCRE2, madc's ONE regex engine** (owner 2026-10-09). Today madc's
  only regex is the C++ library's `std::regex` (ECMAScript grammar), in
  `src/ns_perl.cpp`, behind five functions — `madc::regex_match`,
  `regex_search`, `regex_replace`, `perl::grep`, `perl::split` — compiled on
  every call, differing between libstdc++ and libc++, and recursing per
  character in libstdc++ (stack depth on long inputs). POSIX `regex.h` was
  weighed and refused: absent on Windows (no `regex.h` in mingw or the UCRT),
  a different engine on macOS (TRE) than on Linux (glibc), and POSIX
  leftmost-longest semantics where vim, Perl, PHP, Python and JS are
  leftmost-first. PCRE2 is BSD-licensed, PHP's own engine (php:: parity —
  `preg_*`), Perl-compatible, leftmost-first like vim, optionally JIT, and
  builds on all three platforms — a DEPENDENCY, never vendored. One engine
  serves the ex core (`:s`, `:g`, `:v`, `/re/`, `?re?`, compiled once per
  command), the five existing functions, and the polyglot namespaces.
  **Linked into libmadc** (owner 2026-10-09: "small enough to include and
  its BSD licensed") — a deliberate exception to the 2026-09-15 rule that an
  unmodified third-party library binds through a lazy module: regex is part
  of the language's own surface, and PCRE2 is BSD and ~610 KB of code (614 KB
  measured, Ubuntu 24.04's 10.42 8-bit library: JIT 236 KB, Unicode tables
  114 KB, matcher 86 KB, compiler 63 KB; ~380 KB without the JIT). The rest of
  that rule stands: no PCRE2 source in the repository — libmadc links the
  platform's library (the distro's `libpcre2-8` on Linux, a fetched or brew
  copy for the macOS and Windows builds), the zstd precedent. Design items
  for Phase 1A.3: no `pcre2_*` symbol exported from libmadc
  (`check-c-abi-surface`; never `--exclude-libs` — a shared-library dependency
  keeps them out); the JIT in or out (the ex core and the five functions do
  not need it); each platform's package dependency; how vim's spellings map
  onto PCRE2 syntax (D24 asks for the modern extended form: unescaped
  `+ ? | ( )`). Riding the same
  move, as their own fixes with reducers: the five functions refuse an
  invalid pattern instead of answering silently (today `perl::grep` falls
  back to a substring search, `perl::split` to a literal delimiter, and
  `madc::regex_search` answers 0 as if nothing matched), the `madc::`
  three leave `ns_perl.cpp` for their own namespace's file, and they gain
  lean `var` / `const char *` forms resolved mangled-direct instead of
  extern-C `std::string` wrappers.
