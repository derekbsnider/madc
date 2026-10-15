# The dialect-faces arc — finishing the split arc's moves (madc 0.106.0)

Owner 2026-10-15: "these all need a specific plan-arc for v0.106.0".

## Why

The split arc ([2026-10-09-madc-madcide-split-arc.md](2026-10-09-madc-madcide-split-arc.md),
madc 0.104.0) moved capabilities madcide had built in dialect code into madc
so they would be reusable outside madcide. A move has two halves: madc takes
the code, then madcide deletes its copy and calls madc's through a dialect
face. The second half was done for the services (1D deleted ~5,600 lines) and
not for 17 other moves. Several madc commits said madcide "moves onto it in
1D" (`abd00fb2d`, `e6eded028`, `e5b782347`, `include/madc_file_kinds.h:44-48`);
1D did not. Nothing checked.

Found when madcide's REPL showed `:%p` uncoloured while `madc -i` coloured it
(madc's painter had no dialect face, the ex and session faces dropped the
code ranges, madcide kept its own theme reader). The rule is now written:
`.claude/rules/dialect-faces.md`, gated by `scripts/check-dialect-faces.sh`
over the registry `scripts/dialect_faces.tsv`.

## Done before this arc (0.105.0)

Row 1, the theme: `shown_code` on the ex and session faces;
`madc::theme_styles` / `madc::theme_paint` (`<ns_madc_host>`); madcide's
`load_theme` reads through madc's reader (its parent map and its hand-listed
chrome names deleted); the REPL transcript paints `:%p`, `%type`/`%pinfo` and
the taken-entry echo through madc's painter (madcide 37f2d0a's second painter
deleted). Tests `testex_theme`, `testsession_shown_code` (madc),
`testmadcide_repl` (madcide).

## The rows (the audit of 2026-10-15; every copy present at madcide v0.7.0)

Order: the SILENT WRONG ANSWERS first (fix-what-you-find), then the copies
whose behaviour already differs, then the pure duplicates.

### A. Silent wrong answers in madcide's copy

| # | Capability | madc owner | madcide copy | Divergence |
|---|---|---|---|---|
| 10 | a path minus its extension (`{base}`) | `host_path_drop_extension` (`src/madc_posix_io.cpp:252`) | `path_sans_ext` (`madcide_core.inc:92`) | `dir.d/foo` → `dir` (the last `.` of the whole path) |
| 9 | a project's default output | `project_default_output` (`src/madc_project.cpp:473`) | `madcide_core.inc:7581-7588` | `foo.prj.json` → `foo.prj`; `output` not resolved against the manifest's directory |
| 6 | manifest writer: create / add TU / remove TU | `src/madc_project.cpp:413,424,454` | `proj_write` / `proj_add` / `proj_del` (`madcide_core.inc:5290,5358,5416`) | stores a path as typed (madc: relative to the manifest; refuses a non-file) |
| 8 | project kind names (console / gui) | `src/madc_project.cpp:254,260` | `madcide_enums.inc:422,426` | an unknown word becomes console (madc refuses it) |
| 7 | manifest reader (object shape, `tus`) | `read_project_manifest` (`src/madc_project.cpp:264`) | `proj_load` (`madcide_core.inc:5463`, own `js::parse`) | rides with 6/8: one manifest owner |

### B. Behaviour already differs

| # | Capability | madc owner | madcide copy | Divergence |
|---|---|---|---|---|
| 5 | history: when an entry is kept, Up/Down recall | `line_history::add`, prefix recall (`include/madcdis/line_edit.h:215,664,681`) | `repl_history_push` / `repl_history_step` (`madcide_repl.inc`) | rtrim variant; sequential recall with a draft (madc: prefix recall) — decide the ONE rule with the owner, then both use it |
| 16 | REPL prompt `std> ` and the continuation pad | `src/madc_repl.cpp:279-281,323,581` | `repl_prompt`; `console_entry_display` (`madcide_core.inc`) | pads by columns where madc pads by bytes |
| 17 | the backend-stopped wording | `session_stop_words` (`src/madc_session_client.cpp:1508`) | `madcide_repl.inc:670-682` | its own "exit N" / NTSTATUS line (kept on purpose by `79a34faad` — revisit) |

