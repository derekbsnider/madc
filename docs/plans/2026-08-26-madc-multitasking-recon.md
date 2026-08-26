# madc multitasking — 2026 industry recon and brainstorm synthesis

Companion to `2026-08-26-madc-multitasking-design.md` (the seed: one
substrate, two surfaces, cooperative-first). Owner asked (2026-08-26)
for a recon of goroutines+channels vs Elixir actors+messages vs Rust
async/await vs C# await vs Kotlin coroutines — plus C++26's new
concurrency model, with the standing directive that madc present such
functionality **through madc conveniences, never by lugging in heavy
C++ header machinery** (this is `dialect-lean.md` restated for
concurrency; recorded here as a ruled-in constraint).

## Part A — the models, their 2026 state, and what each costs a C dialect

### 1. Go — goroutines + channels (CSP, stackful, colorless)

- Model: stackful green threads, M:N scheduled; channels synchronize;
  `select` multiplexes; any function can block — no annotations.
- 2026 state: the model itself is finished and won its argument; the
  news is around the edges — `testing/synctest` went stable (Go 1.25,
  old API removed in 1.26): deterministic "bubbles" with VIRTUAL TIME
  so concurrent tests stop flaking, and `sync.WaitGroup.Go` finally
  gives spawn-into-a-join-set a blessed one-liner.
- The two known warts, admitted by Go's own ecosystem: structured
  concurrency was retrofitted (errgroup, now WaitGroup.Go) rather than
  designed in, and `context.Context` cancellation is VIRAL — a threaded
  first parameter through every call chain.
- Cost to madc: the CHEAPEST of all six — a pure Tier-1 runtime
  library (contexts + run queue + event loop). No front-end transform,
  no new types in the IR, `--emit=c11` unaffected.
- madc steals: the whole substrate shape (the seed doc already claims
  it); channels-as-the-sync-primitive; `select`; the DETERMINISM
  lesson inverted — see Part B.

### 2. Elixir/Erlang — actors + messages (BEAM processes)

- Model: millions of preemptible processes, each with an ISOLATED HEAP;
  share-nothing; mailboxes with selective receive; supervision trees +
  let-it-crash restart discipline.
- 2026 state: the concurrency model is unchanged (it won for fault
  tolerance); the action is the type system — v1.19 (Oct 2025) infers
  anonymous functions + protocols with ~4x faster parallel compilation,
  v1.20 (2026) reaches full inference including guards, on OTP 29.
- Why the literal model is WRONG for madc: per-actor heaps require
  copying messages and per-process GC — madc is shared-memory C with
  the value/ring machinery and raw pointers crossing everywhere;
  enforced isolation is unimplementable without abandoning C semantics.
- Cost to madc of the STEALABLE part: near zero once channels exist —
  an actor is a context that OWNS one mailbox channel; a supervisor is
  a scope with a restart policy.
- madc steals: supervision/restart as a library pattern (the MUD-server
  consumer is real: SMAUG-shaped servers want a crashed
  connection-handler restarted, not the process down); mailbox
  ownership as a stated isolation CONVENTION (share by communicating),
  not an enforced memory model.

### 3. Rust — async/await (stackless, colored, no runtime)

- Model: `async fn` compiles to a state machine; `.await` yields;
  executors are third-party (tokio et al.); zero-allocation, no
  runtime — the embedded/kernel constraint drives everything.
