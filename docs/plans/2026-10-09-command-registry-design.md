# The command registry's extension point and the modal prefixes (split arc 1A.2)

Phase 1A.2 of `2026-10-09-madc-madcide-split-arc.md`: madc's session command
table becomes the ONE registry of REPL commands that a module or a host
extends, and an entry's first character picks its MODE — D24's recognition
rule (`madc-repl-thonny-plan-2026-09-24.md` D24), as the owner refined it on
2026-10-09. 1B.6 (the ex core: colon mode), 1B.10 (`%git`) and 1D.18
(madcide's verbs) stand on it.

## 0. The rule (owner, 2026-10-09)

"Reserve `:` only for ed/ex/vim/neovim style commands, and use `.` `%` and
`?` for the REPL-specific commands … `:` in REPL mode calls colon-mode
commands … so the REPL is modal, supporting `.` `%` `?` and `:`, but each are
their own respective modes."

| Prefix | Mode | Reaches | Runs in |
|---|---|---|---|
| `%` | REPL command, IPython / clang-repl style | every session command by name; `%`-origin aliases (none yet; clang-repl's `%undo`, `%lib` are candidates) | the session |
| `.` | REPL command, cling / Node style | every session command by name; cling's and Node's aliases (`.L` `.x` `.q` `.exit` `.?`) | the session |
| `?` | query | `?` = `%help`, `?NAME` = `%pinfo NAME` | the session |
| `:` | colon mode | ed / ex / vim / neovim commands ONLY — the REPL owns no `:` name | the HOST (the process that holds the documents) |

This supersedes two earlier rulings: §7f's "an alias is valid under every
prefix" (`%L`, `:L`, `:?`, `%x`, `:q` worked) and D24's "`:` accepts the
session commands AND ex's" with its fall-through. Each alias now lives in the
mode of the REPL it came from; `:` names only colon-mode commands, so `:x`,
`:?re?`, `:q!` and `:exit` keep their vim meanings with nothing to collide.

## 1. What exists (2026-10-09, `fa8e5214d`)

- **The table** (`src/madc_session.cpp`): `command_rows` (11 rows: name,
  `session_command` code, usage, summary) and `alias_rows` (`?` `L` `q` `exit`
  `x`). `command_named` resolves a typed name to its row once; `run_command`
  switches on the code.
- **Recognition** (`command_text`): the first line's first non-blank is `?`,
  or `%` `:` `.` followed AT ONCE by a name (a word run, not starting with a
  digit) or `?`. `:5`, `:%s/a/b/` and `% x` are not commands today: they are
  offered as C and refused as C.
- **Where commands run**: in the session — `InteractiveSession::enter`, in
  this process (Windows `madc -i`, unit tests) or the backend
  (`serve_session`, `src/madc_session_client.cpp`). What the session cannot
  do itself it asks its host for with a PAYLOAD (IPython's): `%load`, `%run`,
  `%call`, `%build`, `%open`/`%edit`, `%quit` — the reply's `payload` +
  `argv`, honoured by `BackendSession::honor` (madc -i) or by the dialect host
  reading `madc::session_poll`'s row (madcide).
- **The only extension point** is madcide's: `plugin_repl_command`
  (`tools/madcide/madcide_plugins.inc:858`) keeps rows in madcide's bag, and
  `repl_ide_command` (`madcide_repl.inc:573`) answers them BEFORE the session
  sees the entry, asking the static `madc::session_command_of` whether the
  session owns the name; `%help` there is the session's text plus madcide's
  rows appended (`repl_ide_help`). `madc -i` has no hook.

## 2. Design

### 2.1 Recognition

An entry is a command when its first line's first non-blank is `%`, `.`, `?`
or `:` — EXCEPT three spellings that stay C: `.` followed by a digit
(`.5 * x`), `::` (C++ global scope) and `%:` (the `#` digraph). The first
character is the MODE (§0). Continuation lines are untouched (only an entry's
first line is read), so a designator's `.x` stays C. A recognised entry is
never parsed as C: it runs or is refused.

- `%` and `.`: what follows is a NAME (the word run, as today; or `?` under
  `.`, cling's `.?`). No name there (`% x`, `.,`) is a refused command.
- `?`: unchanged.
- `:`: everything after the `:` on the first line is the colon command's
  text (`%s/a/b/`, `1,5d`, `w file.c`, `!ls`), handed whole to the colon mode
  (1B.6's parser reads addresses and names). The session never runs one: a
  host with a colon mode answers it before the session sees the entry (§2.5),
  and the session refuses one offered to it.

The mode is an enum in `<bits/session_enums>` (the dialect's text too):

```cpp
enum class command_mode : unsigned char { none, percent, dot, query, colon };
```

`InteractiveSession::command_of` (the static question a host asks) answers
the mode beside `word`, `command` and `argument`; for `:` the word is `""`
and the argument the colon text. `madc::session_command_of`'s row gains
`mode`.

### 2.2 One table of REPL commands

| Row | Registered by | Its handler runs | Modes |
|---|---|---|---|
| built-in (`help` … `quit`) | the table (compile time) | the session (`run_command`'s switch) | `%` `.` |
| alias (`?` `L` `q` `exit` `x`) | the table | its command's | its origin's: all five are `.` |
| **session-registered** | code running IN the session: a module loaded there (`-l`, madc.ini `modules` (1A.4), `import` at the prompt) | the session, in the session's process | `%` `.` |
| **host-registered** | the process DRIVING the session (madc -i's prompt, madcide) | the host: the session answers with a payload | `%` `.` |

A registered row is `{name, usage, summary}` plus its handler (a session row)
or its host CODE (a host row). ONE namespace: a registration is refused
(false) when the name is not a whole command name (§2.1), or is a built-in's,
an alias's, or a registered row's already. First registration wins. A typed
name resolves to its row ONCE (`command_named`, now given the mode), and
every later step dispatches on the row (enum-over-strings).

### 2.2a Aliases across modes (owner, 2026-10-09)

"It is possible, though, for one mode's command to be an alias for another
mode's command." An alias row is `{mode, name} → {target mode, target
command}`, and the target runs where its mode runs:

- a REPL alias of a REPL command (`.L` → `%load`, every alias today):
  dispatched in the session on the target's code;
- a COLON alias of a REPL command (`:help` / `:h` → `?`, as D24 lists;
  `:q` → `%quit`): held in the colon mode's table (1B.6) and resolved there;
  the colon mode issues the target to the session;
- a REPL alias of a COLON command (a candidate: clang-repl's `%undo` → ex's
  `:u`): held in the session's table and resolved there; the session answers
  with `session_payload::colon` and the target's colon text, and the host's
  colon mode runs it — the same payload road as a host row (§2.3).

1A.2 gives the session's alias rows their mode and their target; the
colon-side rows and the first cross-mode aliases land with 1B.6, the first
table that can be a target or hold one.

### 2.3 Running a registered command

- **A session row**: `run_command` calls the handler inside the Program's
  runtime scope (`push_runtime_scope` / `pop_runtime_scope`, as every host
  call into JIT code), with the argument (the first line after the name,
  leading blanks dropped). One shape, C++ and dialect alike:

  ```cpp
  bool handler(madc::value &out, const char *argument);
  ```

  True: `out`'s text is what the command shows (`shown()`, like `%type`'s).
  False: the command is refused; `out`'s text, when not empty, is its error
  diagnostic, citing the argument's column. A crash in a handler is a crash
  in the session: in a backend the session restarts, as for an entry.
- **A host row**: the session takes the entry (numbered, kept in history) and
  answers with a new payload, `session_payload::command`, the row's CODE and
  `argv` = `{argument}`. The host dispatches on the code; nothing runs in the
  session.

### 2.4 Unknown and `:` texts in the session

- `%name` / `.name` with no row: refused as today — `unknown command '%x'
  (%help lists them)`, citing the prefix's column; a nameless one cites its
  first word (`unknown command '%'`).
- A `:` text offered to the session (its host has no colon mode, or passed
  it on): refused — `':w' is a colon-mode command, and there is no colon
  mode here (%help lists the REPL's commands)`. Until 1B.6, `madc -i` is such
  a host.

### 2.5 Colon mode is the host's

Colon commands edit documents, and documents live in the process that edits
(1A.1), never the one that runs user code. So the colon mode is a host
component: at the plain prompt, 1B.6's ex core, which the REPL loops
(`madc_repl_edit`, `madc_repl_run`) consult for every `:` entry before
offering anything to the session; in madcide, its colon verbs plus madc's ex
core (1D.18). Its own table (ex's commands, madcide's `view*` verbs) is 1B.6's
design; 1A.2 only fixes that `:` goes there and nowhere else.

### 2.6 `%help` and completion

`%help` lists the one REPL table: the built-ins (each with its aliases,
written with their mode: `(also .L)`), then every registered row in
registration order (its usage as the registrant wrote it, and its summary),
then the footer — every name is also written with `.`; `?NAME` asks about a
name; `:` starts a colon-mode command. Completion of a command's name offers
every row valid in the typed mode, aliases included. madcide's appended
`IDE commands:` block retires when its rows move into the registry (1D.18).

### 2.7 The doors

**C++ (the one implementation; cpp-first-api).**

```cpp
// InteractiveSession (madc_session.h) — the table's owner:
typedef bool (*CommandHandler)(madc::value &out, const char *argument);
bool add_command(const std::string &name, const std::string &usage,
		 const std::string &summary, CommandHandler handler);	// session row
bool add_host_command(const std::string &name, const std::string &usage,
		      const std::string &summary, int64_t code);	// host row
```

Code running in the session reaches it through its Program:
`Program::session` (a non-owning `InteractiveSession *`, set by `begin()`),
read through `Program::active_runtime_program()`.

**The wire (backend).** The client keeps its host rows and sends them as one
`commands` request after every backend start (the first and each restart, so
a crash-restart keeps them), before any entry; the backend replaces its host
rows with them. No reply (the stream is ordered; `stdin_renew` is the
precedent). An offer's reply carries `payload` = `command`, `argv` and
`code`.

**Host side, C++.** `SessionClient::set_host_commands(rows)`;
`BackendSession::add_command(name, usage, summary, handler)` with a
`std::function<bool(const std::string &argument, std::string &out)>`, its
code assigned by the BackendSession, honoured on the `command` payload (out
shown on success, said on the error stream on refusal).

**Dialect** (`<ns_madc>`, mangled-direct):

```cpp
// in the session — a module's own registration:
bool command_add(const char *name, const char *usage, const char *summary,
		 bool (*handler)(value &out, const char *argument));
// on a session handle — a dialect host (madcide in 1D.18):
bool session_command_add(int64_t h, const char *name, const char *usage,
			 const char *summary, int64_t code);
```

`command_add` answers false outside a session (a plain `madc file.mad` has
no registry) and on a taken name. A dialect host reads the payload from the
offer reply's row (`payload`, `argv`, `code`) and runs its own command.

### 2.8 What changes for users and for madcide

- At `madc -i`: `:type`, `:help`, `:L`, `:q` and the other colon spellings of
  REPL commands are refused until 1B.6 brings the colon mode (where `:q` and
  `:help` are ex's); `%L`, `%x`, `%q`, `%exit` and `%?` are refused (their
  aliases are cling's and Node's, `.` only). On the arc's branch, never in a
  release: the seam is the end of Phase 2.
- madcide: `session_command_of` answers every REPL command as before plus
  `mode`; a `:` text in its REPL pane was refused by the session before and
  still is (its pane reaches `colon_command` in 1D.18). madcide moves onto
  the doors in 1D.18: its REPL rows become host rows on its handle, and the
  §7f order "the IDE answers before the session sees the entry" becomes "the
  session resolves, the IDE runs" — one resolver, the same result for the
  user.

## 3. Thread safety

Registered rows are per-session state, written when a module or the host
registers and read at input, on the session's thread (D9: one session is
driven by one thread). A host's rows live on its client handle, confined to
the thread that opened it, as every handle is. `Program::session` is a
per-Program pointer. No globals.

## 4. Tests

- Unit (`tests/unit/test_repl_session.cpp`): the modes — `:type x`, `:L` and
  `:%s/a/b/` refused as colon-mode commands; `%L`, `%?` unknown; `.L`, `.?`,
  `.x`, `.q`, `.exit` resolve; `.5`, `::x`, `%:` stay C; `% x` refused; the
  static `command_of` answers each mode. A session row registered from C++:
  runs, shows `out`; a false answer's `out` is its diagnostic; `%help` lists
  it; completion offers it under `%` and `.`. Refusals: a built-in's name,
  an alias's, twice, a non-name. A host row: taken, payload `command` + code
  + argument, nothing run.
- Integration (`tests/testcommandreg.mad`): the dialect doors over a real
  backend — `madc::session_command_add` on a handle and the offer reply's
  payload / code / argv; a session row registered from an entry
  (`madc::command_add`, then `%name ARGS` runs it, its output shown, `%help`
  listing it); `madc::command_add` outside a session answers false.
