/* madc_task_chan.cpp — value channels between cooperative tasks (MT-2).
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
#include <map>
#include <stdint.h>
#include <string.h>
#include <vector>

#include "libmadc/value.h"
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

// Wake a recv waiter exactly once (see SelectGroup::woken). Plain waiters
// (group NULL) park once and are popped once — always unpark.
void wake_recv_waiter(ChanWaiter *w)
{
	if (w->group) {
		if (w->group->woken)
			return;
		w->group->woken = true;
	}
	__madc_task_unpark(w->task);
}

struct MadcChan {
	std::deque<madc::value> q;
	long long cap = 0;
	bool closed = false;
	std::deque<ChanWaiter *> recv_waiters;
	std::deque<ChanWaiter *> send_waiters;
};

std::map<int64_t, MadcChan *> g_chans;
int64_t g_next_chan = 1;

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

MadcChan *chan_of(int64_t h, const char *who)
{
	std::map<int64_t, MadcChan *>::iterator it = g_chans.find(h);
	if (it == g_chans.end()) {
		// Immortal literals per rt_except.h's cstr contract — one
		// message per public, not a formatted (ring-lifetime) string.
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
	return it->second;
}

} // namespace

namespace madc {

int64_t chan_make(int64_t capacity)
{
	if (capacity < 0)
		__madc_throw_cstr("chan_make: negative capacity");
	MadcChan *c = new MadcChan();
	c->cap = capacity;
	int64_t h = g_next_chan++;
	g_chans[h] = c;
	return h;
}

bool chan_send(int64_t h, value &v)
{
	MadcChan *c = chan_of(h, "send");
	if (c->closed)
		__madc_throw_cstr("send on closed channel");
	if (ChanWaiter *w = pop_recv_waiter(c)) {
		*w->slot = v;
		w->ok = true;
		if (w->group)
			w->group->fired_index = w->index;	// select won
		wake_recv_waiter(w);
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
	if (me.woken_closed)
		__madc_throw_cstr("send on closed channel");
	return true;
}

bool chan_recv(value &out, int64_t h)
{
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
	std::vector<MadcChan *> cs(n);
	for (size_t i = 0; i < n; i++)
		cs[i] = chan_of(hs[i].as_integer(), "select");
	for (;;) {
		// Ready scan, lowest index first — buffered values on a
		// closed channel still drain here (only closed-AND-drained
		// disables a case).
		bool any_open = false;
		for (size_t i = 0; i < n; i++) {
			int r = chan_poll_recv(cs[i], &out);
			if (r == 1)
				return (int64_t)i;
			if (r == 0)
				any_open = true;
		}
		if (!any_open) {
			out = value();
			return -1;
		}
		// Park on every still-open case; the first delivery claims
		// the group and fills `out` directly.
		SelectGroup grp;
		std::vector<ChanWaiter> ws(n);
		for (size_t i = 0; i < n; i++) {
			if (cs[i]->closed)
				continue;	// dead case: never registered
			ws[i].task = __madc_task_current();
			ws[i].slot = &out;
			ws[i].group = &grp;
			ws[i].index = (int64_t)i;
			cs[i]->recv_waiters.push_back(&ws[i]);
		}
		__madc_task_park();
		// Eager removal FIRST: no case channel may keep a pointer
		// into this (about-to-unwind) frame past this line.
		for (size_t i = 0; i < n; i++) {
			std::deque<ChanWaiter *> &rq = cs[i]->recv_waiters;
			for (std::deque<ChanWaiter *>::iterator it = rq.begin();
			     it != rq.end(); )
				it = (*it == &ws[i]) ? rq.erase(it) : it + 1;
		}
		if (grp.fired_index >= 0)
			return grp.fired_index;
		// Woken by a close: rescan (a buffer may have filled
		// meanwhile; all-dead returns -1 above).
	}
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

} // namespace madc
