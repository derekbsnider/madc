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
#include "rt_except.h"	/* per-context exception-state switch */

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
#include <time.h>	/* the real time source: clock_gettime + nanosleep */
#endif

// ---------------------------------------------------------------------------
// The task (a stack + state)
// ---------------------------------------------------------------------------

// Opaque per-task exception-state buffer (rt_except's try/cleanup chains +
// the in-flight exception — per-CONTEXT state switched like registers).
// Zero-filled = the empty state a fresh task starts with; capacity checked
// loud at init against __madc_except_state_size().
#define MADC_TASK_EXC_BYTES 64

typedef struct madc_task {
	struct madc_task *qnext;   // ready-queue link
	struct madc_task *tnext;   // timer-list link (deadline ascending)
	long long deadline;        // ms on the time source; timer list only
	int queued;                // on the ready queue (enqueue is idempotent:
				   // a task queued twice RUNS twice — and the
				   // blind relink would truncate the queue)
	void (*fn)(void *);
	void *arg;
	unsigned char exc[MADC_TASK_EXC_BYTES];
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
	if (t->queued)
		return;	// idempotent: two io events (or an io event racing a
			// close wake) may both try to wake one task per round
	t->queued = 1;
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
		t->queued = 0;
	}
	return t;
}

// ---------------------------------------------------------------------------
// The time source (sched layer, MT-4) — pluggable per the ctx/sched/loop
// separation: real (monotonic) by default, VIRTUAL under MADC_TASK_VTIME=1.
// Virtual time never sleeps: when only timer-parked tasks exist the clock
// JUMPS to the earliest deadline — sleep-based tests run instantly and
// deterministically (recon amendment 4's virtual-time gate).
// ---------------------------------------------------------------------------

static long long g_vclock_ms;      // virtual now; only advances at idle

static int vtime_on(void)
{
	static int on = -1;
	if (on < 0)
		on = getenv("MADC_TASK_VTIME") ? 1 : 0;
	return on;
}

