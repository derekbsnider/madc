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
  except `.`-digit, `::`, `%:`) and fall-through replace `command_text`'s
  narrow rule.
- **Edit notification at the engine layer.** A `text_buffer` edit publishes
  one change record (offset, erased, inserted) that every observer consumes;
  madcide's anchor shift and change journal subscribe to it instead of being
  the mutation path. Without this, an ex edit made through madc's core would
  skip madcide's anchors and journal — the exact defect lined's verbs have
  today. This is the deepest layer for "one mutation path" (Rule #2).
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
Markdown preview), LSP and MCP faces (bound to madcide's buffers and stream
today; `madc --lsp` is a later question once madc has headless buffers).

**Retired: lined** (owner 2026-10-09, "completely deprecated") —
`tools/texteditor/lined.mad`, `run_session`, its ed verbs
(`tools/texteditor/verbs/*.madv`, `checks/editable.madv`) and
`tests/testlineed.mad`, superseded by madc's ex core. madcide's `w`/`q`/`q!`
and its read-only verdict call the registry instead; `lined_core.inc`'s
document helpers (`doc_set_path`, `doc_kind`, `doc_untitled`, `doc_name`,
`new_document`, `setup_document`) and `resolve_data_dir` move into madcide.

**Stays in madc as an example, rewritten: vised** (owner 2026-10-09: keep it
as a simple example). `examples/vised/` — a self-contained visual editor on
madc's PUBLIC surface only: `ui::` (one tree, the terminal UI and `--web`),
`ui::text_*` (the buffer and its undo), the command registry for `w`/`q`. It
includes nothing from madcide (today's 197-line `vised_core.inc` leans on
`editor_events.inc`'s key/pointer handlers and composers, which move with
madcide; keeping `editor_events.inc` in madc for it would make madc ship
application code). Its test is the engine-level check of the R4/R5 contracts
lined's retirement leaves uncovered: edits through the one mutation path,
read-only refusing an edit, save/quit through the registry. Built in Phase 1
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

## Phases

**Phase 0 — independent, any order (engine fixes and releases):**
1. Chthonia 0.0.4 on madc 0.103.0 (the `dark-plus` branch; madcide still inside madc).
2. The two C++14 defects (`cpp1y/lambda-generic-ice2.C` abort,
   `cpp1y/var-templ24.C` > 30 s) — a parser session.
3. The Outline's kinds beyond functions (`madc::parse_outline`) — a parser
   session.
4. The GUI gutter in `page.js` / `page.css` (engine), with its gui test.

**Phase 1 — the interactive core into madc (one repository still):**
1. The registry extension point + D24 recognition and fall-through.
2. Engine edit notification; madcide's anchors and journal subscribe.
3. The ex core and buffers at the prompt; the regex decision.
4. `%history` (one store), `%std`, `%reset`, the one completion insert rule.
5. `%git` as the madcgit module's command (registered on load: `-lmadcgit`, a madc.ini modules key, or `import`).
6. madcide re-plumbed: `colon_command` splits (editing → madc, `view*`
   registered), lined's verbs retired, `--line` = madc's line mode, the pane's
   history and completion from madc.
7. madcide features that ride the move: the Symbols view in madcide's base
   (Chthonia drops its plugin in its paired release), View ▸ Line Numbers
   (the gutter toggle, persisted through the layout).
8. `examples/vised/`: the self-contained rewrite on `ui::`, `ui::text_*` and
   the registry, with its R4/R5 contract test; `tools/texteditor/vised*`,
   `lined*`, the `.madv` verbs, testlineed and testvised deleted.
9. The engine twins (above) land.

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

The edit notification is per-world, session-thread state like the hub's
components (`include/madcdis/hub.h`); the registry extension point is written
at activation and read at input on the session's thread. No new globals.

## Owner decisions open

1. ~~lined and vised~~ — decided (above): lined retired, vised kept as a
   rewritten madc example.
2. ~~The ex regex flavour~~ — decided: PCRE2 (below).
3. ~~Symbols~~ — decided (below): madcide's built-in view; Chthonia uses it.
4. **`madcide --line`:** becomes an alias for madc's line mode on the file,
   or is retired with a pointer to `madc -i`.
5. Whether a madc release may land while madcide is red against it (default
   yes).

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
  for Phase 1 step 3: no `pcre2_*` symbol exported from libmadc
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
