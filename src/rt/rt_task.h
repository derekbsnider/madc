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
// no-op when the runtime was never used. The JIT host calls it right after
// the program's main returns; native artifacts reach it through the
// atexit() registration made on first spawn (same function, one owner).
void __madc_task_join_all(void);

// Live spawned-but-unfinished task count (diagnostics and tests).
long __madc_task_live(void);

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
