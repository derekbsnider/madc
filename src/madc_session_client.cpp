/* madc_session_client.cpp — the interactive session in its own process (plan
 * §41.9a, D2): the client, and the backend's serve loop.
 *
 * The backend is the running madc, forked through the one spawn owner
 * (Process + child_body, as the madcrun:// Run is), so nothing execs. Its
 * requests and replies are JSON lines on a socketpair, through the one
 * value<->JSON bridge (wt_value_to_json); the program's own stdout and
 * stderr are the Process's output pipe.
 *
 * Windows has no fork: the backend is a run child of self
 * (include/madc_run_child.h, kind `session`), and the requests ride a
 * loopback connection the child makes to the client's listener, checked by
 * a token — Jupyter's kernel transport (ports plus a key). Both ends speak
 * through a DataChannel, so the protocol below is one text for both.
 */

#ifdef _WIN32
#define _CRT_RAND_S		// rand_s: the session token (the OS generator)
#endif
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
#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <io.h>			// _dup2: the backend's stderr onto its output pipe
#endif
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
#include "madc_session_interrupt.h"	// D8: the interrupt between processes
#include "madc_cir.h"		// madc_session_interrupt_reset
#include "madcdis/world_text.h"	// wt_value_to_json / wt_json_to_value
#include "rt/rt_task.h"		// __madc_task_atfork_child
#include "madc_task_io.h"	// taskio::handle_closing, taskio::poll_readable
#include "madc_datachannel_internal.h"	// socket_channel_over: the socketpair's ends
#include "madc_run_child.h"	// Windows: the backend is a run child of self

namespace madc {
// src/madc_program.cpp: the one diagnostic-row builder, and the fork child's
// first steps (CLI signal dispositions, stderr onto the output stream).
void diagnostic_rows_from_child(::Program &child, madc::value &out);
#ifndef _WIN32
void run_child_prologue(bool merge_stderr);
#endif
}

