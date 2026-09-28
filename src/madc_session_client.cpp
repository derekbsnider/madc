/* madc_session_client.cpp — the interactive session in its own process (plan
 * §41.9a, D2): the client, and the backend's serve loop.
 *
 * The backend is the running madc, forked through the one spawn owner
 * (Process + child_body, as the madcrun:// Run is), so nothing execs. Its
 * requests and replies are JSON lines on a socketpair, through the one
 * value<->JSON bridge (wt_value_to_json); the program's own stdout and
 * stderr are the Process's output pipe.
 */

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <list>
#include <queue>
#include <iostream>
#include <sstream>
#include <fstream>
#include <memory>
#include <stdint.h>

#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_session.h"
#include "madc_session_client.h"
#include "madcdis/process.h"
#include "madcdis/world_text.h"	// wt_value_to_json / wt_json_to_value
#include "rt/rt_task.h"		// __madc_task_atfork_child
#include "madc_task_io.h"	// taskio::handle_closing

namespace madc {
// src/madc_program.cpp: the one diagnostic-row builder, and the fork child's
// first steps (CLI signal dispositions, stderr onto the output stream).
void diagnostic_rows_from_child(::Program &child, madc::value &out);
#ifndef _WIN32
void run_child_prologue();
#endif
}

#ifndef _WIN32
// The wire's words. Text only on the wire, each converted once at each end
// (enum-over-strings): the request's op, the offer's verdict.
namespace {

// `running` is a reply's only: the backend's notice that an offered entry
// was taken and runs now.
enum class Op : unsigned char { begin, offer, complete, running, unknown };

struct OpRow { const char *name; Op op; };
const OpRow op_rows[] = {
    { "begin", Op::begin }, { "offer", Op::offer }, { "complete", Op::complete },
    { "running", Op::running },
};

const char *op_name(Op op)
{
    for ( const OpRow &r : op_rows )
	if ( r.op == op )
	    return r.name;
    return "unknown";
}

Op op_of(const std::string &name)
{
    for ( const OpRow &r : op_rows )
	if ( name == r.name )
	    return r.op;
    return Op::unknown;
}

struct StateRow { const char *name; InteractiveSession::OfferState state; };
const StateRow state_rows[] = {
    { "taken", InteractiveSession::OfferState::taken },
    { "incomplete", InteractiveSession::OfferState::incomplete },
    { "extendable", InteractiveSession::OfferState::extendable },
};

const char *state_name(InteractiveSession::OfferState s)
{
    for ( const StateRow &r : state_rows )
	if ( r.state == s )
	    return r.name;
    return "taken";
}

InteractiveSession::OfferState state_of(const std::string &name)
{
    for ( const StateRow &r : state_rows )
	if ( name == r.name )
	    return r.state;
    return InteractiveSession::OfferState::taken;
}

// One line out, whole; a peer that went away is an error, never a SIGPIPE
// (the client must outlive its backend).
bool write_line(int fd, const std::string &line)
{
    std::string out = line + "\n";
    size_t done = 0;
    while ( done < out.size() )
    {
	int flags = 0;
#ifdef MSG_NOSIGNAL
	flags = MSG_NOSIGNAL;
#endif
	ssize_t n = ::send(fd, out.data() + done, out.size() - done, flags);
	if ( n < 0 && errno == EINTR )
	    continue;
	if ( n <= 0 )
	    return false;
	done += (size_t)n;
    }
    return true;
}

// The backend: one session, a request per line until the channel closes.
// The program's output is flushed before each reply, so the client reads
// an entry's output before its result.
int serve_session(int fd, std::unique_ptr<Program> prog, const std::string &std_option)
{
    InteractiveSession session(std::move(prog));
    std::ostringstream err;
    session.program().error_stream = &err;
    auto flush_program = [] {
	std::cout.flush();
	std::cerr.flush();
	fflush(stdout);
	fflush(stderr);
    };
    nlohmann::json hello;
    hello["seq"] = 0;
    hello["op"] = op_name(Op::begin);
    hello["ok"] = session.begin(std_option);
    hello["rendered"] = err.str();
    // The standard in force, by its canonical name (the prompt's, D22).
    hello["standard"] = Program::standard_canonical_name(session.program().language_std);
    flush_program();
    if ( !write_line(fd, hello.dump()) || !hello["ok"].get<bool>() )
	return 1;
    std::string buf;
    char chunk[4096];
    for (;;)
    {
	size_t nl;
	while ( (nl = buf.find('\n')) == std::string::npos )
	{
	    ssize_t n = ::read(fd, chunk, sizeof(chunk));
	    if ( n < 0 && errno == EINTR )
		continue;
	    if ( n <= 0 )
		return 0;		// the client closed the channel
	    buf.append(chunk, (size_t)n);
	}
	const std::string line = buf.substr(0, nl);
	buf.erase(0, nl + 1);
	nlohmann::json req = nlohmann::json::parse(line, nullptr, false);
	if ( req.is_discarded() || !req.is_object() )
	    continue;
	const Op op = op_of(req.value("op", std::string()));
	err.str(std::string());
	err.clear();
	nlohmann::json rep;
	rep["seq"] = req.value("seq", 0u);
	rep["op"] = op_name(op);
	switch ( op )
	{
	    case Op::offer:
	    {
		const std::string text = req.value("text", std::string());
		// Once the entry is final, before it renders or runs, the client
		// hears it runs (Jupyter's execute_input): what it prints comes
		// after this line, so a transcript shows the entry first.
		const unsigned seq = req.value("seq", 0u);
		InteractiveSession::TakenHook taken = [&]() {
		    flush_program();
		    nlohmann::json note;
		    note["seq"] = seq;
		    note["op"] = op_name(Op::running);
		    write_line(fd, note.dump());
		};
		InteractiveSession::Offered o;
		if ( req.value("final", false) )
		    o = InteractiveSession::Offered{ InteractiveSession::OfferState::taken,
						     session.submit(text, taken) };
		else
		    o = session.offer(text, taken);
		rep["state"] = state_name(o.state);
		rep["ok"] = o.ok;
		if ( o.state == InteractiveSession::OfferState::taken )
		{
		    rep["shown"] = session.shown();
		    rep["submitted"] = session.submitted();
		    madc::value rows;
		    madc::diagnostic_rows_from_child(session.program(), rows);
		    rep["diagnostics"] = madc::hub::detail::wt_value_to_json(rows);
		}
		break;
	    }
	    case Op::complete:
	    {
		size_t start = 0;
		std::vector<std::string> names =
		    session.complete(req.value("text", std::string()),
				     req.value("caret", (size_t)0), start);
		rep["start"] = start;
		rep["names"] = names;
		break;
	    }
	    case Op::begin:
	    case Op::running:
	    case Op::unknown:
		rep["ok"] = false;
		break;
	}
	rep["rendered"] = err.str();
	flush_program();
	if ( !write_line(fd, rep.dump()) )
	    return 0;
    }
}

} // namespace
#endif

