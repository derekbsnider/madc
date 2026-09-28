// The interactive session in its own process (plan §41.9a, D2): a
// SessionClient forks the running binary, the child serves one
// InteractiveSession, and the client reads each entry's output before its
// result. The gate of slice 1: output order, a refused entry's rendered
// diagnostics and their rows, completion, and a crash that ends the backend
// and not the client, after which restart() begins an empty session.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <deque>
#include <iostream>
#include <map>
#include <queue>
#include <stack>
#include <string>
#include <vector>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_session.h"
#include "madc_session_client.h"

#ifndef _WIN32
#include <signal.h>

namespace {

const int kWait = 20000;		// ms: a JIT'd entry on a loaded host

bool has(const std::vector<std::string> &names, const char *name)
{
    for ( const std::string &n : names )
	if ( n == name )
	    return true;
    return false;
}

// The message of the first error row (by its severity code, the
// diag_severity enum), or "" when there is none.
std::string first_error_row(const madc::value &rows)
{
    if ( !rows.is_array() )
	return std::string();
    for ( const madc::value &row : rows.as_array() )
    {
	if ( !row.is_object() )
	    continue;
	const std::map<std::string, madc::value> &f = row.as_object();
	std::map<std::string, madc::value>::const_iterator code = f.find("severity_code");
	std::map<std::string, madc::value>::const_iterator msg = f.find("message");
	if ( code != f.end() && msg != f.end()
	  && code->second.as_integer() == (int64_t)madc::diag_severity::error )
	    return msg->second.as_string();
    }
    return std::string();
}

} // namespace

TEST_CASE("session backend: an entry's output comes before its result")
{
    SessionClient c;
    REQUIRE(c.start("--std=c11"));
    CHECK(c.running());
    SessionClient::Reply r;
    std::string out;
    REQUIRE(c.offer_wait("#include <stdio.h>", true, r, out, kWait) == 1);
    CHECK(r.ok);
    REQUIRE(c.offer_wait("int twice(int v) { return 2 * v; }", true, r, out, kWait) == 1);
    CHECK(r.kind == SessionClient::Reply::Kind::offer);
    CHECK(r.state == InteractiveSession::OfferState::taken);
    CHECK(r.ok);
    out.clear();
    REQUIRE(c.offer_wait("printf(\"first\\n\"); printf(\"%d\\n\", twice(21));",
			 true, r, out, kWait) == 1);
    CHECK(r.ok);
    CHECK(out == "first\n42\n");
    CHECK(r.submitted == 3);
    // An expression shows its value; a declaration keeps its definitions
    // in the backend for the next entry.
    out.clear();
    REQUIRE(c.offer_wait("twice(5)", true, r, out, kWait) == 1);
    CHECK(r.ok);
    CHECK(r.shown.find("10") != std::string::npos);
    // The program's stdin is the client's to write.
    REQUIRE(c.input("41\n"));
    out.clear();
    REQUIRE(c.offer_wait("int n = 0; scanf(\"%d\", &n); printf(\"%d\\n\", n + 1);",
			 true, r, out, kWait) == 1);
    CHECK(r.ok);
    CHECK(out == "42\n");
}

// The next reply, however many polls it takes.
int next_reply(SessionClient &c, SessionClient::Reply &r, std::string &out)
{
    int got;
    do
	got = c.poll(r, out, kWait);
    while ( got == 0 );
    return got;
}

TEST_CASE("session backend: a taken entry's running notice comes before its output")
{
    SessionClient c;
    REQUIRE(c.start("--std=c11"));
    SessionClient::Reply r;
    std::string out;
    REQUIRE(c.offer_wait("#include <stdio.h>", true, r, out, kWait) == 1);
    out.clear();
    const unsigned seq = c.offer("printf(\"after\\n\");", true);
    REQUIRE(next_reply(c, r, out) == 1);
    CHECK(r.kind == SessionClient::Reply::Kind::running);
    CHECK(r.seq == seq);
    CHECK(out.empty());
    REQUIRE(next_reply(c, r, out) == 1);
    CHECK(r.kind == SessionClient::Reply::Kind::offer);
    CHECK(r.seq == seq);
    CHECK(out == "after\n");
    // An entry that is not taken runs nothing and sends no notice.
    const unsigned more = c.offer("int f(void) {", false);
    REQUIRE(next_reply(c, r, out) == 1);
    CHECK(r.kind == SessionClient::Reply::Kind::offer);
    CHECK(r.seq == more);
    CHECK(r.state == InteractiveSession::OfferState::incomplete);
}

TEST_CASE("session backend: an offer's verdict, a refused entry's diagnostics")
{
    SessionClient c;
    REQUIRE(c.start("--std=c11"));
    SessionClient::Reply r;
    std::string out;
    // Not final: an unfinished entry is Incomplete, and nothing is kept.
    REQUIRE(c.offer_wait("int f(void) {", false, r, out, kWait) == 1);
    CHECK(r.state == InteractiveSession::OfferState::incomplete);
    REQUIRE(c.offer_wait("int x = ;", true, r, out, kWait) == 1);
    CHECK(r.state == InteractiveSession::OfferState::taken);
    CHECK_FALSE(r.ok);
    CHECK(r.rendered.find("error") != std::string::npos);
    CHECK_FALSE(first_error_row(r.diagnostics).empty());
    // The refused entry left the session as it was.
    REQUIRE(c.offer_wait("int x = 7;", true, r, out, kWait) == 1);
    CHECK(r.ok);
    CHECK(first_error_row(r.diagnostics).empty());
}

TEST_CASE("session backend: completion asks the backend's session")
{
    SessionClient c;
    REQUIRE(c.start("--std=c11"));
    SessionClient::Reply r;
    std::string out;
    REQUIRE(c.offer_wait("int twice(int v) { return 2 * v; }", true, r, out, kWait) == 1);
    REQUIRE(c.complete_wait("twi", 3, r, kWait) == 1);
    CHECK(r.kind == SessionClient::Reply::Kind::complete);
    CHECK(r.start == 0);
    CHECK(has(r.names, "twice"));
}

TEST_CASE("session backend: a crash ends the backend, restart begins afresh")
{
    SessionClient c;
    REQUIRE(c.start("--std=c11"));
    SessionClient::Reply r;
    std::string out;
    REQUIRE(c.offer_wait("int kept = 5;", true, r, out, kWait) == 1);
    CHECK(r.ok);
    CHECK(c.offer_wait("int *p = 0; *p = 1;", true, r, out, kWait) == -1);
    CHECK(r.kind == SessionClient::Reply::Kind::stopped);
    CHECK(r.exit_status == 128 + SIGSEGV);
    CHECK_FALSE(c.running());
    // The client outlived its backend; the new session has no `kept`.
    REQUIRE(c.restart());
    CHECK(c.running());
    REQUIRE(c.offer_wait("int again = kept;", true, r, out, kWait) == 1);
    CHECK_FALSE(r.ok);
    REQUIRE(c.offer_wait("int again = 6;", true, r, out, kWait) == 1);
    CHECK(r.ok);
    CHECK(r.submitted == 2);
}

TEST_CASE("session backend: a standard begin refuses does not start")
{
    SessionClient c;
    CHECK_FALSE(c.start("--std=no-such-standard"));
    CHECK_FALSE(c.running());
    CHECK(c.last_error().find("Unknown --std target: no-such-standard")
	  != std::string::npos);
}

#else

TEST_CASE("session backend: POSIX only for now")
{
    SessionClient c;
    CHECK_FALSE(c.start());
    CHECK_FALSE(c.last_error().empty());
}

#endif
