/* madc_task_chan.cpp — the cooperative waitables: value channels (MT-2),
 * select + byte-endpoint readiness (MT-4/4b), and the time/drain verbs.
 *
 * ONE file on purpose: the select discipline (SelectGroup / first-fire
 * claim / husk skip / wake-once / eager removal) spans value-channel
 * waiters AND io waiters — splitting them would put one discipline in two
 * files. The scheduler (src/rt/rt_task.c) stays both channel- and fd-blind:
 * it sees park/unpark and the __madc_task_io_wait_hook this file installs.
 *
 * The Go-shaped synchronization primitive over the MT-1 substrate: a queue
 * of madc::value with blocking send/recv that PARK the running task
 * (src/rt/rt_task.h park/unpark — the scheduler owns switching, THIS file
 * owns the wait queues and why a task parked). Semantics follow Go's
 * channel contract, chosen as the default and flagged in the design doc
 * (docs/plans/2026-08-26-madc-multitasking-design.md, owner fork 3):
 *
 *   - capacity 0 = rendezvous (send parks until a receiver takes the value
 *     and vice versa); capacity N buffers N values.
 *   - recv on a closed channel drains the buffer first, then returns false
 *     with a null value (Go's ok=false / zero value).
 *   - send on a closed channel — and close of a closed channel — throw a
 *     madc exception (catchable `const char *`; Go panics here).
 *   - values are COPIED through the channel (send copies in, recv copies
 *     out) — share by communicating.
 *
 * WHY C++ AND NOT src/rt/: the payload is madc::value (vector/map-backed),
 * so the channel store inherently needs the C++ script runtime — exactly
 * rt_dump_value.cpp's reasoning. The park/unpark primitives stay C in
 * rt_task.c.
 *
 * Waiter records live ON THE PARKED TASK'S STACK (Go's sudog shape): a
 * parked task's stack is stable until unparked, and the runtime is single
 * OS thread by contract — no locks, no heap per wait.
 *
 * THREAD-SAFETY CONTRACT (thread-safety.md): single OS thread,
 * cooperative; every function here runs to its park/return without
 * interleaving. The M:N upgrade revisits this file with the hub/verb
 * machinery (design doc, demand 15).
 *
 * Handles are int64 ids in a process registry (madc::project_open
 * precedent). Channels are never freed in MT-2 (a handful of pointers
 * each); a drop verb arrives with the ownership design in a later slice.
 */

#include <deque>
#include <limits.h>
#include <map>
#include <stdint.h>
#include <string.h>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#include <io.h>
#else
#include <poll.h>
#endif

#include "libmadc/value.h"
#include "madcdis/channel.h"
#include "madc_task_io.h"
#include "rt/rt_task.h"
#include "rt/rt_except.h"

