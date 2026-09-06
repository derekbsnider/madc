#ifndef __MADCDIS_CHANNEL_H
#define __MADCDIS_CHANNEL_H 1

#include "madcdis/datachannel.h"
#include "libmadc/value.h"

#include <string>
#include <stdint.h>

namespace madc {

// madc::channel — one URI-addressed byte channel with line helpers, the
// script-facing convenience surface over the DataChannel registry (and the
// embedding host's convenience wrapper — same class, cpp-first).
//
// LAYOUT CONTRACT: a single void *impl_ member, append-only. The embedded
// header <ns_madc> declares this class declaration-only and scripts resolve
// its methods mangled-direct against libmadc — the two declarations must
// stay layout- and signature-identical (SysInfo precedent).
//
// Modes: "r" read, "w" write, "rw" read+write (default), "a" append.
// readline() strips the trailing newline (and a preceding '\r' if present)
// and returns the final unterminated tail; it returns false only at EOF.
class channel
{
public:
	explicit channel(const char *uri);
	channel(const char *uri, const char *mode);
	~channel();

	bool ok() const;
	const char *last_error() const;

	int64_t read(void *buffer, int64_t capacity);
	bool readline(std::string &out);
	bool readall(std::string &out);
	// value-carrier twins (slice V1): the line/payload lands as a
	// string-kind madc::value; write() sends the value's text view.
	bool readline(value &out);
	bool readall(value &out);
	bool write(const char *text);
	bool write(const char *buffer, int64_t size);
	bool write(std::string &text);
	bool write(value &text);
	bool close_write();
	void close();

	// Readiness (MT-4b): poll_state() = 1 when read/readline makes
	// progress NOW (buffered text, readable bytes, or an EOF/error one
	// read will surface), 0 = it would wait, -1 = dead (failed, closed,
	// or EOF fully drained). Channels with no waitable read side
	// (memory, file) always report 1 — their reads never block.
	// wait_readable() parks the calling task until progress is possible
	// (true) or the channel is dead (false). read_wait_handle() is the
	// raw poll handle for event-loop plumbing (a CRT fd; -1 = not
	// waitable) — int64_t on purpose: the embedded-header twin must
	// mangle identically on every platform (intptr_t does not).
	int64_t poll_state();
	bool wait_readable();
	int64_t read_wait_handle();

	// Abandon the transfer NOW (IDE-10b stop): tear down the endpoint
	// without waiting for graceful completion — an exec:// child is
	// SIGTERMed — so a following close() returns promptly. No-op on
	// channels with nothing to abandon.
	void cancel();

private:
	channel(const channel &);
	channel &operator=(const channel &);

	void *impl_;
};

} // namespace madc

#endif // __MADCDIS_CHANNEL_H
