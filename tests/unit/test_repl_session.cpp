// The persistent interactive session (plan §41.2a, decisions D1/D25): one
// Program accepts appended entries, each entry lowers to its own MIR module,
// and every module links into ONE live MIR context. The §41.2 gate: entry 2
// calls a function and reads a global that entry 1 defined, through a second
// module, and writing the global through its live address changes what entry
// 2's code reads (entry 2 declares it, it never defines a copy).
//
// Everything runs through InteractiveSession, the production core the REPL
// and madcide's panel are clients of.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <deque>
#include <fstream>
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

namespace {

typedef int (*int_fn)(void);
typedef int (*int_int_fn)(int);

// The first error the refused entry recorded, or "" when it has none.
std::string first_error(InteractiveSession &s)
{
    const std::vector< ::Program::Diagnostic> &d = s.program().diagnostics;
    for ( size_t i = 0; i < d.size(); ++i )
	if ( d[i].severity == ::Program::DiagnosticSeverity::error )
	    return d[i].message;
    return std::string();
}

// The §41.2 gate, under one standard. The session looks definitions up by
// EMITTED name: a function with C++ linkage (the madc dialect) is Itanium-
// mangled, a namespace-scope object keeps its name.
void check_entries_link(const std::string &std_option, const char *f_sym,
			const char *h_sym)
{
    CAPTURE(std_option);
    InteractiveSession s;
    REQUIRE(s.begin(std_option));
    REQUIRE(s.submit("int g = 5;\nint f(int a) { return a + g; }"));
    REQUIRE(s.submit("int h(void) { return f(2) + g; }"));
    CHECK(s.entries() == 2);

    int_fn h = (int_fn)s.function(h_sym);
    int_int_fn f = (int_int_fn)s.function(f_sym);
    int *g = (int *)s.data("g");
    REQUIRE(h != (int_fn)NULL);
    REQUIRE(f != (int_int_fn)NULL);
    REQUIRE(g != (int *)NULL);
    CHECK(f(1) == 6);
    CHECK(h() == 12);		// f(2) + g = 7 + 5

    // Entry 2 reads entry 1's storage: one g, in the live context.
    *g = 10;
    CHECK(h() == 22);		// f(2) + g = 12 + 10
    CHECK(f(1) == 11);
}

} // namespace

TEST_CASE("a later entry calls a function and reads a global an earlier entry defined")
{
    check_entries_link("--std=c17", "f", "h");
    check_entries_link("--std=madc", "_Z1fi", "_Z1hv");	// D4: the REPL's default
}

TEST_CASE("macros, includes and types persist across entries")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    REQUIRE(s.submit("#define N 4\n#include <string.h>\n"
		     "struct P { int x; int y; };"));
    REQUIRE(s.submit("int span(void) { struct P p = { N, 3 };"
		     " return p.x * p.y + (int)strlen(\"abc\"); }"));
    int_fn span = (int_fn)s.function("span");
    REQUIRE(span != (int_fn)NULL);
    CHECK(span() == 15);	// 4 * 3 + 3
}

TEST_CASE("an entry reaches back past the entry before it")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    REQUIRE(s.submit("int base = 100;"));
    REQUIRE(s.submit("int twice(int a) { return 2 * a; }"));
    REQUIRE(s.submit("int both(void) { return twice(base) + 1; }"));
    int_fn both = (int_fn)s.function("both");
    REQUIRE(both != (int_fn)NULL);
    CHECK(both() == 201);
}

TEST_CASE("an entry the session refuses says so, and the session goes on")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    REQUIRE(s.submit("int g = 5;\nint f(int a) { return a + g; }"));

    // A file-scope static has internal linkage: a later entry's module
    // cannot import it, and its REPL rule is D6's.
    CHECK_FALSE(s.submit("static int hidden(void) { return 1; }"));
    CHECK(first_error(s).find("internal linkage") != std::string::npos);

    REQUIRE(s.submit("int k(void) { return f(3); }"));
    int_fn k = (int_fn)s.function("k");
    REQUIRE(k != (int_fn)NULL);
    CHECK(k() == 8);
    CHECK(s.entries() == 2);
}

TEST_CASE("an entry with a statement that does not compile runs none of it")
{
    InteractiveSession s;
    REQUIRE(s.begin());
    REQUIRE(s.submit("int g = 5;"));
    CHECK_FALSE(s.submit("g = 99; undeclared_fn(1);"));
    CHECK(first_error(s).find("undeclared_fn") != std::string::npos);
    CHECK(*(int *)s.data("g") == 5);
    // The next entry's run is a fresh one.
    REQUIRE(s.submit("g = g + 1;"));
    CHECK(*(int *)s.data("g") == 6);
    CHECK(s.entries() == 2);
}

