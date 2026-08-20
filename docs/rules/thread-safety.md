# Thread Safety — reasoning

## Why the law exists

Owner, 2026-08-20, during the Track 7 data-hub design sessions: most CPUs
are multi-core; a language that cannot exploit them "stays a hobby
language." madc had not addressed threads or IPC at all. Retrofitting
thread-safety onto a language surface is the most expensive way to get it —
every feature shipped without a contract becomes an audit item, and the
audits arrive exactly when a user hits a data race in production. The cheap
time to decide a feature's concurrency contract is while the feature is
being designed, so the law binds every addition from 2026-08-20 forward.

## Why "stated contract," not "everything takes a lock"

Blanket internal locking is the wrong meaning of thread-safe:

- It taxes the single-threaded case, and madc's standing performance value
  is that -O0 must be fast (feedback: optimize algorithms, not flags).
- Fine-grained internal locks compose into deadlocks; the caller cannot see
  the lock order.
- The C++ standard library already solved this vocabulary problem, and madc
  is a C/C++ dialect: concurrent reads are safe, distinct objects are safe,
  shared mutation is the caller's synchronization problem. Matching that
  convention means C++ programmers' instincts transfer directly.

So the deliverable per feature is a CONTRACT — one or two sentences in the
plan doc saying which of its operations are concurrently safe and what the
caller owes for the rest. "Not thread-safe; confine to one thread" is a
legal contract for the right feature. An UNSTATED contract is the only
illegal state.

## Why shared mutation routes through the hub

The Track 7 design (demand 15) makes collaboration and concurrency the same
design at different latencies: mutations flow through verbs (serializable
units), reads flow through snapshot projections (MVCC — readers never block
writers), and propagation is semantic diffs over channels that ride
identically between threads (in-memory), processes (FIFO/UDS), and machines
(sockets). Ad-hoc locks inside individual runtime helpers would create a
second, incompatible concurrency model with no owner — the exact
parallel-implementation drift the no-parallel-implementations rule exists
to prevent.

## Known pre-law debt (do not claim these safe)

- `DBG(x)` — `madc_verbose` gating is effectively dead on worker threads
  (thread_local interaction; recorded in the gated-debug feedback memory).
- `MadValue`/`madc::value` cell refcounts are not atomic; the value-ABI arc
  (2026-06-12 design) must land them atomic per demand 15.
- Runtime globals (stream objects, `madc::sys`, ledger state) predate any
  contract.
- MIR: generation and execution state is per-`MIR_context_t`; sharing one
  context across threads is not covered by any contract we have verified.

These are owned by the F2 arc's audit, not by whoever happens to touch the
file next — but per fix-what-you-find, a data race actually OBSERVED is a
defect to fix now, not audit debt.

## The gate

A rule without a gate decays. Until language threads exist there is nothing
for a sanitizer to run, so the interim gate is procedural: plan-doc review
checks for the contract sentence. When the F2 arc lands thread primitives,
a ThreadSanitizer lane joins the battery (same pattern as the warning
census: a clean baseline, then a ratchet), and the procedural gate retires
in favor of the mechanical one.
