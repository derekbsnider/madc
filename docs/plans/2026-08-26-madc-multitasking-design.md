# madc multitasking — coroutines and goroutines on one substrate

Owner direction (2026-08-26, mid-session): "there are two paths…
threading and cooperative (like how nginx and node.js work); it might be
useful for multitasking support to be an inherent part of the madc
language, and maybe it should support both lua-style coroutines and
golang style goroutines."

Seed doc for the owner brainstorm. **2026 industry recon + synthesis:
`2026-08-26-madc-multitasking-recon.md`** — it confirms this doc's
substrate and amends the surface (structured scopes primary,
scope-carried cancellation, futures-as-values, deterministic test
gates; C++26 std::execution = semantics through madc conveniences,
never its header machinery — owner directive 2026-08-26).

Constraints it must honour: the
thread-safety law (stated contracts; shared mutation through hub/verbs),
the vision invariants (LanguageStd-gated keywords, one IR, C11 lowering,
no bespoke non-C paths), value-first surfaces, and the F2
"programs-use-cores" arc's audit debt (the front end's static
active-owner state).

## The core claim: two surfaces, one substrate

- **The substrate**: stackful execution contexts + a run queue + an
  event loop. A context is a stack you can switch to and from
  (ucontext or a small assembly pair; MIR-jitted code runs on ordinary
  stacks, so switching just works under the JIT, the interpreter, and
  emitted-C alike).
- **Lua-style coroutines** = the substrate exposed directly:
  create/resume/yield, caller-driven, no scheduler. Deterministic,
  perfect for generators/iterators and protocol state machines.
- **Go-style goroutines** = a scheduler over the same contexts:
  `go f(args)` enqueues a context; channels synchronize. Go promises
  CONCURRENCY, not parallelism — which makes the sequencing below
  legal.

## Sequencing (the cheap path to both)

1. **Cooperative-first, one OS thread.** The nginx/node model: the
   scheduler runs contexts on the single main thread, switching at
   yield points (channel ops, sleep/io waits, explicit yield). This
   ships WITHOUT touching the F2 wall — the engine's static
   active-owner state (`TokenBase::_active_strpool`, the parse cursor
   statics) is never raced because there is never a second thread.
   Thread-safety contract: trivial (single-threaded), stated.
2. **Both surfaces on it.** Coroutine API (Lua shape) and `go` +
   channels (Go shape) over the same contexts. A goroutine blocked on
   a channel parks its context; the scheduler runs the next.
3. **The M:N upgrade rides F2.** When the audit lands (thread_local
   actives, per-thread rings/value pools, the TSan lane), the SAME
   scheduler spreads contexts over worker threads. User programs do
   not change — that is Go's own contract. This is the only step that
   pays the audit.

## Surfaces and the invariants

- Start as `madc::` publics (value-first; intrinsics always available):
  e.g. spawn/yield/resume/channel make/send/recv — exact spellings are
  brainstorm material. Keywords (`go`, `yield`) come later, gated via
  the LanguageStd keyword/feature registry (I-invariants: nothing
  hardcoded, `--std=` decides).
- **Channels ARE hub machinery.** The thread-safety law routes shared
  mutation through hub/verbs; Go channels and the data-hub verbs must
  be ONE design (no-parallel-implementations), with the channel as the
  script-facing face of the hub's queue/verb seam.
- **Lowering**: Tier 1 — everything is C11 calls into a runtime
  library (madc_ctx_*, madc_chan_*). `--emit=c11` stays portable; no
  c2mir or MIR raise. C++20-style STACKLESS coroutines (co_await state
  machines) are required by neither model — a C++23-interop seat for
  the transpiler later, not this arc.

## Honest limits

- A coroutine cannot yield inside an ARBITRARY long C++ engine call —
  but an engine call WE instrument can: the owner ruled (2026-08-26
  evening) that madcide's stage-2 background parse runs as COOPERATIVE
  CHUNKS on this substrate (yield points in the parser's token pump;
  the active-owner statics switched per task via the MT-2
  state-switch seam; fork DROPPED — no win64 fork, serialization cost;
  the THREAD deferred to F2/M:N). See the RULED section in
  2026-08-26-madcide-staged-parse-and-state.md. The event-loop wake-fd
  seam remains one design serving both arcs.
