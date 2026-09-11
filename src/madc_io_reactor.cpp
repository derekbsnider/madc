// SPDX-License-Identifier: MPL-2.0
// madc_io_reactor — the completion-oriented async I/O engine's Linux/epoll
// backend (design: docs/plans/2026-09-11-async-io-reactor.md). epoll is a
// READINESS interface, so this file is the readiness->syscall->completion
// adapter: the I/O thread waits for a fd to be ready, then performs the
// accept/recv/send ITSELF and posts a completion carrying the result. That
// is the same shape kqueue and select will share; io_uring and IOCP will be
// native-completion backends behind the identical Reactor contract.
//
// Slice 1 (this file): the abstraction + the epoll backend + the dedicated
// I/O thread + the producer/consumer queues + the doorbell. The taskio
// migration, io_uring, kqueue and IOCP are the follow-on slices.

#include "madc_io_reactor.h"

#include <stdexcept>
#include <string>

#if defined(__linux__)

#include <atomic>
#include <cerrno>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace madc {
namespace io {
namespace {

// One queued operation, owned first by the submission queue, then by the
// I/O thread's per-fd op list.
struct pending
{
	uint64_t id;
	op_kind kind;
	int fd;
	void *buf;	// read: destination; write: source (const, held as void*)
	std::size_t len;
	int events;	// poll op: the requested poll_flag mask (else unused)
	uint64_t target;	// cancel op: the id of the op to remove (else unused)
	void *user;
};

// accept + read wait for readability; write for writability; a poll op waits
// on the direction(s) its events mask names.
uint32_t epoll_events_for(const pending &p)
{
	if ( p.kind == op_kind::write )
		return (uint32_t)EPOLLOUT;
	if ( p.kind == op_kind::poll )
	{
		uint32_t mask = 0;
		if ( p.events & readable )
			mask |= (uint32_t)EPOLLIN;
		if ( p.events & writable )
			mask |= (uint32_t)EPOLLOUT;
		return mask ? mask : (uint32_t)EPOLLIN;
	}
	return (uint32_t)EPOLLIN;	// accept, read
}

} // namespace

struct Reactor::impl
{
	int epfd = -1;
	int submit_evt = -1;		// producer -> I/O thread wake
	int doorbell_evt = -1;		// I/O thread -> consumer wake
	std::atomic<bool> stop;
	std::atomic<uint64_t> next_id;

	std::mutex sub_mtx;
	std::deque<pending> submissions;	// producer -> I/O thread

	std::mutex comp_mtx;
	std::deque<completion> completions;	// I/O thread -> consumer

	// I/O-thread-owned: fd -> its queued ops (FIFO). Never touched off-thread.
	std::unordered_map<int, std::deque<pending> > fd_ops;

	std::thread th;

	impl() : stop(false), next_id(1) {}

	void wake_iothread()
	{
		uint64_t one = 1;
		ssize_t r = ::write(submit_evt, &one, sizeof(one));
		(void)r;
	}

	void ring_doorbell()
	{
		uint64_t one = 1;
		ssize_t r = ::write(doorbell_evt, &one, sizeof(one));
		(void)r;
	}

	void clear_doorbell()
	{
		uint64_t v = 0;
		ssize_t r = ::read(doorbell_evt, &v, sizeof(v));
		(void)r;	// EFD_NONBLOCK: EAGAIN when already zero
	}

	uint64_t enqueue(const pending &p)
	{
		{
			std::lock_guard<std::mutex> g(sub_mtx);
			submissions.push_back(p);
		}
		wake_iothread();
		return p.id;
	}

	void post(const completion &c)
	{
		{
			std::lock_guard<std::mutex> g(comp_mtx);
			completions.push_back(c);
		}
		ring_doorbell();
	}

	// --- I/O-thread side (everything below runs ONLY on the I/O thread) ---

