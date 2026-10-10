# Windows `madc -i` gets the crash-isolated backend (split arc 1A.5)

Design for Phase 1A step 5 of `2026-10-09-madc-madcide-split-arc.md` (owner
2026-10-09: make Windows work like Linux and macOS; INTERACTIVE ONLY).

## 1. What exists (2026-10-10)

- POSIX `madc -i` (`run_repl`, `src/madc.cpp`): a `SessionClient` forks the
  backend; the child's Program is the fork's copy of the CLI's configured
  one (`--std`, `-I`, `-D`, `-l`, madc.ini), copied again on each restart. A
  crash in an entry ends the backend, and a fresh session starts.
- Windows `madc -i`: no fork, so the session runs IN the CLI process; a
  crash ends madc. `testrepl_crash` and `testrepl_runcmd` carry
  `.win64_skip` / `.wine64_skip` saying so.
- Windows `madc::session_open` (a script's or an IDE's session) already has a
  backend: a run child of self (`SessionClient::spawn_backend`, kind
  `rckSession` in `MADC_RUN_CHILD`), which builds a bare `Program` and
  applies the request's `std_option` — the POSIX fork child's `new Program()`
  twin. A configured Program cannot cross (`make_program` is refused there).

## 2. Decisions

1. **The child configures itself through the CLI's own parser.** The REPL
   host passes its OPTIONS — every argument before the first positional —
   as the child's command line. A valid `madc -i` line carries no mode or
   output flag (the parent refuses `-i` with an artifact or a dump), so the
   options ARE the configuration: `--std`, `-I`, `-D`, `-l`, `-stdlib=`,
   `--config` / `--no-config`, the forest and warning flags. The child runs
   `main`'s argument parse, madc.ini and the `-l` loop exactly as the
   parent did, then serves the session where the parent would have started
   the REPL. No second list of "configuration options" exists to drift.
2. **A new run-child kind, `rckConfiguredSession`** (`configured-session`):
   served by the CLI's `main` only, after its configuration; the bare
   `rckSession` keeps serving `madc::session_open` before `main` (any image:
   the CLI, madcide, chthonia) with a bare Program, unchanged. The request
   rides `MADC_RUN_CHILD` (endpoint, token, terminal flag) as before; only
   this kind's command line is the parent's options.
3. **Every restart re-spawns with the same options** (`SessionClient`
   keeps them; `restart` → `start` → `spawn_backend`).
4. **The in-process Windows branch of `run_repl` is deleted**: one
   `SessionClient` path, the fork's factory on POSIX, the options on Windows.
5. **Interactive only**: the backend is reached from `run_repl` and
   `madc::session_open`; running a file, `-c`, `-o`, `--project` and `--emit`
   start no child.
6. **A piped stdin on genuine Windows is a measured limit.** Host and
   backend share stdin and both read it unbuffered (the POSIX design: an
   entry's `scanf` takes what it parses, the host's next line follows). The
   UCRT reads an unbuffered stream two bytes at a time — measured on the
   owner's Windows 11 box: after `scanf("%d")` on a piped `4321\nn * 2`, the
   next OS byte is ` `, the `n` sitting in the backend's FILE — so an entry
   that reads a PIPED stdin can take one byte past what it parses, which the
   host then never sees. A console delivers whole lines (CRLF is two bytes)
   and is exact; wine's CRT reads byte by byte and is exact.
   `testrepl_stdin` carries `.win_skip` with the measurement. **Owner call:**
   accept the limit, or keep a Windows REPL whose stdin is a pipe (a
   transcript fed to `madc -i`) in-process, without crash isolation.

## 3. Commits

1. The kind, `main`'s deferred serve, `run_repl`'s one path,
   `SessionClient::set_backend_arguments`; `testrepl_crash` and
   `testrepl_runcmd` lose their Windows skips (they are the reducers: a
   crashing entry restarts the session; `%run` restarts the backend);
   `testrepl_stdin` gains `.win_skip` (decision 6). Tier 1 on the wine
   domain (`MADC_SKIP_EXT='win64 wine64'`) and on genuine Windows
   (`scripts/win_suite.sh`, the packed PE) + Tier 2. The stop line's
   wording on Windows ("signal 0") is its own fix, the commit before.
2. Docs + mirrors.

## 4. Thread safety

Process-level: the child is a separate process; the request is read once at
`main`'s start, before any thread, into a local of `main` (no global).