- Blocking libc calls (read/accept/sleep) block the whole cooperative
  world unless routed through the event loop (nonblocking + park) —
  the node/nginx discipline; the runtime's io verbs are where that
  lives, not user code.

## OWNER RULINGS (2026-08-26, post-recon)

- **"This is what we're doing with madc — bringing in features from
  other languages and letting them coexist with C/C++."** The arc's
  north star, verbatim.
- **Start cooperative, with `go`/`yield`; `await` is in.** The
  cooperative-first sequencing and the Go-shaped + Lua-shaped + C#-
  shaped surfaces are approved. (Futures-as-values carry `await` —
  recon Part B amendment 3 — so it costs no coloring.)
- **C++26 std::execution: semantics through madc conveniences, never
  its header machinery** (the dialect-lean law restated for
  concurrency).
- **Build ON the existing channel foundation** — the sub-process
  channel work is the seed, not a parallel design (see next section).

## The combination (owner proposal 2026-08-26, endorsed)

Owner: "combine Go's tiny syntax with Kotlin-style structured
concurrency and Java-style cheap blocking tasks." That IS the recon
synthesis — three views of one design, not a compromise:

- **Go = the spelling.** `go f(x)`, argless `yield()`, colorless calls.
- **Kotlin = the ownership.** Every spawn lands in a scope owning
  join/error/cancel; cancellation rides the scope implicitly (never
  Go's viral context parameter). Where the parents disagree (Go
  detaches by default, Kotlin attaches), the combination picks Kotlin:
  `go` inside a scope attaches to it; top-level `go` attaches to the
  root scope `main` owns.
- **Java/Loom = the cost model.** Blocking IS the programming model:
  a parked context is free, so the ENTIRE existing madc surface
  (php::, perl::, madc::channel reads) works unchanged inside a
  goroutine — nothing ever needs an async variant.

## Task → scheduler → execution resource (external counsel 2026-08-26, adopted runtime-internal)

Relayed by the owner from an outside analysis: steal C++26's
separation of task → scheduler → execution resource, Kotlin's
structured lifetimes, Go's source-level simplicity. The latter two are
the ruled combination above (independent convergence = good signal).
The first is ADOPTED — with a placement rule:

- The runtime keeps the three concepts as DISTINCT layers with stated
  narrow contracts — `madc_ctx_*` (a task: stack + state),
  `madc_sched_*` (who runs next: queue + park/wake + time source),
  `madc_loop_*` (what it runs on: the event loop / a future pool /
  a fork+pipe venue) — even while cooperative-first means exactly one
  scheduler on one resource.
- It pays three times on our own roadmap: MT-4's virtual-time gates =
  a swapped scheduler/time source (pluggable, not hacked); M:N (F2) =
  an ADDED resource with user programs unchanged (the Go contract);
  the stage-2 fork venue and the hub loop are both "resources" behind
  one seam.
- **The guard: the separation NEVER reaches user code.** One ambient
  default resource; user code is venue-blind; `go f(x)` stays two
  words. Rust's executor fragmentation (libraries demanding "which
  runtime?") and C++26's threaded-scheduler-parameter surface are the
  named anti-patterns. An explicit affinity spelling is a much-later
  owner fork, post-F2.

## The C++11 thread family (owner: "contemplate std::thread/async/future/mutex/condition_variable")

Two clean piles — semantics to absorb now, interop names to serve later:

- **std::async / std::future — absorbed.** `go` as an expression IS
  std::async (sane launch policy); the future value's `get()`/`await`
  IS std::future::get() at goroutine cost; one-shot value-or-error is
  the contract of the one-shot channel (MT-2).
- **std::mutex / std::condition_variable — context-aware twins.** The
  Loom PINNING lesson (an OS-level lock blocks the carrier thread,
  freezing every context on it; Java needed JEP 491 for
  `synchronized`): the dialect gets primitives that park the CONTEXT
  (Go sync.Mutex shape); condvar-style waiting is park/notify on the
  loop, and the thread-safety law already routes shared mutation
  through hub verbs — hub wait/notify is the blessed condvar. Real
  pthread-backed std::mutex remains correct for genuine C++ interop
  under real threads; holding one across a yield is a documented
  pinning hazard.
