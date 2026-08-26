// SPDX-License-Identifier: MPL-2.0
// madc cooperative task runtime (MT-1 substrate). See rt_task.h for the
// thread-safety contract and the internal task/scheduler/resource layering.
//
// Hosted runtime object (host compiler, not the AOT ledger): the context
// backend is ucontext on POSIX and fibers on Win64. Scheduling is strict
// FIFO run-to-yield, so task interleaving is DETERMINISTIC by construction
// — the test gates pin exact orders.

// Feature macros before ANY libc include: ucontext declarations need the
// XOPEN surface under a strict -std=c11 compile (and on macOS outright).
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 600
#endif
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif

#include "rt_task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#else
#include <ucontext.h>
#endif

// ---------------------------------------------------------------------------
// The task (a stack + state)
// ---------------------------------------------------------------------------

typedef struct madc_task {
	struct madc_task *qnext;   // ready-queue link
	void (*fn)(void *);
	void *arg;
	int is_main;
#if defined(_WIN32)
	void *fiber;               // CreateFiberEx handle (main: converted)
#else
	ucontext_t uc;
	void *stack;               // malloc'd; freed by the reaper
#endif
} madc_task;

// ---------------------------------------------------------------------------
// The scheduler (who runs next) — one FIFO ready queue, one current task.
// ---------------------------------------------------------------------------

static madc_task g_main_task;      // the calling thread's flow, adopted lazily
static madc_task *g_current;       // NULL until first spawn
static madc_task *g_ready_head;
static madc_task *g_ready_tail;
static long g_live;                // spawned, not yet finished
static madc_task *g_reap;          // finished task whose stack the NEXT
				   // context frees (never free a live stack)
static int g_main_waiting;         // main is parked in __madc_task_join_all
#if !defined(_WIN32)
// makecontext passes int arguments only, so a first entry reads its task
// from this slot; it is (re)set immediately before EVERY switch, and only a
// context's very first run consumes it. Exact under the single-OS-thread
// contract.
static madc_task *g_starting;
#endif

// Unbuffered scheduler trace, MADC_TASK_TRACE=1 (diagnostics only; stdout
// buffering makes crash-adjacent println output vanish, stderr does not).
static int task_trace_on(void)
{
	static int on = -1;
	if (on < 0)
		on = getenv("MADC_TASK_TRACE") ? 1 : 0;
	return on;
}
#define TASK_TRACE(...) \
	do { if (task_trace_on()) fprintf(stderr, __VA_ARGS__); } while (0)

static void task_enqueue(madc_task *t)
{
	t->qnext = NULL;
	if (g_ready_tail)
		g_ready_tail->qnext = t;
	else
		g_ready_head = t;
	g_ready_tail = t;
}

static madc_task *task_dequeue(void)
{
	madc_task *t = g_ready_head;
	if (t) {
		g_ready_head = t->qnext;
		if (!g_ready_head)
			g_ready_tail = NULL;
		t->qnext = NULL;
	}
	return t;
}

static void task_reap(void)
{
	if (!g_reap)
		return;
#if defined(_WIN32)
	if (g_reap->fiber)
		DeleteFiber(g_reap->fiber);
#else
	free(g_reap->stack);
#endif
	free(g_reap);
	g_reap = NULL;
}

// Liberal default (stacks page in lazily); the knob is the
// MADC_TASK_STACK_KB environment variable, named in the failure messages
// per the liberal-resource-guards rule.
static size_t task_stack_bytes(void)
{
	static size_t bytes;
	if (!bytes) {
		const char *kb = getenv("MADC_TASK_STACK_KB");
		long v = kb ? atol(kb) : 0;
		bytes = (v >= 64) ? (size_t)v * 1024u : (size_t)512 * 1024u;
	}
	return bytes;
}

// ---------------------------------------------------------------------------
// The execution resource (what it runs on) — the context backend.
// ---------------------------------------------------------------------------

// Switch from the running task to `to` (which may be entering for the first
// time). When control eventually returns here, free any corpse left behind.
static void task_switch(madc_task *from, madc_task *to)
{
	TASK_TRACE("[task] switch %p -> %p (main=%p)\n", (void *)from,
		   (void *)to, (void *)&g_main_task);
	g_current = to;
#if defined(_WIN32)
	(void)from;
	SwitchToFiber(to->fiber);
#else
	g_starting = to;
	swapcontext(&from->uc, &to->uc);
#endif
	task_reap();
}

