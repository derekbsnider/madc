// SPDX-License-Identifier: MPL-2.0
// madc_io_reactor — the completion-oriented async I/O engine's backends
// (design: docs/plans/2026-09-11-async-io-reactor.md). epoll (Linux) and
// WSAPoll (Windows) are READINESS interfaces, so each arm is a
// readiness->syscall->completion adapter: the I/O thread waits for a fd to be
// ready, then performs the accept/recv/send ITSELF and posts a completion
// carrying the result. That is the same shape kqueue will share; io_uring and
// IOCP will be native-completion backends behind the identical Reactor
// contract.
//
// ONE face, N I/O-thread sides (no-parallel-implementations.md): everything a
// real backend shares — the op record, the two queues, submit_*, drain, wait,
// wait_doorbell — is written ONCE below the platform split; a platform arm
// supplies only its impl (the wait syscall, the wake spelling, the doorbell
// object) behind the small internal contract the shared face calls:
// enqueue / drain_into / clear_doorbell / block_doorbell.
//
// Slice 1 (2026-09-11): the abstraction + the epoll backend + the dedicated
// I/O thread + the queues + the doorbell. Slice 4 (2026-09-15, the reactor's
// Windows backend plan): the WSAPoll arm — the design's "select floor" — so
// the taskio park on Windows blocks on the doorbell beside its console
// handles instead of never reaching the reactor. io_uring, kqueue and IOCP
// remain the follow-on arms.

#include "madc_io_reactor.h"

#include <stdexcept>
#include <string>

#if defined(__linux__) || defined(_WIN32)
#define MADC_IO_REACTOR_BACKEND 1
#endif

#if defined(MADC_IO_REACTOR_BACKEND)

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace madc {
namespace io {
namespace {

// One queued operation, owned first by the submission queue, then by the
// I/O thread's per-fd op list. Shared by every backend.
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

// The readiness->completion rule an op waits on, as the abstract pair the
// platform flag alphabets both render: accept + read + a readable poll op
// want readability; write + a writable poll op want writability.
bool op_wants_readable(const pending &p)
{
	if ( p.kind == op_kind::poll )
		return (p.events & readable) != 0 || (p.events & writable) == 0;
	return p.kind != op_kind::write;
}

bool op_wants_writable(const pending &p)
{
	if ( p.kind == op_kind::poll )
		return (p.events & writable) != 0;
	return p.kind == op_kind::write;
}

// The ready poll_flag set a poll completion reports: a hangup/error is BOTH
// (the caller's own syscall surfaces it — poll(2)'s POLLHUP/POLLERR rule).
int poll_result_flags(bool fd_readable, bool fd_writable)
{
	int flags = 0;
	if ( fd_readable )
		flags |= readable;
	if ( fd_writable )
		flags |= writable;
	return flags;
}

} // namespace
} // namespace io
} // namespace madc

#endif // MADC_IO_REACTOR_BACKEND

#if defined(__linux__)

#include <cerrno>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace madc {
namespace io {
namespace {

uint32_t epoll_events_for(const pending &p)
{
	uint32_t mask = 0;
	if ( op_wants_readable(p) )
		mask |= (uint32_t)EPOLLIN;
	if ( op_wants_writable(p) )
		mask |= (uint32_t)EPOLLOUT;
	return mask;
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
		ssize_t r = ::write(submit_evt, &one, sizeof(one)); // EVENTFD-WAKE
		(void)r;
	}

	void ring_doorbell()
	{
		uint64_t one = 1;
		ssize_t r = ::write(doorbell_evt, &one, sizeof(one)); // EVENTFD-WAKE
		(void)r;
	}

	void clear_doorbell()
	{
		uint64_t v = 0;
		ssize_t r = ::read(doorbell_evt, &v, sizeof(v));
		(void)r;	// EFD_NONBLOCK: EAGAIN when already zero
	}

