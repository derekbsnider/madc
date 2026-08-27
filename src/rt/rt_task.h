// SPDX-License-Identifier: MPL-2.0
// madc cooperative task runtime (MT-1 substrate) — the engine side of the
// dialect's `go` / `yield()` surface.
//
// THREAD-SAFETY CONTRACT (stated per .claude/rules/thread-safety.md): the
// entire task runtime is SINGLE OS THREAD, cooperative. Contexts interleave
// only at explicit switch points (spawn never switches; yield and the root
// join do). No engine static is ever raced because there is never a second
// thread; the M:N upgrade rides the F2 static-actives audit
// (docs/plans/2026-08-26-madc-multitasking-design.md).
//
// INTERNAL LAYERING (task -> scheduler -> execution resource, adopted
// runtime-internal only): madc_task (a stack + state), the ready queue +
// switch discipline (who runs next), and the backend context primitive
// (what it runs on: ucontext on POSIX, fibers on Win64) are kept as
// separate seams inside rt_task.c so a swapped scheduler/time source
// (virtual-time test gates, MT-4) and added resources (M:N pools, F2)
// never touch the surface. User code stays venue-blind.
//
// Hosted runtime object (compiled into madc/libmadc like the madc_value_*
// accessors) — NOT on the AOT ledger yet: a `-static-libmadc` artifact that
// spawns tasks fails its link loudly (named MT-1 residue; the ledger seat
// needs a strict-C11 context backend story).

#ifndef __RT_TASK_H
#define __RT_TASK_H 1