static long long time_now_ms(void)
{
	if (vtime_on())
		return g_vclock_ms;
#if defined(_WIN32)
	return (long long)GetTickCount64();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void time_real_sleep_ms(long long ms)
{
	if (ms <= 0)
		return;
#if defined(_WIN32)
	Sleep((DWORD)ms);
#else
	struct timespec ts;
	ts.tv_sec = (time_t)(ms / 1000);
	ts.tv_nsec = (long)(ms % 1000) * 1000000;
	nanosleep(&ts, NULL);
#endif
}

// Timer-parked tasks, deadline ascending (FIFO among equal deadlines — the
// insert walks past equals, so wake order stays deterministic). A sorted
// list, not a binary heap: cooperative scale is a handful of sleepers, and
// the list keeps the discipline readable.
static madc_task *g_timer_head;
static long g_timer_count;

static void timer_insert(madc_task *t)
{
	madc_task **pp = &g_timer_head;
	while (*pp && (*pp)->deadline <= t->deadline)
		pp = &(*pp)->tnext;
	t->tnext = *pp;
	*pp = t;
	++g_timer_count;
	TASK_TRACE("[task] timer+ %p deadline=%lld (%ld pending)\n",
		   (void *)t, t->deadline, g_timer_count);
}

// Enqueue every due sleeper. Called at each scheduling decision so a due
// timer never starves behind busy yielders.
static void timer_fire_due(void)
{
	if (!g_timer_head)
		return;
	long long now = time_now_ms();
	while (g_timer_head && g_timer_head->deadline <= now) {
		madc_task *t = g_timer_head;
		g_timer_head = t->tnext;
		t->tnext = NULL;
		--g_timer_count;
		TASK_TRACE("[task] timer! %p now=%lld\n", (void *)t, now);
		task_enqueue(t);
	}
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

// The io-wait seat (MT-4b): NULL until an io layer installs its hook — the
// scheduler stays fd-blind (the __madc_task_join_hook precedent). Contract
// in rt_task.h.
int (*__madc_task_io_wait_hook)(long long timeout_ms);

// THE scheduling decision (MT-4): fire due timers, take the ready head, and
// when ONLY parked tasks exist, wait on whatever can wake one — the io hook
// (fd readiness) bounded by the earliest timer deadline, else the time
// source alone (the virtual clock JUMPS to the deadline, the real one
// sleeps to it). NULL means genuine deadlock territory: nothing runnable,
// no timer pending, no io waiter registered (the caller owns its own
// message/fallback). Every "pick next" site routes here so a sleeper or an
// io waiter can never be starved or misread as a deadlock.
static madc_task *task_next_or_wait(void)
{
	for (;;) {
		timer_fire_due();
		madc_task *next = task_dequeue();
		if (next)
			return next;
		// Timeout for the io wait: block forever with no timers;
		// probe only under virtual time (the jump owns the advance).
		long long tmo = -1;
		if (g_timer_head) {
			if (vtime_on()) {
				tmo = 0;
			} else {
				tmo = g_timer_head->deadline - time_now_ms();
				if (tmo < 0)
					tmo = 0;
			}
		}
		int (*io)(long long) = __madc_task_io_wait_hook;
		int fired = io ? io(tmo) : -1;
		if (fired > 0)
			continue;	// the io layer enqueued a waiter
		if (fired == 0 && !g_timer_head)
			continue;	// blocking wait woke empty (EINTR)
		if (!g_timer_head)
			return NULL;	// no io waiters, no timers: deadlock
		if (vtime_on()) {
			g_vclock_ms = g_timer_head->deadline;
			TASK_TRACE("[task] vclock -> %lld\n", g_vclock_ms);
		} else if (fired < 0) {
			// No io waiters took the wait — sleep it ourselves.
			time_real_sleep_ms(tmo);
		}
		// fired == 0 with a timer pending: the io wait consumed the
		// timeout; the loop head fires the now-due deadline.
	}
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
	__madc_except_state_save(from->exc);
	__madc_except_state_restore(to->exc);
	g_current = to;
#if defined(_WIN32)
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
	madc_task *next = task_next_or_wait();
	TASK_TRACE("[task] exit %p -> %p live=%ld waiting=%d\n", (void *)self,
		   (void *)next, g_live, g_main_waiting);
	if (!next) {
		if (!g_main_waiting) {
			// Reachable since parking exists: the exiting task was
			// the last runnable flow while main and others sit
			// parked on channels nobody can now signal.
			fprintf(stderr, "madc tasks: deadlock — %ld task(s)"
				" live, all blocked, and the last runnable"
				" task exited\n", g_live);
			abort();
		}
		next = &g_main_task;
	}
	g_reap = self;
	g_current = next;
	// The dying task's exception state is dropped with it.
	__madc_except_state_restore(next->exc);
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
	if (__madc_except_state_size() > (unsigned long)MADC_TASK_EXC_BYTES) {
		fprintf(stderr, "madc tasks: exception-state buffer too small"
			" (%lu > %d; grow MADC_TASK_EXC_BYTES)\n",
			__madc_except_state_size(), MADC_TASK_EXC_BYTES);
		abort();
	}
	memset(&g_main_task, 0, sizeof g_main_task);
#if defined(_WIN32)
	// No GetCurrentFiber() fallback for an already-converted thread: the
	// mingw intrinsic (__readgsqword) trips gcc's array-bounds -Werror,
	// and an embedding host that fiber-converts its thread before letting
	// madc spawn is a contract we refuse LOUD until it exists.
	g_main_task.fiber = ConvertThreadToFiber(NULL);
	if (!g_main_task.fiber) {
		fprintf(stderr, "madc tasks: ConvertThreadToFiber failed"
			" (already a fiber? fiber-converted embedding hosts"
			" are not supported yet)\n");
		abort();
	}
#endif
	g_current = &g_main_task;
	// The PRINCIPLED join point (MT-2b): every --std=madc main's emitted
	// wrapper calls __madc_task_join_point() at MAIN'S END — before any
	// teardown (glibc runs TLS destructors before the atexit list). The
	// dispatcher lives on the AOT ledger (rt_task_join.c); installing the
	// hook here arms it only in programs that actually spawned.
	__madc_task_join_hook = __madc_task_join_all;
	// Belts, both idempotent no-ops after the wrapper's join: the JIT
	// host joins right after main returns, and this atexit copy covers
	// artifacts whose main predates the wrapper (strict-mode mains in
	// mixed-TU projects spawning through a madc TU).
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

// Fire everything due without running anyone (rt_task.h contract). The io
// hook's zero-timeout probe is cheap when idle: no registered waiter means
// no syscall (the hook returns -1 before polling).
void __madc_task_fire_due(void)
{
	timer_fire_due();
	int (*io)(long long) = __madc_task_io_wait_hook;
	if (io)
		io(0);
}

void __madc_yield(void)
{
	if (!g_current)
		return;                        // runtime never used
	__madc_task_fire_due();	// due sleepers AND fd-parked tasks join the
				// queue NOW — a busy yielder must never
				// starve either (MT-4 timers, MT-4b io)
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
		madc_task *next = task_next_or_wait();
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

long __madc_task_runnable(void)
{
	long n = 0;
	madc_task *t;
	for (t = g_ready_head; t; t = t->qnext)
		n++;
	return n;
}

void *__madc_task_current(void)
{
	task_runtime_init();
	return (void *)g_current;
}

void __madc_task_park(void)
{
	task_runtime_init();
	madc_task *self = g_current;
	madc_task *next = task_next_or_wait();
	TASK_TRACE("[task] park %p -> %p\n", (void *)self, (void *)next);
	if (next == self)
		return;	// a sleeper whose own deadline was the wake (MT-4)
	if (!next) {
		// Single OS thread: with the parker off the CPU and nothing
		// runnable, no flow exists that could ever unpark anyone —
		// including main, whether it is joining or parked itself.
		fprintf(stderr, "madc tasks: deadlock — a blocking operation"
			" parked the last runnable flow (%ld task(s) live)\n",
			g_live);
		abort();
	}
	// NOT re-enqueued: the waker holds the handle and unparks it.
	task_switch(self, next);
}

void __madc_task_unpark(void *task)
{
	if (!task)
		return;
	TASK_TRACE("[task] unpark %p\n", task);
	task_enqueue((madc_task *)task);
}

void __madc_task_sleep_ms(long long ms)
{
	task_runtime_init();
	if (ms < 0)
		ms = 0;
	madc_task *self = g_current;
	self->deadline = time_now_ms() + ms;
	timer_insert(self);
	// Parked on the TIMER list, not the ready queue: task_next_or_wait
	// (inside park) fires due deadlines and advances the time source
	// when nothing else is runnable — park returns immediately when our
	// own deadline was the wake (next == self).
	__madc_task_park();
}