	uint32_t interest_mask(int fd)
	{
		std::unordered_map<int, std::deque<pending> >::iterator it =
			fd_ops.find(fd);
		if ( it == fd_ops.end() )
			return 0;
		uint32_t mask = 0;
		for ( std::deque<pending>::iterator p = it->second.begin();
		      p != it->second.end(); ++p )
			mask |= epoll_events_for(*p);
		return mask;
	}

	void arm_fd(int fd, bool already_registered)
	{
		epoll_event ev;
		std::memset(&ev, 0, sizeof(ev));
		ev.events = interest_mask(fd);
		ev.data.fd = fd;
		int action = already_registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
		if ( ::epoll_ctl(epfd, action, fd, &ev) != 0
		  && action == EPOLL_CTL_ADD && errno == EEXIST )
			::epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
	}

	void unregister_fd(int fd)
	{
		if ( fd_ops.erase(fd) )
			::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
	}

	// Remove a still-pending op by id (a cancel): drop it from its fd's
	// queue and re-arm or unregister the fd. A no-op if it already fired.
	void cancel_op(uint64_t target_id)
	{
		for ( std::unordered_map<int, std::deque<pending> >::iterator it =
			      fd_ops.begin(); it != fd_ops.end(); ++it )
		{
			std::deque<pending> &q = it->second;
			for ( std::deque<pending>::iterator pi = q.begin();
			      pi != q.end(); ++pi )
			{
				if ( pi->id != target_id )
					continue;
				int fd = it->first;
				q.erase(pi);
				if ( q.empty() )
				{
					fd_ops.erase(it);
					::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
				}
				else
					arm_fd(fd, true);
				return;
			}
		}
	}

	void complete_op(pending &p, uint32_t revents)
	{
		completion c;
		c.id = p.id;
		c.kind = p.kind;
		c.user = p.user;
		int result;
		if ( p.kind == op_kind::poll )
		{
			// Readiness only — no syscall; report the ready set (a
			// hangup/error is readable so the caller's read surfaces it).
			int flags = 0;
			if ( revents & (EPOLLIN | EPOLLHUP | EPOLLERR) )
				flags |= readable;
			if ( revents & (EPOLLOUT | EPOLLHUP | EPOLLERR) )
				flags |= writable;
			result = flags;
		}
		else if ( p.kind == op_kind::accept )
		{
#if defined(SOCK_CLOEXEC)
			int a = ::accept4(p.fd, nullptr, nullptr, SOCK_CLOEXEC);
#else
			int a = ::accept(p.fd, nullptr, nullptr);
#endif
			result = (a >= 0) ? a : -errno;
		}
		else if ( p.kind == op_kind::read )
		{
			ssize_t n = ::recv(p.fd, p.buf, p.len, 0);
			result = (n >= 0) ? (int)n : -errno;
		}
		else	// write
		{
			ssize_t n = ::send(p.fd, p.buf, p.len, MSG_NOSIGNAL);
			result = (n >= 0) ? (int)n : -errno;
		}
		c.result = result;
		post(c);
	}

	void drain_submissions()
	{
		std::deque<pending> batch;
		{
			std::lock_guard<std::mutex> g(sub_mtx);
			batch.swap(submissions);
		}
		for ( std::deque<pending>::iterator pi = batch.begin();
		      pi != batch.end(); ++pi )
		{
			if ( pi->kind == op_kind::cancel )
			{
				cancel_op(pi->target);	// no completion posted
				continue;
			}
			if ( pi->kind == op_kind::close )
			{
				unregister_fd(pi->fd);	// drop any pending interest
				int rc = ::close(pi->fd);
				completion c;
				c.id = pi->id;
				c.kind = op_kind::close;
				c.result = (rc == 0) ? 0 : -errno;
				c.user = pi->user;
				post(c);
				continue;
			}
			bool had = fd_ops.count(pi->fd) != 0;
			fd_ops[pi->fd].push_back(*pi);
			arm_fd(pi->fd, had);
		}
	}

