// SPDX-License-Identifier: MPL-2.0
// madc_io_reactor — the engine's one platform-native async I/O engine
// (design: docs/plans/2026-09-11-async-io-reactor.md). A COMPLETION-oriented
// (proactor) interface: a caller SUBMITS an operation and later reaps a
// COMPLETION carrying the result (and, for a read, the bytes already in the
// caller's buffer). One dedicated OS thread runs the platform backend (epoll
// today; io_uring / kqueue / IOCP are the follow-on backends behind this same
// contract). The completion contract is what makes the completion-native
// platforms (io_uring, IOCP) first-class; the readiness platforms (epoll,
// kqueue, select) emulate it (wait readable -> do the syscall on the I/O
// thread -> post a completion).
//
// THREAD-SAFETY (thread-safety.md): the I/O thread is PURE PLUMBING — it
// touches only fds, its backend handle, and the two queues; it never enters
// dialect / session / parser / world code. submit_*() are safe to call from
// the producer thread(s); drain() / wait() / doorbell() belong to the SINGLE
// consumer (the cooperative scheduler thread in the engine; the test thread
// in tests). A buffer handed to submit_read/submit_write must outlive the op
// (until its completion drains) and must not be touched by the caller before
// then — the standard async-buffer contract.

#ifndef __MADC_IO_REACTOR_H
#define __MADC_IO_REACTOR_H 1

#include <cstddef>
#include <cstdint>

namespace madc {
namespace io {

enum class op_kind : unsigned char
{
	accept,
	read,
	write,
	close
};

// The result of one submitted operation. `result` is >= 0 on success (bytes
// transferred for read/write, the accepted fd for accept, 0 for close) and
// < 0 as -errno on failure. `user` is the opaque cookie echoed from submit;
// `id` is the monotonic submission id, for callers that correlate by id.
struct completion
{
	uint64_t id;
	op_kind kind;
	int result;
	void *user;
};

class Reactor
{
public:
	Reactor();		// starts the I/O thread; throws if unavailable
	~Reactor();		// stops + joins the I/O thread
	Reactor(const Reactor &) = delete;
	Reactor &operator=(const Reactor &) = delete;

	// A real backend is compiled for this platform (epoll on Linux today).
	// When false, the constructor throws — a caller probes this first.
	static bool available();

	// Submit one operation; returns its monotonic id. `user` is echoed back
	// in the completion. For read/write the buffer must outlive the op.
	uint64_t submit_accept(int listen_fd, void *user);
	uint64_t submit_read(int fd, void *buffer, std::size_t len, void *user);
	uint64_t submit_write(int fd, const void *buffer, std::size_t len,
			      void *user);
	uint64_t submit_close(int fd, void *user);

	// Consumer side (single thread). drain() moves up to `max` ready
	// completions into `out` without blocking and returns the count.
	// doorbell() is a fd that reads readable while completions are pending —
	// an event loop folds it into its wait, then drains. wait() blocks up to
	// timeout_ms (< 0 = forever) for at least one completion, then drains —
	// the standalone driver (tests; a server with no cooperative scheduler).
	std::size_t drain(completion *out, std::size_t max);
	intptr_t doorbell() const;
	std::size_t wait(completion *out, std::size_t max, int timeout_ms);

private:
	struct impl;
	impl *_;
};

} // namespace io
} // namespace madc

#endif // __MADC_IO_REACTOR_H