// The wire's words. Text only on the wire, each converted once at each end
// (enum-over-strings): the request's op, the offer's verdict.
namespace {

// `running` is a reply's only: the backend's notice that an offered entry
// was taken and runs now. `load` / `run` are the cores of %load / %run (D25):
// a program file into the session, then its main (`madc -i file`).
// `continues` is D11's question: does a line continue an if that ended an
// entry (its first word is the session's `else`). `bindings`: the names the
// session defined, as rows (InteractiveSession::bindings, %whos's). `build`:
// %build's (InteractiveSession::build_file), the host's text of a file.
enum class Op : unsigned char { begin, offer, complete, running, load, run, continues, bindings, build, unknown };

struct OpRow { const char *name; Op op; };
const OpRow op_rows[] = {
    { "begin", Op::begin }, { "offer", Op::offer }, { "complete", Op::complete },
    { "running", Op::running }, { "load", Op::load }, { "run", Op::run },
    { "continues", Op::continues }, { "bindings", Op::bindings },
    { "build", Op::build },
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

// A taken command's ask of its host (<bits/session_enums>' session_payload):
// the wire's word for each; none rides no field.
struct PayloadRow { const char *name; madc::session_payload payload; };
const PayloadRow payload_rows[] = {
    { "load", madc::session_payload::load },
    { "run", madc::session_payload::run },
    { "run_here", madc::session_payload::run_here },
    { "build", madc::session_payload::build },
    { "call", madc::session_payload::call },
    { "open", madc::session_payload::open },
    { "quit", madc::session_payload::quit },
};

const char *payload_name(madc::session_payload p)
{
    for ( const PayloadRow &r : payload_rows )
	if ( r.payload == p )
	    return r.name;
    return "";
}

madc::session_payload payload_of(const std::string &name)
{
    for ( const PayloadRow &r : payload_rows )
	if ( name == r.name )
	    return r.payload;
    return madc::session_payload::none;
}

// One line out, whole; a peer that went away is an error, never a SIGPIPE
// (the client must outlive its backend — the socket channel's write).
bool write_line(madc::DataChannel &wire, const std::string &line)
{
    const std::string out = line + "\n";
    return madc::write_all(wire, out.data(), out.size());
}

// One read of the wire: false at its end (the peer closed) or an error.
bool read_wire(madc::DataChannel &wire, std::string &into)
{
    char chunk[4096];
    size_t got = 0;
    if ( !wire.read(chunk, sizeof(chunk), got) || got == 0 )
	return false;
    into.append(chunk, got);
    return true;
}

// A channel's read side as a waitable handle; value -1 when it has none.
madc::poll_handle wait_handle_of(madc::DataChannel *c)
{
    madc::PollableDataChannel *pc = c ? madc::pollable_surface(c) : NULL;
    madc::poll_handle h = { -1, madc::poll_handle_kind::descriptor };
    if ( pc && pc->read_poll_handle() >= 0 )
    {
	h.value = pc->read_poll_handle();
	h.kind = pc->read_poll_kind();
    }
    return h;
}

// Wait up to timeout_ms (-1: no limit) until one of `hs` reads readable
// (data, its end, or an error the read surfaces). The mask of the readable
// ones; 0 when the time ran out, -1 when the wait itself failed. POSIX
// blocks in poll(). Windows has no one wait over a socket and a pipe, so it
// probes each with the reactor's own probe (taskio::poll_readable) on a
// short cadence.
int wait_any_readable(const std::vector<madc::poll_handle> &hs, int timeout_ms)
{
#ifndef _WIN32
    std::vector<struct pollfd> p;
    for ( const madc::poll_handle &h : hs )
    {
	struct pollfd one = { (int)h.value, POLLIN, 0 };
	p.push_back(one);
    }
    for (;;)
    {
	int r = ::poll(p.data(), p.size(), timeout_ms);
	if ( r < 0 && errno == EINTR )
	    continue;
	if ( r <= 0 )
	    return r;
	int mask = 0;
	for ( size_t i = 0; i < p.size(); ++i )
	    if ( p[i].revents & (POLLIN | POLLHUP | POLLERR) )
		mask |= 1 << i;
	return mask;
    }
#else
    const std::chrono::steady_clock::time_point deadline =
	std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
	int mask = 0;
	for ( size_t i = 0; i < hs.size(); ++i )
	    if ( madc::taskio::poll_readable(hs[i].value, hs[i].kind) )
		mask |= 1 << i;
	if ( mask != 0 || timeout_ms == 0
	  || (timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline) )
	    return mask;
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#endif
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
int serve_session(madc::DataChannel &wire, std::unique_ptr<Program> prog,
		  const std::string &std_option)
{
    InteractiveSession session(std::move(prog));
    // A file command's payload is the client's to honor: it reads the file
    // (an IDE's buffer is its live text) and, for %run's fresh session,
    // restarts this backend first, as F5 does.
    session.host_honors_payloads(true);
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
    if ( !write_line(wire, hello.dump()) || !hello["ok"].get<bool>() )
	return 1;
    std::string buf;
    for (;;)
    {
	size_t nl;
	while ( (nl = buf.find('\n')) == std::string::npos )
	    if ( !read_wire(wire, buf) )
		return 0;		// the client closed the channel
	const std::string line = buf.substr(0, nl);
	buf.erase(0, nl + 1);
	nlohmann::json req = nlohmann::json::parse(line, nullptr, false);
	if ( req.is_discarded() || !req.is_object() )
	    continue;
	const Op op = op_of(req.value("op", std::string()));
	// An interrupt from before this request (one sent while the backend
	// waited) is none; one from here on waits for the entry's first poll.
	madc_session_interrupt_reset();
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
		    write_line(wire, note.dump());
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
		    if ( session.payload() != madc::session_payload::none )
		    {
			rep["payload"] = payload_name(session.payload());
			rep["argv"] = session.payload_argv();
		    }
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
		rep["main"] = session.loaded_main();	// the file's main, to run
		attach_diagnostics(rep, session);
		break;
	    case Op::continues:
		rep["continues"] = session.continues_if(req.value("text", std::string()));
		break;
	    case Op::build:
		// %build's: the host's text of the file (an editor's buffer),
		// or, without, the file at path.
		rep["ok"] = session.build_file(req.value("path", std::string()),
					       req.value("text", std::string()),
					       req.value("out", std::string()));
		rep["shown"] = session.shown();
		attach_diagnostics(rep, session);
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
		// program's arguments. Its status is not the backend's. With
		// `call` (%call's, cling's .x), argv is the file and the call's
		// text, and call_file makes the call; its value shows.
		std::vector<std::string> args =
		    req.value("argv", std::vector<std::string>());
		int status = 0;
		if ( req.value("call", false) )
		{
		    rep["ok"] = args.size() == 2
				&& session.call_file(args[0], args[1], &status);
		    rep["shown"] = session.shown();
		}
		else
		{
		    std::vector<char *> argv;
		    for ( std::string &a : args )
			argv.push_back(&a[0]);
		    argv.push_back(NULL);
		    rep["ok"] = session.run_main((int)args.size(), argv.data(), &status);
		}
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
	if ( !write_line(wire, rep.dump()) )
	    return 0;
    }
}

} // namespace

SessionClient::Reply::Reply()
    : kind(Kind::offer), seq(0), state(InteractiveSession::OfferState::taken),
      ok(false), submitted(0), payload(::madc::session_payload::none),
      start(0), defines_main(false), status(0), continues(false),
      exit_status(-1), signal(0)
{
}

SessionClient::SessionClient()
    : output_done(false), next_seq(1), answered_seq(0), inherit_stdio(false)
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
    return wire != nullptr;
}

// The backend answers its requests in order, one reply each.
bool SessionClient::busy() const
{
    return wire && answered_seq + 1 < next_seq;
}

bool SessionClient::start(const std::string &std_opt, const ProgramFactory &make)
{
    stop();
    std_option = std_opt;
    make_program = make;
    error_text.clear();
    standard_name.clear();
    inbuf.clear();
    output_done = false;
    next_seq = 1;
    answered_seq = 0;
    // The spawn may leave the greeting's first bytes in inbuf (they can
    // arrive with the connection's token).
    if ( !spawn_backend() )
	return false;
    // The backend's first line says whether its session began.
    Reply hello;
    std::string output;
    std::vector<madc::poll_handle> hs(1, wait_handle_of(wire.get()));
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
	if ( wait_any_readable(hs, -1) < 0 || !read_wire(*wire, inbuf) )
	{
	    error_text = "session: the backend ended before it began";
	    stopped(hello, output);
	    return false;
	}
    }
}