// Runs on the DYING task's stack: hand the corpse to the reaper, pick the
// next context, and never return.
static void task_exit_switch(void)
{
	madc_task *self = g_current;
	madc_task *next = task_dequeue();
	TASK_TRACE("[task] exit %p -> %p live=%ld waiting=%d\n", (void *)self,
		   (void *)next, g_live, g_main_waiting);
	if (!next) {
		if (!g_main_waiting) {
			fprintf(stderr, "madc tasks: internal scheduler state"
				" broken (a task finished with no runnable"
				" successor and main not joining)\n");
			abort();
		}
		next = &g_main_task;
	}
	g_reap = self;
	g_current = next;
#if defined(_WIN32)
	SwitchToFiber(next->fiber);
#else
	g_starting = next;
	setcontext(&next->uc);
#endif
	abort();               // unreachable
}

#if defined(_WIN32)
static void CALLBACK task_trampoline(void *param)
{
	madc_task *t = (madc_task *)param;
	task_reap();
	t->fn(t->arg);
	--g_live;
	task_exit_switch();
}
#else
static void task_trampoline(void)
{
	madc_task *t = g_starting;
	task_reap();
	TASK_TRACE("[task] trampoline enter t=%p fn=%p\n", (void *)t,
		   (void *)(size_t)t->fn);
	t->fn(t->arg);
	--g_live;
	task_exit_switch();
}
#endif

static int task_backend_create(madc_task *t)
{
#if defined(_WIN32)
	t->fiber = CreateFiberEx(64 * 1024, task_stack_bytes(), 0,
				 task_trampoline, t);
	return t->fiber != NULL;
#else
	if (getcontext(&t->uc) != 0)
		return 0;
	t->stack = malloc(task_stack_bytes());
	if (!t->stack)
		return 0;
	t->uc.uc_stack.ss_sp = t->stack;
	t->uc.uc_stack.ss_size = task_stack_bytes();
	t->uc.uc_link = NULL;      // exits go through task_exit_switch
	makecontext(&t->uc, task_trampoline, 0);
	return 1;
#endif
}

// ---------------------------------------------------------------------------
// The surface
// ---------------------------------------------------------------------------

static void task_runtime_init(void)
{
	if (g_current)
		return;
	memset(&g_main_task, 0, sizeof g_main_task);
	g_main_task.is_main = 1;
#if defined(_WIN32)
	g_main_task.fiber = ConvertThreadToFiber(NULL);
	if (!g_main_task.fiber)        // already a fiber (embedding host)
		g_main_task.fiber = GetCurrentFiber();
#endif
	g_current = &g_main_task;
	// Native artifacts join the root scope at process exit; the JIT host
	// calls __madc_task_join_all right after main instead (join is
	// idempotent, so the atexit copy then no-ops).
	atexit(__madc_task_join_all);
}

void __madc_go(void (*fn)(void *), void *arg)
{
	task_runtime_init();
	madc_task *t = (madc_task *)calloc(1, sizeof *t);
	if (!t) {
		fprintf(stderr, "madc tasks: task allocation failed\n");
		abort();
	}
	t->fn = fn;
	t->arg = arg;
	if (!task_backend_create(t)) {
		fprintf(stderr, "madc tasks: context creation failed"
			" (stack %zu bytes; knob MADC_TASK_STACK_KB)\n",
			task_stack_bytes());
		abort();
	}
	++g_live;
	TASK_TRACE("[task] go t=%p fn=%p arg=%p live=%ld\n", (void *)t,
		   (void *)(size_t)fn, arg, g_live);
	task_enqueue(t);
}

void __madc_yield(void)
{
	if (!g_current)
		return;                        // runtime never used
	madc_task *next = task_dequeue();
	if (!next)
		return;                        // only runner — keep going
	madc_task *self = g_current;
	task_enqueue(self);
	task_switch(self, next);
}

void __madc_task_join_all(void)
{
	if (!g_current || g_current != &g_main_task)
		return;                        // never used, or not main
	while (g_live > 0) {
		madc_task *next = task_dequeue();
		TASK_TRACE("[task] join live=%ld next=%p\n", g_live,
			   (void *)next);
		if (!next) {
			fprintf(stderr, "madc tasks: deadlock — %ld task(s)"
				" still live with nothing runnable\n", g_live);
			abort();
		}
		g_main_waiting = 1;
		task_switch(&g_main_task, next);
		g_main_waiting = 0;
	}
}

long __madc_task_live(void)
{
	return g_live;
}
