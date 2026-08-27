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
  in a STD_MADC TU that SPAWNS (`Program::_uses_go_spawn`, set by
  parse_go_statement — a pure program keeps its unwrapped,
  runtime-free main, preserving the conditional-DT_NEEDED purity cover
  pinned by test_native_shared), the user's main emits as
  `__madc_main` behind a synthesized `int main(...)` wrapper
  (parameter list mirrored, args forwarded) that calls
  `__madc_task_join_point()` BEFORE returning:
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

- **MT-4a (SHIPPED, session 137)**: `chan_select` (deterministic
  lowest-index fan-in; sudog stack waiters, first-fire group claim,
  husk skip, wake-once guard, eager removal), `chan_try_recv`,
  `sleep_ms` on the pluggable time source (virtual under
  MADC_TASK_VTIME=1), `task_next_or_wait` = THE one pick-next decision.

- **MT-4b (SHIPPED, session 138)**: io/fd select — byte endpoints
  waitable beside value channels. `madc::chan_readable(channel)`
  registers an `exec://` endpoint as a select case (fires `out = null`
  on readable progress; drained-EOF/failed DISABLES like
  closed-and-drained — registration re-probes `poll_state()`, never
  the raw handle, because a drained fd is still POLLHUP-readable);
  reads under live tasks PARK on the fd via the scheduler's io-wait
  seat (`__madc_task_io_wait_hook` in task_next_or_wait, bounded by
  the earliest timer deadline; the scheduler stays fd-blind — the
  join-hook precedent; POSIX poll() arm + win32 PeekNamedPipe
  cheap-blocking arm). task_enqueue is IDEMPOTENT (`queued` flag).
  `select_fire` = THE one claim+wake owner across BOTH waiter kinds
  (ChanWaiter + IoWaiter), gated by check-select-fire-owner.sh.
  Data-layer surface: PollableDataChannel + pollable_surface
  (seekable_surface's twin); the poll handle is a CRT fd on every
  platform. channel object: poll_state (1/0/-1) / wait_readable /
  read_wait_handle (int64_t — the embedded twin must mangle
  identically everywhere). Gates: test_task_io (park/EOF/double-unpark
  belt), testgoselectio (phased deterministic mixed select, three
  lanes byte-identical). NAMED RESIDUES: mixing MADC_TASK_VTIME with
  fd waits gives io only zero-timeout probes at scheduling decisions
  (documented; vtime tests don't mix); ui_term's input_ready is a
  POLLIN-only divergent copy of the readable-progress rule (KG
  DupFamily fd_readable_progress_probe, open — the MT-4c unification
  migrates the tui stdin wait onto taskio); tcp:// rides the same
  pollable surface when a tcp factory lands.
- **MT-4c — stdin unification** (DESIGN decided 2026-08-27, s139): the
  tui's input wait joins the ONE scheduler poll; read_keys' 50ms
  live-but-parked cadence retires. The model:
  - **The host wait**: `taskio::host_wait_readable(fd)` — the tui flow
    (the main task) PARKS on stdin as an io waiter (the existing
    IoWaiter machinery; one host at a time), so task_next_or_wait's
    hook blocks on {stdin, io waiters} bounded by the earliest timer —
    ONE poll for everything. Returns true = the fd fired (read keys),
    false = a SYNTHETIC wake (recompose; the fd may also be readable —
    the caller's read path re-probes anyway).
  - **The synthetic wake = the ran→wake seam moved into the
    scheduler**: rt counts task switches (`__madc_task_switch_count`,
    one increment in task_switch); the host waiter records the count
    at park. At the hook's QUIESCENT point (nothing runnable), the
    order is: (1) zero-timeout poll — real readiness wins; (2) host
    registered && switches advanced since its park → unpark the host
    UNFIRED and return (a pending repaint must beat blocking); (3) the
    blocking poll. EINTR with a host registered also wakes it unfired
    (SIGWINCH must reach read_keys — the resize check lives at its
    loop head). Every semantics of the cadence loop is preserved:
    runnable tasks still get the CPU between zero-timeout input polls
    (that branch is untouched), activity-then-drain still surfaces as
    the wake event, zero live tasks still take the old blocking read.
  - **The family retires**: input_ready's readable test adopts
    POLLIN|POLLHUP|POLLERR (the fd_readable_progress_probe divergent
    site — a dead terminal was not "readable", so the EOF-surfacing
    read never ran; own commit, fix-what-you-find) and a NEW gate
    (check-fd-readable-progress.sh: no bare `revents & POLLIN)` test
    in src/, negative-controlled) moves the family open→gated.
  - **Scope**: POSIX only, like ui_term.cpp itself (the win tui is not
    a lane). Gates: test_task_io grows host-wait legs (readable-now,
    fd-fired after park, synthetic wake after activity, EINTR shape);
    the pty smoke/scroll gates and test_coop_parse pin the live loop.

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
- **MT-3 scopes + cancellation — SHIPPED (session 139, both halves)**:
  3a = the design below, verbatim (testgoscope pins the complete
  schedule; test_rt_task/test_task_io green). 3b consumers: the token
  pumps + the top-level parse loop honor task cancellation with a clean
  recorded diagnostic (testbuildcancel pins a mid-parse build cancel);
  madcide's Stop returns for internal builds (the build task's own
  scope IS the job handle — scope_cancel reaches the child parse
  through the chain; a pre-start flag covers the spawn-to-first-run
  window); channel::cancel() grew the SIGKILL escalation
  (Process::wait_or_kill — 2s grace, then hard-kill; testchancancel's
  SIGTERM-ignoring child gates it by wall clock). Residues: emit phase
  has no yield points; cancellation lands at declaration/1024-token
  grain; faithful non-text error rethrow; NonCancellable cleanup
  regions.
  DESIGN (decided 2026-08-27, session 139 — the Kotlin-ownership ruling
  made concrete; publics-first, keywords are MT-5):
  - Surface: `madc::scope_begin()` -> int64 handle (opens a scope,
    becomes the CURRENT task's innermost); `madc::scope_end(s)` (joins
    every task spawned in the scope — and its child scopes' tasks —
    then rethrows the FIRST child error, else throws the cancelled
    text if the scope was cancelled, else returns; owner-only, must be
    the innermost); `madc::scope_cancel(s)` (request, returns
    immediately, any task may call — madcide's Stop is the consumer);
    `madc::cancelled()` (current task's flag OR any open scope on its
    current-scope chain — the compute-loop poll).
  - Attachment (the Kotlin pick): `go` attaches the new task to the
    SPAWNER's innermost open scope; no open scope = the root scope
    (task->scope NULL — exactly today's semantics, drained at main's
    end). A task spawned into a cancel-requested scope is BORN
    cancelled.
  - Cancellation = a flag + a wake: `__madc_task_cancel_request(t)`
    sets `cancel_req`, unlinks a timer-parked task from the timer
    list, and unparks (idempotent enqueue). EVERY blocking verb
    (chan_send/recv/select, sleep_ms, taskio wait_readable, the
    channel object's park) checks the flag on resume — and BEFORE
    parking — eagerly removes its own waiter record (stack-resident;
    a husk pointing into an unwound frame is a use-after-free), and
    throws THE one cancelled literal
    (`__madc_task_cancelled_text()` — pointer identity distinguishes
    it from user text). Sticky (Kotlin): cleanup after cancellation
    must not use blocking verbs. yield() is NOT a cancellation point
    (a throw through the engine's parse seam would corrupt it; the
    parse-seam abort is MT-3b's clean-diagnostic path).
  - Error ownership: the trampoline arms a C-side SJLJ catch-all
    (setjmp on `__madc_try_push`) for SCOPED tasks only: an uncaught
    child error is rendered to text (`__madc_exception_text`, the one
    renderer the four Unhandled printers share), recorded as the
    scope's FIRST error, and CANCELS the scope (Kotlin: a failed
    child cancels its siblings); the cancellation literal itself is
    swallowed silently (cancellation is not a failure). Root tasks
    keep today's abort-on-uncaught (Go semantics). scope_end always
    finishes joining before it throws (no leaked bookkeeping; a
    cancelled opener re-parks until its children complete
    cancellation). Faithful rethrow of non-text exceptions = named
    residue (text rethrow first).
  - Layering: scope struct + membership lists live in rt_task.c (task
    lifecycle: birth attachment, death detachment, last-exit joiner
    wake); the int64 handle registry + throws live in the C++ surface
    (madc_task_chan.cpp, the chan registry idiom). task_drain /
    task_live stay (root-scope verbs; retirement is an owner call).
  - MT-3b (consumers, after 3a): parse_yield_point honors task
    cancellation via the parser's own clean abort (diagnostics row,
    never a throw through parse state) so an in-process build cancels
    during its parse phase; madcide Stop returns for internal builds
    (build task in a scope, stop = scope_cancel); channel::cancel()
    grows SIGKILL escalation for a SIGTERM-ignoring child.
- **MT-4 io + time**: `madc::channel` endpoints on the event loop,
  `sleep` via a timer heap, VIRTUAL-TIME test verbs — deterministic
  fulltest gates from the first slice (recon amendment 4).
- **MT-5 keywords**: `go` / `yield` / `await` into the LanguageStd
  keyword/feature registry under `--std=madc` (publics land first;
  keywords are sugar over the same runtime entries; strict C/C++
  modes stay pristine).
  DESIGN (decided 2026-08-27, session 139 — go/yield shipped with MT-1;
  this slice adds the STRUCTURE spellings; all contextual per the MT-1
  error-shape rule — never keyword_map-reserved, a declared name always
  wins, strict modes stay byte-identical):
  - **`scope { ... }`** — a structured-concurrency block: opens a task
    scope for the block's extent; `go` inside attaches (the MT-3
    Kotlin attachment); the block's end JOINS every member and
    rethrows the first member error (exactly `madc::scope_end`).
    Claimed at statement head only when `scope` is undeclared and the
    next token is `{` (otherwise ill-formed today — the error-shape
    rule). Lowering rides the __madc_scope_* seams DIRECTLY (the
    validate-then-consume split exists for this): NEW rt pair
    `__madc_scope_block_enter()` / `__madc_scope_block_exit(s)` in
    rt_task.c. enter = scope_begin + push an EMBEDDED cleanup entry
    (rt_except's caller-owned `__madc_cleanup_push`) whose handler
    quietly abandons the scope (cancel members + join + free + pop
    task->cur — the failing-opener semantics: the in-flight error
    wins, children cancelled; NO stderr warn — an unwind exit is
    legitimate). exit = remove the entry (NEW
    `__madc_cleanup_remove(entry)` in rt_except.c — a mid-stack
    unlink, the FOURTH conscious host-consumer widening; top-pop is
    wrong because an enclosing try's body locals registered above the
    scope's entry outlive the block), then end_check (a nonzero here
    is an engine bug — still checked loud), then `__madc_scope_end`
    (join + rethrow). The cleanup-stack MARK discipline gives the
    right nesting for free: a throw CAUGHT INSIDE the block unwinds
    only to the inner try's mark and never touches the scope's entry
    (the block continues); a throw ESCAPING the block runs the
    abandon handler mid-unwind — and the join it performs may PARK,
    which is safe because the in-flight exception is per-context
    state (the MT-2 except-state switch composes). return / break /
    continue / goto that would EXIT the block are refused LOUD at
    parse time ("… crosses a task scope — end the scope first");
    early-exit support = named residue. The handle is deliberately
    not spelled: cancellable-job patterns (madcide Stop) keep the
    publics; `madc::cancelled()` serves polling inside the block.
  - **`await <chan-expr>`** — receive from a value channel; Go's
    `<-ch` semantics (blocks; a closed drained channel yields the
    ZERO value = empty madc::value — the ok-form stays the public's
    job). Sugar over the ONE recv implementation
    `madc::chan_recv(out, ch)` through a thin extern-C machinery seat
    (`__madc_chan_await`, the translate_go category — no parallel
    recv). TWO positions shipped in slice 1 — assignment RHS
    (`v = await ch;`, claimed at STATEMENT level: the value carrier's
    operator= machinery resolves the assignment shape inside the
    ladder before any statement-root fold could see it) and bare
    statement (`await ch;` — the done-channel wait, claimed at
    statement head: the identifier dispatch would otherwise swallow
    the two-identifier shape silently). The declaration-initializer
    form (`var v = await ch;`) and every deeper operand position are
    refused LOUD naming the supported forms — both unlock with L3
    value-by-value returns (named residue). A scalar target
    (`long p; p = await ch;`) is refused at parse time. Claimed only
    when `await` is undeclared (error-shape rule).
  - **`select` keyword: DEFERRED** (decided default). Go's
    `case v := <-ch:` grammar does not transplant into a C statement
    grammar without inventing a non-Go spelling — which would violate
    the Go-spelling ruling harder than omitting the statement. The
    `madc::chan_select` public + `await` cover fan-in; revisit beside
    the coroutine/generator surface.
  - Gating: all three ride `go_statement_enabled()` (== STD_MADC).
    testgoident / testgogate grow `scope` / `await` arms.
  - Thread-safety contract: unchanged — single OS thread, cooperative;
    the new rt entries touch only per-task/per-context state.
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
