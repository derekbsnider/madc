// The command-line REPL's loop (plan §41.5a, D20): madc_repl_run, the loop
// `madc` and `madc -i` run over stdin and stdout, driven here over string
// streams. The session decides what an entry is and what it shows; the loop
// reads lines, prompts, and prints.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <sstream>
#include <stack>
#include <string>
#include <vector>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_session.h"
#include "madc_repl.h"

namespace {

// How many times `needle` occurs in `hay`.
size_t occurrences(const std::string &hay, const std::string &needle)
{
    size_t n = 0;
    for ( size_t at = hay.find(needle); at != std::string::npos;
	  at = hay.find(needle, at + needle.size()) )
	++n;
    return n;
}

// Plan §37 items 1-6, off a terminal: no banner and no prompt (D23), so the
// output is the shown values alone. The refused entry renders once, when it
// is final, citing its number; the function typed over four lines renders
// nothing on the way.
void check_first_slice(const std::string &std_option)
{
    CAPTURE(std_option);
    InteractiveSession s;
    REQUIRE(s.begin(std_option));
    std::ostringstream err;
    s.program().error_stream = &err;
    std::istringstream in(
	"int x = 10;\n"
	"x * 2\n"
	"x * 2;\n"
	"int twice(int a)\n"
	"{\n"
	"\treturn a * 2;\n"
	"}\n"
	"twice(x)\n"
	"int y = ;\n"
	"x\n");
    std::ostringstream out;
    CHECK(madc_repl_run(s, in, out, false) == 0);
    CHECK(out.str() == "20\n20\n10\n");
    CAPTURE(err.str());
    CHECK(occurrences(err.str(), "error:") == 1);
    CHECK(occurrences(err.str(), "REPL[6]:1:") == 1);
    CHECK(s.submitted() == 7);
}

} // namespace

TEST_CASE("the first slice's transcript runs off a terminal (§37, D20)")
{
    check_first_slice("--std=madc");
    check_first_slice("--std=c17");
    check_first_slice("--std=c++17");
}

TEST_CASE("on a terminal the prompt names the standard and continuation lines are indented (D22)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c++17"));
    std::istringstream in("int x = 3\nint f(int a)\n{ return a; }\n");
    std::ostringstream out;
    CHECK(madc_repl_run(s, in, out, true) == 0);
    const std::string o = out.str();
    CAPTURE(o);
    CHECK(o.compare(0, 5, "madc ") == 0);	// the banner
    CHECK(o.find("\nc++17> 3\n") != std::string::npos);
    // The function's second line is typed after a continuation indent as
    // wide as the prompt; the end of input ends the line.
    CHECK(o.find("\nc++17> " + std::string(7, ' ') + "c++17> \n")
	  != std::string::npos);

    InteractiveSession m;
    REQUIRE(m.begin("--std=madc"));
    std::istringstream none("");
    std::ostringstream mout;
    CHECK(madc_repl_run(m, none, mout, true) == 0);
    CHECK(mout.str().find("\nmadc> \n") != std::string::npos);
}

TEST_CASE("a finished if waits one line for its else (D11)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    std::istringstream in(
	"int x = 10;\n"
	"if (x > 5) x = 1;\n"	// continued by the else
	"else x = 2;\n"
	"x\n"
	"if (x < 5) x = 7;\n"	// run by the empty line
	"\n"
	"x\n"
	"if (x > 5) x = 0;\n"	// run by the next entry, which then runs
	"x\n");
    std::ostringstream out;
    CHECK(madc_repl_run(s, in, out, false) == 0);
    CHECK(out.str() == "1\n7\n0\n");
}

TEST_CASE("the end of input submits a pending entry")
{
    // An extendable if runs.
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    std::istringstream in("int x = 1;\nif (x) x = 5;\n");
    std::ostringstream out;
    CHECK(madc_repl_run(s, in, out, false) == 0);
    REQUIRE(s.submit("x"));
    CHECK(s.shown() == "5");

    // An incomplete entry is refused with its end-of-input diagnostic.
    InteractiveSession t;
    REQUIRE(t.begin("--std=c17"));
    std::ostringstream err;
    t.program().error_stream = &err;
    std::istringstream open("int f(int a)\n{\n");
    std::ostringstream tout;
    CHECK(madc_repl_run(t, open, tout, false) == 0);
    CHECK(tout.str().empty());
    CAPTURE(err.str());
    CHECK(occurrences(err.str(), "error:") == 1);
    CHECK(err.str().find("'{' is not closed") != std::string::npos);
    CHECK(t.submitted() == 1);
}
