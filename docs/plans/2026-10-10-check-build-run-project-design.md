# `%check`, `%build` / `%run` on a buffer, `%args`, `%project` — design (split arc 1B.8)

Status: DESIGN, 2026-10-10. Arc: `2026-10-09-madc-madcide-split-arc.md`,
Phase 1B item 8 (owner: build and projects are madc's). Inventory section C
(`2026-10-09-madc-madcide-inventory.md:94-105`).

## 1. What exists

- **The engine's owners**, all in libmadc: `parse_check` (a parse handle's
  diagnostic rows: severity, phase, message, file, line, column), `parse_build`
  (a live tree to a native image), `project_open` / `project_tus` /
  `project_build` / `project_run` (`src/ns_madc.cpp:255-483`,
  `src/madc_program.cpp`), the manifest READER `read_project_manifest`
  (`src/madc_project.cpp:246`: an object is madc's manifest — `tus`, `entry`,
  `output`, `kind`, `icon`, `libs`, `madc`; unknown keys ignored).
- **No manifest writer** in C++: madcide's `proj_write` / `proj_add` /
  `proj_del` / `proj_toggle_kind` (`madcide_core.inc:5210-5409`) write it with
  `js::stringify(proj, 2)` — nlohmann's `dump(2)`, keys sorted.
- **The session's file commands**: `%build FILE [-o OUT]` and `%run [-i] FILE
  [ARGS]` answer payloads; a payload host supplies the file's text (madcide:
  the open buffer's live text) or the disk's; the CLI host reads the disk
  (`BackendSession::build_file` / `run_file`), though the wire carries text
  (`build` + `text`, `load` + `text`). A FILE must open on disk before any
  payload (`file_command`).
- **No `%check`**; madcide's `check` (`run_check`) and its documented, never
  handled `:check`.
- **Program args**: madcide's `progargs` (a prompt; `program_argv` splits with
  `ns_common::shell_words`, as `%run` does); `parse_run` and `project_run` pass
  `argv[0]` only.
- **The CLI host's buffers**: `repl_loops` (`src/madc.cpp`) holds the ex
  session over its own world — `madc -i FILE`'s buffer, `:e`'s. The session
  (a backend process) has none.
- **Restarts**: `%run`, `%reset`, `%std` and a crash start a fresh backend;
  what the host must restore it re-sends after every start (the `commands`
  and `history` requests).

## 2. Decisions

1. **The buffer is the host's; the work is the backend's.** A command on a
   buffer names it (FILE) or not (the host's CURRENT buffer — the ex
   session's); the session answers a payload whose FILE is "" for "the
   current buffer", the host resolves it to a path and its LIVE text, and the
   backend parses, builds or runs that text (crash isolation, D2). A FILE no
   buffer holds is the disk's. A host's resolver: `ReplSession::set_buffers`
   (a function from a name, "" = current, to path + text; the CLI's reads the
   ex session's world). An untitled buffer is refused (`:w FILE` names it),
   as madcide refuses it. A session no payload host drives reads FILE from
   disk and refuses a missing FILE.
2. **`%check [FILE]`** — the buffer's diagnostics, rendered as an entry's
   are, under the session's standard; shown: `FILE: clean` or `FILE: N
   errors, M warnings`. The `check` payload; the wire's new `check` request
   (path + text) runs `InteractiveSession::check_text` in the backend, which
   reads through `parse_check`'s owner (a child parse; nothing joins the
   session). madcide honors the payload with its `run_check` (Problems).
   Colon mode gains no `:check` (ex has none).
3. **`%build [FILE] [-o OUT]`, `%run [-i] [FILE] [ARGS]`** — FILE optional:
   the current buffer's live text (`build` + text; `load` + text, then the
   run). OUT defaults to the buffer's path without its extension. `%run`
   with no FILE runs with the program args `%args` set. `%run FILE ARGS` is
   unchanged.
4. **`%args [ARGS]`** — the program's arguments for `%run` of a buffer
   (madcide's `progargs` at the prompt): no ARGS prints them; ARGS (shell
   words) replaces them; `%args --` clears them. HOST state, as madcide's
   is: the `args` payload, argv the argument line; the host keeps them across
   restarts and composes `%run`'s argv. madcide honors it into `progargs`.
5. **The manifest's one writer**, beside its reader (`src/madc_project.cpp`,
   `include/madc_project.h`): `project_manifest_create` (`{"entry": "main",
   "tus": []}`), `project_manifest_add_tu` / `project_manifest_remove_tu` (a
   TU named as the manifest names it, relative to its directory when it lies
   below it), each a read-modify-write of the JSON object that keeps every
   key it does not change (madcide's `commands`), written as
   `dump(2)` + newline — madcide's bytes. madcide's writers move onto it in 1D.
6. **`%project`** — the session's project, read and written through the
   owners above:
   - `%project` — the manifest, its entry, output, kind and TUs;
   - `%project MANIFEST` — open it; a missing file is created (decision 5);
   - `%project add FILE...` / `%project del FILE...` — the TU list;
   - `%project check` — every TU's diagnostics (`project_open` /
     `project_tus` / `parse_check`); shown: a count per TU with problems;
   - `%project build [-o OUT]` — `project_build` (OUT: the manifest's
     `output`, else its base name);
   - `%project run [ARGS]` — `project_run`, with ARGS (`project_run` gains
     the program's arguments);
   - `%project close`.
   The open manifest is session state the HOST restores: opening or closing
   answers the `project` payload (argv: the absolute path, "" for none); the
   CLI's client re-sends it after every start (the `history` request's
   route); madcide opens its project there (`proj_load`).
7. **Thread-safety**: the C++ standard library's convention. One session and
   its host are confined to one thread each (D9); the manifest writer is a
   read-modify-write of one file — two writers race as two editors saving
   one file do (last write wins); no new global state.

## 3. Slices (each its own commit, Tier 1 + Tier 2)

1. **`%check`** — `check_text`, the `check` request and payload, the CLI's
   buffer resolver, madcide's honor.
2. **Buffers for `%build` / `%run`, and `%args`** — FILE optional, live text
   on the wire, the `args` payload.
3. **The manifest writer** — create / add / remove, unit-tested against
   madcide's bytes.
4. **`%project`** — the subcommands, the `project` payload and request,
   madcide's honor; `project_run` with arguments.

## 4. Oracle

gcc `-fsyntax-only` (a check prints diagnostics and builds nothing), IPython
`%run FILE ARGS`, madcide's own commands (check, build, F5, progargs, the
project menu) for the shapes moving down, and madcide's manifest bytes for
the writer. Tests: unit (`test_repl_session`, a manifest-writer case), and
`tests/testrepl_*` over piped `madc -i` (stdout only: refusals are checked by
their effect).
