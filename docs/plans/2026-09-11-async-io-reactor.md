# Async I/O reactor — the engine's one platform-native I/O engine (plan)

**Status:** design agreed (owner, 2026-09-11); implementation not started.
**Motivating consumer:** the V6 client-server transports
(`docs/plans/2026-09-11-v6-transports-headless-plan.md`) — the headless
`api` server (V6a), the `ws` remote window (V6b), and every future
networked provider (V7). But this is **engine infrastructure**, not a V6
subsystem: it becomes the ONE readiness/completion engine the whole runtime
uses, superseding the current `poll()`-based io-wait.

## Why

Today the engine has exactly one readiness layer: `taskio` (the cooperative
scheduler's io-wait seat, MT-4b), implemented over plain **`poll()`** on the
single cooperative OS thread (`src/madc_task_chan.cpp`). Tasks that wait on
an fd — tui stdin, process pumps, data channels — park while the scheduler
blocks in `poll()` over their handles. There is no `epoll`/`kqueue`/`IOCP`
and no dedicated I/O thread anywhere.

For a real headless code server (the north-star: many clients, live event
push, low latency) `poll()` on the logic thread is the 1970s shape. The
owner's directive (2026-09-11): a top-notch, 2026 platform-native async I/O
layer — `epoll` (Linux), `kqueue` (macOS), `IOCP` (Windows), `io_uring` as
the Linux fast path — on a **dedicated networking I/O thread**, behind one
abstraction.

## The contract: completion-oriented (proactor)

The abstraction is **completion-oriented**, NOT readiness-oriented: a caller
SUBMITS an operation (`accept`, `read`, `write`, `close`, `cancel`, a timer)
and later receives a **completion** (the op's result + the bytes, for a
read). This is the ASIO / libuv / tokio shape and the ONLY contract under
which the completion-native platforms are first-class:

| Platform | Kind | Fit |
|---|---|---|
| **io_uring** (Linux) | completion (SQ/CQ rings) | **native** — submit SQE, reap CQE |
| **IOCP** (Windows) | completion | **native** — post op, `GetQueuedCompletionStatus` |
| **epoll** (Linux) | readiness | **emulated** — wait readable → do the `recv` on the I/O thread → synthesize a completion |
| **kqueue** (macOS) | readiness | **emulated** — same adapter as epoll |
| **select/WSAPoll** | readiness | **emulated** — the portable floor |

A readiness backend hides behind a single `readiness→syscall→completion`
adapter (one file), so the caller-facing contract is identical on every
platform. Picking readiness as the contract instead would make IOCP and
io_uring — the two we most want — the awkward ones; hence completion.

## Threading model

A **dedicated I/O thread** runs the platform backend's event loop. It is
**pure plumbing**: it owns ONLY fd/handle registration, the ring/queue, and
the raw byte transfer for readiness backends. It **never touches session,
task, dialect, or `world` state** — so the single-threaded contract that the
whole dialect + `IdeSession` rely on is untouched (thread-safety.md OWNER
LAW: the reactor mutates nothing the language can observe).

Hand-off, both directions:
- **Logic → I/O:** a submission queue (MPSC into the I/O thread). Submitting
  wakes the I/O thread (io_uring: `io_uring_enter`; epoll/kqueue: an internal
  eventfd/self-pipe registered in the set; IOCP: `PostQueuedCompletionStatus`).
- **I/O → logic:** a completion queue drained ON the cooperative scheduler
  thread. The I/O thread signals readiness by writing a **wakeup handle that
  is folded into the scheduler's existing wait** (the scheduler already
  blocks waiting for io; it gains one more fd — the reactor's completion
  doorbell). The scheduler drains completions and resumes the parked tasks.
  No task/session code ever runs off the scheduler thread.

Only ONE new OS thread is introduced (the I/O thread). No new bare mutable
globals: the reactor is a single owned object with an explicit lifetime; the
two queues are its members; the scheduler holds a handle to it.

## Relationship to `taskio` (no parallel implementations)

This does NOT sit beside the existing `poll()` io-wait — it **supersedes its
backend** (no-parallel-implementations.md). `taskio::wait_readable` /
`poll_readable` / `host_wait_readable` keep their signatures (the
scheduler-facing contract is stable), but their body migrates from a bare
`poll()` to: submit a one-shot read-readiness/`recv` op to the reactor and
park the task until its completion drains. The `poll()` in
`madc_task_chan.cpp` is retired as the reactor lands, not left as a second
path. The migration is staged (below) but ends with ONE engine.

## Backends & sequencing

Build order is chosen so the mandatory, universally-available fallback lands
and is proven FIRST, then the fast paths:

1. **The abstraction + the epoll backend (Linux).** epoll is on every Linux
   we target (incl. the QNAP `5.10` NAS and hardened containers), so it is
   the fallback we can never skip and can test everywhere. Prove the whole
   contract — submission, completion, the dedicated thread, the scheduler
   doorbell, `taskio` riding it — on epoll. Gate: a unit test (loopback
   socket: submit accept, connect, submit read/write, reap completions).
2. **The io_uring backend (Linux fast path).** Runtime-detected: probe
   `io_uring_setup` at startup (as the recon probe did) and fall back to
   epoll on `ENOSYS`/`EPERM`/seccomp refusal. The container (WSL2 6.18,
   `features=0x3ffff`, seccomp permits it) CAN CI-gate this. Uses the
   in-tree `linux/io_uring.h` uapi via raw syscalls — no `liburing`
   dependency (it is not installed; a raw-syscall shim keeps the build
   dependency-free, matching the channel layer's ethos).
3. **The kqueue backend (macOS).** The readiness adapter is shared with
   epoll; only the wait syscall differs. Gate on the mac runners.
4. **The IOCP backend (Windows), select/WSAPoll fallback.** Native
   completion. Gate on genuine-win; the `select` floor covers old Windows
   and keeps the wine lane simple.

Each backend is one translation unit selected at build time by platform
(`#ifdef`), with io_uring vs epoll chosen at RUNTIME on Linux.

## Runtime backend selection (Linux)

At startup, probe io_uring (`io_uring_setup` with a tiny ring; success →
close it and remember "available"). Prefer io_uring when available AND not
disabled (`/proc/sys/kernel/io_uring_disabled`) AND the probe was not seccomp-
refused; else epoll. The decision is logged once (a `-v` line) and is
per-process, immutable after startup (no new mutable global — a `const` set
during init).

## Thread-safety contract (stated, per thread-safety.md)

- The reactor object: its `submit()` is safe to call from the cooperative
  scheduler thread (the producer) and from the I/O thread's internal
  re-submission; the two internal queues are the only shared mutable state
  and are explicitly synchronized (lock-free MPSC or a small mutex —
  measured, not assumed). Everything else is single-owner.
- The I/O thread touches ONLY: its backend handle (ring/epoll fd), the fds it
  was handed, and the two queues. It calls NOTHING in the dialect, session,
  parser, or `world`.
- Completions are consumed ONLY on the scheduler thread. A DataChannel's
  bytes therefore still cross into dialect code on the one cooperative
  thread — the carrier/single-thread contract is preserved.
- No new bare mutable global (thread-safety.md): the reactor is an owned
  object with an explicit lifetime, started on first networked use and
  stopped at shutdown.

## How the channel layer plugs in

`PollableDataChannel::read_poll_handle()` (the existing waitable-fd seam) is
what the reactor registers. `SocketDataChannel` / `ListenSocketDataChannel`
(V6a) already expose their fds through it. The new
`ListenSocketDataChannel::accept()` becomes a submitted `accept` op; a
socket read becomes a submitted `read`. The V6a `api` seat's blocking
`channel.readline()` loop becomes: submit a read, park, resume on completion
— the seat logic itself is unchanged (it already goes through the channel
wrapper).

## Test strategy

- A reactor unit test (`tests/unit/test_io_reactor.cpp`): stand up the
  reactor, a loopback listener, submit accept; a client connects; submit
  read/write on both ends; assert completions carry the right bytes and
  order; cancel + close paths; the epoll backend first, the io_uring backend
  behind a runtime-available guard (skip cleanly when unavailable so the
  darwin/win lanes and locked-down hosts stay green).
- The V6a `api`/`ws` seats ride the reactor once `taskio` is migrated; their
  existing integration tests (`testmadcide_serve`, …) become the end-to-end
  gate.

## Out of scope (for now)

- Zero-copy / registered buffers / multishot io_uring ops — a later
  optimization once the plain path is proven (MEASURE first; the code-server
  workload is dozens of connections, so the ceiling is not the point yet).
- Replacing the process-pump helper threads — they have their own contract
  (they must never enter `taskio`); revisit only if a measured win exists.

## Open decisions (deferred, not blocking the epoll slice)

- MPSC queue: lock-free vs. a small mutex — decide by measurement on the
  epoll slice, not up front.
- Whether the I/O thread is always-on or lazily started on first networked
  use (lean: lazy — a solo compile/JIT run spawns no I/O thread, zero new
  overhead, mirroring `taskio`'s "no tasks live → plain blocking" ethos).
