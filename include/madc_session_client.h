/* madc_session_client.h — the interactive session in its own process (plan
 * §41.9a, D2).
 *
 * A SessionClient runs one InteractiveSession in a backend process: the
 * running madc forks (nothing execs), the child builds the session and
 * answers requests until its channel closes. A crash in an entry ends the
 * backend, not the client's process; the client reports it, and restart()
 * starts a fresh session, whose state is empty (Thonny's and Jupyter's
 * rule).
 *
 * Two channels: the requests and replies are one JSON object per line on a
 * socketpair (Jupyter's message shape: an offer's reply follows the output
 * the entry printed), and the program's own stdout and stderr, merged, are
 * the backend's output pipe, read with the replies.
 *
 * Thread contract: one client is driven by one thread (or one cooperative
 * task). The backend is single-threaded, so its requests are serialized
 * (D9).
 */

#ifndef __MADC_SESSION_CLIENT_H
#define __MADC_SESSION_CLIENT_H 1

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "madc_session.h"
#include "libmadc/value.h"
#include "madcdis/datachannel.h"	// poll_handle

class Program;
namespace madc { class Process; }

class SessionClient
{
public:
    // Builds the backend's Program, in the child, after the fork; empty is a
    // fresh Program.
    typedef std::function<std::unique_ptr<Program>()> ProgramFactory;

    SessionClient();
    ~SessionClient();			// stops the backend

    // Start the backend: fork, build the session, begin(std_option) there.
    // False when it cannot start (or begin refuses the standard; the
    // backend's rendered diagnostics are then in last_error()). POSIX only
    // for now: Windows has no fork (plan §41.9a).
    bool start(const std::string &std_option = std::string(),
	       const ProgramFactory &make_program = ProgramFactory());
    // Start again with the same standard and factory (after a stop).
    bool restart();
    // End the backend (its state goes with it).
    void stop();
    bool running() const;

    struct Reply
    {
	typedef ::madc::session_reply Kind;	// <bits/session_enums>: the dialect's text too
	Kind kind;
	unsigned seq;			// the request's
	// An offer's: the verdict, and once taken, the result, the shown
	// value, the entries taken so far (REPL[N]), the rendered diagnostics
	// (the text the CLI prints) and their rows (diagnostic rows: severity,
	// phase, message, file, line, column).
	InteractiveSession::OfferState state;
	bool ok;
	unsigned submitted;
	std::string shown;
	std::string rendered;
	madc::value diagnostics;
	// A completion's: the word's start and the names.
	size_t start;
	std::vector<std::string> names;
	// stopped: the backend's exit status (128 + signal for a signal).
	int exit_status;
	Reply();
    };

    // Send an entry (final: submit it whatever its verdict) or a completion
    // query. Each returns its request's seq, 0 when the backend is not
    // running. Neither waits.
    unsigned offer(const std::string &text, bool final);
    unsigned complete(const std::string &text, size_t caret);
    // Text for the program's stdin: a scanf in an entry reads it. False when
    // the backend is not running. It waits while the pipe is full.
    bool input(const std::string &text);

    // Wait up to timeout_ms (-1: no limit) for the next reply. The output the
    // backend printed before it is appended to `output` first. 1: a reply;
    // 0: the time ran out (output may still have grown); -1: the backend
    // stopped (reply.kind is stopped, with its exit status).
    int poll(Reply &reply, std::string &output, int timeout_ms);
    // Offer or complete and wait for its reply: poll() until the reply with
    // that seq, or the backend stopped (-1). A synchronous caller's (the
    // CLI's): replies to earlier requests that arrive meanwhile are dropped,
    // their output kept.
    int offer_wait(const std::string &text, bool final, Reply &reply,
		   std::string &output, int timeout_ms = -1);
    int complete_wait(const std::string &text, size_t caret, Reply &reply,
		      int timeout_ms = -1);

    // Readiness, for a select (plan §41.9a slice 2). pending(): 1 when a
    // reply or output waits (or the backend's end, which poll() reports), 0
    // when neither does, -1 when the backend is not running; never waits.
    // wait_handles(): the handles whose readability changes it, the reply
    // socket and the output pipe (until its end), replaced at a restart.
    int pending() const;
    void wait_handles(std::vector<madc::poll_handle> &out) const;
    // What the program printed so far; never waits.
    void take_output(std::string &output);

    const std::string &last_error() const { return error_text; }

private:
    bool send(const std::string &line);
    bool take_line(std::string &line);
    void read_output(std::string &output, int timeout_ms);
    int stopped(Reply &reply, std::string &output);
    void release_waiters() const;	// before the streams close

    std::unique_ptr<madc::Process> process;
    int fd;				// the parent's end of the socketpair
    std::string inbuf;			// reply bytes not yet a whole line
    bool output_done;			// the output pipe reached its end
    unsigned next_seq;
    std::string std_option;
    ProgramFactory make_program;
    std::string error_text;
    SessionClient(const SessionClient &);
    SessionClient &operator=(const SessionClient &);
};

// The script surface (<ns_madc>'s session_* verbs, src/madc_session_verbs.cpp),
// declared here for C++ hosts and tests; the contract is <ns_madc>'s.
namespace madc {
int64_t session_open(const char *std);
bool session_running(int64_t handle);
const char *session_error(int64_t handle);
int64_t session_offer(int64_t handle, const char *text, bool final);
int64_t session_complete(int64_t handle, const char *text, int64_t caret);
int64_t session_poll(value &reply, int64_t handle);
value &session_output(value &out, int64_t handle);
bool session_input(int64_t handle, const char *text);
bool session_restart(int64_t handle);
int64_t session_readable(int64_t handle);
bool session_close(int64_t handle);
}

#endif