#ifdef __cplusplus
extern "C" {
#endif

// Spawn: enqueue fn(arg) as a new cooperative task (FIFO; the spawner keeps
// running — Go semantics). Lazily converts the calling thread's flow into
// the main task on first use.
void __madc_go(void (*fn)(void *), void *arg);

// Reschedule: enqueue the current task at the ready tail and run the head.
// No-op when nothing else is runnable.
void __madc_yield(void);

// Root-scope join: run ready tasks until none remain live. Idempotent and a
// no-op when the runtime was never used. The PRIMARY caller is the emitted
// main wrapper (MT-2b) via __madc_task_join_point below — the join runs at
// MAIN'S END in every lane. Belts: the JIT host calls this right after main
// returns, and the atexit() registration made on first spawn covers
// strict-mode mains in mixed-TU projects (same function, one owner).
void __madc_task_join_all(void);

// Live spawned-but-unfinished task count (diagnostics and tests).
long __madc_task_live(void);

// Ready-queue length: tasks runnable RIGHT NOW (live minus parked minus
// running). The cooperative event loop's poll-or-block decision — a parked
// task must not busy-spin the loop (it is woken by an unpark, never by a
// poll). O(queue length); the queue is short by construction.
long __madc_task_runnable(void);

// Park/unpark — the blocking primitive channels (and every later blocking
// verb) are built on. `current` returns the running task's opaque handle
// (adopting the calling flow as the main task on first use). `park` takes
// the CURRENT task off the CPU without re-enqueueing it — the waker must
// hold the handle and `unpark` it back onto the ready queue; parking with
// nothing else runnable is a DEADLOCK and aborts loud (single OS thread:
// nobody is left to wake anyone). Wait-queue bookkeeping (who holds the
// handle, why it parked) belongs to the CALLER — e.g. the channel runtime
// (src/madc_task_chan.cpp) — never to this scheduler.
void *__madc_task_current(void);
void __madc_task_park(void);
void __madc_task_unpark(void *task);

// Sleep on the scheduler's TIME SOURCE (MT-4): park the current task for
// `ms` milliseconds of scheduler time. The source is pluggable per the
// ctx/sched/loop layering — real monotonic time by default; under
// MADC_TASK_VTIME=1 a VIRTUAL clock that jumps to the earliest deadline
// whenever only timer-parked tasks exist, so sleep-based tests run
// instantly and deterministically. ms <= 0 still parks through one
// scheduling decision (a fair yield to equal-deadline sleepers).
void __madc_task_sleep_ms(long long ms);

// ---- Structured scopes + cancellation (MT-3) ------------------------------
// The Kotlin ownership over Go's spelling: `go` attaches the new task to
// the spawner's innermost open scope (none = the root scope — exactly the
// pre-scope semantics, drained at main's end). A scope owns join, error,
// and cancel. The int64 handle registry and its dialect throws live with
// the C++ surface (madc_task_chan.cpp); these are the raw seams.

// Open a scope: becomes the calling task's innermost (its spawns attach
// here). Returns the opaque scope.
void *__madc_scope_begin(void);

// Join, then outcome. Blocks (parks the caller) until every member — and,
// transitively, every member of scopes opened inside it — has finished;
// the join ALWAYS completes before any throw, so bookkeeping never leaks.
// Then: throws the FIRST captured member error (as text), else throws the
// cancelled literal when the scope was cancel-requested, else returns.
// Caller must be the opener and the scope its innermost (throws otherwise).
void __madc_scope_end(void *scope);

// Pre-validate an end WITHOUT consuming anything: 0 = would proceed,
// 1 = caller is not the opener, 2 = not the caller's innermost open scope.
// The handle layer checks this first so a refused end leaves its registry
// consistent (scope_end's own throws bypass C++ frames via longjmp).
int __madc_scope_end_check(void *scope);

// The MT-5 `scope { ... }` keyword block (the CIR builder emits this pair;
// no int64 handle). enter = scope_begin + an unwind registration on the
// exception runtime's cleanup stack, so a throw ESCAPING the block quietly
// abandons the scope mid-unwind (cancel members + join + pop cur + free;
// the in-flight error wins) instead of leaving a dead block on the task's
// chain; a throw caught INSIDE the block never touches it. exit = remove
// the registration, then scope_end (join + rethrow) — its check failures
// are engine bugs and throw loud.
void *__madc_scope_block_enter(void);
void __madc_scope_block_exit(void *scope);

// Request cancellation of the scope's whole subtree: flag + wake every
// member (transitively). Returns immediately; any task may call it.
void __madc_scope_cancel(void *scope);

// Request cancellation of ONE task: sticky flag + a wake (a timer-parked
// sleeper leaves the timer list now; a channel/io-parked task is enqueued
// and its blocking verb removes its own waiter record on resume). Every
// blocking verb throws THE cancelled literal before parking and on resume.
void __madc_task_cancel_request(void *task);

// Is the CURRENT task cancel-requested — its own flag, or any scope on its
// open-scope chain (the opener of a cancelled scope polls true). The
// compute-loop poll; yield() is NOT a cancellation point.
int __madc_task_cancelled(void);

// THE check-and-throw: throws the cancelled literal when the current task
// is cancel-requested (the chain predicate above). Every blocking verb's
// entry and resume gates call this one owner.
void __madc_task_throw_if_cancelled(void);

// THE cancellation literal: every cancellation throw uses exactly this
// pointer, so pointer identity distinguishes a completing cancellation
// from user text at the trampoline's capture.
const char *__madc_task_cancelled_text(void);

// Fire everything due — expired timers and the io hook's zero-timeout
// probe — WITHOUT running anyone (IDE-10b): woken tasks land on the ready
// queue and the caller reads __madc_task_runnable() to see what changed.
// The cooperative event loop's probe between input polls; __madc_yield
// fires the same set at its head so a busy yielder can starve neither a
// sleeper nor an fd-parked task.
void __madc_task_fire_due(void);

// The io-wait seat (MT-4b): the scheduler stays fd-BLIND — an io layer
// (src/madc_task_chan.cpp's taskio) installs this hook, and
// task_next_or_wait calls it whenever nothing is runnable. Contract:
//   return -1  = no io waiters registered (the hook did NOT wait);
//   return  0  = waited up to timeout_ms (-1 = block until an event;
//                0 = probe) and enqueued nothing (timeout or EINTR);
//   return >0  = enqueued that many tasks (via __madc_task_unpark).
// A hook that returns 0 without having waited ~timeout_ms busy-spins the
// scheduler — it must actually wait. Under MADC_TASK_VTIME=1 with virtual
// timers pending the hook only gets zero-timeout probes (the clock jumps
// deadlines; io readiness is checked opportunistically at each decision).
extern int (*__madc_task_io_wait_hook)(long long timeout_ms);

// The ledger-safe join point (MT-2b, src/rt/rt_task_join.c): every
// --std=madc main's emitted wrapper calls this at MAIN'S END. It dispatches
// through the hook, which THIS runtime installs at init (first spawn) —
// a program that never spawned leaves it NULL and the call is a no-op, so
// a -static-libmadc artifact links the tiny ledger dispatcher without
// pulling the hosted context backend.
void __madc_task_join_point(void);
extern void (*__madc_task_join_hook)(void);

#ifdef __cplusplus
}
#endif

#endif // __RT_TASK_H