SessionClient::Reply::Reply()
    : kind(Kind::offer), seq(0), state(InteractiveSession::OfferState::taken),
      ok(false), submitted(0), start(0), exit_status(-1)
{
}

SessionClient::SessionClient() : fd(-1), output_done(false), next_seq(1)
{
}

SessionClient::~SessionClient()
{
    stop();
}

bool SessionClient::running() const
{
    return fd >= 0;
}

#ifndef _WIN32

bool SessionClient::start(const std::string &std_opt, const ProgramFactory &make)
{
    stop();
    std_option = std_opt;
    make_program = make;
    error_text.clear();
    standard_name.clear();
    int sv[2];
    if ( ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 )
    {
	error_text = std::string("session: socketpair: ") + strerror(errno);
	return false;
    }
    const int parent_end = sv[0], child_end = sv[1];
    madc::ProcessOptions options;
    options.inherit_stderr = true;	// the prologue moves it onto the output pipe
    const std::string opt = std_option;
    const ProgramFactory factory = make_program;
    options.child_body = [parent_end, child_end, opt, factory]() -> int {
	__madc_task_atfork_child();	// a fork child running madc code
	madc::run_child_prologue();
	::close(parent_end);
	std::unique_ptr<Program> prog(factory ? factory() : std::unique_ptr<Program>(new Program()));
	return serve_session(child_end, std::move(prog), opt);
    };
    process.reset(new madc::Process(madc::DataSource("exec://<madcsession>"), options));
    madc::error perr;
    if ( !process->start(&perr) )
    {
	error_text = "session: the backend did not start";
	process.reset();
	::close(parent_end);
	::close(child_end);
	return false;
    }
    ::close(child_end);
    fd = parent_end;
    inbuf.clear();
    output_done = false;
    next_seq = 1;
    // The backend's first line says whether its session began.
    Reply hello;
    std::string output;
    for (;;)
    {
	std::string line;
	if ( take_line(line) )
	{
	    nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
	    bool ok = !j.is_discarded() && j.value("ok", false);
	    if ( ok )
		standard_name = j.value("standard", std::string());
	    if ( !ok )
	    {
		error_text = j.is_discarded() ? std::string("session: no greeting")
					      : j.value("rendered", std::string());
		while ( !error_text.empty() && error_text[error_text.size() - 1] == '\n' )
		    error_text.erase(error_text.size() - 1);	// a message, not a rendered block
		stop();
	    }
	    return ok;
	}
	struct pollfd p = { fd, POLLIN, 0 };
	int r = ::poll(&p, 1, -1);
	if ( r < 0 && errno == EINTR )
	    continue;
	char chunk[4096];
	ssize_t n = r > 0 ? ::read(fd, chunk, sizeof(chunk)) : -1;
	if ( n <= 0 )
	{
	    error_text = "session: the backend ended before it began";
	    stopped(hello, output);
	    return false;
	}
	inbuf.append(chunk, (size_t)n);
    }
}

