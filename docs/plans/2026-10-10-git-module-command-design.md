# `%git` as the madcgit module's command — design (split arc 1B.10)

Status: DESIGN, 2026-10-10. Arc: `2026-10-09-madc-madcide-split-arc.md`,
Phase 1B item 10 (owner 2026-10-09: "%git depends on the git module, so it
would only be present if perhaps the madc.ini or a command line option
indicated to load that module"; `import madcgit;` at the prompt: yes).

## 1. What exists

- **The module**: `src/modules/madcgit/madcgit.cpp`, a read-only view of a
  local repository over libgit2 (statically linked into `libmadcgit`). Its C++
  class `madc::GitRepo` (open, head, revparse, log, commit, show, blame,
  blame_buffer, dirty, status, diff) is the one caller of libgit2; its C
  interface `madcgit_*` (`include/madc/madcgit.h`) is what `import madcgit;`
  binds. The map row (`src/madc_modules.cpp:26`) is LAZY: an import binds
  typed first-call slots and opens nothing.
- **The verbs**: madcide's `tools/madcide/madcide_git.inc:85-613` — `log`,
  `show`, `blame`, `status`, `diff`, git's spellings (`git_relative_date`,
  git's date.c thresholds; `git_state_letter`; the log line; the blame line)
  and the dispatch `git_command`. Each prints what git prints for the same
  question. They are dialect code over `git::*`, reading a buffer's live text
  and repository handle from madcide's state (`w`, `es`).
- **The registry's door** (1A.2, `2026-10-09-command-registry-design.md`
  §2.3, §2.7): a session row `{name, usage, summary, handler}` whose handler
  `bool (*)(madc::value &out, const char *argument)` runs in the session,
  inside the Program's runtime scope (`run_registered`, "or a native
  module's"); code reaches the session through
  `active_runtime_program()->owning_session`.
- **The host's buffers** (1B.8): `ReplSession::BufferSource` — a FILE (""
  the current buffer) to its path and LIVE text. Only the host asks it
  (`BackendSession::resolve`), before it sends a payload's request; the
  session never asks. `InteractiveSession` keeps none.
- **Loading a module**: `-lNAME` and the ini's `modules` key open it in the
  configured Program (`loaded_lib_paths`); `import NAME;` in an entry opens an
  eager row (`loaded_lib_paths`) or records a LAZY one
  (`module_optional_libs`). No module gets control when it loads.

## 2. Decisions

1. **The verbs move into the module, in C++.** `src/modules/madcgit/
   madcgit_verbs.cpp` beside `madcgit.cpp`, over `GitRepo` directly: the
   five verbs, git's spellings, the dispatch, and the usage line, with the
   same texts and refusals madcide's verbs have (its test's `.expect` is the
   oracle's record). FILE's live text and the current buffer come from the
   session's buffer question (decision 4). The repository: the one above
   FILE; else the current buffer's file's; else the working directory's —
   opened for the verb and closed before it returns.
2. **A module brings commands as data.** The map row gains `commands`: the
   name of the module's exported function that returns its command rows
   (`{name, usage, summary, handler}`; NULL: the module brings none). One
   header, `include/madc_module_commands.h`, holds the row shape. madcgit's
   row names `madcgit_commands`, one row: `git`. A module's command is a
   SESSION row: its handler runs in the session (D2: in the backend).
3. **Registered on load.** A session registers a module's rows when the
   module enters its Program: at `begin()` (the configured Program's `-l` and
   ini modules) and after each taken entry whose parse added a spelling to
   `loaded_lib_paths` or `module_optional_libs`. For each spelling it has not
   registered yet whose row names `commands`: open it (`madc_module_open` —
   a LAZY module opens here, as its first call would; one that does not open
   brings no command, as `module_available` answers false), look the
   function up (`madcdl_sym`), add each row (`add_command`; a taken name is
   refused, the first registration stands). A restart begins afresh: the
   configured Program's modules register again at begin; an `import` typed in
   the ended session went with it, as its names did. Without the module,
   `%git` is an unknown command like any other (registry §2.4).
4. **The session's buffer question.** `InteractiveSession::buffer_text(name,
   path, text, has_text, why)` asks the session's BufferSource
   (`set_buffers`, which `InteractiveSession` now keeps; none: the name is the
   path and the disk is read). In a backend, `serve_session` installs a source
   that asks the HOST: a `buffer` request mid-entry (backend to host:
   `{op: buffer, name}`), which the host answers from inside its wait
   (`SessionClient::poll` answers it and keeps waiting, as it takes a running
   notice) with its own source (`SessionClient::set_buffers`, given by
   `BackendSession::set_buffers`); a host with none answers "no buffer". The
   CLI's colon mode (1B.6) is the CLI's source, so `%git diff FILE` at
   `madc -i` diffs an edited buffer's unsaved text, as madcide's does.
5. **madcide**: its Changes / History / Blame views stay its own and load
   the module themselves. Its `%git` verbs are the second implementation
   until **1D.18** (deletion deadline), when its REPL pane resolves through
   the session: `%git` is then the module's row, and madcide's buffers answer
   the buffer question through its session handle. `testmadcide_git` runs
   every commit until then.
6. **Thread-safety** (C++ standard library convention): the verbs run on the
   session's thread; a repository handle lives inside one verb; the
   registered-spellings set is the session's state; the buffer question is
   one request at a time on the session's wire.

## 3. Slices (each its own commit, Tier 1 + Tier 2)

1. **The door**: the row's `commands`, the row header, registration at
   begin and after an import. Unit tests: a row table in the test binary
   registered through the same step (`madcdl_open_self`), once; a taken
   name refused; an unopenable module brings nothing.
2. **The buffer question**: in-process and over the wire. Unit tests: a
   handler asks for a buffer in-process and through a backend whose host
   answers; a host with no source answers "no buffer".
3. **`%git`**: the verbs in the module, `madcgit_commands`, the integration
   test (`testrepl_git`: `-lmadcgit`, the shapes madcide's test asserts, the
   refusals, `import madcgit;` at the prompt, `%git` unknown without it).

## 4. Oracle

git itself, verb by verb (the command each verb names: `git log
--format='%h %s (%ar, %an)'`, `git show`, `git blame --date=relative
[--contents]`, `git status -sb`, `git diff HEAD -- FILE`), as madcide's
verbs already match it; IPython's extension model (`%load_ext` registers
magics when the extension loads) for registration on load.
