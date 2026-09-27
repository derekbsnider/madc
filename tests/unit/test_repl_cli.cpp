// The command-line REPL's loop (plan §41.5a, D20): madc_repl_run, the loop
// `madc` and `madc -i` run over stdin and stdout, driven here over string
// streams. The session decides what an entry is and what it shows; the loop
// reads lines, prompts, and prints.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <cstdio>
#include <cstdlib>
#include <ctime>
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
#include "madcdis/line_edit.h"
#include "madcdis/tui_model.h"
#include "madcdis/tui_provider.h"

namespace {

// A terminal the test types at (plan §41.7a): each read delivers one chunk
// of keystroke bytes through the production key parser, and everything the
// editor paints is kept. It records whether the terminal was the editor's
// when each entry ran (`held` at a show), so a test can see the entry run
// on the terminal handed back.
struct scripted_terminal : madc::hub::line_target
{
    std::vector<std::string> chunks;
    size_t next;
    std::string painted;
    unsigned begins, ends;
    bool held;
    madc::hub::tui_keyparse parse;

    explicit scripted_terminal(const std::vector<std::string> &c)
	: chunks(c), next(0), begins(0), ends(0), held(false) {}
    bool begin(size_t &cols) override
    {
	cols = 80;
	++begins;
	held = true;
	return true;
    }
    void end() override
    {
	++ends;
	held = false;
    }
    void write(const std::string &bytes) override { painted += bytes; }
    bool read_keys(std::vector<madc::hub::tui_keyev> &out) override
    {
	if ( next >= chunks.size() )
	    return false;
	const std::string &c = chunks[next++];
	parse.feed(c.data(), c.size(), out);
	parse.flush(out);
	return true;
    }
    size_t columns() override { return 80; }
};

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
    // The function's second line is typed after a continuation indent as
    // wide as the prompt; the end of input ends the line. A greeting is the
    // host's, not the loop's.
    CHECK(out.str() == "c++17> 3\nc++17> " + std::string(7, ' ')
			+ "c++17> \n");