#ifndef _WIN32

// The backend: this process forked through the one spawn owner; the
// requests ride a socketpair.
bool SessionClient::spawn_backend()
{
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
	madc::session_interrupt_arm_backend(std::string());	// SIGINT (D8)
	std::unique_ptr<Program> prog(factory ? factory() : std::unique_ptr<Program>(new Program()));
	std::unique_ptr<madc::DataChannel> child_wire =
	    madc::detail::socket_channel_over(child_end, "session");
	return serve_session(*child_wire, std::move(prog), opt);
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
    wire = madc::detail::socket_channel_over(parent_end, "session");
    return true;
}

#else

namespace {

// The loopback connection's key: 128 bits from the OS generator, as hex.
std::string session_token()
{
    std::string t;
    for ( int i = 0; i < 4; ++i )
    {
	unsigned int r = 0;
	if ( rand_s(&r) != 0 )
	    return std::string();
	char hex[9];
	snprintf(hex, sizeof(hex), "%08x", r);
	t += hex;
    }
    return t;
}

} // namespace

// The backend: a run child of self (no fork). It connects to this
// listener and sends the token first; the first connection that does is
// the wire. A child that dies before it connects ends its output pipe.
bool SessionClient::spawn_backend()
{
    if ( make_program )
    {
	error_text = "session: a configured Program cannot cross to a Windows"
		     " backend (it builds its own)";
	return false;
    }
    const std::string token = session_token();
    madc::error lerr;
    std::unique_ptr<madc::DataChannel> listener =
	madc::DataChannelRegistry::instance().open(
	    madc::DataSource("listen://127.0.0.1:0"), madc::ChannelOpenMode::read_write, &lerr);
    madc::AcceptorDataChannel *acceptor =
	listener ? madc::acceptor_surface(listener.get()) : NULL;
    if ( token.empty() || !acceptor )
    {
	error_text = "session: no loopback listener for the backend";
	return false;
    }
    madc::ProcessOptions options;
    // Piped: the child moves its stderr onto the output pipe. On the host's
    // terminal: all three are the host's.
    options.inherit_stderr = true;
    options.inherit_stdin = inherit_stdio;
    options.inherit_stdout = inherit_stdio;
    if ( !interruptor.open(token) )	// D8: the event the child waits on
    {
	error_text = "session: no interrupt event for the backend";
	return false;
    }
    process = madc_run_child_process(
	rckSession, acceptor->local_endpoint() + " " + token + " "
		    + (inherit_stdio ? "1" : "0") + " " + std_option, options);
    madc::error perr;
    if ( !process->start(&perr) )
    {
	error_text = "session: the backend did not start";
	process.reset();
	return false;
    }
    std::string early;		// what a child printed before it connected
    std::vector<madc::poll_handle> hs;
    hs.push_back(wait_handle_of(listener.get()));
    madc::DataChannel *out = inherit_stdio ? NULL : &process->stdout_channel();
    if ( out && wait_handle_of(out).value >= 0 )
	hs.push_back(wait_handle_of(out));
    for (;;)
    {
	std::unique_ptr<madc::DataChannel> conn;
	madc::AcceptResult r = acceptor->accept(conn, &lerr);
	if ( r == madc::AcceptResult::accepted )
	{
	    // The token is the connection's first line.
	    std::string first;
	    std::vector<madc::poll_handle> ch(1, wait_handle_of(conn.get()));
	    while ( first.find('\n') == std::string::npos
		 && wait_any_readable(ch, 5000) > 0 && read_wire(*conn, first) )
		;
	    const size_t nl = first.find('\n');
	    if ( nl != std::string::npos && first.compare(0, nl, token) == 0 )
	    {
		wire = std::move(conn);
		inbuf = first.substr(nl + 1);
		return true;
	    }
	    continue;		// not the child: refused, keep listening
	}
	if ( r == madc::AcceptResult::error )
	    break;
	int ready = wait_any_readable(hs, -1);
	if ( ready < 0 )
	    break;
	// A pending connection first: a child that refused its standard
	// connects, says so and exits, so its output ends at once — the
	// refusal is on the connection, not lost with the pipe.
	if ( ready & 1 )
	    continue;
	if ( hs.size() > 1 && (ready & 2) && !read_wire(*out, early) )
	    break;		// the child's output ended unconnected: it is gone
    }
    error_text = "session: the backend ended before it connected";
    if ( !early.empty() )
	error_text += ": " + early;
    process->terminate();
    process->wait();
    process.reset();
    return false;
}