// Slice 2 (D25): an entry's statements lower into its own entry function,
// which runs once, after the entry's module links. The oracle is clang-repl
// (tmp/repl/s2/order2.repl): the same entries give log=123 y=2,
// log=12345 w=4, x=30.
TEST_CASE("an entry's statements run once, on the session's globals")
{
    InteractiveSession s;
    REQUIRE(s.begin());			// D4: --std=madc
    REQUIRE(s.submit("int x = 10;"));
    REQUIRE(s.submit("x = x * 2;"));
    int *x = (int *)s.data("x");
    REQUIRE(x != (int *)NULL);
    CHECK(*x == 20);
    REQUIRE(s.submit("for (int i = 1; i <= 4; ++i)\n\tx += i;"));
    CHECK(*x == 30);			// entry 2's run did not run again
    REQUIRE(s.submit("if (x > 25) x = 1; else x = 2;"));
    CHECK(*x == 1);
    // A block is a statement too, with its own locals.
    REQUIRE(s.submit("{ int t = 4; x = x + t; }"));
    CHECK(*x == 5);
    CHECK(s.entries() == 5);
}

TEST_CASE("a declaration's initializer runs at its place among the entry's statements")
{
    InteractiveSession s;
    REQUIRE(s.begin());
    REQUIRE(s.submit("int log_v = 0;\n"
		     "int step(int d) { log_v = log_v * 10 + d; return d; }"));
    int *log_v = (int *)s.data("log_v");
    REQUIRE(log_v != (int *)NULL);
    // clang-repl: the statement before the declaration runs first.
    REQUIRE(s.submit("step(1); int y = step(2); step(3);"));
    CHECK(*log_v == 123);
    CHECK(*(int *)s.data("y") == 2);
    // A declaration before the entry's first statement.
    REQUIRE(s.submit("int w = step(4); step(5);"));
    CHECK(*log_v == 12345);
    CHECK(*(int *)s.data("w") == 4);
}

TEST_CASE("a top-level := declares a session global, and defer runs at the entry's end")
{
    InteractiveSession s;
    REQUIRE(s.begin());
    REQUIRE(s.submit("int log_v = 0;\n"
		     "int step(int d) { log_v = log_v * 10 + d; return d; }"));
    int *log_v = (int *)s.data("log_v");
    REQUIRE(log_v != (int *)NULL);
    // D25: an entry's declarations persist; `:=` is one.
    REQUIRE(s.submit("step(1); n := step(2) + 5;"));
    CHECK(*log_v == 12);
    REQUIRE(s.submit("int get_n(void) { return n; }"));
    int_fn get_n = (int_fn)s.function("_Z5get_nv");
    REQUIRE(get_n != (int_fn)NULL);
    CHECK(get_n() == 7);
    REQUIRE(s.submit("n = n * 3;"));
    CHECK(get_n() == 21);
    // `defer` binds to the entry's run: it runs when the entry ends.
    REQUIRE(s.submit("defer step(9); step(8);"));
    CHECK(*log_v == 1289);
}

TEST_CASE("call statements run under a C standard too (D3)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    REQUIRE(s.submit("int g = 5;\nint bump(int a) { g += a; return g; }"));
    REQUIRE(s.submit("bump(1); bump(2);"));
    CHECK(*(int *)s.data("g") == 8);
}

// A cast statement is an expression statement, whatever the cast: an
// entry's `(void)f();` used to be dropped under every C and C++ standard,
// and a named cast under madc too. Oracle: the same statements in a
// function body, g++ and clang++.
TEST_CASE("a cast statement runs at an entry's top level, under every standard")
{
    const char *stds[] = { "--std=c89", "--std=c17", "--std=c++17", "--std=madc" };
    for ( size_t i = 0; i < sizeof(stds) / sizeof(stds[0]); ++i )
    {
	std::string std_option = stds[i];
	CAPTURE(std_option);
	InteractiveSession s;
	REQUIRE(s.begin(std_option));
	REQUIRE(s.submit("int log_v = 0;\n"
			 "int step(int d) { log_v = log_v * 10 + d; return d; }"));
	int *log_v = (int *)s.data("log_v");
	REQUIRE(log_v != (int *)NULL);
	REQUIRE(s.submit("(void)step(1); (long)step(2);"));
	CHECK(*log_v == 12);
	if ( i < 2 )
	    continue;			// the named casts are C++'s
	REQUIRE(s.submit("static_cast<void>(step(3));"));
	CHECK(*log_v == 123);
    }
}