    InteractiveSession m;
    REQUIRE(m.begin("--std=madc"));
    std::istringstream none("");
    std::ostringstream mout;
    CHECK(madc_repl_run(m, none, mout, true) == 0);
    CHECK(mout.str() == "madc> \n");
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

// §37's transcript again, typed at the line editor (plan §41.7a): the same
// entries, the same values, one diagnostic citing REPL[6]. The function's
// body line starts with a Tab, which indents. Every entry reads on a
// terminal the editor holds and runs on one it handed back.
static void check_first_slice_typed(const std::string &std_option,
				    const std::string &prompt)
{
    CAPTURE(std_option);
    InteractiveSession s;
    REQUIRE(s.begin(std_option));
    std::ostringstream err;
    s.program().error_stream = &err;
    scripted_terminal term({ "int x = 10;\r", "x * 2\r", "x * 2;\r",
			     "int twice(int a)\r", "{\r", "\treturn a * 2;\r",
			     "}\r", "twice(x)\r", "int y = ;\r", "x\r",
			     "\x04" });
    std::ostringstream out;
    CHECK(madc_repl_edit(s, term, out, "") == 0);
    CHECK(out.str() == "20\n20\n10\n");
    CAPTURE(err.str());
    CHECK(occurrences(err.str(), "error:") == 1);
    CHECK(occurrences(err.str(), "REPL[6]:1:") == 1);
    CHECK(s.submitted() == 7);
    CHECK(term.begins == 8u);
    CHECK(term.ends == 8u);
    CHECK_FALSE(term.held);
    // The prompt names the standard (D22); the body line is indented to
    // its width, then by the Tab's four columns.
    CHECK(term.painted.find(prompt + "int twice(int a)") != std::string::npos);
    CHECK(term.painted.find("\r\n" + std::string(prompt.size(), ' ')
			    + "    return a * 2;") != std::string::npos);
}

TEST_CASE("the first slice's transcript typed at the line editor (§37, D23)")
{
    check_first_slice_typed("--std=madc", "madc> ");
    check_first_slice_typed("--std=c17", "c17> ");
    check_first_slice_typed("--std=c++17", "c++17> ");
}

TEST_CASE("a paste of three entries runs them one by one (D23)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    scripted_terminal term({ "\x1b[200~int a = 1;\na\nint b = 2;\n\x1b[201~",
			     "b\r", "\x04" });
    std::ostringstream out;
    CHECK(madc_repl_edit(s, term, out, "") == 0);
    CHECK(out.str() == "1\n2\n");
    CHECK(s.submitted() == 4);
}

TEST_CASE("Ctrl-C drops an entry, and an entry pending at the end runs (D23)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    std::ostringstream err;
    s.program().error_stream = &err;
    scripted_terminal term({ "int q = 5", "\x03", "q\r", "int z = 4;\rz" });
    std::ostringstream out;
    CHECK(madc_repl_edit(s, term, out, "") == 0);
    // The dropped entry never ran, so q is undeclared; z runs at the end.
    CHECK(out.str() == "4\n");
    CAPTURE(err.str());
    CHECK(occurrences(err.str(), "error:") == 1);
    CHECK(term.painted.find("c17> int q = 5\r\x1b[14C^C\r\n")
	  != std::string::npos);
    CHECK(s.submitted() == 3);
    CHECK(term.begins == term.ends);
}

TEST_CASE("Enter's verdicts through the session: extendable, and the third Enter (D23)")
{
    // D11 at the editor: the else continues the if; a blank line runs it.
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    scripted_terminal term({ "int x = 10;\r", "if (x < 5) x = 7;\r", "\r",
			     "x\r", "if (x > 5) x = 1;\r", "else x = 2;\r",
			     "x\r", "\x04" });
    std::ostringstream out;
    CHECK(madc_repl_edit(s, term, out, "") == 0);
    CHECK(out.str() == "10\n1\n");

    // An unbalanced brace: the third Enter in a row takes it, refused.
    InteractiveSession t;
    REQUIRE(t.begin("--std=c17"));
    std::ostringstream err;
    t.program().error_stream = &err;
    scripted_terminal open({ "int f(int a)\r{\r", "\r", "\r", "\x04" });
    std::ostringstream tout;
    CHECK(madc_repl_edit(t, open, tout, "") == 0);
    CAPTURE(err.str());
    CHECK(occurrences(err.str(), "error:") == 1);
    CHECK(err.str().find("'{' is not closed") != std::string::npos);
    CHECK(t.submitted() == 1);
}

// History (plan §41.7a, slice 2): Up recalls the entries a history file
// holds in the session's language (a C++ record stays out of a C session),
// and each entry taken is appended as Julia's record, a repeat of the
// newest skipped.
TEST_CASE("history: the file's entries are recalled and taken entries appended (D23)")
{
#ifdef _WIN32
    const char *tmp = getenv("TEMP");
#else
    const char *tmp = getenv("TMPDIR");
#endif
    std::string path = std::string(tmp && *tmp ? tmp : ".")
	+ "/madc_test_repl_history_" + std::to_string((long long)time(NULL))
	+ "_" + std::to_string((long long)(size_t)&path);
    {
	std::ofstream f(path.c_str(), std::ios::binary);
	f << madc::hub::history_record("2026-09-27 10:00:00Z", "c17", "3 * 3")
	  << madc::hub::history_record("2026-09-27 10:00:01Z", "c++17", "4 * 4")
	  << madc::hub::history_record("2026-09-27 10:00:02Z", "c11", "5 * 5");
    }
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    scripted_terminal term({ "\x1b[A\r", "\x1b[A\x1b[A\x1b[A\r", "\x04" });
    std::ostringstream out;
    CHECK(madc_repl_edit(s, term, out, path) == 0);
    // The newest C entry, then the oldest (Up stops there); the C++ one
    // never shows.
    CHECK(out.str() == "25\n9\n");
    std::ifstream in(path.c_str(), std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    std::vector<std::pair<std::string, std::string> > recs =
	madc::hub::history_records(text.str());
    REQUIRE(recs.size() == 4u);		// 5 * 5 again was a repeat: no record
    CHECK(recs[3].first == "c17");
    CHECK(recs[3].second == "3 * 3");
    in.close();
    std::remove(path.c_str());

    // In-session only (no file): the ring still recalls this session's
    // entries, and Ctrl-R finds one and leaves it for Enter.
    InteractiveSession m;
    REQUIRE(m.begin("--std=c17"));
    scripted_terminal again({ "11 + 0\r", "22 + 0\r", "33 + 0\r",
			      "\x12" "22", "\r", "\r", "2\x1b[A\r", "\x04" });
    std::ostringstream mout;
    CHECK(madc_repl_edit(m, again, mout, "") == 0);
    CHECK(mout.str() == "11\n22\n33\n22\n22\n");
}

// §37 item 7 (plan §41.7a, slice 3): Tab completes `x` and function names
// at the line editor, from the session's names; a second Tab lists the
// candidates under the entry.
TEST_CASE("Tab completes the session's names at the line editor (§37, D23)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    scripted_terminal term({ "int xylophone = 3;\r", "xylo\t\r",
			     "int twice(int a) { return a * 2; }\r",
			     "twi\t(4)\r", "int xyz1 = 1;\r", "xy\t", "\t",
			     "\x03", "\x04" });
    std::ostringstream out;
    CHECK(madc_repl_edit(s, term, out, "") == 0);
    CHECK(out.str() == "3\n8\n");
    CHECK(term.painted.find("xylophone  xyz1\r\n") != std::string::npos);
}