// A task parked on the streams (a select on the session's case) wakes and
// re-reads them: a restart replaces both, and epoll forgets a closed one.
void SessionClient::release_waiters() const
{
    std::vector<madc::poll_handle> hs;
    wait_handles(hs);
    for ( const madc::poll_handle &h : hs )
	madc::taskio::handle_closing(h.value, h.kind);
}

void SessionClient::stop()
{
    release_waiters();
    if ( fd >= 0 )
    {
	::close(fd);			// the backend's read sees EOF and returns
	fd = -1;
    }
    if ( process )
    {
	process->wait_or_kill(2000);
	process.reset();
    }
    inbuf.clear();
}

bool SessionClient::send(const std::string &line)
{
    return fd >= 0 && write_line(fd, line);
}

bool SessionClient::take_line(std::string &line)
{
    size_t nl = inbuf.find('\n');
    if ( nl == std::string::npos )
	return false;
    line = inbuf.substr(0, nl);
    inbuf.erase(0, nl + 1);
    return true;
}

// What the program printed, waiting up to timeout_ms for the first byte.
void SessionClient::read_output(std::string &output, int timeout_ms)
{
    if ( !process || output_done )
	return;
    madc::DataChannel &out = process->stdout_channel();
    madc::PollableDataChannel *pc = madc::pollable_surface(&out);
    intptr_t h = pc ? pc->read_poll_handle() : -1;
    if ( h < 0 )
	return;
    for (;;)
    {
	struct pollfd p = { (int)h, POLLIN, 0 };
	int r = ::poll(&p, 1, timeout_ms);
	if ( r < 0 && errno == EINTR )
	    continue;
	if ( r <= 0 || !(p.revents & (POLLIN | POLLHUP)) )
	    return;
	char chunk[4096];
	size_t got = 0;
	if ( !out.read(chunk, sizeof(chunk), got) || got == 0 )
	{
	    output_done = true;		// its end: never readable again
	    return;
	}
	output.append(chunk, got);
	timeout_ms = 0;			// then only what is already there
    }
}