	void service_fd(int fd, uint32_t revents)
	{
		std::unordered_map<int, std::deque<pending> >::iterator it =
			fd_ops.find(fd);
		if ( it == fd_ops.end() )
			return;
		std::deque<pending> &q = it->second;

		bool fd_readable = (revents & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0;
		bool fd_writable = (revents & (EPOLLOUT | EPOLLHUP | EPOLLERR)) != 0;

		// One op per readiness signal; level-triggered epoll re-reports if
		// more remain, so we never starve a queued op.
		for ( std::deque<pending>::iterator pi = q.begin(); pi != q.end();
		      ++pi )
		{
			bool ready;
			if ( pi->kind == op_kind::write )
				ready = fd_writable;
			else if ( pi->kind == op_kind::poll )
				ready = ((pi->events & readable) && fd_readable)
				     || ((pi->events & writable) && fd_writable);
			else	// accept, read
				ready = fd_readable;
			if ( !ready )
				continue;
			pending p = *pi;
			q.erase(pi);
			complete_op(p, revents);
			break;
		}
		if ( q.empty() )
			unregister_fd(fd);
		else
			arm_fd(fd, true);
	}

	void run()
	{
		std::vector<epoll_event> events(64);
		for ( ;; )
		{
			if ( stop.load() )
				return;
			int n = ::epoll_wait(epfd, events.data(),
					     (int)events.size(), -1);
			if ( n < 0 )
			{
				if ( errno == EINTR )
					continue;
				return;
			}
			for ( int i = 0; i < n; ++i )
			{
				int fd = events[i].data.fd;
				if ( fd == submit_evt )
				{
					uint64_t v = 0;
					ssize_t rr = ::read(submit_evt, &v,
							    sizeof(v));
					(void)rr;
					if ( stop.load() )
						return;
					drain_submissions();
					continue;
				}
				service_fd(fd, events[i].events);
			}
		}
	}
};

bool Reactor::available()
{
	return true;
}

Reactor::Reactor() : _(new impl())
{
	_->epfd = ::epoll_create1(EPOLL_CLOEXEC);
	_->submit_evt = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	_->doorbell_evt = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if ( _->epfd < 0 || _->submit_evt < 0 || _->doorbell_evt < 0 )
	{
		if ( _->epfd >= 0 )
			::close(_->epfd);
		if ( _->submit_evt >= 0 )
			::close(_->submit_evt);
		if ( _->doorbell_evt >= 0 )
			::close(_->doorbell_evt);
		delete _;
		throw std::runtime_error(
			"io::Reactor: epoll/eventfd initialization failed");
	}
	epoll_event ev;
	std::memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.fd = _->submit_evt;
	::epoll_ctl(_->epfd, EPOLL_CTL_ADD, _->submit_evt, &ev);
	_->th = std::thread([this]() { _->run(); });
}

Reactor::~Reactor()
{
	_->stop.store(true);
	_->wake_iothread();		// interrupt epoll_wait; run() returns
	if ( _->th.joinable() )
		_->th.join();
	if ( _->epfd >= 0 )
		::close(_->epfd);
	if ( _->submit_evt >= 0 )
		::close(_->submit_evt);
	if ( _->doorbell_evt >= 0 )
		::close(_->doorbell_evt);
	delete _;
}

uint64_t Reactor::submit_accept(int listen_fd, void *user)
{
	pending p;
	p.id = _->next_id.fetch_add(1);
	p.kind = op_kind::accept;
	p.fd = listen_fd;
	p.buf = nullptr;
	p.len = 0;
	p.events = 0;
	p.target = 0;
	p.user = user;
	return _->enqueue(p);
}

uint64_t Reactor::submit_read(int fd, void *buffer, std::size_t len, void *user)
{
	pending p;
	p.id = _->next_id.fetch_add(1);
	p.kind = op_kind::read;
	p.fd = fd;
	p.buf = buffer;
	p.len = len;
	p.events = 0;
	p.target = 0;
	p.user = user;
	return _->enqueue(p);
}

uint64_t Reactor::submit_write(int fd, const void *buffer, std::size_t len,
			       void *user)
{
	pending p;
	p.id = _->next_id.fetch_add(1);
	p.kind = op_kind::write;
	p.fd = fd;
	p.buf = const_cast<void *>(buffer);
	p.len = len;
	p.events = 0;
	p.target = 0;
	p.user = user;
	return _->enqueue(p);
}

uint64_t Reactor::submit_close(int fd, void *user)
{
	pending p;
	p.id = _->next_id.fetch_add(1);
	p.kind = op_kind::close;
	p.fd = fd;
	p.buf = nullptr;
	p.len = 0;
	p.events = 0;
	p.target = 0;
	p.user = user;
	return _->enqueue(p);
}

uint64_t Reactor::submit_poll(int fd, int events, void *user)
{
	pending p;
	p.id = _->next_id.fetch_add(1);
	p.kind = op_kind::poll;
	p.fd = fd;
	p.buf = nullptr;
	p.len = 0;
	p.events = events;
	p.target = 0;
	p.user = user;
	return _->enqueue(p);
}

void Reactor::submit_cancel(uint64_t target_id)
{
	pending p;
	p.id = _->next_id.fetch_add(1);
	p.kind = op_kind::cancel;
	p.fd = -1;
	p.buf = nullptr;
	p.len = 0;
	p.events = 0;
	p.target = target_id;
	p.user = nullptr;
	_->enqueue(p);
}

std::size_t Reactor::drain(completion *out, std::size_t max)
{
	std::lock_guard<std::mutex> g(_->comp_mtx);
	std::size_t n = 0;
	while ( n < max && !_->completions.empty() )
	{
		out[n++] = _->completions.front();
		_->completions.pop_front();
	}
	return n;
}

intptr_t Reactor::doorbell() const
{
	return _->doorbell_evt;
}

std::size_t Reactor::wait(completion *out, std::size_t max, int timeout_ms)
{
	// Anything already posted drains first (so a leftover past a previous
	// max-capped drain is never stranded behind a cleared doorbell).
	std::size_t n = drain(out, max);
	if ( n > 0 )
	{
		_->clear_doorbell();
		return n;
	}
	pollfd pfd;
	pfd.fd = _->doorbell_evt;
	pfd.events = POLLIN;
	pfd.revents = 0;
	int pr = ::poll(&pfd, 1, timeout_ms);
	if ( pr <= 0 )
		return 0;		// timeout or interrupted
	_->clear_doorbell();
	return drain(out, max);
}

} // namespace io
} // namespace madc

