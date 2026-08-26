# madc multitasking — coroutines and goroutines on one substrate

Owner direction (2026-08-26, mid-session): "there are two paths…
threading and cooperative (like how nginx and node.js work); it might be
useful for multitasking support to be an inherent part of the madc
language, and maybe it should support both lua-style coroutines and
golang style goroutines."

Seed doc for the owner brainstorm. Constraints it must honour: the
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

- A coroutine cannot yield inside a long C++ ENGINE call: madcide's
  background parse stays the fork-vs-threads owner fork
  (2026-08-26-madcide-staged-parse-and-state.md). But the event-loop
  wake-fd seam designed there is the same event loop a cooperative
  scheduler needs — one design serves both.
- Blocking libc calls (read/accept/sleep) block the whole cooperative
  world unless routed through the event loop (nonblocking + park) —
  the node/nginx discipline; the runtime's io verbs are where that
  lives, not user code.

## Open owner forks (brainstorm agenda)

1. Surface spellings: publics-first vs keywords-first; `go` as keyword
   under `--std=madc` only?
2. Channel semantics: buffered/unbuffered, select, close — how much of
   Go's contract; how it unifies with hub verbs/datachannels.
3. Scheduler shape: run-to-yield only, or also preemption-by-budget
   (Go's async preemption is a much later problem).
4. Where generators (Lua-style) surface in the polyglot namespaces
   (python:: generators are the obvious consumer).