- 2026 state: async closures stabilized (RFC 3668, early 2025); async
  fn in traits works statically (RPITIT, 1.75) but `dyn` dispatch of
  async traits REMAINS unsolved; the runtime ecosystem is still
  fragmented into silos (tokio vs io_uring thread-per-core runtimes
  like glommio — libraries written for one don't run on another); the
  official async book still opens by admitting rough edges, and the
  2025–2026 project goal is literally "parity with sync Rust".
- Lesson for madc: Rust pays function coloring, `Pin`, and ecosystem
  fragmentation to get async WITHOUT a runtime. madc HAS a runtime.
  Buying Rust's costs without Rust's constraint would be a category
  error.
- Cost to madc: the most expensive option — a CPS/state-machine
  lowering pass in the CIR builder, future types threaded through the
  type system, and every polyglot namespace function needing an async
  variant (coloring is infectious by definition).

### 4. C# — await (stackless, colored, runtime-assisted)

- Model: the original async/await (C# 5, 2012); compiler-generated
  state machines over Tasks; pervasive and ergonomic; cancellation via
  explicitly threaded CancellationTokens.
- 2026 state — the most instructive datapoint of the sweep: the GREEN
  THREADS experiment was formally REJECTED (native-interop cost, shadow
  stacks, two-model coexistence) because .NET already carries a
  15-year async ecosystem; instead, "Runtime Async" moves the state
  machine OUT of the compiler INTO the runtime — the headline feature
  of .NET 11 preview 1 (ships Nov 2026). Read that plainly: the
  compiler-transform approach was expensive enough that its inventor is
  spending a decade-class migration getting rid of it, and the thing
  they're moving toward (the runtime owning suspension) is what a
  stackful substrate has on day one.
- Why .NET's green-thread blockers DON'T apply to madc: their pain was
  a managed runtime (GC stack scanning, security shadow stacks) plus a
  legacy async ecosystem to interoperate with. madc has C stacks, no
  moving GC, and NO legacy async surface — the blockers are absent
  precisely because we're early.
- madc steals: await's ERGONOMICS without its coloring — a future is a
  value, `await` is a receive on a one-shot channel; on a stackful
  substrate that's a blocking library call, no transform (Part B).

### 5. Kotlin — coroutines (stackless, minimally colored, structured)

- Model: one keyword (`suspend`), CPS transform in the compiler;
  its real invention is STRUCTURED CONCURRENCY — every coroutine is
  born inside a CoroutineScope that owns joining, error propagation,
  and cancellation; cancellation flows implicitly with the scope.
- 2026 state: with Loom's virtual threads final on the JVM since 21,
  "will coroutines become obsolete" is a live conference question;
  the pragmatic answer is interop (Loom-backed dispatchers). Kotlin
  built stackless coroutines because the pre-Loom JVM COULDN'T do
  stackful — another platform constraint madc doesn't share.
- madc steals: structured concurrency as the DEFAULT spawn surface and
  scope-carried implicit cancellation — the two ideas the whole
  industry copied from it (see the convergence list).

### 6. C++26 — std::execution (senders/receivers) — the owner's addition

- What it is: the standardized async model adopted into C++26
  (P2300R10, St. Louis 2024; the C++26 draft is complete and
  senders/receivers is its headline concurrency feature). Three
  abstractions — schedulers (where), senders (lazily-described work),
  receivers (completion handlers) — plus composition algorithms
  (`then`, `let_value`, `when_all`, ...), `sync_wait`, and, adopted
  alongside for C++26: `std::execution::task` (a coroutine task type,
  P3552, Sofia 2025), `counting_scope`/async-scope (P3149 — spawn a
  dynamic amount of work, await it all), and the parallel "system
  scheduler" (P2079 — one ABI-stable process-wide context so libraries
  stop oversubscribing the host).
- The semantics worth having: work as a lazily-composed DAG (describe,
  THEN launch); structured lifetimes — "data-race-free by
  construction" via rigorous lifetime nesting; cancellation built into
  the protocol (stop tokens, the third completion channel); scheduler
  affinity as data (WHERE work runs is a first-class parameter).
- The machinery NOT worth having: it is the most template-heavy
  library the C++ committee has ever shipped — deeply generic
  compile-time composition, concepts, customization points. Under
  `dialect-lean.md` none of that can sit anywhere near the madc
  surface, and the owner has now said so explicitly for concurrency.
- Disposition (two seams, cleanly separable):
  a. **Semantics through madc conveniences** — every std::execution
     capability maps onto the scope/channel/hub design: async scopes =
     counting_scope, scope-carried cancellation = stop tokens, hub
     verbs/queues = schedulers ("which loop runs this" as data),
     future-value composition (`when_all` = wait on N futures;
     `then` = ordinary sequential code, because on a stackful
     substrate CONTINUATIONS ARE JUST CODE AFTER A BLOCKING CALL —
     the laziness senders buy with templates, a context gets for
     free by not running until scheduled).
  b. **Interop later, at the transpiler arc** — when the C++23/26
     north star reaches `<execution>`-using sources, std::execution is
     a library-lowering problem for the C++ front (coroutine task +
     library calls), and the madc runtime's contexts can implement a
     sender-compatible completion seam. NOT this arc; named so nobody
     re-poses it.

### Adjacent datapoints (not asked, but they decide the argument)

- **Java/Loom**: virtual threads (stackful, colorless) FINAL since 21;
  JDK 25 (LTS, Sept 2025) finalized Scoped Values and carried
  Structured Concurrency's FIFTH preview (JEP 505 —
  `StructuredTaskScope.open()` + Joiner policies), sixth preview in 26
  (JEP 525), finalization expected ~27. The largest managed runtime on
  earth chose stackful + colorless + scopes, and had to rewrite the
  JDK's blocking IO to park virtual threads — madc's io verbs are
  being designed now, so that routing is a day-one property, not a
  retrofit.