#else // !defined(__linux__)

// No backend on this platform yet (kqueue/IOCP are follow-on slices). The
// interface still compiles and links so cross-platform TUs that reference it
// build; construction refuses loudly and available() says so.
namespace madc {
namespace io {

struct Reactor::impl
{
};

bool Reactor::available()
{
	return false;
}

Reactor::Reactor() : _(nullptr)
{
	throw std::runtime_error(
		"io::Reactor: no async I/O backend on this platform yet");
}

Reactor::~Reactor()
{
}

uint64_t Reactor::submit_accept(int, void *)
{
	return 0;
}

uint64_t Reactor::submit_read(int, void *, std::size_t, void *)
{
	return 0;
}

uint64_t Reactor::submit_write(int, const void *, std::size_t, void *)
{
	return 0;
}

uint64_t Reactor::submit_close(int, void *)
{
	return 0;
}

uint64_t Reactor::submit_poll(int, int, void *)
{
	return 0;
}

void Reactor::submit_cancel(uint64_t)
{
}

std::size_t Reactor::drain(completion *, std::size_t)
{
	return 0;
}

intptr_t Reactor::doorbell() const
{
	return -1;
}

std::size_t Reactor::wait(completion *, std::size_t, int)
{
	return 0;
}

} // namespace io
} // namespace madc

#endif // defined(__linux__)