// The backend ended: what it printed last, then its status.
int SessionClient::stopped(Reply &reply, std::string &output)
{
    read_output(output, 0);
    release_waiters();
    if ( fd >= 0 )
    {
	::close(fd);
	fd = -1;
    }
    reply = Reply();
    reply.kind = Reply::Kind::stopped;
    if ( process )
    {
	process->wait();
	reply.exit_status = process->exit_status();
	process.reset();
    }
    inbuf.clear();
    return -1;
}

unsigned SessionClient::offer(const std::string &text, bool final)
{
    nlohmann::json req;
    const unsigned seq = next_seq++;
    req["seq"] = seq;
    req["op"] = op_name(Op::offer);
    req["text"] = text;
    req["final"] = final;
    return send(req.dump()) ? seq : 0;
}

unsigned SessionClient::complete(const std::string &text, size_t caret)
{
    nlohmann::json req;
    const unsigned seq = next_seq++;
    req["seq"] = seq;
    req["op"] = op_name(Op::complete);
    req["text"] = text;
    req["caret"] = caret;
    return send(req.dump()) ? seq : 0;
}

bool SessionClient::input(const std::string &text)
{
    if ( fd < 0 || !process )
	return false;
    madc::DataChannel &in = process->stdin_channel();
    size_t done = 0;
    while ( done < text.size() )
    {
	size_t n = 0;
	if ( !in.write(text.data() + done, text.size() - done, n) || n == 0 )
	    return false;
	done += n;
    }
    return true;
}

void SessionClient::take_output(std::string &output)
{
    read_output(output, 0);
}

void SessionClient::wait_handles(std::vector<madc::poll_handle> &out) const
{
    out.clear();
    if ( fd < 0 )
	return;
    madc::poll_handle socket_end = { fd, madc::poll_handle_kind::descriptor };
    out.push_back(socket_end);
    madc::PollableDataChannel *pc =
	process && !output_done ? madc::pollable_surface(&process->stdout_channel()) : NULL;
    if ( pc && pc->read_poll_handle() >= 0 )
    {
	madc::poll_handle pipe_end = { pc->read_poll_handle(), pc->read_poll_kind() };
	out.push_back(pipe_end);
    }
}

int SessionClient::pending() const
{
    if ( fd < 0 )
	return -1;
    if ( inbuf.find('\n') != std::string::npos )
	return 1;
    std::vector<madc::poll_handle> hs;
    wait_handles(hs);
    for ( const madc::poll_handle &h : hs )
    {
	struct pollfd p = { (int)h.value, POLLIN, 0 };
	if ( ::poll(&p, 1, 0) > 0 )
	    return 1;
    }
    return 0;
}

