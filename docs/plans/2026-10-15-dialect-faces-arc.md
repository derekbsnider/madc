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
| 19 | `mkdir -p` (found 2026-10-16, landed with row 10) | `host_path_make_parents` (`src/madc_posix_io.cpp:263`) | `dir_ensure` (`madcide_plugins.inc:324`) | grew a Windows absolute path (`C:\...`) from `.` (it split on `/` only) |
| 20 | a product's user configuration directory (BUGS.md B91, 2026-09-30) | `config_file::user_config_dir` (`src/madc_config_file.cpp:239`) | `config_dir_of` (`madcide_plugins.inc:44`) | on Windows madc read `$HOME/.config`, madcide `%APPDATA%` — owner 2026-10-16: madc's one rule gains the Windows-only `%APPDATA%` arm, madcide keeps only its `MADCIDE_CONFIG_DIR` override |

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

### D. The REPL colours as the editor does (owner 2026-10-15)

| # | Capability | Today | Wanted |
|---|---|---|---|
| 18 | the REPL's classes (`madc -i` and madcide's pane) | lexical only — `theme_paint` / `theme_marks` lex each text alone (`lex_classes`, the 1B.9 design), so in `std::string world("World")` only the literal is coloured | the session's parse classes (namespace, typename, variable, function — what `madc::parse_spans` gives the editor): the session parsed the entry with its includes; the painter takes those classes, so the CLI, the pane and the editor agree |

### E. SQLite compiles and passes its tests (owner 2026-10-16: "added to the v0.106.0 arc")

KG Milestone `sqlite_compiles_and_passes_its_tests`; ROADMAP "The SQLite Goal".
Source: SQLite 3.54.0 on the build container (`/workspace/sqlite/3.54.0`, the
amalgamation in `/workspace/sqlite/build-3.54.0`); Tcl 8.6 + headers installed
there for the test suite. The first compile (`madc -c sqlite3.c`) refuses with
ten errors from the four gaps below, each a KG Gap that BLOCKS the milestone.
A parse error stops the compile, so more surface as each lands.

| # | Gap (KG) | Reducer (`tmp/sqlite_dims/`) | Oracle (gcc = clang) |
|---|---|---|---|
| S1 | `c_array_dim_typedef_cast_not_folded` — `int a[(u32)4];` refused (`(unsigned)4` folds); SQLite's `BITVEC_NPTR` | `r1_castdim.c` | `4 4 8 3 72 8` |
| S2 | `c_ternary_middle_operand_comma_expression` — `x ? (v = 7), 3 : 4` (C11 6.5.15); `getVarint32` / `putVarint32` | `r2_ternary_comma.c` | `3 1 5 2 1200` |
| S3 | `c_if_condition_fnptr_then_paren_read_as_call` — `if( fp ) (void)f(p);` read as the call `(fp)(void)` | `r3_void_stmt.c` | `called=9` |
| S4 | `cli_fpic_and_flags_after_source` — `-fPIC` read as a file; options after the source refused | `pic.c` | `madc -c` objects are already position-independent (`gcc -shared` links one, no TEXTREL): `-fPIC` / `-fpic` / `-fPIE` / `-fpie` are accepted no-ops |

Then: the amalgamation compiles (`-c`, then the shell linked), runs, and the
Tcl suite builds `testfixture` with madc as `CC` and runs. S1-S3 are parser
changes: each its own commit with its reducer promoted to `tests/` (gcc and
clang outputs), Tier 1 + Tier 2, never folded together.

Place in the arc: after section A and the three dialect silent wrong answers,
before sections B-D — a focused parser stretch beside the two C++ compile gaps.

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

PLANNED 2026-10-15 (owner).

- Rows 10 + 19 (2026-10-16): `madc::path_drop_extension` / `path_stem` /
  `path_make_parents` (`<ns_madc>`); `src/madc_posix_io.h` registered in
  `dialect_faces.tsv` (every public a face or an exempt reason); test
  `testpath_faces`. madcide: `path_sans_ext` and `dir_ensure` deleted, a
  `layout-path:` line in `testmadcide`, and `check-madcide-single-owners.sh`
  red on a hand-rolled extension split.