- **std::thread — F2-gated compat seat.** Real OS parallelism rides
  the F2 static-actives audit (same gate as M:N). It is an interop
  surface for transpiling real C++ code, never the foundation of the
  madc surface (dialect-lean).

## The existing foundation (inventory, don't rebuild)

Shipped in v0.72.0 (Track 5C slice 1,
`2026-08-08-track5c-script-channels-plan.md`) and since:

- **`madc::channel`** (`include/madcdis/channel.h`,
  `src/madc_channel_object.cpp`; `<ns_madc>` twin, mangled-direct):
  URI-addressed BYTE channels — `exec://` sub-processes, `tcp://`,
  `file://` — read/readline/readall/write (+ value-carrier twins),
  modes, half-close (`close_write`).
- **`DataChannel` registry** (`include/madcdis/datachannel.h`):
  capabilities (read/write/half_close/seek), scheme factories,
  `ExecDataChannel` (+ `Process::start`, `pump_process`), and a STATED
  thread-safety contract: single-threaded EXCEPT close_read/
  close_write as a cross-thread WAKE for a blocked peer — already a
  park/wake seam.
- **`DatagramDataChannel`**: the message-oriented extension (one
  complete datagram per call) — the natural kin of a value-message
  queue.
- **Hub verbs** (`2026-08-20-data-hub-projection-rendering.md`,
  demand 15): shared mutation behind verbs; F3 gives the language arc
  a data-race-free default.

Unification claims (no-parallel-implementations):

- The Go-style channel between CONTEXTS is a VALUE queue — a new
  in-memory channel kind in the SAME family, not a second queue
  concept; its blocking send/recv are where the scheduler parks and
  wakes contexts.
- `madc::channel` byte endpoints (exec://, tcp://) become WAITABLE on
  the same event loop (nonblocking + park), so `select` can span both:
  "recv from a goroutine channel OR readline from exec://sort" is one
  wait. The fd is the wake mechanism the staged-parse plan already
  designed for stage 2.
- A future is a one-shot value channel; `await` is its receive.

## Slice status

- **MT-1 SHIPPED** (feature/mt1-substrate-claude, session 136):
  src/rt/rt_task.c substrate (ucontext POSIX / fibers Win64, FIFO
  run-to-yield, ctx/sched/loop seams), contextual `go`/`yield` under
  STD_MADC (error-shape rule), translate_go linkonce thunks (3-slot
  ABI), root join in CirJitSession::run_main + loaded-object entry +
  atexit (one owner, idempotent). Docs: docs/language/tasks.md. Tests:
  testgo (complete schedule pinned), testgoident, testgogate,
  test_rt_task. NAMED RESIDUES: ~~try/catch across a yield~~ FIXED by
  the MT-2 opener (per-task exception-state save/restore at every
  switch — __madc_except_state_* in rt_except, wired in task_switch/
  task_exit_switch; testgotry pins it, pre-fix state segfaulted);
  ring-lifetime text across a yield rots (standing ring
  discipline); -static-libmadc spawn links loud (hosted runtime, no
  AOT-ledger context backend yet); go-in-late-template-bodies thunk
  flush (Pass 1.9) untested; darwin ucontext deprecation at the mac
  lane; method/class-arg/fn-ptr spawns refused loud pending MT-2+.

- **MT-2 SHIPPED** (same session): (a) per-task EXCEPTION STATE — the
  SJLJ try/cleanup chains + in-flight exception switch with the task
  (__madc_except_state_* seam owned by rt_except; testgotry pins it,
  pre-fix segfaulted — negative-controlled). (b) VALUE CHANNELS — Go's
  contract (madc_task_chan.cpp: rendezvous at cap 0, buffered at N,
  waiter records on the parked task's stack, close wakes receivers
  false/senders throw, values copied); scheduler grew park/unpark/
  current (wait-queue bookkeeping stays with the caller; parking the
  last runnable flow aborts loud). Surface: madc::chan_make/send/recv/
  close/len publics (owner fork 3 defaulted to Go semantics,
  documented). testgochan pins a complete schedule including MAIN
  parked as a rendezvous receiver and the post-main drain. (c) the RING
  is now deliberately IMMORTAL (leaky singleton): glibc runs TLS dtors
  BEFORE the atexit list, so the native lane's atexit join resumed a
  task whose format() assigned into a DESTROYED ring slot (double
  free, exe lane only — the JIT joins in-session).

- **MT-2b (SHIPPED, session 137)**: the PRINCIPLED native join point —
  under STD_MADC the user's main emits as `__madc_main` behind a
  synthesized `int main(...)` wrapper (parameter list mirrored, args
  forwarded) that calls `__madc_task_join_point()` BEFORE returning:
  the join runs at MAIN'S END (the Kotlin block semantic), before ANY
  teardown, identically in the JIT / `-o` / `-r` / `-static-libmadc`
  lanes. ONE predicate (`main_wraps_task_join`, gated on
  `go_statement_enabled()`) drives both the `var_emit_name` rename and
  the wrapper emission so they can never disagree. The join callee is
  the LEDGER-SAFE dispatcher `__madc_task_join_point`
  (src/rt/rt_task_join.c, strict C11, on scripts/ledger_sources.txt):
  the hosted task runtime installs its hook at first spawn, so a
  program that never spawned no-ops and a `-static-libmadc` artifact
  links without the ucontext backend. Belts kept, both idempotent:
  run_main's post-main join (JIT) and the atexit twin (covers
  strict-mode mains in mixed-TU projects spawning through a madc TU —
  the one lane still riding atexit). `tests/testgojoin.mad` pins the
  drain schedule + argv forwarding; `--std=gnu17` emits byte-identical
  unwrapped mains (testgogate; zero `__madc_main`).