	// Block up to timeout_ms for the doorbell: 1 = rang, 0 = timeout,
	// -1 = interrupted / error (the shared wait face's contract).
	int block_doorbell(int timeout_ms)
	{
		pollfd pfd;
		pfd.fd = doorbell_evt;
		pfd.events = POLLIN;
		pfd.revents = 0;
		int pr = ::poll(&pfd, 1, timeout_ms);
		if ( pr > 0 )
			return 1;
		return pr == 0 ? 0 : -1;
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

	std::size_t drain_into(completion *out, std::size_t max)
	{
		std::lock_guard<std::mutex> g(comp_mtx);
		std::size_t n = 0;
		while ( n < max && !completions.empty() )
		{
			out[n++] = completions.front();
			completions.pop_front();
		}
		return n;
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
			// Readiness only — no syscall; report the ready set.
			result = poll_result_flags(
				(revents & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0,
				(revents & (EPOLLOUT | EPOLLHUP | EPOLLERR)) != 0);
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
			bool ready = (op_wants_readable(*pi) && fd_readable)
				  || (op_wants_writable(*pi) && fd_writable);
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

intptr_t Reactor::doorbell() const
{
	return _->doorbell_evt;
}

} // namespace io
} // namespace madc

#elif defined(_WIN32)

// The Windows backend — the design's "select floor" (item 4): a WSAPoll
// READINESS adapter with the epoll arm's exact I/O-thread shape. IOCP, the
// native-completion backend, slots in behind this same contract once the
// completion ops have a consumer (the channel-layer migration); the reactor's
// one consumer today is submit_poll + the doorbell (the taskio park), which
// IOCP has no native spelling for (no readiness op; a listener's "connection
// pending" needs AcceptEx's pre-created socket). Platform spellings: the
// submit wake is a loopback UDP socket the I/O thread polls beside the
// watched sockets (WSAPoll takes only sockets; there is no eventfd); the
// doorbell is a manual-reset Event whose HANDLE doorbell() returns, so the
// scheduler's console wait joins it in ONE WaitForMultipleObjects. The fd
// space this backend accepts is SOCKETs — the int fd IS the SOCKET
// (create_socket's "kernel handles fit in 32 bits" model); the taskio seat
// arms only socket-kind waiters here (reactor_watches).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <winsock2.h>	// before windows.h (the winsock1 collision)
#include <ws2tcpip.h>
#include <windows.h>
#include <limits.h>

namespace madc {
namespace io {
namespace {

// The WSAPoll flag alphabet for the shared readiness rule (epoll_events_for's
// twin — one rule, two alphabets; keep them side by side).
SHORT wsapoll_events_for(const pending &p)
{
	SHORT mask = 0;
	if ( op_wants_readable(p) )
		mask |= POLLRDNORM;
	if ( op_wants_writable(p) )
		mask |= POLLWRNORM;
	return mask;
}

// A hangup, an error, or an invalid (closed-under-us) socket is BOTH
// readable and writable: the caller's op surfaces it. POLLNVAL must count,
// or WSAPoll returns at once every pass and the I/O thread spins.
bool wsapoll_readable(SHORT revents)
{
	return (revents & (POLLRDNORM | POLLHUP | POLLERR | POLLNVAL)) != 0;
}

bool wsapoll_writable(SHORT revents)
{
	return (revents & (POLLWRNORM | POLLHUP | POLLERR | POLLNVAL)) != 0;
}

// One process-wide WSAStartup (the socket channel's socket_stack_ready
// twin; the reactor can be the first winsock caller of a process that
// adopted a socket handle). Never a WSACleanup — the OS reclaims at exit.
bool winsock_ready()
{
	static const bool ready = []() {
		WSADATA data;
		return WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}();
	return ready;
}

// The winsock error of the last call as the completion's -errno spelling.
int negative_wsa_error()
{
	int code = WSAGetLastError();
	return code > 0 ? -code : -1;
}

// A loopback UDP socket connected to itself: the I/O thread's wake object
// (the eventfd spelling for a poll set that takes only sockets). Non-blocking
// so the drain loop ends on WSAEWOULDBLOCK and a full buffer drops a wake
// that is already pending anyway (eventfd's coalescing, by another name).
SOCKET make_wake_socket()
{
	SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if ( s == INVALID_SOCKET )
		return INVALID_SOCKET;
	sockaddr_in address;
	std::memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	int len = sizeof(address);
	if ( ::bind(s, reinterpret_cast<sockaddr *>(&address), len) != 0
	  || ::getsockname(s, reinterpret_cast<sockaddr *>(&address), &len) != 0
	  || ::connect(s, reinterpret_cast<sockaddr *>(&address), len) != 0 )
	{
		::closesocket(s);
		return INVALID_SOCKET;
	}
	u_long nonblocking = 1;
	::ioctlsocket(s, FIONBIO, &nonblocking);
	SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
	return s;
}

} // namespace

struct Reactor::impl
{
	SOCKET wake_sock = INVALID_SOCKET;	// producer -> I/O thread wake
	HANDLE doorbell_evt = NULL;		// I/O thread -> consumer wake
	std::atomic<bool> stop;
	std::atomic<uint64_t> next_id;

	std::mutex sub_mtx;
	std::deque<pending> submissions;	// producer -> I/O thread

	std::mutex comp_mtx;
	std::deque<completion> completions;	// I/O thread -> consumer

	// I/O-thread-owned: fd -> its queued ops (FIFO). Never touched off-thread.
	// The WSAPoll set is rebuilt from it before every wait (there is no
	// persistent registration to keep in step, unlike epoll's interest).
	std::unordered_map<int, std::deque<pending> > fd_ops;

	std::thread th;

	impl() : stop(false), next_id(1) {}

	void wake_iothread()
	{
		char one = 1;
		::send(wake_sock, &one, 1, 0);
	}

	void ring_doorbell()
	{
		SetEvent(doorbell_evt);
	}

	void clear_doorbell()
	{
		ResetEvent(doorbell_evt);
	}

	// Block up to timeout_ms for the doorbell: 1 = rang, 0 = timeout,
	// -1 = error (Windows has no EINTR axis).
	int block_doorbell(int timeout_ms)
	{
		DWORD r = WaitForSingleObject(doorbell_evt,
					      timeout_ms < 0 ? INFINITE
							     : (DWORD)timeout_ms);
		if ( r == WAIT_OBJECT_0 )
			return 1;
		return r == WAIT_TIMEOUT ? 0 : -1;
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

	std::size_t drain_into(completion *out, std::size_t max)
	{
		std::lock_guard<std::mutex> g(comp_mtx);
		std::size_t n = 0;
		while ( n < max && !completions.empty() )
		{
			out[n++] = completions.front();
			completions.pop_front();
		}
		return n;
	}

	// --- I/O-thread side (everything below runs ONLY on the I/O thread) ---

	// Remove a still-pending op by id (a cancel). A no-op if it already
	// fired. The next wait's set simply omits an fd whose queue emptied.
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
				q.erase(pi);
				if ( q.empty() )
					fd_ops.erase(it);
				return;
			}
		}
	}

	void complete_op(pending &p, SHORT revents)
	{
		completion c;
		c.id = p.id;
		c.kind = p.kind;
		c.user = p.user;
		int result;
		if ( p.kind == op_kind::poll )
		{
			result = poll_result_flags(wsapoll_readable(revents),
						   wsapoll_writable(revents));
		}
		else if ( p.kind == op_kind::accept )
		{
			SOCKET a = ::accept((SOCKET)p.fd, nullptr, nullptr);
			if ( a == INVALID_SOCKET )
				result = negative_wsa_error();
			else
			{
				// Born inheritable (the socket channel's note);
				// clear it, as create_socket / accept() do.
				SetHandleInformation((HANDLE)a, HANDLE_FLAG_INHERIT, 0);
				result = (int)a;
			}
		}
		else if ( p.kind == op_kind::read )
		{
			int chunk = p.len > (std::size_t)INT_MAX ? INT_MAX
							       : (int)p.len;
			int n = ::recv((SOCKET)p.fd, (char *)p.buf, chunk, 0);
			result = (n >= 0) ? n : negative_wsa_error();
		}
		else	// write (no SIGPIPE to suppress on Windows)
		{
			int chunk = p.len > (std::size_t)INT_MAX ? INT_MAX
							       : (int)p.len;
			int n = ::send((SOCKET)p.fd, (const char *)p.buf, chunk, 0);
			result = (n >= 0) ? n : negative_wsa_error();
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
				fd_ops.erase(pi->fd);	// drop any pending interest
				int rc = ::closesocket((SOCKET)pi->fd);
				completion c;
				c.id = pi->id;
				c.kind = op_kind::close;
				c.result = (rc == 0) ? 0 : negative_wsa_error();
				c.user = pi->user;
				post(c);
				continue;
			}
			fd_ops[pi->fd].push_back(*pi);
		}
	}

	void service_fd(int fd, SHORT revents)
	{
		std::unordered_map<int, std::deque<pending> >::iterator it =
			fd_ops.find(fd);
		if ( it == fd_ops.end() )
			return;
		std::deque<pending> &q = it->second;

		bool fd_readable = wsapoll_readable(revents);
		bool fd_writable = wsapoll_writable(revents);

		// One op per readiness signal; WSAPoll is level-triggered too, so
		// the next pass re-reports whatever remains — no queued op starves.
		for ( std::deque<pending>::iterator pi = q.begin(); pi != q.end();
		      ++pi )
		{
			bool ready = (op_wants_readable(*pi) && fd_readable)
				  || (op_wants_writable(*pi) && fd_writable);
			if ( !ready )
				continue;
			pending p = *pi;
			q.erase(pi);
			complete_op(p, revents);
			break;
		}
		if ( q.empty() )
			fd_ops.erase(fd);
	}

	void run()
	{
		std::vector<WSAPOLLFD> fds;
		std::vector<int> owners;	// fds[i + 1] -> its fd_ops key
		for ( ;; )
		{
			if ( stop.load() )
				return;
			fds.clear();
			owners.clear();
			WSAPOLLFD wake;
			wake.fd = wake_sock;
			wake.events = POLLRDNORM;
			wake.revents = 0;
			fds.push_back(wake);
			for ( std::unordered_map<int, std::deque<pending> >::iterator
				      it = fd_ops.begin(); it != fd_ops.end(); ++it )
			{
				WSAPOLLFD e;
				e.fd = (SOCKET)it->first;
				e.events = 0;
				for ( std::deque<pending>::iterator p =
					      it->second.begin();
				      p != it->second.end(); ++p )
					e.events |= wsapoll_events_for(*p);
				e.revents = 0;
				fds.push_back(e);
				owners.push_back(it->first);
			}
			int n = ::WSAPoll(fds.data(), (ULONG)fds.size(), -1);
			if ( n == SOCKET_ERROR )
				return;		// the set itself is bad: stop
			if ( fds[0].revents != 0 )
			{
				char sink[64];
				while ( ::recv(wake_sock, sink, sizeof(sink), 0) > 0 )
				{
				}
				if ( stop.load() )
					return;
				drain_submissions();
			}
			for ( std::size_t i = 1; i < fds.size(); ++i )
				if ( fds[i].revents != 0 )
					service_fd(owners[i - 1], fds[i].revents);
		}
	}
};

Reactor::Reactor() : _(new impl())
{
	if ( winsock_ready() )
		_->wake_sock = make_wake_socket();
	_->doorbell_evt = CreateEventW(NULL, TRUE, FALSE, NULL);	// manual reset
	if ( _->wake_sock == INVALID_SOCKET || _->doorbell_evt == NULL )
	{
		if ( _->wake_sock != INVALID_SOCKET )
			::closesocket(_->wake_sock);
		if ( _->doorbell_evt != NULL )
			CloseHandle(_->doorbell_evt);
		delete _;
		throw std::runtime_error(
			"io::Reactor: WSAPoll/event initialization failed");
	}
	_->th = std::thread([this]() { _->run(); });
}

Reactor::~Reactor()
{
	_->stop.store(true);
	_->wake_iothread();		// interrupt WSAPoll; run() returns
	if ( _->th.joinable() )
		_->th.join();
	if ( _->wake_sock != INVALID_SOCKET )
		::closesocket(_->wake_sock);
	if ( _->doorbell_evt != NULL )
		CloseHandle(_->doorbell_evt);
	delete _;
}

intptr_t Reactor::doorbell() const
{
	return (intptr_t)_->doorbell_evt;
}

} // namespace io
} // namespace madc