- **Zig 0.16** (~Apr 2026, the closest C-like cousin): after DELETING
  its old colored async, ships `std.Io` — io capability passed as a
  PARAMETER (like Allocator), making async explicit and colorless;
  the same program runs blocking, thread-pooled, or stackless
  under different Io implementations. Zig needed the parameter because
  it refuses a runtime; madc has one (the hub), so the capability can
  be ambient — but the pluggable-execution-model idea is the same
  stage-1→3 sequencing the seed doc already carries.
- Score for stackful/colorless where the platform allows it: Go always,
  Java shipped it, .NET wanted it (blocked by legacy, now moving the
  state machine runtime-ward), Zig redesigned to escape coloring.
  Stackless/colored persists exactly where there is no runtime (Rust,
  embedded) or immovable legacy (C#, JS). madc has a runtime and no
  legacy: the substrate question has an industry-verified answer.

## Part B — synthesis: what the recon changes in the seed design

The seed doc's core (stackful contexts, two surfaces, cooperative-first,
channels=hub verbs, Tier-1 C11 lowering) SURVIVES the recon intact.
Four amendments earn their way in:

1. **Structured concurrency is the primary spawn surface.** The single
   strongest 2026 convergence: Kotlin scopes → Java
   StructuredTaskScope → C++26 counting_scope → Swift task groups →
   Python TaskGroups — and Go, the lone outlier, retrofitting
   (errgroup → WaitGroup.Go). madc should not re-live Go's retrofit:
   spawn lands INSIDE a scope that owns join/error/cancel; the
   program's main is the root scope; bare-detached `go` is the
   explicit escape hatch, not the default idiom.
2. **Cancellation rides the scope, implicitly.** Go's viral
   context parameter is the named cautionary tale; Kotlin/Java scoped
   propagation and C++26 stop tokens are the convergent answer. A
   context's scope carries the stop flag; blocking verbs (channel ops,
   sleep, io) are the cancellation points. No user-threaded parameter.
3. **Futures are values; `await` is a convenience, not a color.** A
   one-shot channel + receive gives C#-shaped ergonomics
   (`var f = madc::go(fn); ... f.await()` — spellings are owner-fork
   material) with zero front-end transform and zero coloring — the
   thing .NET is spending a decade migrating toward, free on day one.
   `when_all`/`then` composition (the C++26 conveniences) are library
   verbs over the same values.
4. **Determinism is an asset — design the tests in now.** Go needed
   synctest's virtual-time bubbles to RECOVER deterministic tests; a
   cooperative single-thread scheduler is deterministic BY
   CONSTRUCTION, and an owned event loop makes virtual-time sleep/io
   under test nearly free. Every concurrency feature ships with
   deterministic fulltest gates from the first slice (this also honors
   the cap-every-test-run rule — no flaky-timing suites, ever).

