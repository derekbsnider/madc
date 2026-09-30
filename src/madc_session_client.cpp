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
#include <cctype>
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
void run_child_prologue(bool merge_stderr);
#endif
}

#ifndef _WIN32
// The wire's words. Text only on the wire, each converted once at each end
// (enum-over-strings): the request's op, the offer's verdict.
namespace {

// `running` is a reply's only: the backend's notice that an offered entry
// was taken and runs now. `load` / `run` are the cores of %load / %run (D25):
// a program file into the session, then its main (`madc -i file`).
// `continues` is D11's question: does a line continue an if that ended an
// entry (its first word is the session's `else`). `bindings`: the names the
// session defined, as rows (InteractiveSession::bindings, %whos's).
enum class Op : unsigned char { begin, offer, complete, running, load, run, continues, bindings, unknown };

struct OpRow { const char *name; Op op; };
const OpRow op_rows[] = {
    { "begin", Op::begin }, { "offer", Op::offer }, { "complete", Op::complete },
    { "running", Op::running }, { "load", Op::load }, { "run", Op::run },
    { "continues", Op::continues }, { "bindings", Op::bindings },
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

// A reply's diagnostic rows: what the unit it answers recorded (a taken
// entry's, a load's, a run's), as parse_check shapes them, so a client can
// list them (madcide's Problems) as well as print the rendered text.
void attach_diagnostics(nlohmann::json &rep, InteractiveSession &session)
{
    madc::value rows;
    madc::diagnostic_rows_from_child(session.program(), rows);
    rep["diagnostics"] = madc::hub::detail::wt_value_to_json(rows);
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
		    attach_diagnostics(rep, session);
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
	    case Op::load:
		// With text, the text is the unit and path names it (an
		// editor's buffer); without, the file at path.
		if ( req.contains("text") )
		    rep["ok"] = session.load_text(req.value("text", std::string()),
						  req.value("path", std::string()));
		else
		    rep["ok"] = session.load(req.value("path", std::string()));
		attach_diagnostics(rep, session);
		break;
	    case Op::continues:
		rep["continues"] = session.continues_if(req.value("text", std::string()));
		break;
	    case Op::bindings:
	    {
		madc::value rows;
		session.bindings(rows);
		rep["rows"] = madc::hub::detail::wt_value_to_json(rows);
		rep["ok"] = true;
		break;
	    }
	    case Op::run:
	    {
		// main(argc, argv) at the entry boundary: the path, then the
		// program's arguments. Its status is not the backend's.
		std::vector<std::string> args =
		    req.value("argv", std::vector<std::string>());
		std::vector<char *> argv;
		for ( std::string &a : args )
		    argv.push_back(&a[0]);
		argv.push_back(NULL);
		int status = 0;
		rep["ok"] = session.run_main((int)args.size(), argv.data(), &status);
		rep["status"] = status;
		attach_diagnostics(rep, session);
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
      ok(false), submitted(0), start(0), status(0), continues(false),
      exit_status(-1), signal(0)
{
}

SessionClient::SessionClient()
    : fd(-1), output_done(false), next_seq(1), inherit_stdio(false)
{
}

void SessionClient::set_inherit_stdio(bool on)
{
    inherit_stdio = on;
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
    // Piped: stderr joins the output pipe (the prologue moves it). On the
    // host's terminal: all three are the host's, and stderr stays stderr.
    options.inherit_stderr = true;
    options.inherit_stdin = inherit_stdio;
    options.inherit_stdout = inherit_stdio;
    const std::string opt = std_option;
    const ProgramFactory factory = make_program;
    const bool on_terminal = inherit_stdio;
    options.child_body = [parent_end, child_end, opt, factory, on_terminal]() -> int {
	__madc_task_atfork_child();	// a fork child running madc code
	madc::run_child_prologue(!on_terminal);
	// The host reads the same stdin unbuffered, so an entry's scanf and
	// the host's next line split it as one FILE would (41.9a slice 4).
	if ( on_terminal )
	    setvbuf(stdin, NULL, _IONBF, 0);
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
	reply.signal = process->term_signal();
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

unsigned SessionClient::load(const std::string &path)
{
    nlohmann::json req;
    const unsigned seq = next_seq++;
    req["seq"] = seq;
    req["op"] = op_name(Op::load);
    req["path"] = path;
    return send(req.dump()) ? seq : 0;
}

unsigned SessionClient::load_text(const std::string &text, const std::string &path)
{
    nlohmann::json req;
    const unsigned seq = next_seq++;
    req["seq"] = seq;
    req["op"] = op_name(Op::load);
    req["path"] = path;
    req["text"] = text;
    return send(req.dump()) ? seq : 0;
}

unsigned SessionClient::continues(const std::string &line)
{
    nlohmann::json req;
    const unsigned seq = next_seq++;
    req["seq"] = seq;
    req["op"] = op_name(Op::continues);
    req["text"] = line;
    return send(req.dump()) ? seq : 0;
}

unsigned SessionClient::bindings()
{
    nlohmann::json req;
    const unsigned seq = next_seq++;
    req["seq"] = seq;
    req["op"] = op_name(Op::bindings);
    return send(req.dump()) ? seq : 0;
}

unsigned SessionClient::run(const std::vector<std::string> &argv)
{
    nlohmann::json req;
    const unsigned seq = next_seq++;
    req["seq"] = seq;
    req["op"] = op_name(Op::run);
    req["argv"] = argv;
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
    // A taken entry's, a load's and a run's reply carry their rows.
    if ( j.contains("diagnostics") )
	reply.diagnostics = madc::hub::detail::wt_json_to_value(j["diagnostics"]);
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
	    break;
	case Op::complete:
	    reply.kind = Reply::Kind::complete;
	    reply.start = j.value("start", (size_t)0);
	    reply.names = j.value("names", std::vector<std::string>());
	    break;
	case Op::load:
	    reply.kind = Reply::Kind::load;
	    break;
	case Op::run:
	    reply.kind = Reply::Kind::run;
	    reply.status = j.value("status", 0);
	    break;
	case Op::continues:
	    reply.kind = Reply::Kind::continues;
	    reply.continues = j.value("continues", false);
	    break;
	case Op::bindings:
	    reply.kind = Reply::Kind::bindings;
	    if ( j.contains("rows") )
		reply.rows = madc::hub::detail::wt_json_to_value(j["rows"]);
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

unsigned SessionClient::load(const std::string &)
{
    return 0;
}

unsigned SessionClient::load_text(const std::string &, const std::string &)
{
    return 0;
}

unsigned SessionClient::run(const std::vector<std::string> &)
{
    return 0;
}

unsigned SessionClient::continues(const std::string &)
{
    return 0;
}

unsigned SessionClient::bindings()
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

bool SessionClient::restart(const std::string &std_opt)
{
    return start(std_opt, make_program);
}

BackendSession::BackendSession(SessionClient &c, std::ostream &e, std::ostream *o)
    : client(c), err(e), out(o), has_ended(false), end_status(0)
{
}

std::string BackendSession::standard_name()
{
    return client.standard();
}

// What the signal that ended a backend is called ("segmentation fault").
static std::string signal_words(int sig)
{
#ifndef _WIN32
    const char *d = strsignal(sig);
    std::string w = d ? d : "";
    if ( !w.empty() )
	w[0] = (char)tolower((unsigned char)w[0]);
    return w;
#else
    (void)sig;
    return std::string();
#endif
}

bool BackendSession::settle(int rc, const SessionClient::Reply &reply,
			    const std::string &output)
{
    if ( out && !output.empty() )
	*out << output << std::flush;
    if ( rc >= 0 )
    {
	if ( !reply.rendered.empty() )
	    err << reply.rendered << std::flush;
	return true;
    }
    if ( reply.signal == 0 && reply.exit_status >= 0 )
    {
	// exit(n) in an entry ends the process, as in this process (§41.5a).
	has_ended = true;
	end_status = reply.exit_status;
	return false;
    }
    err << "madc: the session stopped (signal " << reply.signal;
    const std::string words = signal_words(reply.signal);
    if ( !words.empty() )
	err << ", " << words;
    err << "); a new one started" << std::endl;
    if ( !client.restart() )
    {
	err << "madc: " << client.last_error() << std::endl;
	has_ended = true;
	end_status = 1;
    }
    return false;
}

namespace {
// While the backend runs an entry, an interrupt is the backend's (D8's
// interim: it stops, and a fresh one starts); the host ignores it, as a
// shell leaves the interrupt to its foreground job.
struct HostIgnoresInterrupt
{
#ifndef _WIN32
    struct sigaction saved;
    HostIgnoresInterrupt()
    {
	struct sigaction ign;
	memset(&ign, 0, sizeof(ign));
	ign.sa_handler = SIG_IGN;
	sigemptyset(&ign.sa_mask);
	sigaction(SIGINT, &ign, &saved);
    }
    ~HostIgnoresInterrupt() { sigaction(SIGINT, &saved, NULL); }
#endif
};
}

bool BackendSession::submit(const std::string &text, const TakenHook &taken)
{
    HostIgnoresInterrupt quiet;
    SessionClient::Reply r;
    std::string output;
    int rc = client.offer_wait(text, true, r, output, -1, taken);
    shown_text = rc > 0 ? r.shown : std::string();
    return settle(rc, r, output) && r.ok;
}

ReplSession::Offered BackendSession::offer(const std::string &text,
					   const TakenHook &taken)
{
    HostIgnoresInterrupt quiet;
    SessionClient::Reply r;
    std::string output;
    int rc = client.offer_wait(text, false, r, output, -1, taken);
    shown_text = rc > 0 ? r.shown : std::string();
    if ( !settle(rc, r, output) )
	return Offered{ OfferState::taken, false };	// it ran, and stopped
    return Offered{ r.state, r.ok };
}

std::vector<std::string> BackendSession::complete(const std::string &text,
						  size_t caret, size_t &start)
{
    SessionClient::Reply r;
    int rc = client.complete_wait(text, caret, r);
    if ( !settle(rc, r, std::string()) )
    {
	start = caret;
	return std::vector<std::string>();
    }
    start = r.start;
    return r.names;
}

bool BackendSession::continues_if(const std::string &line)
{
    SessionClient::Reply r;
    int rc = client.continues_wait(line, r);
    return settle(rc, r, std::string()) && r.continues;
}

bool BackendSession::run_file(int argc, char **argv)
{
    if ( argc < 1 )
	return false;
    SessionClient::Reply r;
    std::string output;
    int rc = client.load_wait(argv[0], r, output);
    if ( !settle(rc, r, output) || !r.ok )
	return false;
    std::vector<std::string> args(argv, argv + argc);
    output.clear();
    HostIgnoresInterrupt quiet;
    rc = client.run_wait(args, r, output);	// no main: ok false, nothing ran
    settle(rc, r, output);
    return true;
}

bool BackendSession::ended(int &status) const
{
    if ( has_ended )
	status = end_status;
    return has_ended;
}

// A synchronous caller's wait for request `seq`'s reply: its running notice
// calls `taken`; replies to earlier requests are dropped, their output kept.
int SessionClient::wait_reply(unsigned seq, Reply &reply, std::string &output,
			      int timeout_ms, const InteractiveSession::TakenHook &taken)
{
    if ( !seq )
	return stopped(reply, output);
    for (;;)
    {
	int r = poll(reply, output, timeout_ms);
	if ( r <= 0 || reply.seq != seq )
	{
	    if ( r <= 0 )
		return r;
	    continue;
	}
	if ( reply.kind != Reply::Kind::running )
	    return r;
	if ( taken )
	    taken();
    }
}

int SessionClient::offer_wait(const std::string &text, bool final, Reply &reply,
			      std::string &output, int timeout_ms,
			      const InteractiveSession::TakenHook &taken)
{
    return wait_reply(offer(text, final), reply, output, timeout_ms, taken);
}

int SessionClient::complete_wait(const std::string &text, size_t caret,
				 Reply &reply, int timeout_ms)
{
    std::string output;
    return wait_reply(complete(text, caret), reply, output, timeout_ms,
		      InteractiveSession::TakenHook());
}

int SessionClient::load_wait(const std::string &path, Reply &reply,
			     std::string &output)
{
    return wait_reply(load(path), reply, output, -1, InteractiveSession::TakenHook());
}

int SessionClient::load_wait_text(const std::string &text, const std::string &path,
				  Reply &reply, std::string &output)
{
    return wait_reply(load_text(text, path), reply, output, -1,
		      InteractiveSession::TakenHook());
}

int SessionClient::run_wait(const std::vector<std::string> &argv, Reply &reply,
			    std::string &output)
{
    return wait_reply(run(argv), reply, output, -1, InteractiveSession::TakenHook());
}

int SessionClient::continues_wait(const std::string &line, Reply &reply)
{
    std::string output;
    return wait_reply(continues(line), reply, output, -1,
		      InteractiveSession::TakenHook());
}

int SessionClient::bindings_wait(Reply &reply, int timeout_ms)
{
    std::string output;
    return wait_reply(bindings(), reply, output, timeout_ms,
		      InteractiveSession::TakenHook());
}