int SessionClient::poll(Reply &reply, std::string &output, int timeout_ms)
{
    if ( fd < 0 )
	return stopped(reply, output);
    madc::PollableDataChannel *pc =
	process && !output_done ? madc::pollable_surface(&process->stdout_channel()) : NULL;
    const int out_fd = pc ? (int)pc->read_poll_handle() : -1;
    std::string line;
    while ( !take_line(line) )
    {
	struct pollfd p[2] = { { fd, POLLIN, 0 }, { out_fd, POLLIN, 0 } };
	int r = ::poll(p, out_fd >= 0 ? 2 : 1, timeout_ms);
	if ( r < 0 && errno == EINTR )
	    continue;
	if ( r == 0 )
	    return 0;
	if ( r < 0 )
	    return stopped(reply, output);
	// The socket first: the backend writes a running notice before the
	// entry prints, so once a line is whole, its op decides where the
	// output waiting in the pipe belongs (below). The pipe is drained
	// only while no line is, so a full pipe never stalls the backend.
	if ( p[0].revents & (POLLIN | POLLHUP | POLLERR) )
	{
	    char chunk[4096];
	    ssize_t n = ::read(fd, chunk, sizeof(chunk));
	    if ( n < 0 && errno == EINTR )
		continue;
	    if ( n <= 0 )
		return stopped(reply, output);
	    inbuf.append(chunk, (size_t)n);
	    if ( inbuf.find('\n') != std::string::npos )
		continue;
	}
	if ( out_fd >= 0 && (p[1].revents & (POLLIN | POLLHUP)) )
	    read_output(output, 0);
    }
    nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
    const Op op = j.is_discarded() || !j.is_object()
	? Op::unknown : op_of(j.value("op", std::string()));
    // The backend flushed an entry's output before its result: take it
    // first, so the output comes before the result. A running notice
    // comes before its entry's output (nothing prints between the last
    // reply's flush and the notice), so what is there waits for the next.
    if ( op != Op::running )
	read_output(output, 0);
    reply = Reply();
    if ( j.is_discarded() || !j.is_object() )
	return 0;
    reply.seq = j.value("seq", 0u);
    reply.rendered = j.value("rendered", std::string());
    reply.ok = j.value("ok", false);
    switch ( op )
    {
	case Op::running:
	    reply.kind = Reply::Kind::running;
	    break;
	case Op::offer:
	    reply.kind = Reply::Kind::offer;
	    reply.state = state_of(j.value("state", std::string()));
	    reply.shown = j.value("shown", std::string());
	    reply.submitted = j.value("submitted", 0u);
	    if ( j.contains("diagnostics") )
		reply.diagnostics = madc::hub::detail::wt_json_to_value(j["diagnostics"]);
	    break;
	case Op::complete:
	    reply.kind = Reply::Kind::complete;
	    reply.start = j.value("start", (size_t)0);
	    reply.names = j.value("names", std::vector<std::string>());
	    break;
	case Op::begin:
	case Op::unknown:
	    break;
    }
    return 1;
}

#else	// _WIN32: no fork (plan §41.9a names a child of self as the later lane)

bool SessionClient::start(const std::string &std_opt, const ProgramFactory &make)
{
    std_option = std_opt;
    make_program = make;
    error_text = "session: the backend process is POSIX-only for now (plan 41.9a)";
    return false;
}

void SessionClient::stop()
{
}

bool SessionClient::send(const std::string &)
{
    return false;
}

bool SessionClient::take_line(std::string &)
{
    return false;
}

void SessionClient::read_output(std::string &, int)
{
}

void SessionClient::release_waiters() const
{
}

int SessionClient::stopped(Reply &reply, std::string &)
{
    reply = Reply();
    reply.kind = Reply::Kind::stopped;
    return -1;
}

unsigned SessionClient::offer(const std::string &, bool)
{
    return 0;
}

bool SessionClient::input(const std::string &)
{
    return false;
}

void SessionClient::take_output(std::string &)
{
}

void SessionClient::wait_handles(std::vector<madc::poll_handle> &out) const
{
    out.clear();
}

int SessionClient::pending() const
{
    return -1;
}

unsigned SessionClient::complete(const std::string &, size_t)
{
    return 0;
}

int SessionClient::poll(Reply &reply, std::string &output, int)
{
    return stopped(reply, output);
}

#endif

bool SessionClient::restart()
{
    return start(std_option, make_program);
}

int SessionClient::offer_wait(const std::string &text, bool final, Reply &reply,
			      std::string &output, int timeout_ms)
{
    const unsigned seq = offer(text, final);
    if ( !seq )
	return stopped(reply, output);
    for (;;)
    {
	int r = poll(reply, output, timeout_ms);
	if ( r <= 0 || (reply.seq == seq && reply.kind != Reply::Kind::running) )
	    return r;
    }
}

int SessionClient::complete_wait(const std::string &text, size_t caret,
				 Reply &reply, int timeout_ms)
{
    std::string output;
    const unsigned seq = complete(text, caret);
    if ( !seq )
	return stopped(reply, output);
    for (;;)
    {
	int r = poll(reply, output, timeout_ms);
	if ( r <= 0 || (reply.seq == seq && reply.kind != Reply::Kind::running) )
	    return r;
    }
}
