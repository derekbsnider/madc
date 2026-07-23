# MADC — SMAUG `--project` BOOT HANDOFF (2026-06-08, read-first)

> **Supersedes** `2026-06-08-codex-integration-and-smaug-revival-handoff.md`
> (which said "SMAUG NOT booting" — that is RESOLVED). For deep arc history
> (template/enum/`<type_traits>`), `2026-06-08-FULL-rehydration-handoff.md`
> still holds the per-commit notes.

## 0. ONE-SCREEN STATE
- **Repo** `/workspace/madc`, **branch** `feature/realhdr-parse-gaps2-claude`,
  **HEAD `4aa0a20`**, working tree clean. NOT pushed. develop/master untouched.
- **Gates:** fulltest **537 / 4** (pre-existing reds: testdefer, testfstream,
  testlargesizeofquery, testloop); gcc.c-torture **1566 / 31 / 57 / 1**. MIR pin `2ffebff`.
- **SMAUG boots two ways from a FRESH compile** (the old "not booting" blocker is gone):
  1. **Umbrella:** `MadSMAUG/src/SMAUG.mad` (comm.c's real `main`, no injected main) via
     `MadSMAUG.sh` (now passes `--std=c`). Committed: MadSMAUG `master` `2140d3f`.
  2. **`--project` (intended path):** `madc --project <compile_commands.json> -lcrypt`
     compiles all 51 non-IMC `.c` as separate TUs, links (one shared c2m), runs comm.c
     `main` → "Realms of Despair ready … port N" + live game_loop (exit 124).

## 1. WHAT LANDED THIS SESSION (madc, on the branch)
- `2887740` **fix(project):** compile `.c` TUs in C mode under `--project` (`class` as a
  C identifier; was hit in TokenIF's C++17 if-init decl-probe).
- `1851d88` **fix(parser):** accept an empty translation unit (`services.c`, all `#ifdef WIN32`).
- `da4145c` **fix(parser): KEYSTONE — record the explicit `*` count for function-type-typedef
  declarators.** madc collapsed `DO_FUN g` (C function decl) and `DO_FUN *g` (fn-ptr var)
  into one bare `DataDefFPTR` (parser consumed the declarator `*` but never recorded it);
  emitter's implicit decay star always rendered one `*`, so `DO_FUN do_look;` emitted as a
  NULL fn-ptr global → multi-TU SIGSEGV / MIR repeated-decl (single-TU/umbrella masked it).
  FIX = count the stars, emit exactly that many; recorded on `Variable::fnptr_explicit_stars`
  (type stays `DataDefFPTR` so the ~40 fn-ptr-CALL sites are untouched); shared
  `Program::consume_declarator_stars` helper replaces the duplicated star-eating loops.
  NO implicit star, NO std-gating (an earlier suppress/std-gate attempt regressed the
  `testfnptr*` family and was reverted). Per-TU c2m also tried + reverted (unneeded).
- `e4005e0` **feat(cli):** `-l<lib>` (dlopen `lib<name>.so` RTLD_GLOBAL before run, so the
  import resolver finds its symbols at MIR link; general, works with/without `--project`) +
  `--help`/`-?`/`-h` (real usage screen — both agents had hit "Failed to open file").
- Synced: claude_status.json (`recent_fixes_2026_06_08`), CHANGELOG `[Unreleased]`,
  KG `Decision{smaug_boots_via_project_starcount}`, ~/.claude memory `project_build_driver`.

## 2. NEXT SESSION — DEFERRED #1 + #2
1. **Productize SMAUG `--project` in MadSMAUG.** The manifest is currently generated to
   `tmp/smaug_project_cc.json` from the upstream Makefile's `C_FILES` (51 non-IMC `.c`,
   `-DSMAUG -DREGEX -DREQUESTS`, `-I<src>`). Commit a generator (a script, or `bear -- make`)
   + a `MadSMAUG.sh --project` mode (`exec "$MADC" --project <db> -lcrypt "$@"` from the data
   tree). compile_commands.json carries no link info → `-lcrypt` supplies it (the spec's
   deferred "link-description" gap; SMAUG's Makefile = `NEED_CRYPT=-lcrypt`).
2. **Auto-`#load` toggle.** Embedded headers (e.g. `include/madc/crypt.h`) auto-`#load`
   libs; expose a CLI flag to turn that off (so linking is explicit via `-l`). Mechanism
   exists: `is_dynamic_library_loading_enabled()` / `registration_policy` (gates `#load` at
   `lexer.cpp:2240`). Likely `--no-auto-load` (or similar) flipping that policy.

## 3. EXACT COMMANDS
```bash
cd /workspace/madc
git rev-parse --short HEAD                 # 4aa0a20
make -C src fulltest 2>&1 | grep passed,   # 537/4
python3 scripts/run_gcc_testsuite.py --root gcc_testsuite --madc bin/madc  # 1566/31/57/1 (run ALONE)
bin/madc --help                            # usage screen
# Regenerate SMAUG manifest (51 non-IMC TUs) -> tmp/smaug_project_cc.json: see the
# python one-liner pattern in this session, or build it from upstream/smaug1.8/src/Makefile.
# SMAUG --project boot (SINGLE run, random port, NO loop; data tree = MadSMAUG/runtime/area):
cd /workspace/MadSMAUG/runtime/area
P=$((9000+RANDOM%400)); MADC_CPU_LIMIT=0 MADC_MEM_LIMIT=0 \
  timeout 200 /workspace/madc/bin/madc --project /workspace/madc/tmp/smaug_project_cc.json \
  -lcrypt smaug "$P" > /tmp/smaug.log 2>&1   # run_in_background; exit 124 = booted+serving
```

## 4. KEY ANCHORS
- Shared star helper: `Program::consume_declarator_stars` (src/parser.cpp); decl sites that
  record `decl_fnptr_stars` → `Variable::fnptr_explicit_stars` (3 `addVariable(*decl_type)`
  sites); emitter uses it at `cir_builder.cpp` (`decl_stars` near the old line 1854).
- `-l`/`--help` + dlopen loop + `print_usage` in `src/madc.cpp`.
- Multi-TU engine `madc_project_execute` + `build_tu_module` in `src/madc_cir.cpp`.
- `--project` reader/manifest: `include/madc_project.h`, `src/madc_project.cpp` (nlohmann/json).

## 5. PROCESS LESSONS
- **NAS mtime trap:** `touch src/<f>.cpp` before `make`; clean-rebuild when results look
  impossible (a "stale binary" scare cost time).
- **Exit-code gotcha:** a trailing `echo` in a backgrounded `timeout … ; echo` makes the
  TASK exit 0 regardless; read the real `timeout` code from the task stdout (124 = stayed up).
- **Single SMAUG runs, NO loops; random port; clear nothing — madc JITs in-process (no `.mir`
  cache).** Don't spawn `while kill -0 …; sleep` wait-wrappers (they linger as runaway shells).
- **Count the stars, emit exactly** — the right model for the fn-ptr fix; suppress/std-gate
  hacks were wrong and reverted.
END OF HANDOFF.
