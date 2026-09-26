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

// The first error the refused entry recorded, or NULL when it has none.
const ::Program::Diagnostic *first_error_diagnostic(InteractiveSession &s)
{
    return s.program().first_error_diagnostic();
}

// Its message, or "" when it has none.
std::string first_error(InteractiveSession &s)
{
    const ::Program::Diagnostic *d = first_error_diagnostic(s);
    return d ? d->message : std::string();
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

// The entry transaction's JIT half (plan §41.3). An entry whose module cannot
// link is refused before it loads, with the linker's diagnostic, and the live
// context stays as the earlier entries left it. The refused entry's run and
// global are not live, and no later module defines them. The failed module
// used to stay loaded, so every later entry was refused, with no diagnostic.
// Oracle: clang-repl-18 and -20 (tmp/repl/s2b/linkfail.repl) report "Symbols
// not found: [ _Z1fv ]" and go on; a later `int k = 3;` gives 3, and once f
// is defined, `f()` gives 7.
TEST_CASE("an entry that cannot link is refused, and the session goes on (§41.3)")
{
    const char *stds[] = { "--std=c89", "--std=c17", "--std=c++17", "--std=madc" };
    for ( size_t i = 0; i < sizeof(stds) / sizeof(stds[0]); ++i )
    {
	std::string std_option = stds[i];
	CAPTURE(std_option);
	InteractiveSession s;
	REQUIRE(s.begin(std_option));
	REQUIRE(s.submit("int f(void);"));
	CHECK_FALSE(s.submit("int z = 5;\nf();"));
	const ::Program::Diagnostic *d = first_error_diagnostic(s);
	REQUIRE(d != (const ::Program::Diagnostic *)NULL);
	// ld's words; a C++ symbol demangled, as ld shows it.
	CHECK(d->message == (i < 2 ? "undefined reference to 'f'"
				   : "undefined reference to 'f()'"));
	CHECK(d->phase == ::Program::DiagnosticPhase::compiler);
	CHECK(d->file == "REPL[2]");
	CHECK(s.data("z") == (void *)NULL);	// never loaded
	CHECK(s.entries() == 1);

	REQUIRE(s.submit("int k = 3;"));
	CHECK(*(int *)s.data("k") == 3);
	REQUIRE(s.submit("int f(void) { return 7; }"));
	REQUIRE(s.submit("int r = 0;\nr = f();"));
	CHECK(*(int *)s.data("r") == 7);
	CHECK(s.entries() == 4);
    }
}

// Entry N is REPL[N] for the Nth entry submitted, refused ones counted, as in
// Julia's REPL[N] and IPython's In [N]. The entry after a refused one used to
// take the refused one's name, so two entries' diagnostics cited REPL[2].
TEST_CASE("every submitted entry has its own number, a refused one's included")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    REQUIRE(s.submit("int g = 1;"));
    CHECK_FALSE(s.submit("int x = ;"));
    const ::Program::Diagnostic *d = first_error_diagnostic(s);
    REQUIRE(d != (const ::Program::Diagnostic *)NULL);
    CHECK(d->file == "REPL[2]");
    CHECK_FALSE(s.submit("g = ;"));
    d = first_error_diagnostic(s);
    REQUIRE(d != (const ::Program::Diagnostic *)NULL);
    CHECK(d->file == "REPL[3]");
    REQUIRE(s.submit("g = 2;"));
    CHECK(s.submitted() == 4);
    CHECK(s.entries() == 2);
}

// A refused entry's definitions stay out of every later module, whatever
// refused it (plan §41.3): its parse, or its translation. Before, the next
// entry's module defined them itself: a function written before a parse
// error came alive there, and a C global whose initializer c2mir refuses
// (gcc: "initializer element is not constant") refused every later entry.
TEST_CASE("a refused entry's definitions never come alive later (§41.3)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    REQUIRE(s.submit("int f(void) { return 7; }"));

    CHECK_FALSE(s.submit("int h(void) { return 1; }\nint x = ;"));
    CHECK_FALSE(s.submit("int r = f();"));
    CHECK(first_error_diagnostic(s) != (const ::Program::Diagnostic *)NULL);

    REQUIRE(s.submit("int k = 0;\nk = f() - 4;"));
    CHECK(*(int *)s.data("k") == 3);
    CHECK(s.function("h") == (void *)NULL);
    CHECK(s.data("r") == (void *)NULL);
    CHECK(s.entries() == 2);
}

// C89's call of an undeclared function is an implicit declaration: the entry
// is refused at link, as clang-repl-20 -xc -std=c89 refuses it ("Symbols not
// found: [ f ]", tmp/repl/s2b/c89.repl), and defining f later lets a call
// link (g = 7).
TEST_CASE("an implicitly declared function links once it is defined (c89)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c89"));
    REQUIRE(s.submit("int g = 0;"));
    CHECK_FALSE(s.submit("f();"));
    CHECK(first_error(s) == "undefined reference to 'f'");
    REQUIRE(s.submit("int f(void) { return 7; }"));
    REQUIRE(s.submit("g = f();"));
    CHECK(*(int *)s.data("g") == 7);
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