// Slice 4 (plan §41.7a): after `.`, Tab Tab lists the object's members at
// the line editor.
TEST_CASE("Tab lists a struct's fields after `.` at the line editor (D23)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    scripted_terminal term({ "struct Point { int x; int y; };\r",
			     "struct Point p = { 1, 2 };\r", "p.\t", "\t",
			     "\x15" "p.y\r", "\x04" });
    std::ostringstream out;
    CHECK(madc_repl_edit(s, term, out, "") == 0);
    CHECK(out.str() == "2\n");
    CHECK(term.painted.find("x  y\r\n") != std::string::npos);
}

// §37 item 8 (plan §41.8a, slice 1): a command reads the same off a terminal
// and at the line editor, and a continuation line is never one.
TEST_CASE("%type answers off a terminal and at the line editor (§37, D13)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    std::ostringstream err;
    s.program().error_stream = &err;
    std::istringstream in("int x = 10;\n%type x * 2.5\n:type &x\n"
			  "int g(int a, int b)\n{ return a\n%b; }\ng(7, 4)\n");
    std::ostringstream out;
    CHECK(madc_repl_run(s, in, out, false) == 0);
    CHECK(out.str() == "double\nint *\n3\n");	// `%b` continuing g's body is C
    CHECK(err.str().empty());

    InteractiveSession e;
    REQUIRE(e.begin("--std=c17"));
    scripted_terminal term({ "int sq(int n) { return n * n; }\r",
			     "%type sq(2)\r", "%ty\t sq\r", "\x04" });
    std::ostringstream eout;
    CHECK(madc_repl_edit(e, term, eout, "") == 0);
    CHECK(eout.str() == "int\nint (int)\n");	// Tab completed `%type`
}

// Slice 2 (plan §41.8a): `?name` at the line editor, the name completed by
// Tab after the `?`.
TEST_CASE("?name answers at the line editor (§37, D15)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    scripted_terminal term({ "int twice(int a) { return a * 2; }\r",
			     "?twice\r", "?twi\t\r", "\x04" });
    std::ostringstream out;
    CHECK(madc_repl_edit(s, term, out, "") == 0);
    const std::string twice =
	"Signature: int twice(int a)  @ REPL[1]:1\nType:      function\n";
    CHECK(out.str() == twice + twice);
}
