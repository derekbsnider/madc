#include "madcdis/channel.h"
#include "madcdis/websocket_channel.h"
#include "madc_datachannel_internal.h"
#include "madc_task_io.h"
#include "ns_common.h"
#include "rt/rt_task.h"

#include <cstring>
#include <map>

namespace madc {
namespace {

// The layout contract in madcdis/channel.h pins channel to one void *impl_;
// everything mutable lives here so the class can grow without ABI breaks.
struct ChannelState
{
	std::unique_ptr<DataChannel> channel;
	error last_error;
	std::string pending;
	std::string endpoint_label;	// ring-lifetime backing for local_endpoint()
	bool eof = false;
	bool failed = false;
	int exit_status = -1;	// kept past close(): the reaped child's status
	// When set, the channel speaks RFC6455 messages (V6b): the framer owns
	// the inner socket and IS `channel` above; this typed alias lets the
	// message path call feed()/next_message()/encode_text() without a
	// dynamic_cast per readline. Nulled with `channel` in close()/accept().
	WebSocketDataChannel *wsc = nullptr;
};

ChannelState *state(void *impl)
{
	return static_cast<ChannelState *>(impl);
}

// The hand-off registry (V6a duplex serve): detach() parks a channel's whole
// ChannelState here under a monotonic handle so an accepted connection can
// ride into a `go` serve task as a `long`; adopt() consumes it. Scheduler-
// thread-only — the same single-thread cooperative contract taskio's g_io_head
// and g_chans registries hold; a byte-channel handle never crosses into the
// value-channel (chan_*) space. A handle is spent by exactly one adopt(); a
// state left here at shutdown is torn down by close()+delete on the process
// exit path (the OS reclaims it — this registry never leaks across a run).
std::map<int64_t, ChannelState *> g_detached;
int64_t g_detach_next = 1;

// The broadcast registry (V6a duplex push): a serve loop KEEPS ownership of
// its channel and registers it here so a peer's task can push change events
// down it (the hub fan-out). Distinct from g_detached (ownership transfer) —
// share() never empties the channel, and the entry is a borrowed pointer the
// owning task removes with unshare() before it closes. Scheduler-thread-only.
// `cookie` is an OPAQUE per-connection value the application owns (madcide
// stores the permission tier there — V6a tiers); the engine never interprets
// it, so the connection registry stays application-agnostic.
struct ConnEntry
{
	channel *ch;
	int64_t cookie;
};
std::map<int64_t, ConnEntry> g_shared;
int64_t g_share_next = 1;

void set_state_error(ChannelState *s, const std::string &message)
{
	s->failed = true;
	detail::set_channel_error(&s->last_error, message);
}

bool parse_mode(const char *mode, ChannelOpenMode &out)
{
	if ( !mode )
		return false;
	if ( std::strcmp(mode, "r") == 0 )
		out = ChannelOpenMode::read;
	else if ( std::strcmp(mode, "w") == 0 )
		out = ChannelOpenMode::write;
	else if ( std::strcmp(mode, "rw") == 0 )
		out = ChannelOpenMode::read_write;
	else if ( std::strcmp(mode, "a") == 0 )
		out = ChannelOpenMode::append;
	else
		return false;
	return true;
}

void open_channel_state(ChannelState *s, const char *uri, const char *mode)
{
	ChannelOpenMode open_mode = ChannelOpenMode::read_write;
	if ( !parse_mode(mode, open_mode) )
	{
		set_state_error(s, std::string("invalid channel mode: ")
				   + (mode ? mode : "(null)"));
		return;
	}
	if ( !uri || !*uri )
	{
		set_state_error(s, "channel requires a URI");
		return;
	}
	s->channel = DataChannelRegistry::instance().open(
		DataSource(uri), open_mode, &s->last_error);
	if ( !s->channel )
		s->failed = true;
}

// MT-4b: when the cooperative runtime has live tasks, park on the poll
// handle until the read can make progress instead of blocking the whole OS
// thread under them (the scheduler's io-wait seat wakes us). Solo programs
// (nothing spawned) keep the plain blocking read — zero new overhead. This
// object layer is the deepest layer PROVABLY on the scheduler's thread
// (single-thread cooperative contract); the pipe channel itself is also
// read by process pump helper threads, which must never park.
void park_until_readable(ChannelState *s)
{
	if ( __madc_task_live() <= 0 )
		return;
	PollableDataChannel *pollable = pollable_surface(s->channel.get());
	if ( !pollable )
		return;
	taskio::wait_readable(pollable->read_poll_handle());
}

// Pull one chunk into pending; false only on a read error (EOF sets s->eof).
bool fill_pending(ChannelState *s)
{
	char buffer[4096];
	std::size_t count = 0;
	park_until_readable(s);
	if ( !s->channel->read(buffer, sizeof(buffer), count, &s->last_error) )
	{
		s->failed = true;
		return false;
	}
	if ( count == 0 )
		s->eof = true;
	else
		s->pending.append(buffer, count);
	return true;
}

// Read HTTP headers off the current channel until the blank line, parking on
// the socket between reads (the WebSocket handshake runs inside a serve task,
// so it must not block the OS thread). `leftover` receives any bytes read past
// the "\r\n\r\n" boundary — the first frame bytes of the message stream.
bool read_http_headers(ChannelState *s, std::string &headers,
		       std::string &leftover)
{
	std::string buf;
	for ( ;; )
	{
		std::size_t end = buf.find("\r\n\r\n");
		if ( end != std::string::npos )
		{
			headers = buf.substr(0, end + 4);
			leftover = buf.substr(end + 4);
			return true;
		}
		if ( buf.size() > 65536 )
		{
			set_state_error(s, "websocket handshake: header too large");
			return false;
		}
		char chunk[4096];
		std::size_t count = 0;
		park_until_readable(s);
		if ( !s->channel->read(chunk, sizeof(chunk), count, &s->last_error) )
		{
			s->failed = true;
			return false;
		}
		if ( count == 0 )
		{
			set_state_error(s, "websocket handshake: EOF before headers");
			return false;
		}
		buf.append(chunk, count);
	}
}

// One WebSocket message off a ws-upgraded channel (V6b): pull complete frames
// from the decoder, pumping raw bytes (parking) when it needs more, and writing
// back any control response (a pong, a close echo) the decoder produced. false
// at a close frame or a socket EOF.
bool ws_recv(ChannelState *s, std::string &out)
{
	out.clear();
	if ( s->failed || s->eof )
		return false;
	for ( ;; )
	{
		bool eof = false;
		std::string reply;
		bool got = s->wsc->next_message(out, eof, reply);
		if ( !reply.empty()
		  && !s->wsc->write_raw(reply.data(), reply.size(), &s->last_error) )
		{
			s->failed = true;
			return false;
		}
		if ( got )
			return true;
		if ( eof )
		{
			s->eof = true;
			return false;
		}
		char chunk[4096];
		std::size_t count = 0;
		park_until_readable(s);
		if ( !s->wsc->read_raw(chunk, sizeof(chunk), count, &s->last_error) )
		{
			s->failed = true;
			return false;
		}
		if ( count == 0 )
		{
			s->eof = true;		// socket closed mid-stream
			return false;
		}
		s->wsc->feed(chunk, count);
	}
}

} // namespace

channel::channel()
	: impl_(new ChannelState())
{
	// Empty: no endpoint until accept() moves one in. ok() stays false.
}

channel::channel(const char *uri)
	: impl_(new ChannelState())
{
	open_channel_state(state(impl_), uri, "rw");
}

channel::channel(const char *uri, const char *mode)
	: impl_(new ChannelState())
{
	open_channel_state(state(impl_), uri, mode);
}

channel::~channel()
{
	close();
	delete state(impl_);
}

bool channel::ok() const
{
	ChannelState *s = state(impl_);
	return s->channel && !s->failed;
}

const char *channel::last_error() const
{
	return state(impl_)->last_error.message.c_str();
}

int64_t channel::accept(channel &client)
{
	ChannelState *s = state(impl_);
	if ( !s->channel )
	{
		set_state_error(s, "accept on a channel with no endpoint");
		return -1;
	}
	AcceptorDataChannel *acceptor = acceptor_surface(s->channel.get());
	if ( !acceptor )
	{
		set_state_error(s, "channel is not a listener");
		return -1;
	}
	std::unique_ptr<DataChannel> accepted;
	AcceptResult result = acceptor->accept(accepted, &s->last_error);
	if ( result == AcceptResult::would_block )
		return 0;
	if ( result == AcceptResult::error )
	{
		s->failed = true;
		return -1;
	}
	// Hand the accepted byte stream to `client`, replacing whatever it held
	// (a fresh accept target is empty; a reused one is closed by the move).
	ChannelState *cs = state(client.impl_);
	cs->channel = std::move(accepted);
	cs->wsc = nullptr;		// a fresh accepted byte stream, not ws yet
	cs->pending.clear();
	cs->eof = false;
	cs->failed = false;
	cs->last_error = error();
	return 1;
}

const char *channel::local_endpoint()
{
	ChannelState *s = state(impl_);
	AcceptorDataChannel *acceptor =
		s->channel ? acceptor_surface(s->channel.get()) : nullptr;
	s->endpoint_label = acceptor ? acceptor->local_endpoint() : std::string();
	return s->endpoint_label.c_str();
}

int64_t channel::detach()
{
	ChannelState *s = state(impl_);
	if ( !s->channel )
		return 0;		// nothing live to hand off
	// Park the whole state (endpoint + buffered bytes + eof/exit) under a
	// fresh handle and re-empty THIS object, so its dtor frees only the new
	// empty state and the accepted stream survives to the adopting task.
	int64_t handle = g_detach_next++;
	g_detached[handle] = s;
	impl_ = new ChannelState();
	return handle;
}

bool channel::adopt(int64_t handle)
{
	ChannelState *s = state(impl_);
	if ( s->channel )
		return false;		// this channel still holds an endpoint
	std::map<int64_t, ChannelState *>::iterator it = g_detached.find(handle);
	if ( it == g_detached.end() )
		return false;		// unknown or already-spent handle
	delete s;			// drop the empty placeholder state
	impl_ = it->second;		// take the detached endpoint whole
	g_detached.erase(it);
	return true;
}

int64_t channel::share()
{
	int64_t id = g_share_next++;
	ConnEntry e;
	e.ch = this;			// a borrowed pointer; unshare() removes it
	e.cookie = 0;			// application sets it (madcide: the tier)
	g_shared[id] = e;
	return id;
}

void channel::unshare(int64_t id)
{
	g_shared.erase(id);
}

// WebSocket facet (V6b): upgrade an accepted byte channel to an RFC6455
// message channel, SERVER role — read the HTTP upgrade request (parking, so a
// serve task never blocks the thread), send the 101 accept, and wrap the inner
// socket in the framer. After this readline() returns one text message and
// write() sends one text frame. false (with last_error) on a request that is
// not a WebSocket upgrade.
bool channel::upgrade_websocket()
{
	ChannelState *s = state(impl_);
	if ( s->wsc )
		return true;		// already a WebSocket channel
	if ( !s->channel )
	{
		set_state_error(s, "upgrade_websocket on a channel with no endpoint");
		return false;
	}
	std::string headers, leftover;
	if ( !read_http_headers(s, headers, leftover) )
		return false;
	std::string response, reason;
	if ( !websocket_server_handshake(headers, response, reason) )
	{
		set_state_error(s, "upgrade_websocket: " + reason);
		return false;
	}
	if ( !write_all(*s->channel, response.data(), response.size(),
			&s->last_error) )
	{
		s->failed = true;
		return false;
	}
	std::unique_ptr<WebSocketDataChannel> ws(new WebSocketDataChannel(
		std::move(s->channel), WebSocketDataChannel::Role::server,
		leftover));
	s->wsc = ws.get();
	s->channel = std::move(ws);
	return true;
}

// The CLIENT role twin: send the upgrade request for `resource`, consume and
// validate the server's 101, then speak masked frames. Makes a madc::channel a
// WebSocket client of any RFC6455 server.
bool channel::connect_websocket(const char *resource)
{
	ChannelState *s = state(impl_);
	if ( s->wsc )
		return true;
	if ( !s->channel )
	{
		set_state_error(s, "connect_websocket on a channel with no endpoint");
		return false;
	}
	std::string key;
	std::string request = websocket_client_request(
		resource ? resource : "/", std::string(), key);
	if ( !write_all(*s->channel, request.data(), request.size(),
			&s->last_error) )
	{
		s->failed = true;
		return false;
	}
	std::string headers, leftover;
	if ( !read_http_headers(s, headers, leftover) )
		return false;
	std::string reason;
	if ( !websocket_client_validate(headers, key, reason) )
	{
		set_state_error(s, "connect_websocket: " + reason);
		return false;
	}
	std::unique_ptr<WebSocketDataChannel> ws(new WebSocketDataChannel(
		std::move(s->channel), WebSocketDataChannel::Role::client,
		leftover));
	s->wsc = ws.get();
	s->channel = std::move(ws);
	return true;
}

int64_t channel::read(void *buffer, int64_t capacity)
{
	ChannelState *s = state(impl_);
	if ( !buffer || capacity <= 0 )
		return 0;
	if ( s->wsc )
	{
		set_state_error(s, "websocket channel is message-oriented;"
				   " use readline()");
		return -1;
	}
	if ( !s->pending.empty() )
	{
		std::size_t count = s->pending.size();
		if ( count > static_cast<std::size_t>(capacity) )
			count = static_cast<std::size_t>(capacity);
		std::memcpy(buffer, s->pending.data(), count);
		s->pending.erase(0, count);
		return static_cast<long>(count);
	}
	if ( s->failed )
		return -1;
	if ( !s->channel )
	{
		set_state_error(s, "channel is closed");
		return -1;
	}
	if ( s->eof )
		return 0;
	std::size_t count = 0;
	park_until_readable(s);
	if ( !s->channel->read(buffer, static_cast<std::size_t>(capacity),
			       count, &s->last_error) )
	{
		s->failed = true;
		return -1;
	}
	if ( count == 0 )
		s->eof = true;
	return static_cast<long>(count);
}

bool channel::readline(std::string &out)
{
	ChannelState *s = state(impl_);
	if ( s->wsc )
		return ws_recv(s, out);		// one WebSocket message
	out.clear();
	if ( s->failed )
		return false;
	for ( ;; )
	{
		std::size_t newline = s->pending.find('\n');
		if ( newline != std::string::npos )
		{
			std::size_t length = newline;
			if ( length && s->pending[length - 1] == '\r' )
				--length;
			out.assign(s->pending, 0, length);
			s->pending.erase(0, newline + 1);
			return true;
		}
		if ( !s->channel || s->eof )
			break;
		if ( !fill_pending(s) )
			return false;
	}
	// EOF with no newline: the unterminated tail is still a line.
	if ( s->pending.empty() )
		return false;
	out.swap(s->pending);
	return true;
}

bool channel::readall(std::string &out)
{
	ChannelState *s = state(impl_);
	out.clear();
	if ( s->wsc )
	{
		set_state_error(s, "websocket channel is message-oriented;"
				   " use readline()");
		return false;
	}
	if ( s->failed )
		return false;
	while ( s->channel && !s->eof )
		if ( !fill_pending(s) )
			return false;
	out.swap(s->pending);
	return true;
}

// value-carrier twins (slice V1): delegate to the string implementations —
// one line/payload owner — and retag the carrier as string kind. On a false
// return the carrier is left null (never a stale previous line).
bool channel::readline(value &out)
{
	std::string line;
	bool got = readline(line);
	out = got ? value(line) : value();
	return got;
}

bool channel::readall(value &out)
{
	std::string payload;
	bool got = readall(payload);
	out = got ? value(payload) : value();
	return got;
}

bool channel::write(const char *text)
{
	if ( !text )
		return false;
	return write(text, static_cast<long>(std::strlen(text)));
}

bool channel::write(const char *buffer, int64_t size)
{
	ChannelState *s = state(impl_);
	if ( !s->channel )
	{
		set_state_error(s, "channel is closed");
		return false;
	}
	if ( !buffer || size <= 0 )
		return size <= 0;
	if ( s->wsc )		// message channel: one text frame per write
	{
		std::string frame = s->wsc->encode_text(
			buffer, static_cast<std::size_t>(size));
		if ( !s->wsc->write_raw(frame.data(), frame.size(),
					&s->last_error) )
		{
			s->failed = true;
			return false;
		}
		return true;
	}
	if ( !write_all(*s->channel, buffer, static_cast<std::size_t>(size),
			&s->last_error) )
	{
		s->failed = true;
		return false;
	}
	return true;
}

// Non-const string& matches the eval-family script surface (const-qualified
// script types are still an active front-end track; see <ns_madc>).
bool channel::write(std::string &text)
{
	return write(text.data(), static_cast<long>(text.size()));
}

// value carrier: send the text view (string kind sends its payload
// directly; other scalar kinds render through the one value->text owner).
bool channel::write(value &text)
{
	if ( text.is_string() )
		return write(static_cast<const char *>(text.data()),
			     static_cast<long>(text.size()));
	std::string rendered;
	if ( !ns_common::value_to_string(text, rendered) )
		return false;
	return write(rendered.data(), static_cast<long>(rendered.size()));
}

bool channel::close_write()
{
	ChannelState *s = state(impl_);
	if ( !s->channel )
		return false;
	if ( !s->channel->flush(&s->last_error) )
	{
		s->failed = true;
		return false;
	}
	s->channel->close_write();
	return true;
}

void channel::close()
{
	ChannelState *s = state(impl_);
	if ( s->channel )
	{
		s->channel->close();
		s->exit_status = s->channel->exit_status();
		s->channel.reset();
		s->wsc = nullptr;	// the framer went with the channel
	}
}

int64_t channel::exit_status()
{
	ChannelState *s = state(impl_);
	return s->channel ? s->channel->exit_status() : s->exit_status;
}

bool channel::is_terminal()
{
	ChannelState *s = state(impl_);
	return s->channel && s->channel->is_terminal();
}

// Readiness probes (MT-4b) — the select scan's three-state contract
// (chan_poll_recv's shape): 1 = progress now, 0 = would wait, -1 = dead.
int64_t channel::poll_state()
{
	ChannelState *s = state(impl_);
	if ( !s->pending.empty() )
		return 1;
	if ( s->failed )
		return -1;
	if ( !s->channel || s->eof )
		return -1;
	PollableDataChannel *pollable = pollable_surface(s->channel.get());
	if ( !pollable )
		return 1;	// memory/file reads never block
	return taskio::poll_readable(pollable->read_poll_handle()) ? 1 : 0;
}

int64_t channel::read_wait_handle()
{
	ChannelState *s = state(impl_);
	if ( !s->channel )
		return -1;
	PollableDataChannel *pollable = pollable_surface(s->channel.get());
	return pollable
		? static_cast<int64_t>(pollable->read_poll_handle()) : -1;
}

bool channel::wait_readable()
{
	for ( ;; )
	{
		int64_t st = poll_state();
		if ( st != 0 )
			return st == 1;
		taskio::wait_readable(
			static_cast<intptr_t>(read_wait_handle()));
	}
}

void channel::cancel()
{
	ChannelState *s = state(impl_);
	if ( s->channel )
		s->channel->cancel();
}

// Write `line` to every share()-registered channel except `except_id` (the
// hub fan-out — V6a duplex push). Best-effort: a peer whose write fails
// (dead/closed) is skipped, its own serve task removes it on EOF. A
// cooperative write never yields mid-line, so this whole pass is atomic
// against the parked reader tasks it writes into.
void conn_broadcast(int64_t except_id, const char *line)
{
	if ( !line )
		return;
	for ( std::map<int64_t, ConnEntry>::iterator it = g_shared.begin();
	      it != g_shared.end(); ++it )
	{
		if ( it->first == except_id )
			continue;
		it->second.ch->write(line);
	}
}

// Set / read the opaque per-connection cookie (madcide's permission tier).
// conn_cookie returns -1 for an unknown id (a client that left). These are
// how a serve task reads its own tier and how the owner's clienttier verb
// re-tiers another connection by id.
void conn_set_cookie(int64_t id, int64_t cookie)
{
	std::map<int64_t, ConnEntry>::iterator it = g_shared.find(id);
	if ( it != g_shared.end() )
		it->second.cookie = cookie;
}

int64_t conn_cookie(int64_t id)
{
	std::map<int64_t, ConnEntry>::iterator it = g_shared.find(id);
	return it != g_shared.end() ? it->second.cookie : -1;
}

// Every live connection id, ascending — the roster the seat scans (is there
// already an owner? which ids may an admin re-tier?).
void conn_ids(value &out)
{
	out = value::make_array();
	for ( std::map<int64_t, ConnEntry>::iterator it = g_shared.begin();
	      it != g_shared.end(); ++it )
		out.array().push_back(value((int64_t)it->first));
}

} // namespace madc
