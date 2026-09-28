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
#include "madc_repl.h"

#ifndef _WIN32
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <cstdio>
#include <fstream>
#include <sstream>

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
    // A diagnostic renders whole into its reply: the header, the offending
    // line and its caret. The echo is never the program's output.
    out.clear();
    REQUIRE(c.offer_wait("int y = nosuch_name;", true, r, out, kWait) == 1);
    CHECK_FALSE(r.ok);
    CAPTURE(r.rendered);
    CHECK(r.rendered.find("use of undeclared identifier 'nosuch_name'") != std::string::npos);
    CHECK(r.rendered.find("int y = nosuch_name;\n") != std::string::npos);
    CHECK(r.rendered.find("^") != std::string::npos);
    CHECK(out.empty());
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
    CHECK(r.signal == SIGSEGV);
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

TEST_CASE("session backend: an exit is not a signal")
{
    // exit(139) and SIGSEGV share the 128+signal shape; the signal tells
    // them apart, so a CLI ends on exit(n) and restarts on a crash.
    SessionClient c;
    REQUIRE(c.start("--std=c11"));
    SessionClient::Reply r;
    std::string out;
    REQUIRE(c.offer_wait("#include <stdlib.h>", true, r, out, kWait) == 1);
    CHECK(c.offer_wait("exit(139);", true, r, out, kWait) == -1);
    CHECK(r.kind == SessionClient::Reply::Kind::stopped);
    CHECK(r.exit_status == 139);
    CHECK(r.signal == 0);
}

TEST_CASE("session backend: load a program file, then run its main")
{
    char path[] = "/tmp/madc_session_load_XXXXXX.c";
    int tfd = mkstemps(path, 2);
    REQUIRE(tfd >= 0);
    close(tfd);
    {
	std::ofstream f(path);
	f << "#include <stdio.h>\n"
	     "static int base = 40;\n"
	     "int main(int argc, char **argv) {\n"
	     "    printf(\"main argc=%d last=%s\\n\", argc, argv[argc - 1]);\n"
	     "    return 7;\n"
	     "}\n";
    }
    SessionClient c;
    REQUIRE(c.start("--std=c17"));
    SessionClient::Reply r;
    std::string out;
    REQUIRE(c.load_wait(path, r, out) == 1);
    CHECK(r.kind == SessionClient::Reply::Kind::load);
    CHECK(r.ok);
    std::vector<std::string> argv;
    argv.push_back(path);
    argv.push_back("extra");
    REQUIRE(c.run_wait(argv, r, out) == 1);
    CHECK(r.kind == SessionClient::Reply::Kind::run);
    CHECK(r.ok);
    CHECK(r.status == 7);
    CHECK(out == "main argc=2 last=extra\n");
    // The file and the session are one unit: its static is a session name.
    REQUIRE(c.offer_wait("base + 2", true, r, out, kWait) == 1);
    CHECK(r.shown == "42");
    // A file that does not open is refused, and nothing is kept.
    REQUIRE(c.load_wait("/nonexistent/madc_session_missing.c", r, out) == 1);
    CHECK_FALSE(r.ok);
    std::remove(path);
}

TEST_CASE("session backend: the CLI loop drives a BackendSession")
{
    // madc_repl_run over string streams, the backend piped: its output goes
    // to the loop's out between the shown values, a crash says so and a
    // fresh session follows, and exit(4) ends the loop with status 4 before
    // the next line is read. The factory runs in the backend: there a crash
    // is the backend's, not this test's, so doctest's handler (inherited
    // through the fork) gives way to the default.
    SessionClient c;
    REQUIRE(c.start("--std=c17", []() {
	signal(SIGSEGV, SIG_DFL);
	return std::unique_ptr<Program>(new Program());
    }));
    std::ostringstream err, out;
    BackendSession session(c, err, &out);
    CHECK(session.standard_name() == "c17");
    std::istringstream in(
	"#include <stdio.h>\n"
	"#include <stdlib.h>\n"
	"int x = 5;\n"
	"printf(\"hi\\n\");\n"
	"x\n"
	"if (x) printf(\"then\\n\");\n"
	"else printf(\"else\\n\");\n"
	"int *p = 0; *p = 1;\n"
	"x\n"
	"exit(4);\n"
	"99\n");
    CHECK(madc_repl_run(session, in, out, false) == 4);
    CAPTURE(err.str());
    CHECK(out.str() == "hi\n5\nthen\n");
    CHECK(err.str().find("madc: the session stopped (signal 11") != std::string::npos);
    CHECK(err.str().find("a new one started") != std::string::npos);
    CHECK(err.str().find("use of undeclared identifier 'x'") != std::string::npos);
    int status = 0;
    CHECK(session.ended(status));
    CHECK(status == 4);
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