namespace {

struct SelectGroup;

struct ChanWaiter {
	void *task;               // __madc_task_current at park time
	madc::value *slot;        // recv: where the sender delivers
	madc::value val;          // send: the value waiting to be taken
	bool ok = false;          // set by the peer that completed us
	bool woken_closed = false;// set by chan_close
	// Select membership (MT-4): chan_select registers ONE waiter per
	// case, all pointing at the group record on the selecting task's
	// stack; the FIRST delivery claims the group. Plain recv/send leave
	// group NULL.
	SelectGroup *group = 0;
	int64_t index = 0;        // this waiter's case index in its group
};

// One select call's shared state — on the selecting task's stack like the
// waiters themselves. fired_index < 0 = still armed. `woken` guards the
// UNPARK, not the delivery: the first wake (a delivery or a close) enqueues
// the selecting task exactly once; a later close on another case must not
// enqueue it a second time (a task queued twice runs twice — catastrophic
// on one stack), and a later DELIVERY may still fill the slot and claim
// fired_index without re-unparking (the awake selector reads it on resume).
struct SelectGroup {
	int64_t fired_index = -1;
	bool woken = false;
};

// Wake a recv waiter exactly once (see SelectGroup::woken) WITHOUT firing —
// close's verb: the woken selector rescans instead of returning this case.
// Plain waiters (group NULL) park once and are popped once — always unpark.
void wake_recv_waiter(ChanWaiter *w)
{
	if (w->group) {
		if (w->group->woken)
			return;
		w->group->woken = true;
	}
	__madc_task_unpark(w->task);
}

// FIRE one case: claim the group and wake its task exactly once — THE one
// claim+wake owner for BOTH waiter kinds (value-channel delivery and io
// readiness). The caller has already husk-filtered (group->fired_index < 0
// on entry); a group woken earlier by a close still gets the claim but not
// a second enqueue. Plain waiters (group NULL) just unpark. Returns whether
// a task was enqueued.
bool select_fire(SelectGroup *group, int64_t index, void *task)
{
	if (group) {
		group->fired_index = index;
		if (group->woken)
			return false;
		group->woken = true;
	}
	__madc_task_unpark(task);
	return true;
}

// ---------------------------------------------------------------------------
// taskio (MT-4b) — the io-wait seat's registry + hook. Waiter records live
// on the parked task's stack exactly like ChanWaiter, share the SelectGroup
// discipline (first fire claims fired_index; the group's wake-once guard
// spans BOTH waiter kinds), and are eagerly unregistered by their owner on
// resume. Handles are CRT fds on every platform (ProcessPipeChannel).
// ---------------------------------------------------------------------------

struct IoWaiter {
	intptr_t handle = -1;     // -1 = never registered (select's dead cases)
	void *task = 0;
	SelectGroup *group = 0;   // NULL = plain wait_readable
	int64_t index = 0;
	bool fired = false;
	IoWaiter *next = 0;
};

IoWaiter *g_io_head;

// The HOST waiter (MT-4c, one at a time — the tui's stdin): beside its fd,
// it wakes SYNTHETICALLY (unfired) when other tasks got the CPU since it
// parked — the scheduler-side spelling of read_keys' old ran->wake seam.
// g_host_mark = the switch count at its park; the io hook compares at the
// quiescent point and consumes the wake by advancing the mark.
IoWaiter *g_host;
long long g_host_mark;

// Fire the host UNFIRED (synthetic wake: activity or EINTR — not the fd).
// task_enqueue is idempotent, so a duplicate is harmless.
static int io_wake_host_synthetic()
{
	g_host_mark = __madc_task_switch_count();	// consume
	__madc_task_unpark(g_host->task);
	return 1;
}

// Zero-timeout "read would make progress" probe: data, EOF, or a surfaced
// error all count (POLLHUP/POLLERR — the read reports them; the waiter must
// wake, not hang).
bool io_probe_readable(intptr_t handle)
{
#if defined(_WIN32)
	// CRT fd -> pipe HANDLE; PeekNamedPipe failing means the pipe ended
	// (EOF/broken) — that IS readable progress (the read surfaces it).
	HANDLE h = (HANDLE)_get_osfhandle((int)handle);
	if (h == INVALID_HANDLE_VALUE)
		return true;
	DWORD avail = 0;
	if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
		return true;
	return avail > 0;
#else
	struct pollfd p;
	p.fd = (int)handle;
	p.events = POLLIN;
	p.revents = 0;
	if (poll(&p, 1, 0) <= 0)
		return false;
	return (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
#endif
}

// Fire one io waiter: husk-filter, then the shared claim+wake discipline
// (select_fire — one owner for both waiter kinds).
bool io_fire_waiter(IoWaiter *w)
{
	if (w->fired)
		return false;
	if (w->group && w->group->fired_index >= 0)
		return false;	// husk: its select already fired
	w->fired = true;
	return select_fire(w->group, w->index, w->task);
}

// The scheduler's io wait (rt_task.h contract): -1 = no waiters (did not
// wait); 0 = waited up to timeout_ms, nothing fired; >0 = tasks enqueued.
int io_wait_hook(long long timeout_ms)
{
	if (!g_io_head)
		return -1;
	// A host with pending activity beats BLOCKING — but real fd
	// readiness beats the synthetic wake, so the order is: zero-timeout
	// probe pass, then the host check, then the blocking wait (MT-4c).
	// PROBE calls (timeout 0 — __madc_task_fire_due / a yield's head)
	// never fire it: the synthetic wake belongs to the BLOCKING
	// quiescent point only, or it steals the CPU from the still-running
	// tasks whose drain it is supposed to announce.
	bool host_pending = timeout_ms != 0 && g_host && !g_host->fired
		&& __madc_task_switch_count() != g_host_mark;
	int woke = 0;
#if defined(_WIN32)
	// Cheap-blocking arm: anonymous pipes are not waitable objects, so
	// probe PeekNamedPipe on a 1ms cadence bounded by real elapsed time.
	long long start = (long long)GetTickCount64();
	for (;;) {
		for (IoWaiter *w = g_io_head; w; w = w->next)
			if (!w->fired && io_probe_readable(w->handle))
				woke += io_fire_waiter(w) ? 1 : 0;
		if (woke || timeout_ms == 0)
			return woke;
		if (host_pending)
			return io_wake_host_synthetic();
		if (timeout_ms > 0
		    && (long long)GetTickCount64() - start >= timeout_ms)
			return 0;
		Sleep(1);
	}
#else
	int n = 0;
	for (IoWaiter *w = g_io_head; w; w = w->next)
		n++;
	std::vector<struct pollfd> fds((size_t)n);
	std::vector<IoWaiter *> ws((size_t)n);
	int i = 0;
	for (IoWaiter *w = g_io_head; w; w = w->next, i++) {
		fds[(size_t)i].fd = (int)w->handle;
		fds[(size_t)i].events = POLLIN;
		fds[(size_t)i].revents = 0;
		ws[(size_t)i] = w;
	}
	int tmo = timeout_ms < 0 ? -1
		: timeout_ms > (long long)INT_MAX ? INT_MAX : (int)timeout_ms;
	if (host_pending)
		tmo = 0;	// probe pass only — the host wake follows
	int r = poll(fds.data(), (nfds_t)n, tmo);
	if (r > 0) {
		for (i = 0; i < n; i++) {
			if (!(fds[(size_t)i].revents
			      & (POLLIN | POLLHUP | POLLERR)))
				continue;
			woke += io_fire_waiter(ws[(size_t)i]) ? 1 : 0;
		}
		if (woke)
			return woke;
	}
	if (host_pending)
		return io_wake_host_synthetic();
	if (r < 0 && timeout_ms != 0 && g_host && !g_host->fired)
		return io_wake_host_synthetic();	// EINTR on a WAIT:
							// SIGWINCH must
							// reach the host
	return 0;	// timeout, or EINTR with no host (spurious)
#endif
}

// Fork discipline (rt_task.h contract): a fork() child's waiter records
// point at parent task stacks it must never wake — drop the whole list.
// Registered beside the wait hook: no registration ever, nothing to reset.
static void io_atfork_child()
{
	g_io_head = 0;
	g_host = 0;
	g_host_mark = 0;
}

void io_register(IoWaiter *w)
{
	__madc_task_io_wait_hook = io_wait_hook;	// installed once, stays
	__madc_task_io_atfork_hook = io_atfork_child;
	w->next = g_io_head;
	g_io_head = w;
}

void io_unregister(IoWaiter *w)
{
	for (IoWaiter **pp = &g_io_head; *pp; pp = &(*pp)->next) {
		if (*pp == w) {
			*pp = w->next;
			w->next = 0;
			return;
		}
	}
}

struct MadcChan {
	std::deque<madc::value> q;
	long long cap = 0;
	bool closed = false;
	std::deque<ChanWaiter *> recv_waiters;
	std::deque<ChanWaiter *> send_waiters;
};

// One handle space, two case kinds (MT-4b): a value channel, or a byte
// endpoint (madc::channel) registered by chan_readable whose READ readiness
// selects beside the value cases. Exactly one pointer is set per entry.
struct ChanEntry {
	MadcChan *chan = 0;
	madc::channel *bytes = 0;
};

std::map<int64_t, ChanEntry> g_chans;
int64_t g_next_chan = 1;

// Remove a waiter record from its queue if still present (MT-3): a
// cancel-woken task was never popped by a deliverer, and its record — on
// the frame the cancel throw is about to unwind — must not outlive it.
// Absent is fine: a delivery or a close popped it first.
void remove_waiter(std::deque<ChanWaiter *> &q, ChanWaiter *w)
{
	for (std::deque<ChanWaiter *>::iterator it = q.begin();
	     it != q.end(); ++it)
		if (*it == w) {
			q.erase(it);
			return;
		}
}

// Pop the next DELIVERABLE recv waiter. A select waiter whose group already
// fired is a HUSK — its task is awake and will remove the record when it
// resumes (records live on the parked stack, so they stay valid through
// this window); delivering into one would overwrite the fired case's value
// and double-enqueue the task. Plain waiters (group NULL) always qualify.
ChanWaiter *pop_recv_waiter(MadcChan *c)
{
	while (!c->recv_waiters.empty()) {
		ChanWaiter *w = c->recv_waiters.front();
		if (w->group && w->group->fired_index >= 0) {
			c->recv_waiters.pop_front();	// husk: discard
			continue;
		}
		c->recv_waiters.pop_front();
		return w;
	}
	return 0;
}

// The ONE "receive right now if possible" implementation — chan_recv's
// fast arms, chan_try_recv, and chan_select's ready scan all route here.
// Returns 1 = *out filled; 0 = would block; -1 = closed AND drained.
int chan_poll_recv(MadcChan *c, madc::value *out)
{
	if (!c->q.empty()) {
		*out = c->q.front();
		c->q.pop_front();
		if (!c->send_waiters.empty()) {
			// A blocked sender refills the freed buffer slot.
			ChanWaiter *w = c->send_waiters.front();
			c->send_waiters.pop_front();
			c->q.push_back(w->val);
			w->ok = true;
			__madc_task_unpark(w->task);
		}
		return 1;
	}
	if (!c->send_waiters.empty()) {
		// Rendezvous (capacity 0): take straight from the sender.
		ChanWaiter *w = c->send_waiters.front();
		c->send_waiters.pop_front();
		*out = w->val;
		w->ok = true;
		__madc_task_unpark(w->task);
		return 1;
	}
	return c->closed ? -1 : 0;
}

// Immortal literals per rt_except.h's cstr contract — one message per
// public, not a formatted (ring-lifetime) string. A byte-endpoint handle
// passed to a value-channel verb throws the same message: it IS a bad
// value-channel handle there.
void throw_bad_handle(const char *who)
{
	if (who && strcmp(who, "send") == 0)
		__madc_throw_cstr("chan_send: bad channel handle");
	else if (who && strcmp(who, "recv") == 0)
		__madc_throw_cstr("chan_recv: bad channel handle");
	else if (who && strcmp(who, "close") == 0)
		__madc_throw_cstr("chan_close: bad channel handle");
	else if (who && strcmp(who, "select") == 0)
		__madc_throw_cstr("chan_select: bad channel handle");
	else if (who && strcmp(who, "try_recv") == 0)
		__madc_throw_cstr("chan_try_recv: bad channel handle");
	else
		__madc_throw_cstr("chan_len: bad channel handle");
}

ChanEntry &entry_of(int64_t h, const char *who)
{
	std::map<int64_t, ChanEntry>::iterator it = g_chans.find(h);
	if (it == g_chans.end())
		throw_bad_handle(who);
	return it->second;
}

MadcChan *chan_of(int64_t h, const char *who)
{
	ChanEntry &e = entry_of(h, who);
	if (!e.chan)
		throw_bad_handle(who);
	return e.chan;
}

} // namespace

namespace madc {
namespace taskio {

bool poll_readable(intptr_t handle)
{
	return io_probe_readable(handle);
}

void wait_readable(intptr_t handle)
{
	__madc_task_throw_if_cancelled();
	if (io_probe_readable(handle))
		return;
	IoWaiter me;
	me.handle = handle;
	me.task = __madc_task_current();
	io_register(&me);
	__madc_task_park();
	// Eager removal — no registered pointer outlives this frame.
	io_unregister(&me);
	if (!me.fired)
		__madc_task_throw_if_cancelled();	// cancel-woken, not readable
}

bool host_wait_readable(intptr_t handle)
{
	__madc_task_throw_if_cancelled();
	if (io_probe_readable(handle))
		return true;
	if (g_host)
		__madc_throw_cstr("host_wait_readable: a host wait is"
				  " already registered");
	IoWaiter me;
	me.handle = handle;
	me.task = __madc_task_current();
	io_register(&me);
	g_host = &me;
	g_host_mark = __madc_task_switch_count();
	__madc_task_park();
	g_host = 0;
	io_unregister(&me);
	if (!me.fired)
		__madc_task_throw_if_cancelled();	// cancel vs synthetic
	return me.fired;
}

} // namespace taskio

int64_t chan_make(int64_t capacity)
{
	if (capacity < 0)
		__madc_throw_cstr("chan_make: negative capacity");
	MadcChan *c = new MadcChan();
	c->cap = capacity;
	ChanEntry e;
	e.chan = c;
	int64_t h = g_next_chan++;
	g_chans[h] = e;
	return h;
}

bool chan_send(int64_t h, value &v)
{
	__madc_task_throw_if_cancelled();
	MadcChan *c = chan_of(h, "send");
	if (c->closed)
		__madc_throw_cstr("send on closed channel");
	if (ChanWaiter *w = pop_recv_waiter(c)) {
		*w->slot = v;
		w->ok = true;
		select_fire(w->group, w->index, w->task);	// select won
		return true;
	}
	if ((long long)c->q.size() < c->cap) {
		c->q.push_back(v);
		return true;
	}
	ChanWaiter me;
	me.task = __madc_task_current();
	me.slot = 0;
	me.val = v;
	c->send_waiters.push_back(&me);
	__madc_task_park();
	if (me.ok)
		return true;	// the value was taken — completion wins; a
				// pending cancel throws at the NEXT verb
	if (__madc_task_cancelled()) {
		remove_waiter(c->send_waiters, &me);
		__madc_task_throw_if_cancelled();
	}
	if (me.woken_closed)
		__madc_throw_cstr("send on closed channel");
	return true;
}

bool chan_recv(value &out, int64_t h)
{
	__madc_task_throw_if_cancelled();
	MadcChan *c = chan_of(h, "recv");
	int r = chan_poll_recv(c, &out);
	if (r == 1)
		return true;
	if (r == -1) {
		out = value();
		return false;
	}
	ChanWaiter me;
	me.task = __madc_task_current();
	me.slot = &out;
	c->recv_waiters.push_back(&me);
	__madc_task_park();
	if (me.ok)
		return true;	// delivered — completion wins
	if (__madc_task_cancelled()) {
		remove_waiter(c->recv_waiters, &me);
		__madc_task_throw_if_cancelled();
	}
	if (me.woken_closed) {
		out = value();
		return false;
	}
	return true;
}

void chan_close(int64_t h)
{
	MadcChan *c = chan_of(h, "close");
	if (c->closed)
		__madc_throw_cstr("close of closed channel");
	c->closed = true;
	while (ChanWaiter *w = pop_recv_waiter(c)) {
		// A select waiter woken by close is NOT fired (the selector
		// rescans: drained buffers first, -1 when every case died);
		// wake_recv_waiter guards the double-unpark when two cases
		// of one group close.
		w->woken_closed = true;
		wake_recv_waiter(w);
	}
	while (!c->send_waiters.empty()) {
		ChanWaiter *w = c->send_waiters.front();
		c->send_waiters.pop_front();
		w->woken_closed = true;
		__madc_task_unpark(w->task);
	}
}

int64_t chan_len(int64_t h)
{
	return (int64_t)chan_of(h, "len")->q.size();
}

// Nonblocking receive (MT-4; the default-arm equivalent until MT-5's
// select syntax): 1 = *out filled, 0 = would block (out untouched),
// -1 = closed and drained (out null).
int64_t chan_try_recv(value &out, int64_t h)
{
	MadcChan *c = chan_of(h, "try_recv");
	int r = chan_poll_recv(c, &out);
	if (r == -1)
		out = value();
	return (int64_t)r;
}

// Fan-in select over value channels (MT-4; the recv side — send cases and
// the keyword spelling arrive with MT-5). `chans` is an array of channel
// handles; parks until SOME case can receive; returns the fired case's
// index with the value in `out`. DETERMINISTIC by contract: the
// LOWEST-INDEX ready case wins (Go randomizes exactly here; the recon
// amendment makes determinism the test asset, so a starvation-shaped
// program pins its own order instead of dodging it — documented, not
// accidental). A closed-and-drained case is DISABLED (chan_recv's `false`
// has no seat in an index return); when EVERY case is dead the select
// returns -1 with a null value — the natural fan-in terminator (each
// producer closes its channel when done). Waiter records live on this
// task's stack (the sudog shape); on every wake they are removed from
// every case channel BEFORE anything else, so no registered pointer ever
// outlives the call (husks another channel discarded lazily erase as
// no-ops).
int64_t chan_select(value &out, value &chans)
{
	if (!chans.is_array())
		__madc_throw_cstr("chan_select: cases must be an array of"
				  " channel handles");
	const std::vector<value> &hs = chans.as_array();
	size_t n = hs.size();
	if (n == 0)
		__madc_throw_cstr("chan_select: empty case list");
	std::vector<ChanEntry> es(n);
	for (size_t i = 0; i < n; i++)
		es[i] = entry_of(hs[i].as_integer(), "select");
	for (;;) {
		// Ready scan, lowest index first — buffered values on a
		// closed channel still drain here (only closed-AND-drained
		// disables a case). A byte case fires with out = null when
		// its read would make progress NOW (data, or an EOF/error
		// the read surfaces once); DEAD (drained EOF or failed)
		// disables it like a closed-and-drained value channel.
		bool any_open = false;
		for (size_t i = 0; i < n; i++) {
			if (es[i].chan) {
				int r = chan_poll_recv(es[i].chan, &out);
				if (r == 1)
					return (int64_t)i;
				if (r == 0)
					any_open = true;
			} else {
				int64_t r = es[i].bytes->poll_state();
				if (r == 1) {
					out = value();
					return (int64_t)i;
				}
				if (r == 0)
					any_open = true;
			}
		}
		if (!any_open) {
			out = value();
			return -1;
		}
		// Park on every still-open case; the first delivery (a value
		// arriving, or an fd turning readable) claims the group.
		SelectGroup grp;
		std::vector<ChanWaiter> ws(n);
		std::vector<IoWaiter> ios(n);
		for (size_t i = 0; i < n; i++) {
			if (es[i].chan) {
				if (es[i].chan->closed)
					continue;   // dead: never registered
				ws[i].task = __madc_task_current();
				ws[i].slot = &out;
				ws[i].group = &grp;
				ws[i].index = (int64_t)i;
				es[i].chan->recv_waiters.push_back(&ws[i]);
			} else {
				// A DEAD endpoint's fd is still readable
				// (drained EOF = POLLHUP) — registering it
				// would fire a dead case. poll_state() is
				// the dead test, NOT the raw handle. A case
				// that turned ready since the scan registers
				// too: the hook fires it immediately.
				if (es[i].bytes->poll_state() < 0)
					continue;
				intptr_t h = (intptr_t)
					es[i].bytes->read_wait_handle();
				if (h < 0)
					continue;   // closed under us: dead
				ios[i].handle = h;
				ios[i].task = __madc_task_current();
				ios[i].group = &grp;
				ios[i].index = (int64_t)i;
				io_register(&ios[i]);
			}
		}
		__madc_task_park();
		// Eager removal FIRST: no case channel and no io registry may
		// keep a pointer into this (about-to-unwind) frame past here.
		for (size_t i = 0; i < n; i++) {
			if (es[i].chan) {
				std::deque<ChanWaiter *> &rq =
					es[i].chan->recv_waiters;
				for (std::deque<ChanWaiter *>::iterator it =
					     rq.begin(); it != rq.end(); )
					it = (*it == &ws[i])
						? rq.erase(it) : it + 1;
			} else if (ios[i].handle >= 0) {
				io_unregister(&ios[i]);
			}
		}
		if (grp.fired_index >= 0) {
			// A fired byte case reports readiness, never a value
			// (the caller holds the channel object and reads it).
			if (es[(size_t)grp.fired_index].bytes)
				out = value();
			return grp.fired_index;
		}
		// Cancel-woken (nothing fired): every record is already off
		// the queues/registry (the eager removal above).
		__madc_task_throw_if_cancelled();
		// Woken by a close: rescan (a buffer may have filled
		// meanwhile; all-dead returns -1 above).
	}
}

// Register a byte endpoint (madc::channel) as a select case (MT-4b): the
// returned handle FIRES in chan_select when the endpoint's read would make
// progress — the caller then reads from the channel object it holds (a
// fired byte case carries out = null). Dead endpoints (EOF drained, or
// failed) disable exactly like closed-and-drained value channels. Register
// once and reuse the handle; the channel object must outlive it (entries
// are never freed — the MT-2 ownership note applies). Throws when the
// channel has no waitable read side (memory/file channels never block, so
// they have nothing to select on).
int64_t chan_readable(channel &c)
{
	if (c.read_wait_handle() < 0)
		__madc_throw_cstr("chan_readable: channel is not waitable"
				  " (no readable poll handle)");
	ChanEntry e;
	e.bytes = &c;
	int64_t h = g_next_chan++;
	g_chans[h] = e;
	return h;
}

// The interim structured-join verb (until MT-3 scopes): drain the task
// root scope NOW — for teardown code that closes resources a still-running
// task writes to. Idempotent; main's-end join (MT-2b) remains the belt.
void task_drain()
{
	__madc_task_join_all();
}

// Sleep on the scheduler's time source (MT-4): parks this task for `ms`
// milliseconds. Under MADC_TASK_VTIME=1 the clock is VIRTUAL — it jumps to
// the earliest deadline whenever only sleepers remain, so timed tests run
// instantly and deterministically.
void sleep_ms(int64_t ms)
{
	__madc_task_sleep_ms((long long)ms);
}

int64_t task_live()
{
	return (int64_t)__madc_task_live();
}

// ---------------------------------------------------------------------------
// Structured scopes + cancellation (MT-3) — the handle layer over the
// rt_task.c seams (the chan registry idiom: int64 handles, immortal-literal
// throws). The <ns_madc> declarations carry the full contract.
// ---------------------------------------------------------------------------

namespace {
std::map<int64_t, void *> g_scopes;
int64_t g_next_scope = 1;
}

int64_t scope_begin()
{
	void *s = __madc_scope_begin();
	int64_t h = g_next_scope++;
	g_scopes[h] = s;
	return h;
}

void scope_end(int64_t handle)
{
	std::map<int64_t, void *>::iterator it = g_scopes.find(handle);
	if (it == g_scopes.end())
		__madc_throw_cstr("scope_end: bad scope handle");
	void *s = it->second;
	// Validate-then-consume: an ordering refusal must leave the handle
	// usable (the rt throw would longjmp past this frame AFTER the
	// registry forgot a still-open scope).
	int rc = __madc_scope_end_check(s);
	if (rc == 1)
		__madc_throw_cstr("scope_end: only the opening task may"
				  " end a scope");
	if (rc == 2)
		__madc_throw_cstr("scope_end: not the innermost open scope");
	g_scopes.erase(it);
	__madc_scope_end(s);	// joins; may throw a member error / the
				// cancelled literal AFTER consuming the scope
}

void scope_cancel(int64_t handle)
{
	std::map<int64_t, void *>::iterator it = g_scopes.find(handle);
	if (it == g_scopes.end())
		__madc_throw_cstr("scope_cancel: bad scope handle");
	__madc_scope_cancel(it->second);
}

bool cancelled()
{
	return __madc_task_cancelled() != 0;
}

} // namespace madc

// MT-5 `await <chan>` — the CIR-emitted machinery symbol (extern-C by the
// cpp-first-api exception: compiler-emitted, never user-resolved). ONE thin
// seat over THE recv implementation, so the keyword and the public cannot
// drift (Go semantics ride along: blocks, closed-and-drained fills the ZERO
// value). out == NULL is the bare-statement form — receive and discard, the
// done-channel wait.
extern "C" void __madc_chan_await(madc::value *out, int64_t h)
{
	if (out) {
		madc::chan_recv(*out, h);
		return;
	}
	madc::value discard;
	madc::chan_recv(discard, h);
}