Unchanged and reaffirmed: actors = library pattern (context + owned
mailbox channel; supervisor = scope with restart policy — the SMAUG
server is the consumer); M:N rides the F2 static-actives audit with
user programs unchanged (Go's own contract: concurrency promised,
parallelism deferred); everything lowers Tier-1 to C11 runtime-library
calls (`madc_ctx_*`, `madc_chan_*`, `madc_scope_*`); blocking libc
routes through the event loop (the Loom lesson, day-one).

Implementation-cost ranking (cheapest first), for sequencing:
substrate + channels (runtime lib only) → Lua-coroutine surface (same
substrate, direct) → scopes + scope-cancellation (runtime lib) →
futures/await conveniences (library sugar) → actor/supervision pattern
(library) → [much later, other arcs] M:N workers (F2), C++26
std::execution interop (transpiler), stackless co_await (C++ front
compat only — never the madc surface).

## Part C — owner forks (supersedes the seed doc's list)

1. **Surface spellings**: `madc::` publics first (value-first), with
   keywords (`go`, `yield`, `await`?) later via the LanguageStd
   registry — or keywords from day one under `--std=madc`? And: does
   `await` deserve to exist as a word at all, or is `.recv()`/`.get()`
   on a future value enough?
2. **Scope ergonomics**: what does the scope look like in madc code —
   a block construct, an object with RAII-ish close, or both? (Kotlin:
   `coroutineScope { }`; Java: try-with-resources; C++26:
   counting_scope + join sender.)
3. **Channel contract**: how much of Go — buffered/unbuffered, select,
   close semantics, send-on-closed panics? And the unification detail:
   is a channel a hub verb pair (a datachannel with queue semantics),
   or the hub's queue exposed directly?
4. **Supervision depth**: restart policies as a first slice
   (one_for_one only?) or deferred until the SMAUG port actually
   demands them?
5. **Generator seat**: where the Lua-coroutine surface meets the
   polyglot namespaces — python:: generators as the first consumer?
6. **Preemption budget**: run-to-yield only (deterministic, testable) —
   or a later opt-in budget preemption (Go's async preemption is a
   hard, years-later problem; recommendation: run-to-yield until F2).

## Sources (recon, 2026-08)

- C++26: cppreference "Execution control library"; InfoQ 2026-04
  "C++26: Reflection, Memory Safety, Contracts, and a New Async
  Model"; Herb Sutter's St. Louis trip report (P2300 adopted); WG21
  P3552 (task, adopted Sofia), P3149 (async_scope/counting_scope),
  P2079 (parallel/system scheduler); modernescpp "An Overview of
  C++26: Concurrency".
- .NET: dotnet/runtimelab issue 2398 (green-thread experiment
  results/rejection); steven-giesel.com "Runtime async is hitting
  .NET 11"; InfoQ 2026-02 ".NET 11 Preview 1" (Runtime Async
  headline).
- Java: JEP 505 (JDK 25, 5th preview), JEP 525 (JDK 26, 6th preview);
  javapro.io on JDK 25 virtual threads; JEP 506 scoped values final.
- Zig: kristoff.it "Zig's New Async I/O"; ziglang.org 0.15.1 release
  notes ("Writergate"); lalinsky.com "Async I/O in Zig 0.16, today".
- Go: go.dev/blog/synctest; pkg.go.dev testing/synctest (stable 1.25,
  experiment API removed 1.26); appliedgo.net on synctest.
- Elixir: elixir-lang.org v1.19 release post (types + 4x parallel
  compilation); hexdocs v1.20 changelog (full inference, OTP 29).
- Rust: RFC 3668 async closures; rust-lang async book "State of Async
  Rust"; corrode.dev "The State of Async Rust: Runtimes"
  (fragmentation); RPITIT since 1.75; dyn-async-trait still open.
- Kotlin: jdriven.com 2026-02 "Virtual Threads and Coroutines";
  codemotion 2026 "Virtual Threads vs. Coroutines in 2026"; xebia
  "Structured Concurrency: Will Java Loom Beat Kotlin's Coroutines?".