- Row 20 (2026-10-16, owner ruling): `cfg::user_config_dir` /
  `user_config_dir_of` (`include/madc_config_file.h`) — XDG, then `%APPDATA%`
  on Windows, then `~/.config` — the one rule madc.ini's lookup and the mcp
  sources read; faces `madc::user_config_dir` / `user_config_dir_of`; test
  `testuserconfigdir`, unit case in `test_config_file`; BUGS.md B91 closed.
  madcide: `config_dir_of` deleted (only `MADCIDE_CONFIG_DIR` stays its own),
  `check-madcide-single-owners.sh` red on an `XDG_CONFIG_HOME` / `APPDATA`
  read in madcide.
- Rows 6-9 (2026-10-16): the manifest's one reader and writer as faces —
  `madc::project_manifest_read` (the struct faces of `ProjectManifest` /
  `ProjectTU`; each TU resolved against the manifest's directory; the file's
  own JSON under "json", so an IDE's "commands" ride), `_create` / `_add_tu`
  / `_remove_tu` / `_set_kind` (the last a new writer verb),
  `project_default_output`, `project_events_path`, `project_kind_name`; the
  kinds one enum text, `<bits/project_enums>` (`ProjectKind` aliases it);
  `include/madc_project.h` registered; test `testproject_manifest_faces`,
  unit case for `set_kind`. madcide: `proj_write`, its JSON parse and
  stringify, `ide_projkind` / `projkind_name` / `projkind_of` and its
  events-path copy deleted; `proj_load` / `proj_add` / `proj_del` /
  `proj_toggle_kind` / the build rows call the faces; `testmadcide`'s
  `proj-faces-*` lines (a TU resolved, `<base>` not `<base>.prj`, `output`
  against the manifest, a relative add, an unknown kind refused) and
  `proj-addcur-nofile`; `check-madcide-single-owners.sh` red on a manifest
  parsed, stringified or given a kind word in madcide.
- The dialect silent wrong answers (2026-10-16):
  - KG `range_for_over_call_result_subscript_empty` — FIXED `7a28c50fc`: a
    carrier returned by reference (`fill(a)["rows"]`, `for (var c :
    fill(a))`) reads its referent, and a carrier range-for evaluates its range
    once (`carrier_expr_addr`); test `testcarrier_ref_call`.
  - KG `rangefor_class_ref_call_receiver` (found on the way) — FIXED
    `8dd64279c`: a C++ range-for over a class container returned by a call or
    held by a reference parameter compiles and evaluates its range once
    (`class_object_expr_addr`, `capture_class_range`); test
    `testrangefor_ref_call`.
  - KG `import_module_member_double_return` — FIXED `d4002f602`:
    `math::sqrt(2.0)` printed 1; an alias-form member is declared as the same
    name's bare call is (the
    pack's prototype, else `dynamic_symbol_fallback_signature`) and its slot
    is called through the declared type; test `testimport_member_types`.
  - KG `var_copy_init_from_keyed_array_slot_empty` — not a carrier defect:
    script mode runs file-scope initializers before the statements (script
    mode semantics #6); source-order interleaving is an owner decision.
  - KG `carrier_byvalue_call_subscript_parse_refused` (found on the way) —
    `make(2)["rows"]` refused at parse; the generic subscript arm reads the
    base's `datadef()`, not its value type.
- Section E, SQLite (2026-10-16):
  - S1-S4 FIXED: `5c49a45cb` (a cast through a typedef heads an integer
    constant array bound), `7edb34163` (the `?:` middle operand is a comma
    expression), `5b6d2b4ee` (a condition's `)` never continues into a call),
    `7d140ebc8` (`-fPIC` accepted; options may follow the source).
  - The amalgamation, then the test sources, surfaced: `3ca71975c`,
    `45c322839`, `11fe93c76`, `d3dd78e88`, `385e7d40e`, `8e64cf623`,
    `8d708f3ec`, `e67a8dac4`, `cebad16e6` (parser / cir, each with its
    reducer).
  - gcc's link vocabulary, so the Makefile's compile-and-link lines run
    unchanged: `35ce6a38f` (several sources on an artifact lane),
    `5fa1e9571` (`-L`, `-Wl,-rpath`), `07e54ff76` (`-l` resolved as ld
    resolves it: SONAME, linker scripts, empty stub archives),
    `f6ec34ad0` (`-dumpmachine`).
  - `68b12640c`: `-g` homes only the locals a debugger describes (the
    testfixture's frames fit the stack again).
  - `fdcf2a26c`: a tag a parameter list names first is declared at file
    scope (Tcl's `struct Tcl_Obj` warnings).
  - `0211249a4`: an object may be aligned beyond 16 bytes, static (the MIR
    item carries it) or automatic (the frame is realigned) — SQLite's
    `ALIGN128` mutex table; c-torture `20010904-1/2` pass.
  - Result: the default configuration's `testfixture`, compiled and linked
    by madc (`make CC=madc testfixture`), runs veryquick with 0 errors out
    of 330460 tests.
  - speedtest1 (`--size 50`, the Makefile's flags): gcc -O2 1.24s, madc
    -O2 1.74s, gcc -O0 2.25s, madc -O2 -g 2.66s. KG
    `sqlite_speedtest1_slower_than_gcc`: madc's `-g` costs 53 percent
    (MIR homes named locals in memory; gcc's `-g` never changes code).
  - SQLite's `make test` (testrunner `mdevtest`: the All-O0 and All-Debug
    `--enable-all` builds, veryquick + fuzz + shell on each) surfaced:
    `6cb2f8c67` (`-Wall` / `-W<name>` accepted, `-Werror` refused),
    `71f64745f` (a macro argument's paint per stretch: SQLITE_DEBUG's
    `NC_IdxExpr` was hidden), `76f2df507` (a statement expression yielding
    no value is void: two c2mir crashes on SQLITE_DEBUG's folded asserts),
    `9681942cc` (a re-spelled string literal keeps its NULs and its wide
    prefix — SILENT: every FTS5 `contentless_delete` table "corrupt"; the
    eleven FTS5 files now pass, 0 errors of 4147).
  - zeroblob, pcache, tkt1644, misc4 and schema were madc's: configure,
    run with madc as CC, concluded wrong things (SIZEOF_OFF_T 4, no
    gmtime_r / localtime_r / readline), and a gcc-built testfixture of that
    configuration fails them the same way. gcc's own `mdevtest` is 0 errors
    of 978512. Fixed in the driver and the linker: `31283e348` (`-E` / `-dM`
    write to `-o`: ccache hashed an empty file and replayed one cached probe
    result), `0942a4f1f` (a C/C++ source with a stage option after it is a
    compiler line — the link probes RAN the program; owner: `.mad` scripts
    keep their argv), `1b6a7d80b` (an executable link refuses an undefined
    reference, as ld does — KG `aot_link_undefined_symbol_not_refused`).
    `configure --enable-all` with madc now writes gcc's `sqlite_cfg.h` except
    HAVE_LOG (madc links libm by default; no SQLite source reads it).
  - On the way: `90cd2ece1` (SILENT: a C function designator not followed
    by `(` was CALLED in `!f`, `f ? a : b` inside an argument) and the
    undefined weak reference, `5a2ba28ed` (KG
    `weak_undefined_reference_unsupported`; Mach-O runtime rides the seam).
  - Result: `mdevtest` with madc is 1 error of 932121 — the only one is the
    All-Debug `fuzzcheck-ubsan` build (`-fsanitize=undefined`: madc has no
    sanitizers), which skips 18 jobs. Every test file passes in both builds.
  - Rule from this stretch (owner): every release consolidates an EXISTING
    duplicate family (`4c63204d5`, `check-release-consolidation.sh`); the
    candidate here is DupFamily `bare_function_name_call_or_address`.
  - Open (KG Gaps on the milestone): `aot_link_undefined_symbol_not_refused`,
    `emit_c11_sqlite_not_recompilable`. Found on the way, not SQLite's:
    `c11_noreturn_function_specifier_unparsed` (its own parser session),
    `cpp_linemarker_input_refused`.