### C. Pure duplicates (same rule, two copies)

| # | Capability | madc owner | madcide copy |
|---|---|---|---|
| 2 | editing tables: comment opener, block open/close, default indent, indent detection, C-syntax lineage | `src/file_kind_editing.cpp:16-73` | `madcide_lang.inc:81-159` |
| 3 | kind family, reaches-the-compiler, per-family default standard | `file_kind_family` / `file_kind_default_standard` (`src/file_kinds.cpp:226,245`), `src/madc_lang.cpp:308` | `lang_rows`, `kind_family`, `doc_standard` (`madcide_lang.inc:23-253`) — redundant: `madc::doc_set_standard` adopts a family head itself |
| 4 | the completion insert rule | `complete_word` / `completed` (`line_edit.h:353,498`) | `repl_completed` (`madcide_repl.inc`) |
| 11 | project check rows (each TU's diagnostics + an unopened-TU row) | `InteractiveSession::project_check` (`src/madc_session.cpp:2285`) | `proj_check_rows` (`madcide_core.inc:7366`) |
| 12 | the REPL command registry (1A.2's extension point) | `session_command_add` (`src/madc_session_verbs.cpp:407`) — face EXISTS (`ns_madc:923`) | `plugin_repl_command` (`madcide_plugins.inc:860,892`), the intercept (`madcide_repl.inc:760`), its own help list, the `view*` colon verbs (`madcide_core.inc:7979`) |
| 13 | LSP semantic-token legend + class mapping | `src/madc_lsp.cpp:98,102` | `madcide_enums.inc:823-863` |
| 14 | git log line `%h %s (%ar, %an)` + blame line | `madcgit_verbs.cpp:137-141,357` | `madcide_git.inc:113-121,352-358` |
| 15 | the `std` / `theme` settings owner (owner decision F6: madc.ini owns them) | `src/madc_config.cpp:36,46` | settings.json `"std"` (`madcide_lang.inc:289-330`), its theme from its own profile |
| — | (suspected) memory record-kind words | `src/madc_memory.cpp:74` | `clog_kind_of` (`editor_events.inc:436`) |

## How each row lands (one shape)

1. madc: the face — a declaration in `<ns_madc>` / `<ns_madc_host>` /
   `<bits/...>` (lean forms: `value` / `const char *`, `dialect-lean.md`),
   its auto-include member row for a host face, a `scripts/dialect_faces.tsv`
   row (struct or public). Row 12 needs no new face.
2. madc: a test through the dialect face against the CLI's own output (the
   oracle is `madc -i` / the C++ owner on the same input).
3. madcide: DELETE the copy, call the face. A divergence (A, B) gets a
   madcide test that fails on the old copy (the silent wrong answer as the
   reducer).
4. madcide gate: a hand-copied madc name table (class names, chrome names,
   kind names, record kinds) is red in madcide's `scripts/gates.sh` — the
   madcide-side twin of `check-dialect-faces.sh`.

## Thread-safety contract

Every face this arc adds is a pure function of its arguments (tables, path
arithmetic, a manifest read/write by path) or runs on the calling thread's
runtime as `lex_spans` does; none keeps state between calls. The manifest
writer follows madc's (`madc_project.cpp`): one writer per manifest is the
caller's contract, as for any file.

## Validation and the seam

Per row: Tier 1 (the row's madc + madcide tests), Tier 2 per madc commit,
madcide's suite + gates + the TUI goldens per madcide commit. The SEAM is the
0.106.0 release boundary: the seam battery once, the interactive lanes, the
packages; madcide's paired release.

## Status

PLANNED 2026-10-15 (owner). Nothing of rows 2-17 started.