#endif

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
    const bool computing = busy();
    release_waiters();
    wire.reset();			// the backend's read sees EOF and returns
    if ( process )
    {
	// A busy backend never reaches that read (an endless entry, Stop's
	// case): it is terminated now, not after the grace.
	if ( computing )
	    process->terminate();
	process->wait_or_kill(2000);
	process.reset();
    }
    inbuf.clear();
}

bool SessionClient::send(const std::string &line)
{
    return wire && write_line(*wire, line);
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
    std::vector<madc::poll_handle> hs(1, wait_handle_of(&out));
    if ( hs[0].value < 0 )
	return;
    while ( wait_any_readable(hs, timeout_ms) > 0 )
    {
	if ( !read_wire(out, output) )
	{
	    output_done = true;		// its end: never readable again
	    return;
	}
	timeout_ms = 0;			// then only what is already there
    }
}

// The backend ended: what it printed last, then its status.
int SessionClient::stopped(Reply &reply, std::string &output)
{
    read_output(output, 0);
    release_waiters();
    wire.reset();
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

unsigned SessionClient::run(const std::vector<std::string> &argv, bool call)
{
    nlohmann::json req;
    const unsigned seq = next_seq++;
    req["seq"] = seq;
    req["op"] = op_name(Op::run);
    req["argv"] = argv;
    if ( call )
	req["call"] = true;
    return send(req.dump()) ? seq : 0;
}

unsigned SessionClient::build(const std::string &path, const std::string &text,
			      const std::string &out)
{
    nlohmann::json req;
    const unsigned seq = next_seq++;
    req["seq"] = seq;
    req["op"] = op_name(Op::build);
    req["path"] = path;
    if ( !text.empty() )
	req["text"] = text;
    req["out"] = out;
    return send(req.dump()) ? seq : 0;
}

bool SessionClient::input(const std::string &text)
{
    if ( !wire || !process )
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

bool SessionClient::interrupt()
{
    return wire && process && interruptor.send(*process);
}

void SessionClient::take_output(std::string &output)
{
    read_output(output, 0);
}

void SessionClient::wait_handles(std::vector<madc::poll_handle> &out) const
{
    out.clear();
    if ( !wire )
	return;
    out.push_back(wait_handle_of(wire.get()));
    madc::poll_handle pipe_end =
	process && !output_done ? wait_handle_of(&process->stdout_channel())
				: wait_handle_of(NULL);
    if ( pipe_end.value >= 0 )
	out.push_back(pipe_end);
}

int SessionClient::pending() const
{
    if ( !wire )
	return -1;
    if ( inbuf.find('\n') != std::string::npos )
	return 1;
    std::vector<madc::poll_handle> hs;
    wait_handles(hs);
    return wait_any_readable(hs, 0) > 0 ? 1 : 0;
}

int SessionClient::poll(Reply &reply, std::string &output, int timeout_ms)
{
    if ( !wire )
	return stopped(reply, output);
    std::string line;
    while ( !take_line(line) )
    {
	std::vector<madc::poll_handle> hs;
	wait_handles(hs);		// the wire, then the output pipe (until its end)
	int r = wait_any_readable(hs, timeout_ms);
	if ( r == 0 )
	    return 0;
	if ( r < 0 )
	    return stopped(reply, output);
	// The wire first: the backend writes a running notice before the
	// entry prints, so once a line is whole, its op decides where the
	// output waiting in the pipe belongs (below). The pipe is drained
	// only while no line is, so a full pipe never stalls the backend.
	if ( r & 1 )
	{
	    if ( !read_wire(*wire, inbuf) )
		return stopped(reply, output);
	    if ( inbuf.find('\n') != std::string::npos )
		continue;
	}
	if ( r & 2 )
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
    if ( op != Op::running && op != Op::unknown && reply.seq > answered_seq )
	answered_seq = reply.seq;
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
	    reply.payload = payload_of(j.value("payload", std::string()));
	    reply.argv = j.value("argv", std::vector<std::string>());
	    break;
	case Op::complete:
	    reply.kind = Reply::Kind::complete;
	    reply.start = j.value("start", (size_t)0);
	    reply.names = j.value("names", std::vector<std::string>());
	    break;
	case Op::load:
	    reply.kind = Reply::Kind::load;
	    reply.defines_main = j.value("main", false);
	    break;
	case Op::run:
	    reply.kind = Reply::Kind::run;
	    reply.status = j.value("status", 0);
	    reply.shown = j.value("shown", std::string());	// a call's
	    break;
	case Op::continues:
	    reply.kind = Reply::Kind::continues;
	    reply.continues = j.value("continues", false);
	    break;
	case Op::build:
	    reply.kind = Reply::Kind::build;
	    reply.shown = j.value("shown", std::string());
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

// While the backend runs an entry, a terminal's interrupt is the backend's
// (D8: the entry returns to the prompt, the session kept; a second one ends
// the backend and a fresh one starts): the host ignores it for the call
// (madc::HostIgnoresInterrupt, madc_session_interrupt.h).
using madc::HostIgnoresInterrupt;

bool BackendSession::submit(const std::string &text, const TakenHook &taken)
{
    HostIgnoresInterrupt quiet;
    SessionClient::Reply r;
    std::string output;
    int rc = client.offer_wait(text, true, r, output, -1, taken);
    shown_text = rc > 0 ? r.shown : std::string();
    if ( !settle(rc, r, output) )
	return false;
    return honor(r) && r.ok;
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
    bool ok = r.ok;
    if ( r.state == OfferState::taken )
	ok = honor(r) && ok;
    return Offered{ r.state, ok };
}

bool BackendSession::honor(const SessionClient::Reply &reply)
{
    if ( reply.payload == madc::session_payload::quit )
    {
	// %quit: the session ends, and the terminal with it (status 0).
	has_ended = true;
	end_status = 0;
	return true;
    }
    if ( reply.payload == madc::session_payload::none || reply.argv.empty() )
	return true;
    if ( reply.payload == madc::session_payload::load )
	return load_file(reply.argv[0]);
    if ( reply.payload == madc::session_payload::call )
	return call_file(reply.argv);
    if ( reply.payload == madc::session_payload::build )
	return build_file(reply.argv);
    if ( reply.payload == madc::session_payload::open )
    {
	// %open, %edit: this terminal's editor, at the line when one is named.
	std::string why;
	const int line = reply.argv.size() > 1 ? atoi(reply.argv[1].c_str()) : 0;
	if ( madc::run_terminal_editor(reply.argv[0], line, why) )
	    return true;
	err << "madc: " << why << std::endl;
	return false;
    }
    if ( reply.payload == madc::session_payload::run && !client.restart() )
    {
	err << "madc: " << client.last_error() << std::endl;
	has_ended = true;
	end_status = 1;
	return false;
    }
    std::vector<std::string> args(reply.argv);
    std::vector<char *> argv;
    for ( std::string &a : args )
	argv.push_back(&a[0]);
    argv.push_back(NULL);
    return run_file((int)args.size(), argv.data());
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

bool BackendSession::load_file(const std::string &path, bool *defines_main)
{
    SessionClient::Reply r;
    std::string output;
    int rc = client.load_wait(path, r, output);
    if ( defines_main )
	*defines_main = r.defines_main;
    return settle(rc, r, output) && r.ok;
}

bool BackendSession::run_file(int argc, char **argv)
{
    bool has_main = false;
    if ( argc < 1 || !load_file(argv[0], &has_main) )
	return false;
    if ( !has_main )
	return true;			// a main an earlier unit defined is not FILE's
    SessionClient::Reply r;
    std::string output;
    std::vector<std::string> args(argv, argv + argc);
    HostIgnoresInterrupt quiet;
    int rc = client.run_wait(args, r, output);	// no main: ok false, nothing ran
    settle(rc, r, output);
    return true;
}

// %call's (cling's .x): FILE loaded, then the backend's call_file makes the
// call; its value is the entry's shown.
bool BackendSession::call_file(const std::vector<std::string> &argv)
{
    if ( argv.size() != 2 || !load_file(argv[0]) )
	return false;
    SessionClient::Reply r;
    std::string output;
    HostIgnoresInterrupt quiet;
    int rc = client.run_wait(argv, r, output, true);
    if ( !settle(rc, r, output) )
	return false;
    shown_text = r.shown;
    return r.ok;
}

// %build's: the backend builds the file (this host has no buffer of it).
bool BackendSession::build_file(const std::vector<std::string> &argv)
{
    if ( argv.size() != 2 )
	return false;
    SessionClient::Reply r;
    std::string output;
    int rc = client.build_wait(argv[0], argv[1], r, output);
    if ( !settle(rc, r, output) )
	return false;
    shown_text = r.shown;
    return r.ok;
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
			    std::string &output, bool call)
{
    return wait_reply(run(argv, call), reply, output, -1,
		      InteractiveSession::TakenHook());
}

int SessionClient::build_wait(const std::string &path, const std::string &out,
			      Reply &reply, std::string &output)
{
    return wait_reply(build(path, std::string(), out), reply, output, -1,
		      InteractiveSession::TakenHook());
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

int madc_session_serve_child(const std::string &request)
{
    std::istringstream words(request);
    std::string endpoint, token, on_terminal, std_opt;
    words >> endpoint >> token >> on_terminal;
    std::getline(words >> std::ws, std_opt);
    if ( token.empty() || (on_terminal != "0" && on_terminal != "1") )
    {
	fprintf(stderr, "madc: '%s' names no session backend\n", request.c_str());
	return 1;
    }
    if ( on_terminal == "1" )
	setvbuf(stdin, NULL, _IONBF, 0);	// the host reads the same stdin
#ifdef _WIN32
    else
    {
	// Piped: stderr joins the output pipe, as the fork child's prologue
	// moves it (the CRT's fd 2 and the process's standard error handle).
	fflush(stderr);
	_dup2(_fileno(stdout), _fileno(stderr));
    }
#else
    madc::run_child_prologue(on_terminal != "1");	// the fork child's own steps
#endif
    madc::error err;
    std::unique_ptr<madc::DataChannel> wire =
	madc::DataChannelRegistry::instance().open(
	    madc::DataSource("tcp://" + endpoint), madc::ChannelOpenMode::read_write, &err);
    if ( !wire || !write_line(*wire, token) )
    {
	fprintf(stderr, "madc: the session backend cannot reach %s\n", endpoint.c_str());
	return 1;
    }
    madc::session_interrupt_arm_backend(token);	// the event, the console (D8)
    return serve_session(*wire, std::unique_ptr<Program>(new Program()), std_opt);
}