## Slice cut (MT arc; every slice Tier-1 C11 runtime-library lowering)

- **MT-1 substrate**: stackful contexts (small in-tree switcher; SysV
  x86-64 + Win64 + arm64 as lanes demand — MIR-jitted code runs on
  ordinary stacks, so switching just works) + run queue; MAIN RUNS AS
  A CONTEXT (any blocking verb enters the scheduler — no explicit
  run() call); `go f(args)` spawn + argless `yield()`; program exit
  joins the root scope. INTERNAL LAYERING from day one: ctx / sched /
  loop as distinct structs with stated contracts (the adopted
  task→scheduler→resource separation — runtime-internal only).
  Thread-safety contract: one OS thread, cooperative — stated per the
  law.
- **MT-2 channels + futures**: value channels (buffered/unbuffered),
  blocking send/recv with park/unpark, close semantics, `select`;
  `go` as an EXPRESSION returns a future (one-shot channel); `await` /
  `.get()`.
- **MT-3 scopes + cancellation**: structured spawn (scope owns
  join/error/cancel), stop flag checked at every blocking verb.
- **MT-4 io + time**: `madc::channel` endpoints on the event loop,
  `sleep` via a timer heap, VIRTUAL-TIME test verbs — deterministic
  fulltest gates from the first slice (recon amendment 4).
- **MT-5 keywords**: `go` / `yield` / `await` into the LanguageStd
  keyword/feature registry under `--std=madc` (publics land first;
  keywords are sugar over the same runtime entries; strict C/C++
  modes stay pristine).
- Later, other arcs: M:N workers (rides F2), supervision/actor library
  pattern (SMAUG consumer), python:: generators on the Lua surface,
  C++26 std::execution interop seat (transpiler arc).

## Open owner forks (brainstorm agenda)

1. Surface spellings: publics-first vs keywords-first; `go` as keyword
   under `--std=madc` only?
2. Channel semantics: buffered/unbuffered, select, close — how much of
   Go's contract; how it unifies with hub verbs/datachannels.
3. Scheduler shape: run-to-yield only, or also preemption-by-budget
   (Go's async preemption is a much later problem).
4. Where generators (Lua-style) surface in the polyglot namespaces
   (python:: generators are the obvious consumer).
