# The reactor's Windows backend: the WSAPoll floor + the taskio park's socket arm

**Owner pick, 2026-09-15:** *"we cannot really have a master release unless we
have the new functionality working under Windows and MacOS"* → *"#1 is first"*.
The V6 transports (`--serve`, the ws window, `--attach`, discovery,
`--lsp --serve`) must work on Windows so the fifteen `.win64_skip` fixtures that
name this slice lift: `testsocketpark`, `testwsframe`, `testmadcide_serve`
(+ `_graph _concurrent _propose _mcp _ws _tiers _edit _push _web`),
`testmadcide_attach`, `testmadcide_discover`, `testmadcide_lsp_serve`.

**Branch:** `feature/reactor-windows-claude` off develop `4db41e6f4`. This slice
is a feature on its own (one merge wave): the battery runs once, when the
fifteen skips lift and the wine lane is green.

**Design it serves:** `docs/plans/2026-09-11-async-io-reactor.md` item 4 — *"The
IOCP backend (Windows), select/WSAPoll fallback. Native completion. Gate on
genuine-win; the select floor covers old Windows and keeps the wine lane
simple."* This slice lands the floor.

## What is actually broken (recon, read — not guessed)

The wine symptom is `testsocketpark` printing `task-eof`: a task parked on a
connected socket's `readline` wakes at once and reads nothing. Two defects
compose it, both below the reactor:

1. **The taskio park's Windows probe has no SOCKET arm.**
   `io_probe_readable(handle)` (`src/madc_task_chan.cpp`) on `_WIN32` treats
   every handle as a CRT fd: `_get_osfhandle` → a console arm
   (`PeekConsoleInputW`) or a pipe arm (`PeekNamedPipe`). A SOCKET — the value
   `SocketDataChannel::read_poll_handle()` returns — is a kernel handle, not a
   CRT fd; `_get_osfhandle` fails and the probe answers "readable"
   unconditionally. The hook's Windows blocking arm has the same blind spot
   (`_get_osfhandle` again; a socket lands in the "pipe" bucket and drives the
   1 ms cadence). The comment at the reactor helpers says it outright:
   *"Windows waits on its own console/pipe path and never reaches the reactor."*

   Underneath that is a CONTRACT defect: `intptr_t handle` carries no
   namespace. On POSIX every waitable is a descriptor; on Windows a CRT fd and
   a SOCKET are two spaces whose values can collide (`_open_osfhandle`'d pipe
   fds are small integers; kernel handles are multiples of 4 and start small
   too). No OS test disambiguates a bare integer — `_get_osfhandle(v)` and
   `getsockopt(v, SO_TYPE)` can BOTH succeed. The kind has to travel with the
   value, from the channel that owns the handle.

2. **An accepted socket inherits the listener's non-blocking mode on Windows
   (and BSD/macOS).** `ListenSocketDataChannel` puts its fd in non-blocking
   mode (`FIONBIO`) so the scheduler drives accept; Winsock's `accept` hands
   back a socket with "the same properties" — non-blocking included (Linux
   does not inherit; Python's `socket.accept` forces blocking for exactly this
   reason). `SocketDataChannel` documents that "the socket stays BLOCKING, so
   the recv that follows a readable wake returns at once": a spurious wake
   (defect 1) then hits `recv` → `WSAEWOULDBLOCK` → read error → `readline`
   false → `task-eof`. Fixing defect 1 alone would leave the inherited mode
   as a latent trap for every solo-program (no live task, plain blocking read)
   path on Windows and macOS.

The reactor itself is NOT the root: on Linux it is only the blocking-WAKEUP
mechanism (its I/O thread rings a doorbell; readiness DETERMINATION stays the
synchronous probe). It has no Windows backend (the stub throws; `available()`
is false), so the Windows hook cannot block on a doorbell today — it would
have to WSAPoll the sockets inline, which cannot be combined with a console's
`WaitForMultipleObjects` in one wait. That is the reason to land the backend
now rather than a bare WSAPoll in the hook.

## Rulings (defaults; the owner can veto any in a word)

