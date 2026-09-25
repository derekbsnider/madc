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

TEST_CASE("an entry the first slice refuses says so, and the session goes on")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    REQUIRE(s.submit("int g = 5;\nint f(int a) { return a + g; }"));

    // A statement lowers into D25's entry function (slice 2).
    CHECK_FALSE(s.submit("f(1);"));
    CHECK(first_error(s).find("statement in an interactive entry") != std::string::npos);

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
