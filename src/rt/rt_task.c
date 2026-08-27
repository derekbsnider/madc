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
#include <setjmp.h>	/* the trampoline's scope catch-all frame (MT-3) */

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

// The trampoline's scope catch-all frame (MT-3) — an opaque MadcTryContext
// on the task body's stack. Capacity checked loud at init against
// __madc_try_context_size() (jmp_buf is 200B on SysV glibc, 256B on Win64).
#define MADC_TASK_TRYCTX_BYTES 320

// A captured scope error / a scope_end rethrow, as one line of text
// (faithful non-text rethrow is a named MT-3 residue).
#define MADC_SCOPE_ERR_BYTES 192

struct madc_scope;

typedef struct madc_task {
	struct madc_task *qnext;   // ready-queue link
	struct madc_task *tnext;   // timer-list link (deadline ascending)
	long long deadline;        // ms on the time source; timer list only
	int queued;                // on the ready queue (enqueue is idempotent:
				   // a task queued twice RUNS twice — and the
				   // blind relink would truncate the queue)
	int cancel_req;            // cancellation requested (MT-3): sticky;
				   // every blocking verb throws THE cancelled
				   // literal on entry and on resume
	struct madc_scope *scope;  // owning scope (NULL = the root scope)
	struct madc_scope *cur;    // innermost scope THIS task has open —
				   // what its spawns attach to
	struct madc_task *snext, *sprev; // scope member-list links
	char scope_err[MADC_SCOPE_ERR_BYTES]; // scope_end rethrow storage
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

// Unlink a task from the timer list if present (MT-3: cancelling a sleeper
// wakes it NOW; a fired-or-woken task must never be reachable from the list
// after it unwinds).
static void timer_unlink(madc_task *t)
{
	madc_task **pp = &g_timer_head;
	while (*pp && *pp != t)
		pp = &(*pp)->tnext;
	if (*pp) {
		*pp = t->tnext;
		t->tnext = NULL;
		--g_timer_count;
		TASK_TRACE("[task] timer- %p (%ld pending)\n", (void *)t,
			   g_timer_count);
	}
}

// ---------------------------------------------------------------------------
// Structured scopes (MT-3) — the Kotlin ownership over Go's spelling: every
// spawn attaches to the spawner's innermost open scope (none = the root
// scope, task->scope NULL — exactly the pre-scope semantics, drained at
// main's end). A scope owns join (scope_end parks its OPENER until every
// member — and every member of its child scopes — finished), error (the
// FIRST uncaught member error is captured as text and rethrown at the join;
// a failed member cancels its siblings), and cancel (a flag + a wake at
// every member, transitively). Bookkeeping lives HERE because attachment
// and detachment are task lifecycle events (spawn / exit); the int64 handle
// registry and its throws live with the C++ surface (madc_task_chan.cpp).
// ---------------------------------------------------------------------------

typedef struct madc_scope {
	struct madc_scope *parent;    // enclosing open scope at begin (NULL = root)
	struct madc_task *opener;     // the task that began it — the only joiner
	struct madc_task *members;    // live attached tasks (doubly linked)
	struct madc_scope *children;  // open scopes begun inside this one
	struct madc_scope *csnext, *csprev; // sibling links in parent's children
	long live;                    // member count
	int cancel_req;
	int joining;                  // opener parked in scope_end
	int err_set;
	void *block_entry;            // exception-runtime cleanup entry while a
				      // `scope { }` keyword block is open
				      // (NULL = publics-opened scope)
	char err[MADC_SCOPE_ERR_BYTES];
} madc_scope;

static void scope_attach(madc_scope *s, madc_task *t)
{
	t->scope = s;
	t->cur = s;	// a member's own spawns attach to the same scope
			// (flat by default; scope_begin nests explicitly)
	t->sprev = NULL;
	t->snext = s->members;
	if (s->members)
		s->members->sprev = t;
	s->members = t;
	++s->live;
	if (s->cancel_req)
		t->cancel_req = 1;     // born cancelled (spawn into a
				       // cancel-requested scope)
}

// Task exit: leave the member list; the LAST member out wakes the joiner.
static void scope_detach(madc_task *t)
{
	madc_scope *s = t->scope;
	if (!s)
		return;
	if (t->sprev)
		t->sprev->snext = t->snext;
	else
		s->members = t->snext;
	if (t->snext)
		t->snext->sprev = t->sprev;
	t->snext = t->sprev = NULL;
	t->scope = NULL;
	--s->live;
	TASK_TRACE("[task] scope- %p task=%p live=%ld\n", (void *)s,
		   (void *)t, s->live);
	if (s->live == 0 && s->joining)
		task_enqueue(s->opener);
}

// The cancel walk: flag + wake every member, recurse into child scopes.
// Waking a timer-parked member unlinks it (its sleep throws on resume); a
// channel/io-parked member is enqueued and its blocking verb removes its
// own waiter record before throwing (the record is stack-resident — the
// verb owns its bookkeeping). The CURRENT task only gets the flag.
static void scope_cancel_walk(madc_scope *s)
{
	s->cancel_req = 1;
	madc_task *t;
	for (t = s->members; t; t = t->snext)
		__madc_task_cancel_request(t);
	// Wake the OPENER too, flaglessly — its cancellation lives in the
	// scope (the chain predicate) and dies at scope_end, never in a
	// sticky per-task flag that would outlive the scope into the
	// opener's parent context. A parked opener must notice NOW: its
	// blocking verb rechecks the chain on resume; a sleeper leaves the
	// timer list.
	if (s->opener && s->opener != g_current) {
		timer_unlink(s->opener);
		task_enqueue(s->opener);
	}
	madc_scope *c;
	for (c = s->children; c; c = c->csnext)
		scope_cancel_walk(c);
}

// Resolve a scope whose end can no longer rethrow — its opener is EXITING
// with the scope still open (an exception unwound past scope_end, or a
// scope_end was never written). The members are cancelled and joined HERE,
// before the opener dies: the alternative is a member's last exit enqueuing
// a dead task. A captured error has nowhere to land, so it prints.
// Join a scope's members (park the caller until the last one detaches),
// then unlink it from its parent's child list — the shared core of
// scope_end and scope_abandon (their difference is the OUTCOME: rethrow
// vs print-and-discard).
static void scope_join_and_unlink(madc_scope *s)
{
	s->joining = 1;
	while (s->live > 0)
		__madc_task_park();
	s->joining = 0;
	if (s->parent) {
		if (s->csprev)
			s->csprev->csnext = s->csnext;
		else
			s->parent->children = s->csnext;
		if (s->csnext)
			s->csnext->csprev = s->csprev;
	}
}

static void scope_abandon(madc_task *t, madc_scope *s)
{
	scope_cancel_walk(s);
	scope_join_and_unlink(s);
	if (s->block_entry) {
		// A keyword-block scope abandoned outside its own unwind
		// handler (the task-death path) must not leave a cleanup
		// entry pointing at freed memory. During the handler itself
		// the entry is already unlinked, so this walk is a no-op.
		__madc_cleanup_remove(s->block_entry);
		s->block_entry = NULL;
	}
	if (s->err_set)
		fprintf(stderr, "madc tasks: abandoned scope error: %s\n",
			s->err);
	t->cur = s->parent;
	free(s);
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

// Fork discipline (rt_task.h contract): the io layer's own post-fork
// reset, NULL until installed beside the wait hook.
void (*__madc_task_io_atfork_hook)(void);

void __madc_task_atfork_child(void)
{
	g_current = NULL;	/* first spawn re-adopts the child's flow */
	g_ready_head = NULL;
	g_ready_tail = NULL;
	g_live = 0;
	g_reap = NULL;
	g_main_waiting = 0;
	g_starting = NULL;
	g_timer_head = NULL;
	g_timer_count = 0;
	if (__madc_task_io_atfork_hook)
		__madc_task_io_atfork_hook();
}

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
// Monotonic switch counter (MT-4c): the host wait's "did other tasks get
// the CPU since I parked" question — read at park, compared at the io
// hook's quiescent point. Never wraps in practice (one increment per
// cooperative switch).
static long long g_switch_count;

long long __madc_task_switch_count(void)
{
	return g_switch_count;
}

static void task_switch(madc_task *from, madc_task *to)
{
	TASK_TRACE("[task] switch %p -> %p (main=%p)\n", (void *)from,
		   (void *)to, (void *)&g_main_task);
	++g_switch_count;
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

// The task body, shared by both platform trampolines (MT-3). A SCOPED task
// runs under a C-side catch-all frame: an uncaught error is captured as the
// scope's FIRST error and cancels the scope (Kotlin: a failed child cancels
// its siblings) — the cancellation literal itself is swallowed, cancellation
// completing is not a failure. Root tasks keep the abort-on-uncaught
// default (Go semantics; the throw never reaches this frame — it has none).
static void task_run_body(madc_task *t)
{
	int caught = 0;
	if (t->scope) {
		unsigned char tryctx[MADC_TASK_TRYCTX_BYTES];
		void *jb = __madc_try_push((struct MadcTryContext *)tryctx);
		if (setjmp(*(jmp_buf *)jb) == 0) {
			t->fn(t->arg);
			__madc_try_pop();
		} else {
			madc_scope *s = t->scope;
			// Cancellation is classified by POINTER identity
			// alone — a task cancelled through a CHILD scope's
			// chain carries no own flag, and that unwind must
			// not read as a failure of the owning scope.
			int cancelled =
			    __madc_exception_type() == MADC_EXCEPT_CSTR
			    && __madc_exception_cstr()
				   == __madc_task_cancelled_text();
			caught = 1;
			TASK_TRACE("[task] scope catch t=%p cancelled=%d\n",
				   (void *)t, cancelled);
			if (!cancelled && !s->err_set) {
				s->err_set = 1;
				__madc_exception_text(s->err, sizeof s->err);
			}
			__madc_exception_clear();
			if (!cancelled)
				scope_cancel_walk(s);
		}
	} else {
		t->fn(t->arg);
	}
	// Scopes the body left open — an exception unwound past their
	// scope_end (expected), or a scope_end is missing (a user bug,
	// warned) — are cancelled, joined, and freed before the task dies.
	while (t->cur != t->scope) {
		if (!caught)
			fprintf(stderr, "madc tasks: task exited with an"
				" open scope (missing scope_end)\n");
		scope_abandon(t, t->cur);
	}
	scope_detach(t);
}

#if defined(_WIN32)
static void CALLBACK task_trampoline(void *param)
{
	madc_task *t = (madc_task *)param;
	task_reap();
	task_run_body(t);
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
	task_run_body(t);
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
	// Structured attachment (MT-3, the Kotlin pick): the spawner's
	// innermost open scope owns the new task; none = the root scope.
	if (g_current && g_current->cur)
		scope_attach(g_current->cur, t);
	TASK_TRACE("[task] go t=%p fn=%p arg=%p live=%ld scope=%p\n",
		   (void *)t, (void *)(size_t)fn, arg, g_live,
		   (void *)t->scope);
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
	madc_task *self = g_current;
	__madc_task_throw_if_cancelled();	// blocking verbs throw
						// before parking and on
						// resume (MT-3)
	if (ms < 0)
		ms = 0;
	self->deadline = time_now_ms() + ms;
	timer_insert(self);
	// Parked on the TIMER list, not the ready queue: task_next_or_wait
	// (inside park) fires due deadlines and advances the time source
	// when nothing else is runnable — park returns immediately when our
	// own deadline was the wake (next == self).
	__madc_task_park();
	if (__madc_task_cancelled()) {
		// Woken by a cancel — or cancelled while due-fired. A direct
		// cancel already unlinked us; a chain-only cancellation
		// arrives with the timer link still live, so unlink again
		// (idempotent) before the unwind.
		timer_unlink(self);
		__madc_task_throw_if_cancelled();
	}
}

// ---------------------------------------------------------------------------
// Cancellation + scope API (MT-3) — contracts in rt_task.h.
// ---------------------------------------------------------------------------

const char *__madc_task_cancelled_text(void)
{
	// THE cancellation literal: every blocking verb throws exactly this
	// pointer, and the trampoline's capture distinguishes a completing
	// cancellation from a user error by pointer identity (user code can
	// spell the same text; it cannot forge this address).
	static const char text[] = "madc: task cancelled";
	return text;
}

void __madc_task_cancel_request(void *task)
{
	madc_task *t = (madc_task *)task;
	if (!t)
		return;
	t->cancel_req = 1;
	TASK_TRACE("[task] cancel %p (current=%d)\n", (void *)t,
		   t == g_current);
	if (t != g_current) {
		// A timer-parked sleeper wakes NOW (and must leave the list —
		// a woken task reachable from the timer list would be
		// re-enqueued, or worse, fired after it exited). A ready task
		// no-ops (idempotent enqueue); a channel/io-parked task is
		// enqueued and its verb removes its own waiter on resume.
		timer_unlink(t);
		task_enqueue(t);
	}
}

void __madc_task_throw_if_cancelled(void)
{
	// THE check-and-throw of the current task's cancellation (dupaudit
	// family current_task_cancel_throw; gate:
	// check-cancel-throw-owner.sh) — every blocking verb's entry and
	// resume gates route here. scope_end's OUTCOME throw is a different
	// rule (rethrowing a scope's state, not checking the current task)
	// and stays a direct throw.
	if (__madc_task_cancelled())
		__madc_throw_cstr(__madc_task_cancelled_text());
}

int __madc_task_cancelled(void)
{
	if (!g_current)
		return 0;
	if (g_current->cancel_req)
		return 1;
	// The OPENER of a cancelled scope polls true too (its own flag is
	// untouched by cancelling the scope's subtree): walk the open-scope
	// chain (Kotlin's isActive).
	madc_scope *s;
	for (s = g_current->cur; s; s = s->parent)
		if (s->cancel_req)
			return 1;
	return 0;
}

void *__madc_scope_begin(void)
{
	task_runtime_init();
	madc_scope *s = (madc_scope *)calloc(1, sizeof *s);
	if (!s) {
		fprintf(stderr, "madc tasks: scope allocation failed\n");
		abort();
	}
	s->opener = g_current;
	s->parent = g_current->cur;
	if (s->parent) {
		s->csnext = s->parent->children;
		if (s->csnext)
			s->csnext->csprev = s;
		s->parent->children = s;
	}
	g_current->cur = s;
	TASK_TRACE("[task] scope+ %p parent=%p opener=%p\n", (void *)s,
		   (void *)s->parent, (void *)s->opener);
	return s;
}

void __madc_scope_cancel(void *scope)
{
	if (!scope)
		return;
	scope_cancel_walk((madc_scope *)scope);
}

int __madc_scope_end_check(void *scope)
{
	madc_scope *s = (madc_scope *)scope;
	if (!s || !g_current || s->opener != g_current)
		return 1;
	if (g_current->cur != s)
		return 2;
	return 0;
}

void __madc_scope_end(void *scope)
{
	madc_scope *s = (madc_scope *)scope;
	if (!s)
		return;
	// Ordering violations refuse BEFORE any mutation (the handle layer
	// pre-validates with __madc_scope_end_check so its registry stays
	// consistent; these throws are the belt for direct C consumers —
	// the MT-5 keyword lowering calls this seam without handles).
	if (!g_current || s->opener != g_current)
		__madc_throw_cstr("scope_end: only the opening task may"
				  " end a scope");
	if (g_current->cur != s)
		__madc_throw_cstr("scope_end: not the innermost open scope");
	// Join FIRST, unconditionally: scope_end never throws until every
	// member (including members of child scopes, who detach themselves)
	// has finished — a cancel wake merely re-parks, so the bookkeeping
	// below always runs and nothing leaks. Cancelled members that never
	// reach a cancellation point keep the join waiting (documented —
	// the Kotlin behavior).
	scope_join_and_unlink(s);
	g_current->cur = s->parent;
	int err_set = s->err_set;
	int cancelled = s->cancel_req;
	if (err_set) {
		// The rethrow text must outlive the freed scope: the
		// opener's per-task buffer carries it to the catch.
		strncpy(g_current->scope_err, s->err,
			sizeof g_current->scope_err - 1);
		g_current->scope_err[sizeof g_current->scope_err - 1] = '\0';
	}
	TASK_TRACE("[task] scope~ %p err=%d cancelled=%d\n", (void *)s,
		   err_set, cancelled);
	free(s);
	if (err_set)
		__madc_throw_cstr(g_current->scope_err);
	if (cancelled)
		__madc_throw_cstr(__madc_task_cancelled_text());
}

// ---------------------------------------------------------------------------
// MT-5 `scope { ... }` keyword block — the direct-C consumer of the seams
// above (the CIR builder emits this pair; no int64 handle, no C++ layer).
// enter = scope_begin + an unwind registration on the exception runtime's
// cleanup stack: a throw ESCAPING the block must not leave the scope on the
// task's chain (a later `go` would attach to a dead block; an outer
// scope_end would refuse "not innermost"), so the handler quietly abandons
// it mid-unwind — cancel members, JOIN (parking here is safe: the in-flight
// exception is per-context state, switched with the task), pop cur, free.
// The in-flight error wins; a captured member error prints (nowhere left to
// land). A throw CAUGHT INSIDE the block never reaches the entry (the try
// mark discipline). exit = remove the registration, then the ordinary
// scope_end above (join + rethrow first member error / cancelled text).
// ---------------------------------------------------------------------------

static void scope_block_unwind(void *scope)
{
	madc_scope *s = (madc_scope *)scope;
	madc_task *t = g_current;
	if (!t)
		return;
	// Unended PUBLIC scopes opened inside the block (no cleanup entry
	// of their own) abandon first — cur walks down to the block's scope.
	while (t->cur && t->cur != s)
		scope_abandon(t, t->cur);
	if (t->cur == s)
		scope_abandon(t, s);
}

void *__madc_scope_block_enter(void)
{
	void *before = __madc_cleanup_top();
	madc_scope *s = (madc_scope *)__madc_scope_begin();
	__madc_cleanup_push_dtor((void *)scope_block_unwind, s);
	s->block_entry = __madc_cleanup_top();
	if (s->block_entry == before) {
		// push_dtor's allocation failed silently — an unregistered
		// block would leak its children on unwind. Refuse loud.
		fprintf(stderr, "madc tasks: scope block registration"
			" failed\n");
		abort();
	}
	return s;
}

void __madc_scope_block_exit(void *scope)
{
	madc_scope *s = (madc_scope *)scope;
	if (!s)
		return;
	if (s->block_entry) {
		__madc_cleanup_remove(s->block_entry);
		s->block_entry = NULL;
	}
	// end_check failures here are engine bugs (enter/exit are emitted
	// as a matched pair in one task) — still checked, loud.
	int rc = __madc_scope_end_check(s);
	if (rc == 1)
		__madc_throw_cstr("scope block: not the opening task");
	if (rc == 2)
		__madc_throw_cstr("scope block: an inner scope is"
				  " still open");
	__madc_scope_end(s);
}