1. **The Windows Reactor backend is the WSAPoll READINESS adapter — the
   design's "select floor" — not IOCP.** The reactor's one consumer today is
   `submit_poll` + the doorbell (the taskio park); IOCP has no native
   readiness op (a zero-byte overlapped `WSARecv` approximates it for streams;
   a listener's "connection pending" has no IOCP spelling at all without
   `AcceptEx`'s pre-created socket). IOCP earns its place when the COMPLETION
   ops (`submit_accept/read/write`) get a consumer — the channel-layer
   migration the design names — and it slots in behind the identical
   `Reactor` contract as a third TU arm. The WSAPoll backend mirrors the epoll
   backend line for line (readiness → the syscall on the I/O thread → a
   completion), so the two arms stay one shape: `fd_ops`, `service_fd`,
   `complete_op`, `drain_submissions`, the two queues. Platform spellings:
   the submit wake is a loopback UDP socket the I/O thread polls (Windows has
   no eventfd and `WSAPoll` takes only sockets); the doorbell is a manual-reset
   Event whose HANDLE `doorbell()` returns, so the scheduler's console wait
   can join it in ONE `WaitForMultipleObjects`; `wait_doorbell` is
   `WaitForSingleObject` + `ResetEvent`. `available()` is true on `_WIN32`.
   The fd space the backend accepts is SOCKETs only (documented): the taskio
   seat arms only socket-kind waiters on Windows.

2. **The kind travels with the handle.** `include/madcdis/datachannel.h`
   gains `enum class poll_handle_kind : unsigned char { descriptor, socket }`
   and `PollableDataChannel::read_poll_kind()` (virtual, default
   `descriptor`); the two socket channels answer `socket`; the three
   forwarders (`WebSocketDataChannel`, `HeaderFramedDataChannel`,
   `ExecDataChannel`) forward the inner channel's answer. `madc::channel`
   gains the engine-side twin `read_wait_kind()` beside `read_wait_handle()`
   (C++ only — not mirrored into `<ns_madc>`: a dialect program never names a
   handle's namespace). `taskio::poll_readable` / `wait_readable` take
   `(intptr_t handle, poll_handle_kind kind)`; `IoWaiter` carries `kind`.
   `host_wait_readable` keeps its shape: the host IS the terminal's stdin, a
   descriptor by definition (stated in its comment). On POSIX the kind is
   carried and ignored — a socket is a descriptor there; no Linux/macOS
   behaviour changes.

3. **The hook's Windows arm waits on consoles AND the doorbell in one
   `WaitForMultipleObjects`; pipes keep their cadence.** Per pass: the shared
   `io_probe_and_fire()` (the `#if !defined(_WIN32)` guard on it and on
   `reactor_drain_discard()` lifts — one probe loop, both arms), then the wait
   set: every unfired descriptor waiter that is a console contributes its
   HANDLE; a pipe sets `cadence`; socket waiters are covered by the reactor's
   doorbell HANDLE (added once) — a socket waiter the reactor could not arm
   (creation failed) sets `cadence`. `cadence` → `Sleep(1)` (today's pipe
   behaviour, unchanged); else `WaitForMultipleObjects(step)`, and a doorbell
   wake is cleared with `wait_doorbell(0)` before the loop re-probes. Headless
   servers (sockets only) therefore block on ONE event with a real timeout —
   no polling; the TUI with `--serve` (console + sockets) wakes instantly on
   either; LSP over stdio pipes with `--serve` keeps the cadence it has.

4. **Accepted sockets are put back in blocking mode on every platform.**
   `set_socket_nonblocking` generalizes to `set_socket_blocking_mode(fd,
   nonblocking)` (one helper, two callers); `accept()` calls it with `false`
   right after `SetHandleInformation` / `set_fd_close_on_exec`. A no-op on
   Linux, the fix on Windows and macOS.

5. **The probe's socket arm is `WSAPoll(1 fd, 0 ms, POLLRDNORM)`**, readable on
   `POLLRDNORM | POLLHUP | POLLERR | POLLNVAL` (a closed-under-us socket wakes
   the waiter so its read surfaces the error, exactly the pipe arm's rule); an
   error return is "readable" too. The I/O thread uses the same flags, so
   probe and wakeup never disagree (the poll op is one-shot: a wake the probe
   disowned would leave the waiter unwatched).

## Thread-safety contract (thread-safety.md)

Unchanged from the epoll slice and restated: the I/O thread is PURE PLUMBING —
it touches the `WSAPOLLFD` set it builds from `fd_ops`, its wake socket, the
doorbell Event and the two mutex-guarded queues; it never enters dialect,
session, parser or `world` code and never fires a waiter. The scheduler thread
owns `g_io_head`, the probe, and every `io_fire_waiter`; completions are
drained (and discarded — the probe is the source of truth) only there. The
`madc_task_chan.cpp` contract stays "single OS thread, cooperative".

## Tasks

| # | what | files |
|---|---|---|
| T1 | `poll_handle_kind` + `read_poll_kind()` (default, two overrides, three forwarders); `channel::read_wait_kind()`; taskio publics take the kind; `IoWaiter.kind`; the four engine call sites pass it | `include/madcdis/datachannel.h`, `include/madcdis/channel.h`, `include/madcdis/websocket_channel.h`, `include/madcdis/header_channel.h`, `src/madc_socket_channel.cpp`, `src/madc_websocket_channel.cpp`, `src/madc_header_channel.cpp`, `src/madc_process.cpp`, `src/madc_channel_object.cpp`, `src/madc_task_io.h`, `src/madc_task_chan.cpp`, `src/ui_term.cpp` |
| T2 | the probe's socket arm; the hook's Windows arm rebuilt on `io_probe_and_fire` + consoles + doorbell; `reactor_watches(kind)` gates arming; the two helpers' `#if` lifted | `src/madc_task_chan.cpp` |
| T3 | the WSAPoll backend TU arm (`#elif defined(_WIN32)`): impl, run loop, wake socket, Event doorbell, `available()` true; the stub keeps the remaining platforms (darwin) | `src/madc_io_reactor.cpp` |
| T4 | accepted sockets back to blocking (`set_socket_blocking_mode`) | `src/madc_socket_channel.cpp` |
| T5 | lift the fifteen `.win64_skip`; the reactor design doc's item 4 gets its "landed" note; CHANGELOG; `docs/madcide.md` Windows note if any | `tests/*.win64_skip`, `docs/plans/2026-09-11-async-io-reactor.md`, `CHANGELOG.md` |
| T6 | seam: pre-build (release + win + `make -k hosted-arm64-macos`), the battery, lanes, ledger, develop merge | — |

Every `src/`/`include/` commit carries the four rule trailers.

## Verification (smallest oracle first, one heavy job at a time)

1. `bash scripts/remote_build.sh sync build win` — the Linux build (the
   Linux arm compiles the kind plumbing; `make -C src test` runs
   `test_io_reactor` unchanged) and the mingw PE.
2. The two oracles under wine with their skips removed:
   `WINEDEBUG=-all MADC_BIN=bin/madc-hosted-x86-64-windows.exe MADC_WRAPPER=wine
   MADC_SKIP_EXT='win64 wine64' bash scripts/run_tests.sh testsocketpark testwsframe`
   — expect `main: post-park write` BEFORE `task-got: hello`, and the ws echo.
3. The thirteen madcide serve/attach/discover/lsp tests under wine; each
   residual failure is classified (this slice's, or a different Windows gap
   with its own reason) before any fixture is re-added.
4. The full wine lane (`remote_build.sh wine`), then the genuine-win lane
   (`scripts/win_suite.sh` from the container — the real Winsock; wine
   approximates it) — the slice is not done until genuine-win agrees.
5. The seam battery per `feedback_seam_prebuild_all_toolchains`, the lane
   ledger, `check --promote`, merge to develop. No master promote (owner
   decision: `check --release` + gcc-torture).

## Out of scope (stated, not forgotten)

- IOCP native completion (ruling 1: after the completion ops have a consumer).
- Waitable process pipes on Windows (named pipes opened `FILE_FLAG_OVERLAPPED`
  so the cadence can go) — the process-pump slice, its own contract.
- The kqueue backend (item 3) — the macOS lane keeps `poll()`, which is correct
  there; only the blocking wakeup would move.
- `#2` madcgit on the cross targets and `#3` the release tier — the next picks.
