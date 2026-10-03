# Chthonia on Windows and macOS, self-contained — plan (2026-10-03)

## 1. The owner's direction (2026-10-03, queue item 5)

"we need to be able to have the chthonia self-contained binary, and make sure
it works fully under Windows and MacOS without any weird errors". The owner
put it after the REPL fixes, `--std=` and the toolbar icons (all done), and
before the TUI (last).

The plugin design settled what the product is (`docs/plans/2026-09-30-madcide-plugins.md`
§9.3 and §9.4, owner 2026-09-30):

- chthonia is its own binary: the chthonia plugin linked in, its bundle's
  data baked in, its own name and configuration directory, GUI by default.
- The standalone package ships libmadc, madc and the `chthonia` binary.
- It ships with "the Windows REPL backend (a child of self: chthonia's shell
  on Windows)".

## 2. What the Windows hands-on showed (v0.101.0)

| Symptom | Cause | State |
|---|---|---|
| Run in `madcide.exe` printed madcide's usage (exit 2) | The Windows Run spawned the running executable with `--run-frozen=` on its command line; only the madc CLI reads that option (from 7225fa035, 2026-09-08). Another AOT program re-ran itself without end. | **Fixed** 6a8a692b4 |
| The REPL tab: "the backend process is POSIX-only for now (plan 41.9a)" | `SessionClient::start` forks; its `_WIN32` arm refuses | Step A |
| 12 s before the window appears | Not traced. Launches without a window measured under 0.3 s, so the cost is in the window path | Step B |
| The macOS tarball has no madcide | An AOT madcide binds libmadc's value runtime; on macOS that is `libmadc-0.dylib`, the darwin port's D5, not built yet | Step C |

## 3. Done: the run child (6a8a692b4)

`include/madc_run_child.h` owns a Run in a child of self where there is no
fork. The parent sets the request in the child's environment
(`MADC_RUN_CHILD=<kind> <path>`) and spawns its own executable
(`madc_run_child_process`). The child serves the request before its own `main`
(`madc_serve_run_child`). Two places call it: the madc CLI's `main`, and a
namespace-scope initializer in `<ns_madc>` (the `<iostream>` `ios_base::Init`
idiom). Any program that can call the run verbs includes `<ns_madc>`, so it
serves its own child: madcide.exe, chthonia, or a user's program. Precedent:
Python's multiprocessing spawn of a frozen executable.

Kinds today: `frozen` (a live parse frozen to a snapshot: `parse_run`,
`madcrun://`) and `project` (`project_run`, `madcproj://`). Gates:
`testparserunfrozen` and `testprojectrun` on every domain; their win64
`--exe` pass is the one that was red. No lane runs that pass (B166).

## 4. Step A — the Windows REPL backend: a third kind of the same owner

The POSIX backend (`src/madc_session_client.cpp`) is a fork child with three
channels:

1. the requests and replies: JSON lines on a `socketpair` (Jupyter's message
   shape);
2. the program's stdout and stderr, merged: the output pipe;
3. the program's stdin: the child's stdin (`session_input`).

On Windows the child is a run child of kind `session` (`MADC_RUN_CHILD=session
<port> <token> <std>`). Channels 2 and 3 are the child's stdio pipes, which
the Process owner already gives a Windows child. Channel 1 cannot be a
`socketpair` (Windows has none) and should not ride inherited handles. Here
the child connects to a loopback listener the parent opened (127.0.0.1, an
ephemeral port) and sends the token as its first line. The parent accepts one
connection and checks the token. That is Jupyter's kernel transport: ports
plus a key, in its connection file. madc's session already speaks Jupyter's
message shape. Python's `socket.socketpair()` on Windows is the same loopback
emulation.

- `serve_session(fd, …)` reads its requests with `::read` and writes with
  `::send`. On Windows a SOCKET is not a CRT descriptor, so both sides use
  `recv` / `send`, behind one socket I/O helper that both arms share.
- The client waits on the reply socket and the output pipe. The task reactor
  already has the Windows arms (`WSAPoll` for sockets, `PeekNamedPipe` for
  pipes, `src/madc_task_chan.cpp`). The client's `poll` and `session_readable`
  case adopt them; no second readiness owner.
- The child builds its Program itself (the POSIX child inherits the host's
  `ProgramFactory` by fork). Its options are the request's standard plus
  the engine's defaults, the same as the `project` kind's engine.
- The CLI REPL (`madc`, `madc -i`) runs its session in the CLI process on
  Windows today, so a crashing entry ends madc. With the backend it runs
  there as on POSIX, and a crash restarts the session.
- Gate: the five `testsession_*` tests and `testrepl_crash` lose their
  `win64` / `wine64` skips (all say "no fork on Windows"), plus
  `gui/madcide_repl` under wine.
- Thread contract: unchanged from §41.9a. One client per thread; the backend
  is single-threaded.

## 5. Step B — the 12 s Windows start

Measure before designing: time `madcide.exe file.c --gui` on the owner's
Windows box (`scripts/win_run.sh`), split into process start, the forest map,
the plugin and profile loads, and the first paint. A window-free launch takes
under 0.3 s, so the cost is in the window path: the webview library's load,
WebView2's runtime start, or the page's first compose. Fix the measured
dominator. Never cache a user program (owner law).

## 6. Step C — macOS

- D5: `libmadc-0.dylib`, built per arch beside `forest.bin`
  (`docs/plans/2026-09-01-darwin-host-port.md`). An AOT madcide and every
  plugin library bind it as `@rpath/libmadc-0.dylib`.
- The macOS tarball then ships `bin/madcide` and the plugins, as Linux and
  Windows do. The REPL backend and Run fork there (POSIX), so steps 3 and 4
  are not needed on macOS.
- Gate: the darwin lane's full suite on both Mac runner arches, plus the
  packaged madcide's `--help` smoke and a Run on the owner's Mac (the
  release tier).

## 7. Step D — the chthonia product

Plugin design §9.3 and §9.4, in its own order:

1. The two-TU link probe: split a small piece of madcide into two translation
   units and link them. Multi-object madc builds are tested only at fixture
   scale, and madcide is one unit.
2. madcide's base as a linkable object, and `madcide_main(argc, argv,
   product)`.
3. The product descriptor (name, configuration directory, default bundle,
   GUI by default), and the chthonia build that links the plugin's object.
4. Packaging: the standalone chthonia package (libmadc, madc, `chthonia`) for
   Windows and macOS, then the MSIX (PK6).

## 8. Order

A (the REPL, which every chthonia user sees first) → B (measure, then fix)
→ D1 (the probe) → C (D5) → D2–D4. Each step is its own commit with its
reducer, Tier 1 + Tier 2 per commit, and the batch after each step.