// D3: top-level statements are an interactive relaxation under every
// standard, not only the madc dialect's. A declared name's `x = 3;` is an
// assignment in a session, also under a K&R-era C standard, where gcc's file
// scope would read a redeclaration (`int x = 3`). Oracle: the same
// statements in a function body, gcc -std=c89/c99/c17 and g++ -std=c++17.
TEST_CASE("top-level statements run under every standard (D3)")
{
    const char *stds[] = { "--std=c89", "--std=c99", "--std=c17", "--std=c++17" };
    for ( size_t i = 0; i < sizeof(stds) / sizeof(stds[0]); ++i )
    {
	std::string std_option = stds[i];
	CAPTURE(std_option);
	InteractiveSession s;
	REQUIRE(s.begin(std_option));
	REQUIRE(s.submit("int x = 10;\nint bump(int a) { x += a; return x; }"));
	int *x = (int *)s.data("x");
	REQUIRE(x != (int *)NULL);
	REQUIRE(s.submit("x = 3;"));
	CHECK(*x == 3);
	REQUIRE(s.submit("bump(1); bump(2);"));
	CHECK(*x == 6);
	REQUIRE(s.submit("if (x > 5) x = 100; else x = 0;"));
	CHECK(*x == 100);
	REQUIRE(s.submit("{ int t = 5; x = x + t; }"));
	CHECK(*x == 105);
	REQUIRE(s.submit("while (x > 100) x--;"));
	CHECK(*x == 100);
	REQUIRE(s.submit("switch (x) { case 100: x = 1; break; default: x = 2; }"));
	CHECK(*x == 1);
	REQUIRE(s.submit("done: x += 40;"));
	CHECK(*x == 41);
    }
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

// A delete-expression statement is an expression statement too; it used to
// be dropped, so the destructor never ran (log 1).
TEST_CASE("a delete statement runs at an entry's top level")
{
    const char *stds[] = { "--std=c++17", "--std=madc" };
    for ( size_t i = 0; i < sizeof(stds) / sizeof(stds[0]); ++i )
    {
	std::string std_option = stds[i];
	CAPTURE(std_option);
	InteractiveSession s;
	REQUIRE(s.begin(std_option));
	REQUIRE(s.submit("int log_v = 0;\n"
			 "int step(int d) { log_v = log_v * 10 + d; return d; }"));
	REQUIRE(s.submit("struct T { int a; ~T(); };\n"
			 "T::~T() { step(9); }"));
	REQUIRE(s.submit("T *tp = new T;\n"
			 "step(1);\n"
			 "delete tp;"));
	CHECK(*(int *)s.data("log_v") == 19);
    }
}

// Slice 3 (plan §41.2a): C++ vague linkage. Each entry's module is a whole
// translation unit, so it emits its own copy of every linkonce definition it
// needs: a class's C2/D2 aliases, its synthesized and deleting destructors,
// its vtable and type_info. MIR's loader keeps the first copy and binds the
// rest to it, as ld keeps the first COMDAT copy. Before, a later entry's func
// copy refused the entry ("multiple definition of 'D::D(int)'"), and its data
// copy took the name over, so an object built in one entry and a typeid in
// another saw two type_infos (typeid(*bp) == typeid(C) was false). Oracle:
// clang-repl-18 and -20 (tmp/repl/s3/vague.repl) give kd=5 kd2=6 kh=1 okc=1
// same=1 ks=4 kdc=1 okpc=1.
TEST_CASE("a later entry shares the live copy of a vague-linkage definition (slice 3)")
{
    const char *stds[] = { "--std=c++17", "--std=madc" };
    for ( size_t i = 0; i < sizeof(stds) / sizeof(stds[0]); ++i )
    {
	std::string std_option = stds[i];
	CAPTURE(std_option);
	InteractiveSession s;
	REQUIRE(s.begin(std_option));
	REQUIRE(s.submit("#include <typeinfo>"));
	// A user constructor and destructor: their C2/D2 aliases.
	REQUIRE(s.submit("struct D { int v; D(int a) : v(a) {} ~D() {} };"));
	REQUIRE(s.submit("D d(5);\nint kd = d.v;"));
	REQUIRE(s.submit("int kd2 = d.v + 1;"));
	// A synthesized destructor.
	REQUIRE(s.submit("struct M { ~M() {} };\nstruct H { M m; };"));
	REQUIRE(s.submit("H h;"));
	REQUIRE(s.submit("int kh = 1;"));
	// One vtable and one type_info for the whole session.
	REQUIRE(s.submit("struct B { virtual int f() { return 1; } };\n"
			 "struct C : B { int f() { return 2; } };"));
	REQUIRE(s.submit("C c;\nB *bp = &c;"));
	REQUIRE(s.submit("int okc = dynamic_cast<C *>(bp) != 0;\n"
			 "int same = typeid(*bp) == typeid(C);"));
	// A virtual destructor: the deleting destructor.
	REQUIRE(s.submit("struct PB { virtual ~PB() {} };\n"
			 "struct PC : PB { int v; };"));
	REQUIRE(s.submit("PC pc;\nPB *pbp = &pc;"));
	REQUIRE(s.submit("int okpc = dynamic_cast<PC *>(pbp) != 0;"));
	// An out-of-line constructor and destructor.
	REQUIRE(s.submit("int dcount = 0;\n"
			 "struct S { int v; S(int a); ~S(); };\n"
			 "S::S(int a) : v(a) {}\n"
			 "S::~S() { dcount++; }"));
	REQUIRE(s.submit("int ks = 0;\n"
			 "void blk() { S s(4); ks = s.v; }"));
	REQUIRE(s.submit("blk();"));
	REQUIRE(s.submit("int kdc = dcount;"));

	const char *names[] = { "kd", "kd2", "kh", "okc", "same", "ks", "kdc", "okpc" };
	const int want[] = { 5, 6, 1, 1, 1, 4, 1, 1 };
	for ( size_t n = 0; n < sizeof(names) / sizeof(names[0]); ++n )
	{
	    CAPTURE(names[n]);
	    int *v = (int *)s.data(names[n]);
	    REQUIRE(v != (int *)NULL);
	    CHECK(*v == want[n]);
	}
    }
}