#endif // platform arms

#if defined(MADC_IO_REACTOR_BACKEND)

// The face every real backend shares — written once, over impl's small
// internal contract (enqueue / drain_into / clear_doorbell / block_doorbell).

namespace madc {
namespace io {

bool Reactor::available()
{
	return true;
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
	return _->drain_into(out, max);
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
	if ( _->block_doorbell(timeout_ms) <= 0 )
		return 0;		// timeout or interrupted
	_->clear_doorbell();
	return drain(out, max);
}

int Reactor::wait_doorbell(int timeout_ms)
{
	int r = _->block_doorbell(timeout_ms);
	if ( r > 0 )
		_->clear_doorbell();
	return r;	// 1 = rang (drain now); 0 = timeout; -1 = EINTR / error
}

} // namespace io
} // namespace madc

#else // no backend on this platform

// No backend on this platform yet (kqueue is the follow-on slice). The
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

// The stub owns the (never allocated) impl like the real backends do — and
// that read is what keeps clang's -Wunused-private-field quiet on darwin,
// where this arm is the whole TU (the V6 seam's macOS lane failed on it:
// -Werror, and a pimpl only initialized is "unused" to clang).
Reactor::~Reactor()
{
	delete _;
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

int Reactor::wait_doorbell(int)
{
	return -1;
}

} // namespace io
} // namespace madc

#endif // MADC_IO_REACTOR_BACKEND
