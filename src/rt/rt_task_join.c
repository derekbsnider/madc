// SPDX-License-Identifier: MPL-2.0
// madc task join point (MT-2b) — the ledger-safe seam between every
// madc-mode main's emitted wrapper and the hosted task runtime.
//
// The wrapper the CIR builder synthesizes around a --std=madc main calls
// __madc_task_join_point() unconditionally at MAIN'S END. The real join
// (__madc_task_join_all, src/rt/rt_task.c) is a HOSTED runtime object —
// ucontext/fibers keep it off the AOT ledger — so the wrapper cannot
// reference it directly without breaking every -static-libmadc artifact
// that never spawns a task. This dispatcher IS on the ledger (strict C11,
// no builtins, no C++ runtime): the task runtime installs the hook when it
// initializes (first spawn); a program that never spawned leaves it NULL
// and the join point is a no-op.
//
// THREAD-SAFETY CONTRACT: set-once from the single scheduler thread before
// any task exists; read from the same thread at main's end. No concurrent
// mutation by construction (the task runtime is single OS thread).

#include "rt_task.h"	/* join-point + hook prototypes, CHECKED here */

void (*__madc_task_join_hook)(void) = 0;

void __madc_task_join_point(void)
{
	void (*h)(void) = __madc_task_join_hook;
	if (h)
		h();
}
