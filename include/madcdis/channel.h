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
	// An empty channel with no endpoint yet — the target accept() fills.
	channel();
	explicit channel(const char *uri);
	channel(const char *uri, const char *mode);
	~channel();

	bool ok() const;
	const char *last_error() const;

	// Listener facet (a `listen://host:port` channel). accept() takes the
	// next pending connection into `client` (an empty channel), returning 1
	// on accept, 0 when none is pending (park via wait_readable() and retry
	// — the listener is poll_state/wait_readable-driven), -1 on error or a
	// non-listener channel. local_endpoint() reports the address actually
	// bound (host:port after an ephemeral :0), "" for a non-listener; the
	// pointer is ring-lifetime (copy it before the next call).
	int64_t accept(channel &client);
	const char *local_endpoint();

	// Hand-off facet (V6a duplex serve): a cooperative serve loop spawns
	// one task per accepted client, but `go` carries only long/double/
	// pointer slots and this channel is non-copyable — so an accepted
	// connection rides into a task as a `long` HANDLE. detach() registers
	// this channel's live endpoint under a fresh handle and re-empties this
	// object (returns 0 when there is nothing to hand off); adopt() moves a
	// detached endpoint into an empty channel and consumes the handle
	// (false = unknown/already-spent handle, or this channel still holds an
	// endpoint). The handle is a ONE-SHOT transfer token; the registry is
	// scheduler-thread-only (the taskio single-thread contract). This is NOT
	// a value-channel (madc::chan_*) handle — the two handle spaces differ.
	int64_t detach();
	bool adopt(int64_t handle);

	// Broadcast facet (V6a duplex push): a serve loop keeps ownership of
	// its channel but shares WRITE access so a peer's task can push change
	// events down this socket (the hub fan-out). share() registers this
	// channel under a fresh id and returns it; unshare() removes it (call
	// it before close()). The free conn_broadcast(except_id, line) writes
	// `line` to every shared channel but `except_id`. Scheduler-thread-only,
	// and safe because a cooperative write never yields mid-line: a peer
	// broadcasts only while this channel's owning task is parked in
	// readline, and writes complete atomically between yields.
	int64_t share();
	void unshare(int64_t id);

	// WebSocket facet (V6b): upgrade an accepted (or connected) byte
	// channel to an RFC6455 message channel. upgrade_websocket() is the
	// SERVER role — it reads the HTTP upgrade request (parking, so a serve
	// task never blocks the thread), sends the 101 accept, and puts this
	// channel in message mode; connect_websocket(resource) is the CLIENT
	// role — it sends the upgrade request for `resource` and consumes the
	// 101. Both return false (with last_error) on a non-WebSocket / failed
	// handshake. Afterwards the channel speaks MESSAGES: readline() returns
	// one text message, write() sends one text frame, and read()/readall()
	// refuse (message-oriented). Control frames (ping/pong/close) are
	// handled internally; a close frame surfaces as readline() EOF.
	bool upgrade_websocket();
	bool connect_websocket(const char *resource);

	// Serve facet (V6b-3): classify one accepted connection and route it so
	// a single --serve port carries all three seats. Returns 2 for an api
	// client (a JSON line — the bytes stay buffered for readline), 1 for a
	// WebSocket upgrade (this channel is now a message channel — run the ws
	// window on it), 0 when a non-upgrade HTTP request was answered with
	// `page` as an HTTP 200 (close the connection), -1 on error.
	int64_t serve_web(const char *page);

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
	// The exit status of the child behind an exec-style channel (exec://,
	// pty://, madcrun://, madcproj://) once close() reaped it — 128+signal
	// for a killed child; -1 = no child / not finished. (The script
	// fragment include/madc/ns_madc declares the same member.)
	int64_t exit_status();
	// The far end is a terminal (a pty:// / ?pty child on POSIX); false
	// on pipes — the caller emulates the line discipline it needs.
	bool is_terminal();

private:
	channel(const channel &);
	channel &operator=(const channel &);

	void *impl_;
};

} // namespace madc

#endif // __MADCDIS_CHANNEL_H
