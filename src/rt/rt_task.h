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
