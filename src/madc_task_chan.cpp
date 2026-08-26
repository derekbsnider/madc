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

#include "libmadc/value.h"
#include "rt/rt_task.h"
#include "rt/rt_except.h"

namespace {

struct ChanWaiter {
	void *task;               // __madc_task_current at park time
	madc::value *slot;        // recv: where the sender delivers
	madc::value val;          // send: the value waiting to be taken
	bool ok = false;          // set by the peer that completed us
	bool woken_closed = false;// set by chan_close
};

struct MadcChan {
	std::deque<madc::value> q;
	long long cap = 0;
	bool closed = false;
	std::deque<ChanWaiter *> recv_waiters;
	std::deque<ChanWaiter *> send_waiters;
};

std::map<int64_t, MadcChan *> g_chans;
int64_t g_next_chan = 1;

MadcChan *chan_of(int64_t h, const char *who)
{
	std::map<int64_t, MadcChan *>::iterator it = g_chans.find(h);
	if (it == g_chans.end()) {
		// Immortal literals per rt_except.h's cstr contract — one
		// message per public, not a formatted (ring-lifetime) string.
		if (who && who[0] == 's')
			__madc_throw_cstr("chan_send: bad channel handle");
		else if (who && who[0] == 'r')
			__madc_throw_cstr("chan_recv: bad channel handle");
		else if (who && who[0] == 'c')
			__madc_throw_cstr("chan_close: bad channel handle");
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
	if (!c->recv_waiters.empty()) {
		ChanWaiter *w = c->recv_waiters.front();
		c->recv_waiters.pop_front();
		*w->slot = v;
		w->ok = true;
		__madc_task_unpark(w->task);
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
	if (!c->q.empty()) {
		out = c->q.front();
		c->q.pop_front();
		if (!c->send_waiters.empty()) {
			// A blocked sender refills the freed buffer slot.
			ChanWaiter *w = c->send_waiters.front();
			c->send_waiters.pop_front();
			c->q.push_back(w->val);
			w->ok = true;
			__madc_task_unpark(w->task);
		}
		return true;
	}
	if (!c->send_waiters.empty()) {
		// Rendezvous (capacity 0): take straight from the sender.
		ChanWaiter *w = c->send_waiters.front();
		c->send_waiters.pop_front();
		out = w->val;
		w->ok = true;
		__madc_task_unpark(w->task);
		return true;
	}
	if (c->closed) {
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
	while (!c->recv_waiters.empty()) {
		ChanWaiter *w = c->recv_waiters.front();
		c->recv_waiters.pop_front();
		w->woken_closed = true;
		__madc_task_unpark(w->task);
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

// The interim structured-join verb (until MT-3 scopes): drain the task
// root scope NOW — for teardown code that closes resources a still-running
// task writes to. Idempotent; main's-end join (MT-2b) remains the belt.
void task_drain()
{
	__madc_task_join_all();
}

int64_t task_live()
{
	return (int64_t)__madc_task_live();
}

} // namespace madc
