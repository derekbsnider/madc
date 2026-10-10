# `%history`, `%std`, `%reset` and the completion rule — design (split arc 1B.7)

Status: DESIGN, 2026-10-10. Arc: `2026-10-09-madc-madcide-split-arc.md`,
Phase 1B item 7. Inventory rows: History, Completion insert rule, `%std` /
`%reset`, Session standard (`2026-10-09-madc-madcide-inventory.md:112-124`).
Oracles: IPython (`%history`, `%reset`, the kernel's history), Julia (the
history file's records, per-mode recall), readline (Tab).

## 1. What exists

- **The history file** (plan §41.7a slice 2): Julia's records (time, mode =
  the standard's canonical name, the entry's lines after a tab), the format
  in `include/madcdis/line_edit.h` (`history_record`, `history_records`).
  The terminal loop (`madc_repl_edit`, `src/madc_repl.cpp:52-161`) owns its
  path, reads the entries typed in the session's LANGUAGE (C, C++, madc) into
  a `line_history` ring for Up and Ctrl-R, and appends each entry as it is
  taken. The cooked loop keeps none. `--history-file=PATH|no` chooses it.
- **madcide's copy**: an in-memory ring per pane (`madcide_repl.inc:124-200`).
- **The session numbers its entries**: `REPL[N]` counts every entry taken,
  refused ones and commands included (`src/madc_session.cpp:449-486`) — IPython's
  `In [N]`. It keeps no entry's text.
- **A fresh session is the host's**: `%run FILE` answers the `run` payload,
  and the host restarts its backend (`BackendSession::honor`,
  `client.restart()`); `SessionClient::restart(std)` already starts again
  under another standard (madcide's language change). A host that cannot
  restart refuses (`%run` without `-i`).
- **The completion rule, twice**: `line_edit::completed` (one name replaces
  the word; several insert their common prefix when it extends the word,
  else a second Tab lists them) and madcide's `repl_completed`, which lists
  on the first Tab and does not ask that the prefix extend the word.
- **The `--std=` spellings** are read by `Program::set_language_standard`
  (`src/parser.cpp`), which changes the Program as it reads.

## 2. Decisions

1. **One store: the history file.** Its reading and writing get one owner,
   `include/madc_history.h` / `src/madc_history.cpp` (`madc::history_*`):
   the default path (`madc_repl_history_path` moves there), the entries of a
   standard's language (`history_entries`), one record appended in one write
   (`history_append`, owner-only, O_APPEND: two sessions' records interleave
   whole). The format stays `line_edit.h`'s. The terminal loop reads and
   writes through it; the session reads through it. madcide's pane moves
   onto it in 1D (item 18), where its script publics bind with their
   consumer.
2. **`%history` is the session's built-in** (IPython's: the kernel answers
   it, so every host gets it — the CLI, madcide's pane, a plugin's):
   - `%history` — this session's entries, `REPL[1]` on: the session keeps
     each entry's text under its number (a refused one and a command too, as
     `REPL[N]` counts them). A multi-line entry prints its lines.
   - `-n` — each prefixed with its number (IPython's).
   - `-l [N]` — the store's last N entries in this language (default 10).
   - `-g [TEXT]` — the store's entries in this language holding TEXT, oldest
     first (Ctrl-R's rule: a literal substring); bare `-g`, all of them.
   The store is the file its host names: a one-way `history` request after
   every start (the `commands` request's route), so the CLI's
   `--history-file` reaches the backend on both OSes; `no` names none, and
   `-l` / `-g` then refuse. A session no host named one for reads the
   default store.
3. **`%reset`** — a fresh session under the same standard, nothing printed
   (IPython's `%reset -f`): the `reset` payload, honored by `restart()` as
   `%run`'s fresh session is. A host that cannot restart refuses, as `%run`
   is refused.
4. **`%std [NAME]`** — no NAME: the standard in force, by its `--std=`
   spelling (`gnu17`, `c++20`, `madc`). NAME: any spelling `--std=` takes.
   A different standard is a fresh session under it (plan D4: switching
   mid-session is unsafe): the `std` payload, honored by `restart(NAME)`;
   the same one changes nothing. An unknown NAME is refused in the session
   before anything restarts (`%run`'s FILE precedent). The spelling reader
   becomes one static owner, `Program::standard_of_spelling` (standard +
   GNU flag, no Program changed); `set_language_standard` calls it. Later
   restarts keep the new standard.
5. **The host follows the standard**: after an entry, a loop whose session's
   `standard_name()` changed re-keys its prompt and reloads its ring from
   the store for the new language (Julia: each mode recalls its own).
6. **madcide honors `reset` and `std`** as its own restart and language
   change do (`repl_restart`, the session standard); every other payload
   kind stays a file payload. Nothing else of madcide moves before 1D.
7. **The completion rule's one owner**: `madc::hub::completion_choice(word,
   candidates)` in `line_edit.h` — one candidate: insert it; several: their
   common prefix when it extends the word, else list; none: nothing.
   `line_edit::completed` applies it (the second-Tab listing is the
   editor's, readline's). madcide's `repl_completed` moves onto it in 1D.

## 3. Slices (each its own commit, Tier 1 + Tier 2)

1. **The store and `%history`** — `madc_history.{h,cpp}`, the terminal
   loop on it, the session's entry texts, the `history` request, the row.
2. **`%reset` and `%std`** — the spelling owner, the rows, the payloads, the
   hosts (both loops re-key; madcide honors both).
3. **The completion rule** — `completion_choice`, `line_edit::completed` on it.

## 4. Oracle

IPython 8 (`%history`, `-n`, `-l`, `-g`; `%reset -f`), Julia 1.10 (records,
per-mode recall), readline 8 (Tab, Tab Tab). Tests: unit (`test_repl_cli`,
`test_session`), and `tests/testrepl_history.mad` / `testrepl_std.mad` over
piped input (stdout only: refusals are checked by their effect).

## 5. Thread-safety contract

The C++ standard library's convention: one session and its loop are confined
to one thread (D9); a store append is one O_APPEND write, so concurrent
sessions — threads or processes — interleave whole records; reads take a
snapshot of the file. No new global state.
