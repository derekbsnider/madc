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
#include <sstream>
#include <stack>
#include <string>
#include <vector>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_session.h"
#include "../../src/madc_posix_io.h"

#include <cstdio>
#include <cstdlib>		// std::system: %build's executable runs
#include <cstring>
#include <sys/wait.h>		// WEXITSTATUS
#include <unistd.h>

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

    // A second definition is refused, as any redefinition is until D5/D6.
    CHECK_FALSE(s.submit("int g = 6;"));
    CHECK(first_error(s) == "redefinition of 'g'");

    REQUIRE(s.submit("int k(void) { return f(3); }"));
    int_fn k = (int_fn)s.function("k");
    REQUIRE(k != (int_fn)NULL);
    CHECK(k() == 8);
    CHECK(s.entries() == 2);
}

// An included header's internal-linkage definitions are each module's own
// copy, as each translation unit has its own (every entry's module is one);
// only a static the entry itself writes is a session name (one unit, plan
// §41.5a). Before, <string>,
// <cmath>, <cstdlib> and <algorithm> were refused for glibc's
// `static __inline` byte swaps, and <iostream> for its
// `static ios_base::Init __ioinit`. Oracle: clang-repl-18 and -20
// (tmp/repl/s4/hdr.repl) include all five and give r=7, then r2=9.
TEST_CASE("an included header's statics do not refuse the entry")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c++17"));
    REQUIRE(s.submit("#include <iostream>"));
    REQUIRE(s.submit("#include <cmath>"));
    REQUIRE(s.submit("#include <string>"));
    REQUIRE(s.submit("#include <cstdlib>"));
    REQUIRE(s.submit("#include <algorithm>"));
    REQUIRE(s.submit("double d = std::sqrt(16.0);"));
    REQUIRE(s.submit("int r = std::abs(-3) + (int)d;"));
    CHECK(*(int *)s.data("r") == 7);
    REQUIRE(s.submit("std::string s = \"ab\";"));
    REQUIRE(s.submit("int r2 = r + (int)(s + \"c\").size() + std::max(1, 2);"));
    CHECK(*(int *)s.data("r2") == 12);
    REQUIRE(s.submit("static int mine = r + 1;"));
    REQUIRE(s.submit("mine"));
    CHECK(s.shown() == "8");
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
// global are not live, and its Program half rolls them back. The failed module
// used to stay loaded, so every later entry was refused, with no diagnostic.
// A static initializer that takes the address of an object nothing defines is
// such an entry: its module's data names the object (D27 defers only a use in
// code). Once nd is defined, the same initializer links.
TEST_CASE("an entry that cannot link is refused, and the session goes on (§41.3)")
{
    const char *stds[] = { "--std=c89", "--std=c17", "--std=c++17", "--std=madc" };
    for ( size_t i = 0; i < sizeof(stds) / sizeof(stds[0]); ++i )
    {
	std::string std_option = stds[i];
	CAPTURE(std_option);
	InteractiveSession s;
	REQUIRE(s.begin(std_option));
	REQUIRE(s.submit("extern int nd;"));
	CHECK_FALSE(s.submit("int z = 5;\nint *pnd = &nd;"));
	const ::Program::Diagnostic *d = first_error_diagnostic(s);
	REQUIRE(d != (const ::Program::Diagnostic *)NULL);
	// ld's words.
	CHECK(d->message == "undefined reference to 'nd'");
	CHECK(d->phase == ::Program::DiagnosticPhase::compiler);
	CHECK(d->file == "REPL[2]");
	CHECK(s.data("z") == (void *)NULL);	// never loaded
	CHECK(s.entries() == 1);

	REQUIRE(s.submit("int k = 3;"));
	CHECK(*(int *)s.data("k") == 3);
	REQUIRE(s.submit("int nd = 7;"));
	REQUIRE(s.submit("int *pnd = &nd;"));
	CHECK(**(int **)s.data("pnd") == 7);
	CHECK(s.entries() == 4);
    }
}

// D27 (owner, 2026-09-26): an entry that names a function no entry defines yet
// is accepted and links, and the refusal waits for the function's first use,
// as in Julia and clang-repl. The entry links against a stub. A later
// definition replaces it and takes its address, so a call, a function pointer
// or a vtable slot bound earlier reaches the definition. A use that comes
// first fails when it runs, with ld's words, and returns to the entry's
// boundary. The entry stays linked and keeps its definitions, as Julia keeps
// `z` after `z = 5; f()`. The failure is no C++ exception: `catch (...)` does
// not see it, but an object the run constructed in a `try` is destroyed, and
// the next `try` works. Oracle: clang-repl-18 and -20 (tmp/repl/s5/d27a.repl)
// give calls_h=11 and f=7. They fail `int z = 5; f();` at its entry with
// "Symbols not found: [ _Z1fv ]" and leave z unusable, which madc does not
// copy (plan §42 D27).
TEST_CASE("a function no entry defines yet is refused at its first use (D27)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c++17"));
    REQUIRE(s.submit("int f();"));
    CHECK_FALSE(s.submit("int z = 5;\nf();"));
    const ::Program::Diagnostic *d = first_error_diagnostic(s);
    REQUIRE(d != (const ::Program::Diagnostic *)NULL);
    CHECK(d->message == "undefined reference to 'f()'");
    CHECK(d->phase == ::Program::DiagnosticPhase::runtime);
    CHECK(d->file == "REPL[2]");
    CHECK(*(int *)s.data("z") == 5);	// linked: kept
    CHECK(s.entries() == 2);
    REQUIRE(s.submit("int f() { return 7; }"));
    REQUIRE(s.submit("int kf = f();"));
    CHECK(*(int *)s.data("kf") == 7);

    // A body, a function pointer and a vtable slot bound before the
    // definition reach it, and the function has one address.
    REQUIRE(s.submit("int h();"));
    REQUIRE(s.submit("int calls_h() { return h() + 1; }"));
    REQUIRE(s.submit("int (*fp)() = h;"));
    REQUIRE(s.submit("struct V { virtual int g(); int n; };"));
    REQUIRE(s.submit("V *mk() { V *v = new V; v->n = 3; return v; }"));
    REQUIRE(s.submit("int h() { return 11; }"));
    REQUIRE(s.submit("int V::g() { return n * 2; }"));
    REQUIRE(s.submit("int kh = calls_h() + 100 * (fp == h) + 1000 * mk()->g();"));
    CHECK(*(int *)s.data("kh") == 6112);

    // A dynamic initializer's use stops the entry's init; the entry is kept.
    REQUIRE(s.submit("int f2();"));
    CHECK_FALSE(s.submit("int zz = 5; int bad = f2();"));
    CHECK(first_error(s) == "undefined reference to 'f2()'");
    CHECK(*(int *)s.data("zz") == 5);
    CHECK(*(int *)s.data("bad") == 0);

    // No `catch` sees it; the try's object is destroyed once.
    REQUIRE(s.submit("int dtor_ran = 0, caught = 0;"));
    REQUIRE(s.submit("struct D { ~D() { dtor_ran += 1; } };"));
    REQUIRE(s.submit("int f3();"));
    CHECK_FALSE(s.submit("try { D d; f3(); } catch (...) { caught = 1; }"));
    CHECK(*(int *)s.data("caught") == 0);
    CHECK(*(int *)s.data("dtor_ran") == 1);
    REQUIRE(s.submit("try { D e; throw 4; } catch (int x) { caught = x; }"));
    CHECK(*(int *)s.data("caught") == 4);
    CHECK(*(int *)s.data("dtor_ran") == 2);

    // Mutual recursion across entries, and an inline definition, which the
    // entry that uses it emits as a linkonce copy, replacing the stub.
    REQUIRE(s.submit("int ev(int n);"));
    REQUIRE(s.submit("int od(int n) { return n == 0 ? 0 : ev(n - 1); }"));
    REQUIRE(s.submit("int ev(int n) { return n == 0 ? 1 : od(n - 1); }"));
    REQUIRE(s.submit("struct S { int f(); };"));
    REQUIRE(s.submit("int gs(S s) { return s.f(); }"));
    REQUIRE(s.submit("inline int S::f() { return 3; }"));
    REQUIRE(s.submit("int ko = od(3) * 10 + od(4) + 100 * (S().f() + gs(S()));"));
    CHECK(*(int *)s.data("ko") == 610);

    // An inline body a stub waits for is emitted by the entry that defines
    // it, used there or not, as Julia's late binding reaches it. clang-repl-20
    // fails gt(T()) ("Symbols not found: [ _ZN1T1fEv ]",
    // tmp/repl/s5/d27i.repl): it emits no body its own input does not use.
    REQUIRE(s.submit("struct T { int f(); };"));
    REQUIRE(s.submit("int gt(T t) { return t.f(); }"));
    REQUIRE(s.submit("inline int T::f() { return 4; }"));
    REQUIRE(s.submit("int h3();"));
    REQUIRE(s.submit("int c3() { return h3(); }"));
    REQUIRE(s.submit("inline int h3() { return 8; }"));
    REQUIRE(s.submit("int kt = gt(T()) + 10 * c3();"));
    CHECK(*(int *)s.data("kt") == 84);
}

// D27, slice 2: code that names an object an entry declared and nothing
// defines yet reads it through a session cell, so the later definition is the
// object it reaches, by value, by write and by address. A read that comes
// first fails when it runs. A static initializer that takes the object's
// address is that entry's own use, so its link still refuses it. Oracle:
// clang-repl-18 and -20 (tmp/repl/s5/d27a.repl) give g2=4 for the same
// entries. They accept `int *pdv = &dv;` too, but only because ORC links that
// module at pdv's first read (plan §42 D27).
TEST_CASE("an object no entry defines yet is refused at its first use (D27)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c++17"));
    REQUIRE(s.submit("extern int later2;"));
    REQUIRE(s.submit("int g2() { return later2; }"));
    REQUIRE(s.submit("void set2(int v) { later2 = v; }"));
    REQUIRE(s.submit("int *addr2() { return &later2; }"));
    CHECK_FALSE(s.submit("int k1 = g2();"));
    const ::Program::Diagnostic *d = first_error_diagnostic(s);
    REQUIRE(d != (const ::Program::Diagnostic *)NULL);
    CHECK(d->message == "undefined reference to 'later2'");
    CHECK(d->phase == ::Program::DiagnosticPhase::runtime);
    CHECK(*(int *)s.data("k1") == 0);	// linked: kept
    REQUIRE(s.submit("int later2 = 4;"));
    REQUIRE(s.submit("int k2 = g2();"));
    CHECK(*(int *)s.data("k2") == 4);
    REQUIRE(s.submit("set2(9);"));
    CHECK(*(int *)s.data("later2") == 9);
    REQUIRE(s.submit("int same = (addr2() == &later2) + 10 * *addr2();"));
    CHECK(*(int *)s.data("same") == 91);

    // A class's static data member, defined after a member function that
    // reads it.
    REQUIRE(s.submit("struct S { static int count; int f(); };"));
    REQUIRE(s.submit("int S::f() { return count + 1; }"));
    CHECK_FALSE(s.submit("int ks0 = S().f();"));
    CHECK(first_error(s) == "undefined reference to 'S::count'");
    REQUIRE(s.submit("int S::count = 5;"));
    REQUIRE(s.submit("int ks = S().f();"));
    CHECK(*(int *)s.data("ks") == 6);

    REQUIRE(s.submit("extern int nd;"));
    CHECK_FALSE(s.submit("int *pnd = &nd;"));
    d = first_error_diagnostic(s);
    REQUIRE(d != (const ::Program::Diagnostic *)NULL);
    CHECK(d->message == "undefined reference to 'nd'");
    CHECK(d->phase == ::Program::DiagnosticPhase::compiler);
}

TEST_CASE("a function no entry defines yet is refused at its first use (D27, C)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    REQUIRE(s.submit("int f(void);"));
    CHECK_FALSE(s.submit("int z = 5;\nf();"));
    CHECK(first_error(s) == "undefined reference to 'f'");
    CHECK(*(int *)s.data("z") == 5);
    REQUIRE(s.submit("int calls(void) { return f() + 1; }"));
    REQUIRE(s.submit("int (*fp)(void) = f;"));
    REQUIRE(s.submit("int f(void) { return 7; }"));
    REQUIRE(s.submit("int kc = 0;\nkc = calls() + 10 * (fp == f) + 100 * fp();"));
    CHECK(*(int *)s.data("kc") == 718);

    // The object half (slice 2), a struct's member access included.
    REQUIRE(s.submit("extern int later2;"));
    REQUIRE(s.submit("int g2(void) { return later2; }"));
    CHECK_FALSE(s.submit("int k1;\nk1 = g2();"));
    CHECK(first_error(s) == "undefined reference to 'later2'");
    REQUIRE(s.submit("int later2 = 4;"));
    REQUIRE(s.submit("struct P { int a, b; };\nextern struct P pp;"));
    REQUIRE(s.submit("int sum(void) { return pp.a + pp.b; }"));
    REQUIRE(s.submit("struct P pp = { 2, 3 };"));
    REQUIRE(s.submit("int k2;\nk2 = g2() + 10 * sum();"));
    CHECK(*(int *)s.data("k2") == 54);
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
// The Program rolls the refused entry back, so a corrected h is h's first
// definition.
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

    REQUIRE(s.submit("int h(void) { return 2; }"));
    REQUIRE(s.submit("int r = 0;\nr = h() + f();"));
    CHECK(*(int *)s.data("r") == 9);
}

// A refused entry is one unit (plan §41.3): whatever refused it (its parse,
// its translation or its link), nothing it declared or defined is left
// behind. A later entry may declare the same name again, as another kind,
// and a corrected definition is not a redefinition. Julia leaves nothing of
// an input that fails to parse, and cling rolls a refused input back whole;
// cling gives these values (tmp/repl/s4/rb.repl, inplace.repl). clang-repl-20
// rolls declarations back too, but keeps a refused macro and include guard,
// leaves a rolled-back namespace's symbol behind ("definition with same
// mangled name"), drops the earlier declaration a refused definition named,
// and keeps a refused out-of-line member defined; madc copies none of that.
namespace {

struct RollbackCase
{
    const char *setup;		// an earlier, accepted entry ("" for none)
    const char *refused;	// declares or defines, then is refused
    const char *again;		// valid only if the refused entry left nothing
    const char *check;		// sets the global `k`
    int want;
};

void check_rollback(const std::string &std_option, const RollbackCase &c)
{
    CAPTURE(std_option);
    CAPTURE(c.refused);
    InteractiveSession s;
    REQUIRE(s.begin(std_option));
    if ( *c.setup )
	REQUIRE(s.submit(c.setup));
    CHECK_FALSE(s.submit(c.refused));
    CHECK(first_error_diagnostic(s) != (const ::Program::Diagnostic *)NULL);
    CHECK(s.submit(c.again));
    CAPTURE(first_error(s));
    REQUIRE(s.submit(c.check));
    int *k = (int *)s.data("k");
    REQUIRE(k != (int *)NULL);
    CHECK(*k == c.want);
}

} // namespace

TEST_CASE("a refused entry leaves nothing behind (§41.3)")
{
    static const RollbackCase cxx[] = {
	{ "", "int z = 1; int bad = undeclared_a; int w = 2;",
	  "double z = 2.5; int w = 3;", "int k = (int)(z * 2) + w;", 8 },
	{ "", "int h() { return 1; } int bad = undeclared_b;",
	  "double h = 2.0;", "int k = (int)h;", 2 },
	{ "", "struct P { int x; }; int bad = undeclared_c;",
	  "struct P { double y; };", "int k = sizeof(P);", 8 },
	{ "struct F;", "struct F { int a; }; int bad = undeclared_d;",
	  "struct F { double a, b; };", "int k = sizeof(F);", 16 },
	{ "", "namespace Q { int v = 1; } int bad = undeclared_e;",
	  "namespace Q { double v = 2.5; }", "int k = (int)(Q::v * 2);", 5 },
	{ "", "template<class T> T id(T x) { return x; } int bad = undeclared_f;",
	  "int id = 3;", "int k = id;", 3 },
	{ "", "template<class T> struct Box { T v; }; int bad = undeclared_g;",
	  "int Box = 4;", "int k = Box;", 4 },
	{ "", "enum E { A, B }; int bad = undeclared_h;",
	  "int A = 5;", "int k = A;", 5 },
	{ "", "typedef int T1; int bad = undeclared_i;",
	  "double T1 = 1.5;", "int k = (int)(T1 * 2);", 3 },
	{ "", "using U1 = long; int bad = undeclared_j;",
	  "double U1 = 1.5;", "int k = (int)(U1 * 2);", 3 },
	{ "", "#define N 4\nint bad = undeclared_k;",
	  "int N = 9;", "int k = N;", 9 },
	{ "", "#include <climits>\nint bad = undeclared_l;",
	  "#include <climits>\nint lim = INT_MAX > 0;", "int k = lim;", 1 },
	{ "", "#include <cmath>\nint bad = undeclared_s;",
	  "#include <cmath>\ndouble sq = std::sqrt(16.0);", "int k = (int)sq;", 4 },
	{ "int decl_only();", "int decl_only() { return 3; } int bad = undeclared_m;",
	  "int decl_only() { return 4; }", "int k = decl_only();", 4 },
	{ "struct S { int f(); static int count; };",
	  "int S::f() { return 2; } int S::count = 1; int bad = undeclared_n;",
	  "int S::f() { return 5; } int S::count = 6;",
	  "int k = S().f() + S::count;", 11 },
	{ "int ov(int a) { return a; }",
	  "double ov(double a) { return a * 2; } int bad = undeclared_o;",
	  "double ov(double a) { return a * 3; }", "int k = (int)(ov(1.5) * 2);", 9 },
	{ "extern int ev;", "int ev = 7; int bad = undeclared_p;",
	  "int ev = 8;", "int k = ev;", 8 },
	{ "", "auto lam = [](int a) { return a + 1; }; int bad = undeclared_q;",
	  "int lam = 6;", "int k = lam;", 6 },
	// Each instantiation is a class journal of its own, nested in the
	// entry's: committed into it, then rolled back with it.
	{ "template<class T> struct Pair { T a; T b; };",
	  "Pair<int> pa; Pair<double> pb; Pair<char> pc; int bad = undeclared_r;",
	  "Pair<char> pd; double pa = 1.5;",
	  "int k = (int)sizeof(Pair<char>) + (int)(pa * 2);", 5 },
	// Refused at its link: its data takes the address of nd, which is
	// declared, never defined (a use in code waits for its run, D27).
	{ "extern int nd;", "int lz = 5;\nint *pnd = &nd;",
	  "double lz = 1.5;", "int k = (int)(lz * 2);", 3 },
    };
    for ( size_t i = 0; i < sizeof(cxx) / sizeof(cxx[0]); ++i )
	check_rollback("--std=c++17", cxx[i]);

    // C's file-scope initializers are constant: a check that reads a global
    // assigns it in the entry's run.
    static const RollbackCase c[] = {
	{ "", "int z = 1; int bad = undeclared_a; int w = 2;",
	  "double z = 2.5; int w = 3;", "int k = 0;\nk = (int)(z * 2) + w;", 8 },
	{ "", "int h(void) { return 1; } int bad = undeclared_b;",
	  "double h = 2.0;", "int k = 0;\nk = (int)h;", 2 },
	{ "", "struct P { int x; }; int bad = undeclared_c;",
	  "struct P { double y; };", "int k = sizeof(struct P);", 8 },
	{ "struct F;", "struct F { int a; }; int bad = undeclared_d;",
	  "struct F { double a, b; };", "int k = sizeof(struct F);", 16 },
	{ "", "enum E { A, B }; int bad = undeclared_h;",
	  "int A = 5;", "int k = 0;\nk = A;", 5 },
	{ "", "typedef int T1; int bad = undeclared_i;",
	  "double T1 = 1.5;", "int k = 0;\nk = (int)(T1 * 2);", 3 },
	{ "", "#define N 4\nint bad = undeclared_k;",
	  "int N = 9;", "int k = 0;\nk = N;", 9 },
	{ "", "#include <limits.h>\nint bad = undeclared_l;",
	  "#include <limits.h>\nint lim = INT_MAX > 0;", "int k = 0;\nk = lim;", 1 },
	{ "int decl_only(void);",
	  "int decl_only(void) { return 3; } int bad = undeclared_m;",
	  "int decl_only(void) { return 4; }", "int k = 0;\nk = decl_only();", 4 },
	{ "extern int ev;", "int ev = 7; int bad = undeclared_p;",
	  "int ev = 8;", "int k = 0;\nk = ev;", 8 },
    };
    for ( size_t i = 0; i < sizeof(c) / sizeof(c[0]); ++i )
	check_rollback("--std=c17", c[i]);
}

// An entry c2mir refuses leaves the session able to compile the next one
// (plan §41.6a, C). c2mir's context keeps what it read of a refused tree, so
// the tree's node arena lives as long as that context: freed, a later
// entry's nodes reused it and met the refused entry's symbols. After two such
// entries, every entry failed "tag P redeclaration".
TEST_CASE("entries c2mir refuses leave the session compiling (§41.6a)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    REQUIRE(s.submit("struct P { int x, y; };\nint g(void);"));
    // A file-scope initializer that is not constant: C refuses it.
    for ( int i = 0; i < 3; ++i )
	CHECK_FALSE(s.submit("int y" + std::to_string(i) + " = g();"));
    REQUIRE(s.submit("1"));
    CHECK(s.shown() == "1");
    REQUIRE(s.submit("struct P q = { 3, 4 };"));
    REQUIRE(s.submit("q.x + q.y"));
    CHECK(s.shown() == "7");
}


// C89's call of an undeclared function is an implicit declaration: the entry
// links, and its run fails at the call (D27), as clang-repl-20 -xc -std=c89
// fails it ("Symbols not found: [ f ]", tmp/repl/s2b/c89.repl). Defining f
// later lets a call reach it (g = 7).
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
// A later entry that names an auto-included header (`format`, `php::`,
// `println`): the lexer splices the header's tokens in front of the
// entry's, counting positions from cursor 0, and a session's stream had the
// earlier entries' consumed tokens in front of the cursor. The splice moved
// the entry's own tokens behind it: `format("now={}", total)` after
// `int total = 3;` read "redefinition of 'total'" (entry 1 re-parsed), and
// the next entries crashed in the balance stage. The same text as one file
// prints now=3, AB, now=3 (tmp/repl/d20s2/autoinc.mad), as g++ and clang++
// -std=c++20 give std::format("now={}", 3) == "now=3".
TEST_CASE("a later entry that names an auto-included header runs")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=madc"));
    REQUIRE(s.submit("int total = 3;"));
    REQUIRE(s.submit("format(\"now={}\", total)"));
    CHECK(s.shown() == "\"now=3\"");
    REQUIRE(s.submit("var up = php::strtoupper(\"ab\");"));
    REQUIRE(s.submit("up"));
    CHECK(s.shown() == "\"AB\"");
    REQUIRE(s.submit("println(\"now={}\", total);"));
    REQUIRE(s.submit("total + 1"));
    CHECK(s.shown() == "4");
}

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

// Two more synthesized destructors a later entry emits again: the complete-
// object destructor of a class with virtual bases, and the helper that
// destroys an array of class objects. Both were strong, so the next entry
// that needed one was refused as a multiple definition of 'J::~J()' or of
// 'E__arr3___dtor'. They are linkonce now, like the other synthesized
// destructors (g++ emits J::~J() weak). Oracle: clang-repl-18 and -20
// (tmp/repl/s3/synth2.repl) give kj=1 kjd=1 kda=6.
TEST_CASE("a later entry shares a synthesized complete-object or array destructor (slice 3)")
{
    const char *stds[] = { "--std=c++17", "--std=madc" };
    for ( size_t i = 0; i < sizeof(stds) / sizeof(stds[0]); ++i )
    {
	std::string std_option = stds[i];
	CAPTURE(std_option);
	InteractiveSession s;
	REQUIRE(s.begin(std_option));
	REQUIRE(s.submit("int dv = 0;\n"
			 "struct V { int x; ~V() { dv++; } };\n"
			 "struct L : virtual V {};\n"
			 "struct R : virtual V {};\n"
			 "struct J : L, R {};"));
	REQUIRE(s.submit("int kj = 0;\nvoid mk() { J j; kj = 1; }"));
	REQUIRE(s.submit("mk();"));
	REQUIRE(s.submit("int kjd = dv;"));
	REQUIRE(s.submit("int da = 0;\nstruct E { int v; ~E() { da++; } };"));
	REQUIRE(s.submit("void a1() { E e[3]; }"));
	REQUIRE(s.submit("void a2() { E f[3]; }"));
	REQUIRE(s.submit("a1();\na2();"));
	REQUIRE(s.submit("int kda = da;"));

	const char *names[] = { "kj", "kjd", "kda" };
	const int want[] = { 1, 1, 6 };
	for ( size_t n = 0; n < sizeof(names) / sizeof(names[0]); ++n )
	{
	    CAPTURE(names[n]);
	    int *v = (int *)s.data(names[n]);
	    REQUIRE(v != (int *)NULL);
	    CHECK(*v == want[n]);
	}
    }
}

// An entry's lexer knows what a file's lexer knows. A file lexes its whole
// text before any of it is parsed, so a class it declares reaches the parser
// as an identifier, resolved by lookup. A session lexes each entry after the
// earlier entries' declarations are registered, and the lexer read those: an
// earlier entry's class name arrived as a type-name, so every out-of-line
// member defined in a later entry was refused ("Expecting identifier after
// type": `int S::get() const`, `S::S(int)`, `S::~S()`, `int S::count = 5;`),
// and so was a local shadowing the name (`int T = 3;`) and `(int)E::B`.
// Oracle: clang-repl-18 and -20 (tmp/repl/s3/later.repl) give kg=40 dc=1
// kc=5 ks=4 ke=1.
TEST_CASE("a later entry defines an earlier entry's members, and may shadow its names")
{
    const char *stds[] = { "--std=c++17", "--std=madc" };
    for ( size_t i = 0; i < sizeof(stds) / sizeof(stds[0]); ++i )
    {
	std::string std_option = stds[i];
	CAPTURE(std_option);
	InteractiveSession s;
	REQUIRE(s.begin(std_option));
	REQUIRE(s.submit("int dc = 0;"));
	REQUIRE(s.submit("struct S { int v; static int count; S(int a); ~S();"
			 " int get() const; };"));
	REQUIRE(s.submit("int S::get() const { return v * 10; }"));
	REQUIRE(s.submit("S::S(int a) : v(a) {}"));
	REQUIRE(s.submit("S::~S() { dc++; }"));
	REQUIRE(s.submit("int S::count = 5;"));
	REQUIRE(s.submit("int kg = 0;\nvoid use() { S s(4); kg = s.get(); }"));
	REQUIRE(s.submit("use();"));
	REQUIRE(s.submit("int kc = S::count;"));
	REQUIRE(s.submit("struct T { int a; };"));
	REQUIRE(s.submit("int shadow() { int T = 3; return T + 1; }"));
	REQUIRE(s.submit("int ks = shadow();"));
	REQUIRE(s.submit("enum class E { A, B };"));
	REQUIRE(s.submit("int ke = (int)E::B;"));

	const char *names[] = { "kg", "dc", "kc", "ks", "ke" };
	const int want[] = { 40, 1, 5, 4, 1 };
	for ( size_t n = 0; n < sizeof(names) / sizeof(names[0]); ++n )
	{
	    CAPTURE(names[n]);
	    int *v = (int *)s.data(names[n]);
	    REQUIRE(v != (int *)NULL);
	    CHECK(*v == want[n]);
	}
    }
}

// At an entry's top level, a qualified expression is a statement, as it is
// in a body: `S::count = 3;`, `S::bump();`, `S::count += 10;`. The file-scope
// reader of `Class::` knew only a constructor or destructor definition there
// and refused the rest ("Qualified member definition requires a return
// type"), which a file's top level rightly does. Oracle: clang-repl-18 and
// -20 (tmp/repl/s3/inl.repl) give kc=14 kv=6.
TEST_CASE("a qualified expression is a statement at an entry's top level")
{
    const char *stds[] = { "--std=c++17", "--std=madc" };
    for ( size_t i = 0; i < sizeof(stds) / sizeof(stds[0]); ++i )
    {
	std::string std_option = stds[i];
	CAPTURE(std_option);
	InteractiveSession s;
	REQUIRE(s.begin(std_option));
	REQUIRE(s.submit("struct S { static int count; static int bump();"
			 " S(int a); int v; };"));
	REQUIRE(s.submit("int S::count = 0;"));
	REQUIRE(s.submit("int S::bump() { return ++count; }"));
	REQUIRE(s.submit("S::count = 3;"));
	REQUIRE(s.submit("S::bump();"));
	REQUIRE(s.submit("S::count += 10;"));
	REQUIRE(s.submit("S::S(int a) : v(a) {}"));	// still a definition
	REQUIRE(s.submit("int kc = S::count;"));
	REQUIRE(s.submit("int kv = 0;\nvoid mk() { S s(6); kv = s.v; }"));
	REQUIRE(s.submit("mk();"));
	CHECK(*(int *)s.data("kc") == 14);
	CHECK(*(int *)s.data("kv") == 6);
    }
}

// An entry emits a vague-linkage body (in-class, `inline`) only where it is
// used, as every C++ TU does. Emitted eagerly, an unused inline member that
// names a static member a later entry defines refused its own entry
// ("undefined reference to 'S::count'"), and so did an inline function naming
// an extern defined later. A strong definition is still emitted whole; since
// D27 its read of an object defined later goes through a session cell, so it
// is accepted too. Oracle: clang-repl-18 and -20 (tmp/repl/s3/inl.repl,
// ext.repl) accept both entries and give kc=14 and r=4.
TEST_CASE("an entry emits an inline body only where it is used")
{
    const char *stds[] = { "--std=c++17", "--std=madc" };
    for ( size_t i = 0; i < sizeof(stds) / sizeof(stds[0]); ++i )
    {
	std::string std_option = stds[i];
	CAPTURE(std_option);
	InteractiveSession s;
	REQUIRE(s.begin(std_option));
	REQUIRE(s.submit("struct S { static int count;"
			 " static int bump() { return ++count; } };"));
	REQUIRE(s.submit("int S::count = 0;"));
	REQUIRE(s.submit("S::count = 3;\nS::bump();\nS::count += 10;"));
	REQUIRE(s.submit("int kc = S::count;"));
	CHECK(*(int *)s.data("kc") == 14);

	REQUIRE(s.submit("extern int later;\ninline int f() { return later; }"));
	REQUIRE(s.submit("int later = 4;"));
	REQUIRE(s.submit("int r = f();"));
	CHECK(*(int *)s.data("r") == 4);

	REQUIRE(s.submit("extern int later2;\nint g2() { return later2; }"));
	REQUIRE(s.submit("int later2 = 5;"));
	REQUIRE(s.submit("int r2 = g2();"));
	CHECK(*(int *)s.data("r2") == 5);
    }
}

// §41.4 result capture (D10, plan §41.4a): an entry whose final statement
// omits its `;` shows its value, and with the `;` it shows nothing. The value
// is spelled so that the text, entered again, yields the value. That rule is
// the oracle: the installed clang-repl-18 and -20 print "Not implement yet."
// for a value, and cling and Julia are not installed. So each shown text is
// entered again below, in `same`, where every `@` stands for it, and the
// comparison must hold.
namespace {

void check_reenters(InteractiveSession &s, const std::string &decl,
		    const char *want, std::string same)
{
    CAPTURE(decl);
    REQUIRE(s.submit(decl));
    const std::string shown = s.shown();
    CAPTURE(shown);
    REQUIRE(!shown.empty());
    if ( want )
	CHECK(shown == want);
    for ( size_t at = same.find('@'); at != std::string::npos;
	  at = same.find('@', at + shown.size()) )
	same.replace(at, 1, shown);
    CAPTURE(same);
    REQUIRE(s.submit("k = " + same + ";"));
    CHECK(*(int *)s.data("k") == 1);
}

} // namespace

TEST_CASE("an entry without its final ; shows its value, re-enterably (D10)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c++17"));
    REQUIRE(s.submit("#include <cstring>\n#include <cmath>\nint k = 0;"));
    CHECK(s.shown().empty());
    REQUIRE(s.submit("int x = 15"));
    CHECK(s.shown() == "15");
    REQUIRE(s.submit("x * 2"));
    CHECK(s.shown() == "30");
    REQUIRE(s.submit("x * 2;"));
    CHECK(s.shown().empty());

    check_reenters(s, "unsigned long long v1 = 18446744073709551615ull",
		   "18446744073709551615u", "(@) == v1");
    check_reenters(s, "double v2 = 1.0 / 3", "0.3333333333333333", "(@) == v2");
    check_reenters(s, "float v3 = 2.5f / 3", "0.8333333f", "(@) == v3");
    check_reenters(s, "long double v4 = 1.0L / 3", NULL, "(@) == v4");
    check_reenters(s, "double v5 = 1e100", "1e+100", "(@) == v5");
    // -0.0's sign, without std::signbit (B23's family misreads the `&&`
    // after a <cmath> call).
    check_reenters(s, "double v6 = -0.0", "-0.0", "1 / (@) < 0 && (@) == 0");
    check_reenters(s, "double v7 = 1.0 / 0.0", "INFINITY", "(@) == v7");
    check_reenters(s, "bool v8 = true", "true", "(@) == v8");
    check_reenters(s, "char v9 = '\\''", "'\\''", "(@) == v9");
    check_reenters(s, "char v10 = '\\x01'", "'\\001'", "(@) == v10");
    check_reenters(s, "const char *v11 = \"a\\\"b\\n\\t\\\\\"",
		   "\"a\\\"b\\n\\t\\\\\"", "std::strcmp(@, v11) == 0");
    // A well-formed UTF-8 sequence is text, written as itself (Julia and
    // Python show "été"); a byte outside one stays octal: a lone 0xff, a
    // stray continuation, an overlong '/', a surrogate's encoding.
    check_reenters(s, "const char *v11u = \"\xc3\xa9t\xc3\xa9 \xe6\x97\xa5 \xf0\x9f\x8c\x80\"",
		   "\"\xc3\xa9t\xc3\xa9 \xe6\x97\xa5 \xf0\x9f\x8c\x80\"",
		   "std::strcmp(@, v11u) == 0");
    check_reenters(s, "const char *v11b = \"\\xff\\x80\\xc0\\xaf\\xed\\xa0\\x80\"",
		   "\"\\377\\200\\300\\257\\355\\240\\200\"",
		   "std::strcmp(@, v11b) == 0");
    check_reenters(s, "int *v12 = &x", NULL, "(@) == v12");
    check_reenters(s, "int *v13 = nullptr", "(int *) nullptr", "(@) == v13");
    REQUIRE(s.submit("enum E { A, B = 5 };"));
    check_reenters(s, "E v14 = B", "E::B", "(@) == v14");
    check_reenters(s, "E v15 = (E)3", "(E) 3", "(@) == v15");
    REQUIRE(s.submit("enum class Color { Red, Green };"));
    check_reenters(s, "Color v16 = Color::Green", "Color::Green", "(@) == v16");
    REQUIRE(s.submit("struct P { int a; };"));
    check_reenters(s, "P *v17 = nullptr", "(P *) nullptr", "(@) == v17");
    check_reenters(s, "int (*v18)(int) = nullptr", "(int (*)(int)) nullptr",
		   "(@) == v18");

    // A void expression shows nothing; a use of an undefined function (D27)
    // stops the run, and the entry shows nothing.
    REQUIRE(s.submit("void vf() {}"));
    REQUIRE(s.submit("vf()"));
    CHECK(s.shown().empty());
    REQUIRE(s.submit("int nf();"));
    CHECK_FALSE(s.submit("nf()"));
    CHECK(s.shown().empty());

    // Slice 2: an aggregate shows as designated initializers, with its type
    // at top level; nested members, a char array (text), an array member, a
    // pointer and an enum member; a call's result, materialized once.
    check_reenters(s, "P v19 = { 3 }", "P{ .a = 3 }", "(@).a == v19.a");
    REQUIRE(s.submit("struct Pt { double x, y; };"));
    REQUIRE(s.submit("struct Line { Pt a, b; int tag; };"));
    check_reenters(s, "Line v20 = { { 0.0, 1.0 }, { 2.0, 3.5 }, 7 }",
		   "Line{ .a = { .x = 0.0, .y = 1.0 }, .b = { .x = 2.0, .y = 3.5 },"
		   " .tag = 7 }",
		   "(@).b.y == v20.b.y && (@).tag == v20.tag");
    REQUIRE(s.submit("struct Named { char name[8]; int id[3]; E e; int *p; };"));
    check_reenters(s, "Named v21 = { \"abc\", { 1, 2, 3 }, B, nullptr }",
		   "Named{ .name = \"abc\", .id = { 1, 2, 3 }, .e = E::B,"
		   " .p = (int *) nullptr }",
		   "std::strcmp((@).name, v21.name) == 0 && (@).id[2] == 3"
		   " && (@).e == v21.e");
    REQUIRE(s.submit("int calls = 0;"));
    REQUIRE(s.submit("Pt mk() { ++calls; return Pt{ 9.0, 8.5 }; }"));
    REQUIRE(s.submit("mk()"));
    CHECK(s.shown() == "Pt{ .x = 9.0, .y = 8.5 }");
    CHECK(*(int *)s.data("calls") == 1);
    REQUIRE(s.submit("union U { int i; float f; };"));
    check_reenters(s, "U v22 = { 5 }", "U{ .i = 5 }", "(@).i == v22.i");
    // A C++ array shows as its initializer, which is not an expression.
    REQUIRE(s.submit("int v23[3] = { 4, 5, 6 }"));
    CHECK(s.shown() == "{ 4, 5, 6 }");
    REQUIRE(s.submit("char v24[4] = { 'a', 'b', 'c', 'd' }"));
    CHECK(s.shown() == "{ 'a', 'b', 'c', 'd' }");
}

// Slice 3 (the containers): a standard container shows its elements as a
// C++ expression that builds it again, `std::vector<int>{ 1, 2, 3 }` and
// `std::map<int,int>{ { 1, 10 } }`. A std::string shows as its text. A class
// is named without C's `struct` (`H{ .name = "hh" }`). A declaration whose
// type comes from a header still shows: its run is the entry's own.
TEST_CASE("an entry without its final ; shows a standard container (D10)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c++17"));
    REQUIRE(s.submit("#include <string>\n#include <vector>\n#include <map>\n"
		     "#include <set>\nint k = 0;"));
    check_reenters(s, "std::string v1 = \"ab\\\"c\"", "\"ab\\\"c\"", "v1 == @");
    // Compared element by element: std::vector's operator== is refused
    // (B31), and a container's equality reaches the same algorithm.
    check_reenters(s, "std::vector<int> v2 = { 1, 2, 3 }",
		   "std::vector<int>{ 1, 2, 3 }",
		   "(@).size() == 3 && (@)[0] == v2[0] && (@)[2] == v2[2]");
    check_reenters(s, "std::vector<int> v3", "std::vector<int>{ }",
		   "(@).size() == v3.size()");
    REQUIRE(s.submit("std::map<int, int> v4;"));
    REQUIRE(s.submit("v4[1] = 10; v4[2] = 20;"));
    REQUIRE(s.submit("v4"));
    CHECK(s.shown() == "std::map<int,int>{ { 1, 10 }, { 2, 20 } }");
    REQUIRE(s.submit("std::set<int> v5;"));
    REQUIRE(s.submit("v5.insert(3); v5.insert(1);"));
    REQUIRE(s.submit("v5"));
    CHECK(s.shown() == "std::set<int>{ 1, 3 }");
    REQUIRE(s.submit("struct H { std::string name; int n; };"));
    REQUIRE(s.submit("H v6 = { \"hh\", 3 }"));
    CHECK(s.shown() == "H{ .name = \"hh\", .n = 3 }");
}

TEST_CASE("an entry without its final ; shows its value, re-enterably (D10, C)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    REQUIRE(s.submit("#include <string.h>\n#include <math.h>\nint k = 0;"));
    REQUIRE(s.submit("int x = 15"));
    CHECK(s.shown() == "15");
    check_reenters(s, "double v2 = 1.0 / 3", "0.3333333333333333", "(@) == v2");
    check_reenters(s, "double v7 = -1.0 / 0.0", "-INFINITY", "(@) == v7");
    check_reenters(s, "_Bool v8 = 1", "true", "(@) == v8");
    check_reenters(s, "char *v11 = 0", "NULL", "(@) == v11");
    check_reenters(s, "const char *v12 = \"hi\\t\"", "\"hi\\t\"",
		   "strcmp(@, v12) == 0");
    check_reenters(s, "int *v13 = 0", "(int *) NULL", "(@) == v13");
    check_reenters(s, "int *v14 = &x", NULL, "(@) == v14");
    REQUIRE(s.submit("enum E { A, B = 5 };"));
    check_reenters(s, "enum E v15 = B", "B", "(@) == v15");
    check_reenters(s, "enum E v16 = (enum E)3", "(enum E) 3", "(@) == v16");
    REQUIRE(s.submit("struct P { int a; };"));
    check_reenters(s, "struct P *v17 = 0", "(struct P *) NULL", "(@) == v17");
    check_reenters(s, "int (*v18)(void) = 0", "(int (*)(void)) NULL",
		   "(@) == v18");

    // Slice 2: C's compound literals, a struct's and an array's.
    REQUIRE(s.submit("struct Pt { double x, y; };"));
    check_reenters(s, "struct Pt v19 = { 1.0, 2.5 }",
		   "(struct Pt){ .x = 1.0, .y = 2.5 }", "(@).y == v19.y");
    check_reenters(s, "int v20[3] = { 4, 5, 6 }", "(int[3]){ 4, 5, 6 }",
		   "(@)[2] == v20[2]");
    // B28: madc refuses this text entered again (gcc and clang accept it).
    REQUIRE(s.submit("int v21[2][2] = { { 1, 2 }, { 3, 4 } }"));
    CHECK(s.shown() == "(int[2][2]){ { 1, 2 }, { 3, 4 } }");
    REQUIRE(s.submit("union U { int i; float f; };"));
    check_reenters(s, "union U v22 = { 5 }", "(union U){ .i = 5 }",
		   "(@).i == v22.i");
    REQUIRE(s.submit("char v23[6] = \"hello\""));
    CHECK(s.shown() == "\"hello\"");
}

// Slice 3 (a madc var, the REPL's default dialect, D4): a var shows as its
// dialect literal, `{ 10, 20 }` for an array and `{ "k": 1 }` for an object
// (dialect-literals.md). A null shows nothing, as Julia shows `nothing`. With
// no var equality to compare by, re-entry is checked as a round trip: the
// shown text, entered again, shows the same text.
TEST_CASE("an entry without its final ; shows its value, re-enterably (D10, madc var)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=madc"));
    const char *cases[][2] = {
	{ "var a = 5", "5" },
	{ "var b = 2.5", "2.5" },
	{ "var c = \"hi\\n\\\"\"", "\"hi\\n\\\"\"" },
	{ "var d = true", "true" },
	{ "var f = { 10, 20, 30 }", "{ 10, 20, 30 }" },
	{ "var g = { \"k\": 1, \"name\": \"x\" }", "{ \"k\": 1, \"name\": \"x\" }" },
    };
    for ( size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i )
    {
	CAPTURE(cases[i][0]);
	REQUIRE(s.submit(cases[i][0]));
	CHECK(s.shown() == cases[i][1]);
	std::string again = "var r" + std::to_string(i) + " = " + s.shown();
	CAPTURE(again);
	REQUIRE(s.submit(again));
	CHECK(s.shown() == cases[i][1]);
    }
    REQUIRE(s.submit("var e"));
    CHECK(s.shown().empty());
}

// The madc dialect is a C++ superset (Program::presents_as_cpp): a value of a
// C++ type shows as C++ writes it, `Pt{ .x = 1.0 }` and `std::vector<int>{ 1,
// 2, 3 }` — never C's compound literal of the lowered tag
// (`(struct vector_int32_t_std__allocator_int32_t_){ … }`), which the dialect
// cannot read back.
TEST_CASE("an entry without its final ; shows its value, re-enterably (D10, madc C++ types)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=madc"));
    REQUIRE(s.submit("#include <vector>\nint k = 0;"));
    check_reenters(s, "std::vector<int> v1 = { 1, 2, 3 }",
		   "std::vector<int>{ 1, 2, 3 }",
		   "(@).size() == 3 && (@)[0] == v1[0] && (@)[2] == v1[2]");
    REQUIRE(s.submit("struct Pt { double x, y; };"));
    check_reenters(s, "Pt v2 = { 1.0, 2.5 }", "Pt{ .x = 1.0, .y = 2.5 }",
		   "(@).y == v2.y");
    REQUIRE(s.submit("enum E { A, B = 5 };"));
    check_reenters(s, "E v3 = B", "E::B", "(@) == v3");
    check_reenters(s, "int *v4 = 0", "(int *) nullptr", "(@) == v4");
}

// An entry's final bare function name (plan §41.6a, A) shows the function's
// pointer and never calls it: the entry's end stands for the `;` the entry
// omitted, and a function name before that `;` decays. WORD is the pointer
// type's spelling under the standard.
static void check_function_name_shown(const std::string &std_option,
				      const std::string &word)
{
    CAPTURE(std_option);
    InteractiveSession s;
    REQUIRE(s.begin(std_option));
    REQUIRE(s.submit("int k = 0;\nint calls = 0;\n"
		     "int f(void) { ++calls; return 3; }\n"
		     "int g(int v) { return v; }"));
    const std::string prefix = "(" + word + ") 0x";
    const char *forms[] = { "f", "(f)", "&f", "*f", "1, f" };
    for ( size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); ++i )
    {
	CAPTURE(forms[i]);
	REQUIRE(s.submit(forms[i]));
	CHECK(s.shown().compare(0, prefix.size(), prefix) == 0);
    }
    CHECK(*(int *)s.data("calls") == 0);
    // `&g`, not `g`: a function name after `==` is called (BUGS.md B41).
    check_reenters(s, "g", NULL, "(@) == &g");
    REQUIRE(s.submit("int (*p)(void) = f"));
    CHECK(s.shown().compare(0, prefix.size(), prefix) == 0);
    REQUIRE(s.submit("p()"));
    CHECK(s.shown() == "3");
    CHECK(*(int *)s.data("calls") == 1);
}

TEST_CASE("an entry's final function name shows the function, never a call (§41.6a)")
{
    check_function_name_shown("--std=c17", "int (*)(void)");
    check_function_name_shown("--std=c++17", "int (*)()");
    // The madc dialect spells types as C++ does (presents_as_cpp).
    check_function_name_shown("--std=madc", "int (*)()");
}

// An entry's `auto` object is a session global like any declaration's, and
// without its `;` it shows its value (plan §41.6a, B): the `auto` arms now
// record it where every declarator arm does.
TEST_CASE("an entry's auto declaration is a session global, and shows its value (§41.6a)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c++17"));
    REQUIRE(s.submit("#include <string>\nint g(int v) { return v * 2; }"));
    REQUIRE(s.submit("auto x = 5"));
    CHECK(s.shown() == "5");
    REQUIRE(s.submit("auto y = x + 1;"));
    CHECK(s.shown().empty());
    REQUIRE(s.submit("x + y"));
    CHECK(s.shown() == "11");
    REQUIRE(s.submit("std::string str = \"hi\";\nauto t = str;"));
    REQUIRE(s.submit("str = \"changed\";"));
    REQUIRE(s.submit("t"));
    CHECK(s.shown() == "\"hi\"");
    REQUIRE(s.submit("auto fp = g;\nauto l = [](int n) { return n + 1; };"));
    REQUIRE(s.submit("fp(5) + l(5)"));
    CHECK(s.shown() == "16");

    InteractiveSession c;
    REQUIRE(c.begin("--std=c23"));
    REQUIRE(c.submit("auto x = 5"));
    CHECK(c.shown() == "5");
    REQUIRE(c.submit("x * 3"));
    CHECK(c.shown() == "15");
}

// D12 (plan §41.6a): a refused entry's first diagnostic contains WANT.
static void check_refused(InteractiveSession &s, const std::string &entry,
			  const std::string &want)
{
    CAPTURE(entry);
    CHECK_FALSE(s.submit(entry));
    CAPTURE(first_error(s));
    CHECK(first_error(s).find(want) != std::string::npos);
}

// D12's table (plan §41.6a, measured against Julia 1.13 and IPython 9.17):
// what keeps a value, what the names mean, and what is refused. The same
// rows under every standard; STD_OPTION's `int` spelling is the entries'.
static void check_result_names(const std::string &std_option)
{
    CAPTURE(std_option);
    InteractiveSession s;
    REQUIRE(s.begin(std_option));
    REQUIRE(s.submit("int x = 5;\nint k = 0;\nvoid vf(void) { }"));	// REPL[1]
    // A shown value is kept; `ans` and `_` name it.
    REQUIRE(s.submit("x + 1"));						// REPL[2]
    CHECK(s.shown() == "6");
    REQUIRE(s.submit("ans * 2"));					// REPL[3]
    CHECK(s.shown() == "12");
    REQUIRE(s.submit("_ + 1"));						// REPL[4]
    CHECK(s.shown() == "13");
    // A value hidden by `;` keeps nothing (IPython), nor does a void one.
    REQUIRE(s.submit("ans + 100;"));					// REPL[5]
    REQUIRE(s.submit("vf()"));						// REPL[6]
    REQUIRE(s.submit("ans"));						// REPL[7]
    CHECK(s.shown() == "13");
    // ___ and __ count kept values back from the last, and each shown one
    // is kept too: 6, 12, 13, 13, then 12.
    REQUIRE(s.submit("___"));						// REPL[8]
    CHECK(s.shown() == "12");
    REQUIRE(s.submit("__"));						// REPL[9]
    CHECK(s.shown() == "13");
    // _N is REPL[N]'s value, and keeps it: a later change to x is not seen.
    REQUIRE(s.submit("x"));						// REPL[10]
    REQUIRE(s.submit("x = 7;"));					// REPL[11]
    REQUIRE(s.submit("_10 + _2"));					// REPL[12]
    CHECK(s.shown() == "11");
    check_refused(s, "_5", "'_5' names the value REPL[5] showed, and it showed none");
    check_refused(s, "_99", "'_99' names the value REPL[99] showed, and there is no REPL[99] yet");
    // A refused entry keeps nothing; the number still advances.
    check_refused(s, "undeclared_zz + 1", "undeclared_zz");
    REQUIRE(s.submit("ans"));
    CHECK(s.shown() == "11");
    // The moving names are refused in code that runs later; _N is not.
    check_refused(s, "int later(void) { return ans; }",
		  "'ans' changes with each value shown, so code that runs later"
		  " cannot use it; name the value REPL[");
    check_refused(s, "int later2(int a) { return a; }\nint later3(int v = __) { return v; }",
		  "'__' changes with each value shown");
    // A default member initializer runs at each construction, later too: its
    // refusal used to be swallowed with the initializer (B43).
    check_refused(s, "struct Later { int m = ans; };",
		  "'ans' changes with each value shown");
    REQUIRE(s.submit("int stable(void) { return _2 * 3; }"));
    REQUIRE(s.submit("stable()"));
    CHECK(s.shown() == "18");
    // A block of the entry's own statements runs now.
    REQUIRE(s.submit("{ k = ans + 1; }"));
    CHECK(*(int *)s.data("k") == 19);
    // A function designator keeps its pointer.
    REQUIRE(s.submit("int three(void) { return 3; }"));
    REQUIRE(s.submit("three"));
    REQUIRE(s.submit("ans() + _2"));
    CHECK(s.shown() == "9");
    // The user's own name wins, as in both precedents.
    REQUIRE(s.submit("int ans = 100;"));
    REQUIRE(s.submit("ans + 1"));
    CHECK(s.shown() == "101");
    REQUIRE(s.submit("_"));
    CHECK(s.shown() == "101");
}

// A type-headed expression at an entry's top level is a statement, as in a
// body ([stmt.ambig]): `std::string("short")` is a functional cast, where a
// file's top level can only read a declaration (plan §41.6a, E). It is
// placed where the entry writes it, not at the header's typedef.
TEST_CASE("a functional cast at an entry's top level is an expression (§41.6a)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c++17"));
    REQUIRE(s.submit("#include <string>\n#include <vector>"));
    REQUIRE(s.submit("std::string(\"short\").size()"));
    CHECK(s.shown() == "5");
    REQUIRE(s.submit("std::vector<int>{ 1, 2 }.size()"));
    CHECK(s.shown() == "2");
    REQUIRE(s.submit("std::string(\"x\");"));
    CHECK(s.shown().empty());
    // A declaration stays one: a direct-initialized object, and the most
    // vexing parse's function declaration.
    REQUIRE(s.submit("std::string named(\"named\")"));
    CHECK(s.shown() == "\"named\"");
    REQUIRE(s.submit("std::string vexing();"));
    CHECK(s.shown().empty());
}

TEST_CASE("an entry's shown value is kept and named ans, _, __, ___ and _N (D12)")
{
    check_result_names("--std=c17");
    check_result_names("--std=c++17");
    check_result_names("--std=madc");
}

// Slice 2 (plan §41.6a): an aggregate result is the object, as Julia and
// IPython keep a mutable one. A glvalue's result refers to it, so a change
// after the show is seen through `ans`; a prvalue's result owns the object; a
// temporary's part is copied when trivially copyable and otherwise not kept;
// an array still needs session result retention.
TEST_CASE("an aggregate result is the object it showed (D12, slice 2)")
{
    InteractiveSession c;
    REQUIRE(c.begin("--std=c17"));
    REQUIRE(c.submit("struct P { int a, b; };\nstruct P p = { 1, 2 };"));
    REQUIRE(c.submit("p"));
    REQUIRE(c.submit("p.b = 9;"));
    REQUIRE(c.submit("ans.b"));
    CHECK(c.shown() == "9");
    REQUIRE(c.submit("int k = 0;\nstruct P mk(void) { struct P r = { 5, 6 }; return r; }"));
    REQUIRE(c.submit("mk()"));
    REQUIRE(c.submit("ans.a + ans.b"));
    CHECK(c.shown() == "11");
    REQUIRE(c.submit("struct P *pp = &p;"));
    REQUIRE(c.submit("*pp"));
    REQUIRE(c.submit("k = &ans == &p;"));
    CHECK(*(int *)c.data("k") == 1);
    REQUIRE(c.submit("int arr[3] = { 1, 2, 3 };\nint (*pa)[3] = &arr;"));
    REQUIRE(c.submit("*pa"));
    CHECK(c.shown() == "(int[3]){ 1, 2, 3 }");
    REQUIRE(c.submit("arr"));
    check_refused(c, "ans", "which was not kept: an array result cannot yet be kept");

    InteractiveSession s;
    REQUIRE(s.begin("--std=c++17"));
    REQUIRE(s.submit("#include <string>\n#include <vector>\n"
		     "std::vector<int> v = { 1, 2 };"));
    REQUIRE(s.submit("v"));						// REPL[2]
    REQUIRE(s.submit("v.push_back(3);"));
    REQUIRE(s.submit("ans.size()"));
    CHECK(s.shown() == "3");
    REQUIRE(s.submit("std::vector<int>{ 7, 8 }"));			// REPL[5]
    REQUIRE(s.submit("ans.size() + _2.size()"));
    CHECK(s.shown() == "5");
    REQUIRE(s.submit("std::string str = \"hi\""));
    REQUIRE(s.submit("str += \"!\";"));
    REQUIRE(s.submit("ans + \"?\""));
    CHECK(s.shown() == "\"hi!?\"");
    // A class with no copy is kept too: the result refers to it.
    REQUIRE(s.submit("struct NC { int v = 3; NC() {} NC(const NC &) = delete; };\nNC nc;"));
    REQUIRE(s.submit("nc"));
    REQUIRE(s.submit("nc.v = 4;"));
    REQUIRE(s.submit("ans.v"));
    CHECK(s.shown() == "4");
    // A temporary's part: a class one is shown and not kept.
    REQUIRE(s.submit("struct H { std::string name; };\nH mkh() { return H{ \"hh\" }; }"));
    REQUIRE(s.submit("mkh().name"));
    CHECK(s.shown() == "\"hh\"");
    check_refused(s, "ans", "which was not kept: a part of a temporary is not kept");
    REQUIRE(s.submit("mkh()"));
    REQUIRE(s.submit("ans.name"));
    CHECK(s.shown() == "\"hh\"");
    // A reference to an array denotes the array (plan §41.6a, G): shown as
    // its elements, and, as an array, not kept yet.
    REQUIRE(s.submit("int arr[2] = { 4, 5 };\nint (&ra)[2] = arr;"));
    REQUIRE(s.submit("ra"));
    CHECK(s.shown() == "{ 4, 5 }");
    check_refused(s, "ans", "an array result cannot yet be kept");
    REQUIRE(s.submit("const int (&cr)[3] = { 1, 2, 3 };"));
    REQUIRE(s.submit("cr"));
    CHECK(s.shown() == "{ 1, 2, 3 }");
}

TEST_CASE("a madc var is kept as the dialect's value (D12)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=madc"));
    REQUIRE(s.submit("var v = { 10, 20 }"));
    REQUIRE(s.submit("ans"));
    CHECK(s.shown() == "{ 10, 20 }");
    REQUIRE(s.submit("var n = 5"));
    REQUIRE(s.submit("ans + 1"));
    CHECK(s.shown() == "6");
}

TEST_CASE("a var holding a number takes arithmetic (D28)")
{
  {
    InteractiveSession s;
    REQUIRE(s.begin("--std=madc"));
    REQUIRE(s.submit("var a = 5;"));
    REQUIRE(s.submit("var t = \"ab\";"));
    // Each result is a new var, shown re-enterably (D10's rule).
    const char *cases[][2] = {
	{ "a + 1", "6" },
	{ "1 + a", "6" },
	{ "a / 2", "2.5" },
	{ "a % 3", "2" },
	{ "-a", "-5" },
	{ "a * 0.5", "2.5" },
	{ "t + \"c\"", "\"abc\"" },
	{ "\"x\" + t", "\"xab\"" },
    };
    for ( size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i )
    {
	CAPTURE(cases[i][0]);
	REQUIRE(s.submit(cases[i][0]));
	CHECK(s.shown() == cases[i][1]);
	std::string again = "var r" + std::to_string(i) + " = " + s.shown();
	CAPTURE(again);
	REQUIRE(s.submit(again));
	CHECK(s.shown() == cases[i][1]);
    }
    REQUIRE(s.submit("a < 10"));
    CHECK(s.shown() == "true");
    // A compound assignment in one entry is seen by the next.
    REQUIRE(s.submit("a += 2;"));
    REQUIRE(s.submit("a"));
    CHECK(s.shown() == "7");
  }
    // A later Program in the same process has the free rows too: they live
    // in each Program's overload set, while the member rows sit on the
    // process-global carrier and register once.
    InteractiveSession s2;
    REQUIRE(s2.begin("--std=madc"));
    REQUIRE(s2.submit("var b = 2;"));
    REQUIRE(s2.submit("10 - b"));
    CHECK(s2.shown() == "8");
}

// The classifier runs inside the entry transaction (plan §41.5a, §41.1a): an
// entry offered line by line is parsed on the session, so a name an earlier
// entry declared counts, and it is taken only when it is final. An attempt
// the client goes on typing keeps nothing and takes no number.
static void check_offered_lines(const std::string &std_option)
{
    CAPTURE(std_option);
    typedef InteractiveSession::OfferState St;
    InteractiveSession s;
    REQUIRE(s.begin(std_option));
    REQUIRE(s.submit("int x = 10;"));
    CHECK(s.submitted() == 1);

    // A session variable's operator waits for its operand.
    CHECK(s.offer("x +\n").state == St::incomplete);
    CHECK(s.submitted() == 1);
    InteractiveSession::Offered r = s.offer("x +\n2\n");
    CHECK(r.state == St::taken);
    CHECK(r.ok);
    CHECK(s.shown() == "12");
    CHECK(s.submitted() == 2);

    // A function over lines: the declarator waits for its body, the body
    // for its close.
    CHECK(s.offer("int twice(int a)\n").state == St::incomplete);
    CHECK(s.offer("int twice(int a)\n{\n").state == St::incomplete);
    CHECK(s.offer("int twice(int a)\n{\n\treturn a * 2;\n").state == St::incomplete);
    r = s.offer("int twice(int a)\n{\n\treturn a * 2;\n}\n");
    CHECK(r.state == St::taken);
    CHECK(r.ok);
    CHECK(s.submitted() == 3);
    REQUIRE(s.submit("twice(x)"));
    CHECK(s.shown() == "20");

    // An open comment waits for its close.
    CHECK(s.offer("/* a note\n").state == St::incomplete);
    r = s.offer("/* a note\n*/ x\n");
    CHECK(r.state == St::taken);
    CHECK(s.shown() == "10");

    // A close that opens nothing is refused, and takes its number.
    unsigned before = s.submitted();
    r = s.offer("x; }\n");
    CHECK(r.state == St::taken);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(first_error(s).empty());
    const ::Program::Diagnostic *d = first_error_diagnostic(s);
    REQUIRE(d != (const ::Program::Diagnostic *)NULL);
    CHECK(d->file == "REPL[" + std::to_string(before + 1) + "]");
    CHECK(s.submitted() == before + 1);

    // The number an entry takes skips no incomplete attempt.
    before = s.submitted();
    CHECK(s.offer("int y =\n").state == St::incomplete);
    r = s.offer("int y =\n;\n");
    CHECK(r.state == St::taken);
    CHECK_FALSE(r.ok);
    d = first_error_diagnostic(s);
    REQUIRE(d != (const ::Program::Diagnostic *)NULL);
    CHECK(d->file == "REPL[" + std::to_string(before + 1) + "]");

    // D11: a finished if with no else waits; its else continues it.
    CHECK(s.offer("if (x > 5) x = 1;\n").state == St::extendable);
    CHECK_FALSE(s.program().entry_line_continues_if("x = 3;"));
    CHECK_FALSE(s.program().entry_line_continues_if(""));
    CHECK_FALSE(s.program().entry_line_continues_if("elsewhere = 1;"));
    CHECK(s.program().entry_line_continues_if("  else x = 2;"));
    r = s.offer("if (x > 5) x = 1;\nelse x = 2;\n");
    CHECK(r.state == St::taken);
    CHECK(r.ok);
    REQUIRE(s.submit("x"));
    CHECK(s.shown() == "1");
    // Submitted as it is, an extendable if runs.
    REQUIRE(s.submit("if (x < 5) x = 7;"));
    REQUIRE(s.submit("x"));
    CHECK(s.shown() == "7");
}

TEST_CASE("an entry offered line by line is taken once it is final (§41.5a)")
{
    check_offered_lines("--std=madc");
    check_offered_lines("--std=c17");
    check_offered_lines("--std=c++17");
}

// A refusal before an entry's end leaves the rest of it unread; the next
// entry starts clean (plan §41.5a).
TEST_CASE("an entry refused before its end leaves no tokens for the next")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c++17"));
    REQUIRE(s.submit("int x = 10;"));
    const char *refused[] = {
	"/* never closed",	// the lexer's refusal, at the end of input
	"f(",			// an open delimiter, before the parse
	"x + (1",		// the same, inside an expression
	"int = 3; int z = 4;",	// a parse error ahead of more text
	"\"cut",		// a literal cut by the new-line
    };
    for ( size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i )
    {
	CAPTURE(refused[i]);
	CHECK_FALSE(s.submit(refused[i]));
	REQUIRE(s.submit("x"));
	CHECK(s.shown() == "10");
    }
}

// A unit's own file-scope statics are session names (plan §41.5a, owner
// 2026-09-27): the session and every unit it takes are one program, as
// cling's and clang-repl's are (tmp/repl/d20s2). There is one object, and a
// second definition is a redefinition, refused as any is today.
static void check_entry_statics(const std::string &std_option)
{
    CAPTURE(std_option);
    InteractiveSession s;
    REQUIRE(s.begin(std_option));
    REQUIRE(s.submit("static int s = 3;"));
    REQUIRE(s.submit("static int twice_s(void) { return s * 2; }"));
    REQUIRE(s.submit("twice_s() + s"));
    CHECK(s.shown() == "9");
    // An entry's write is what the static function reads.
    REQUIRE(s.submit("s = 4;"));
    REQUIRE(s.submit("twice_s()"));
    CHECK(s.shown() == "8");
    CHECK_FALSE(s.submit("static int s = 5;"));
    CHECK(first_error(s) == "redefinition of 's'");
    REQUIRE(s.submit("s"));
    CHECK(s.shown() == "4");
}

TEST_CASE("an entry's statics are session names (§41.5a)")
{
    check_entry_statics("--std=c17");
    check_entry_statics("--std=c++17");
    check_entry_statics("--std=madc");
}

// TEXT in a fresh temporary file (the temp-file owner places it); its path.
static std::string temp_source(const char *text, const char *prefix)
{
    std::string path;
    int fd = madc::detail::make_temp_file(prefix, path);
    REQUIRE(fd >= 0);
    size_t n = strlen(text);
    CHECK(madc::detail::write_fd_without_sigpipe(fd, text, n) == (ssize_t)n);
    ::close(fd);
    return path;
}

// The measured shapes (cling 1.2 .L / #include, clang-repl-20 #include):
// helper=10 count=1 once a.c is loaded; b.c's clashing static refused whole,
// use_b undeclared afterwards, a.c unchanged.
TEST_CASE("a loaded file and the session are one unit (§41.5a)")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    std::string a = temp_source(
	"static int count = 1;\n"
	"static int helper(void) { return count * 10; }\n"
	"int use_a(void) { return helper() + count; }\n", "madc_repl_a");
    std::string b = temp_source(
	"static int count = 2;\n"
	"int use_b(void) { return count; }\n", "madc_repl_b");
    REQUIRE(s.load(a));
    CHECK(s.submitted() == 0);		// a file takes no REPL[N]
    REQUIRE(s.submit("helper()"));
    CHECK(s.shown() == "10");
    REQUIRE(s.submit("count"));
    CHECK(s.shown() == "1");
    CHECK_FALSE(s.load(b));
    CHECK(first_error(s) == "redefinition of 'count'");
    CHECK_FALSE(s.submit("use_b()"));
    REQUIRE(s.submit("use_a()"));
    CHECK(s.shown() == "11");
    CHECK_FALSE(s.load("/nonexistent/madc_repl_no_such_file.c"));
    CHECK(first_error(s) == "Failed to open file");
    std::remove(a.c_str());
    std::remove(b.c_str());
}

// python -i: the file runs, then the prompt has its names (plan §41.5a). Its
// main's status is not the session's end.
TEST_CASE("a loaded file's main runs with its argv, and the session goes on (§41.5a)")
{
    char a0[] = "prog", a1[] = "x", a2[] = "y";
    char *argv[] = { a0, a1, a2, NULL };
    int status = 0;

    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    std::string p = temp_source(
	"int ran = 0;\n"
	"int main(int argc, char **argv) { ran = argc; return 7; }\n",
	"madc_repl_main");
    REQUIRE(s.load(p));
    REQUIRE(s.function("main") != (void *)NULL);
    REQUIRE(s.run_main(3, argv, &status));
    CHECK(status == 7);
    REQUIRE(s.submit("ran"));
    CHECK(s.shown() == "3");
    std::remove(p.c_str());

    // A script's statements are its main, as script mode makes it: loading
    // runs nothing, main runs them, and a later entry's statements are the
    // entry's own run.
    InteractiveSession m;
    REQUIRE(m.begin("--std=madc"));
    std::string sp = temp_source(
	"int total = 0;\n"
	"for (int i = 1; i <= 4; ++i)\n"
	"\ttotal += i;\n", "madc_repl_script");
    REQUIRE(m.load(sp));
    REQUIRE(m.submit("total"));
    CHECK(m.shown() == "0");
    REQUIRE(m.run_main(1, argv, &status));
    REQUIRE(m.submit("total"));
    CHECK(m.shown() == "10");
    REQUIRE(m.submit("total += 1;"));
    REQUIRE(m.submit("total"));
    CHECK(m.shown() == "11");
    std::remove(sp.c_str());

    // A file without main just loads.
    InteractiveSession l;
    REQUIRE(l.begin("--std=c17"));
    std::string lp = temp_source("int sq(int v) { return v * v; }\n",
				 "madc_repl_lib");
    REQUIRE(l.load(lp));
    CHECK(l.function("main") == (void *)NULL);
    CHECK_FALSE(l.run_main(1, argv, &status));
    REQUIRE(l.submit("sq(9)"));
    CHECK(l.shown() == "81");
    std::remove(lp.c_str());
}

// How often NEEDLE occurs in TEXT.
static size_t occurrences(const std::string &text, const char *needle)
{
    size_t n = 0;
    for ( size_t at = text.find(needle); at != std::string::npos;
	  at = text.find(needle, at + 1) )
	++n;
    return n;
}

// The file commands (plan §7f). A host that honors no payloads (the
// in-process terminal): %load and %run -i load the file in the session
// itself, %run (a fresh session) is refused naming %run -i, and a refused
// file's diagnostics show once — its load rendered them, the command's end
// does not again. A payload host (the backend server) gets the command's
// payload with the file and its arguments, and the session loads nothing.
TEST_CASE("session commands: %load and %run, with and without a payload host (§7f)")
{
    std::string lib = temp_source("int thrice(int v) { return 3 * v; }\n",
				  "madc_repl_cmdlib");
    std::string bad = temp_source("int broken = nosuch;\n", "madc_repl_cmdbad");
    std::string prog = temp_source(
	"int ran = 0;\n"
	"int main(int argc, char **argv) { ran = argc; return 0; }\n",
	"madc_repl_cmdprog");
    std::string other = temp_source("int sq(int v) { return v * v; }\n",
				    "madc_repl_cmdother");
    const std::string missing = "/nonexistent/madc_repl_no_such_file.c";

    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    std::ostringstream err;
    s.program().error_stream = &err;
    REQUIRE(s.submit("%load " + lib));
    CHECK(s.payload() == madc::session_payload::none);
    REQUIRE(s.submit("thrice(7)"));
    CHECK(s.shown() == "21");
    REQUIRE(s.submit("%run -i " + prog + " a 'b c'"));
    REQUIRE(s.submit("ran"));
    CHECK(s.shown() == "3");
    CHECK(s.loaded_main());
    // A file with no main of its own runs none: prog's is not its main.
    REQUIRE(s.submit("ran = 0;"));
    REQUIRE(s.submit("%run -i " + other + " z"));
    CHECK_FALSE(s.loaded_main());
    REQUIRE(s.submit("ran"));
    CHECK(s.shown() == "0");
    REQUIRE(s.submit("sq(9)"));
    CHECK(s.shown() == "81");
    CHECK_FALSE(s.submit("%run " + prog));
    CHECK(first_error(s) == "%run starts a fresh session, which this host"
			    " cannot; %run -i FILE runs it in this one");
    CHECK_FALSE(s.submit("%run"));
    CHECK(first_error(s) == "%run needs a FILE");
    CHECK_FALSE(s.submit("%run -i"));
    CHECK(first_error(s) == "%run needs a FILE");
    CHECK_FALSE(s.submit("%load"));
    CHECK(first_error(s) == "%load takes one FILE");
    CHECK_FALSE(s.submit("%load " + lib + " " + lib));
    CHECK(first_error(s) == "%load takes one FILE");
    CHECK_FALSE(s.submit("%run \"unclosed"));
    CHECK(first_error(s) == "%run: no closing quotation, or a backslash at the end");
    err.str("");
    CHECK_FALSE(s.submit("%load " + bad));
    CHECK(occurrences(err.str(), "undeclared identifier 'nosuch'") == 1);
    err.str("");
    CHECK_FALSE(s.submit("%load " + missing));
    CHECK(occurrences(err.str(), "Failed to open file") == 1);

    InteractiveSession h;
    REQUIRE(h.begin("--std=c17"));
    h.host_honors_payloads(true);
    REQUIRE(h.submit("%run " + prog + " x"));
    CHECK(h.payload() == madc::session_payload::run);
    CHECK(h.payload_argv() == std::vector<std::string>{ prog, "x" });
    REQUIRE(h.submit("%run -i " + prog));
    CHECK(h.payload() == madc::session_payload::run_here);
    REQUIRE(h.submit("1 + 1"));		// the next entry asks nothing
    CHECK(h.payload() == madc::session_payload::none);
    REQUIRE(h.submit("%load " + lib));
    CHECK(h.payload() == madc::session_payload::load);
    CHECK(h.payload_argv() == std::vector<std::string>{ lib });
    CHECK(h.function("thrice") == (void *)NULL);
    CHECK(h.function("main") == (void *)NULL);
    CHECK_FALSE(h.submit("%load " + missing));
    CHECK(h.payload() == madc::session_payload::none);
    CHECK(first_error(h) == "Failed to open file");

    std::remove(lib.c_str());
    std::remove(bad.c_str());
    std::remove(prog.c_str());
    std::remove(other.c_str());
}

// Completion (plan §41.7a, slice 3): the names that complete the word before
// the caret, walked from the registries; the query lexes the text before the
// word as an attempt that rolls back, so it leaves nothing.
namespace {

std::vector<std::string> complete_at_end(InteractiveSession &s,
					 const std::string &text,
					 size_t *start_out = NULL)
{
    size_t start = 0;
    std::vector<std::string> c = s.complete(text, text.size(), start);
    if ( start_out )
	*start_out = start;
    return c;
}

bool has(const std::vector<std::string> &v, const char *name)
{
    for ( size_t i = 0; i < v.size(); ++i )
	if ( v[i] == name )
	    return true;
    return false;
}

} // namespace

TEST_CASE("completion: the session's names, the standard's keywords, a header's names")
{
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    REQUIRE(s.submit("int xylophone = 3;"));
    REQUIRE(s.submit("int xyz1(int a) { return a; }"));
    size_t start = 99;
    std::vector<std::string> c = complete_at_end(s, "1 + xy", &start);
    CHECK(start == 4u);
    REQUIRE(c.size() == 2u);			// sorted, the object and the function
    CHECK(c[0] == "xylophone");
    CHECK(c[1] == "xyz1");
    CHECK(has(complete_at_end(s, "whi"), "while"));
    CHECK_FALSE(has(complete_at_end(s, "cla"), "class"));	// C has no class
    REQUIRE(s.submit("#include <stdio.h>"));
    c = complete_at_end(s, "prin");
    CHECK(has(c, "printf"));
    CHECK(has(complete_at_end(s, "EO"), "EOF"));	// a macro
    CHECK(has(complete_at_end(s, "std"), "stdout"));	// a header's object

    // Nowhere a name is written: a string, a comment, a directive, a number.
    CHECK(complete_at_end(s, "\"xylo").empty());
    CHECK(complete_at_end(s, "x; // xylo").empty());
    CHECK(complete_at_end(s, "/* xylo").empty());
    CHECK(complete_at_end(s, "#xylo").empty());
    CHECK(complete_at_end(s, "1 + ").empty());	// an empty word
    CHECK(complete_at_end(s, "12").empty());

    // A tag completes after struct; alone it is no name in C.
    REQUIRE(s.submit("struct Point { int x; };"));
    CHECK(complete_at_end(s, "struct Poi") == std::vector<std::string>{ "Point" });
    CHECK(complete_at_end(s, "Poi").empty());

    // A reserved name completes a word that starts with `_` (IPython's
    // rule); the session's own names never.
    CHECK(has(complete_at_end(s, "_B"), "_Bool"));
    CHECK(complete_at_end(s, "__madc").empty());

    // The query leaves nothing: a macro its text defines is gone after it,
    // and the next entry is numbered as before.
    unsigned before = s.submitted();
    CHECK_FALSE(has(complete_at_end(s, "#define QQ 5\nQ"), "QQ"));
    CHECK(s.submitted() == before);
    std::ostringstream err;
    s.program().error_stream = &err;
    CHECK_FALSE(s.submit("QQ"));
    REQUIRE(s.submit("xylophone + 1"));
    CHECK(s.shown() == "4");
}

TEST_CASE("completion: C++ names, madc's words, and the result names")
{
    InteractiveSession p;
    REQUIRE(p.begin("--std=c++17"));
    CHECK(has(complete_at_end(p, "cla"), "class"));
    REQUIRE(p.submit("struct Point { int x; };"));
    CHECK(has(complete_at_end(p, "Poi"), "Point"));	// a class name is a type name
    REQUIRE(p.submit("namespace geometry { int area = 2; }"));
    CHECK(has(complete_at_end(p, "geo"), "geometry"));
    CHECK(complete_at_end(p, "geometry::ar") == std::vector<std::string>{ "area" });
    CHECK(complete_at_end(p, "Point{}.x").empty());
    // Only what C++ lets a top-level entry write bare: after <vector>, std's
    // names and madc's lowered ones (`allocator_char__operator=`, the
    // instantiations) are not offered, until a using-directive names std.
    REQUIRE(p.submit("#include <vector>"));
    CHECK_FALSE(has(complete_at_end(p, "vec"), "vector"));
    std::vector<std::string> al = complete_at_end(p, "alloc");
    CHECK_FALSE(has(al, "allocator"));
    CHECK_FALSE(has(al, "allocator_char"));
    CHECK_FALSE(has(al, "allocator_arg"));
    for ( size_t i = 0; i < al.size(); ++i )
	CHECK(al[i].find("__") == std::string::npos);
    CHECK(has(complete_at_end(p, "st"), "std"));
    REQUIRE(p.submit("using namespace std;"));
    CHECK(has(complete_at_end(p, "vec"), "vector"));

    InteractiveSession m;
    REQUIRE(m.begin("--std=madc"));
    CHECK(has(complete_at_end(m, "printl"), "println"));	// the auto-include table
    CHECK(has(complete_at_end(m, "vec"), "vector"));	// the dialect writes std bare
    CHECK(has(complete_at_end(m, "ph"), "php"));
    CHECK_FALSE(has(complete_at_end(m, "WE"), "WEB"));	// a member row: qualified only
    REQUIRE(m.submit("var total = 5;"));
    CHECK(has(complete_at_end(m, "tot"), "total"));
    CHECK_FALSE(has(complete_at_end(m, "an"), "ans"));	// no result kept yet
    REQUIRE(m.submit("total * 2"));
    CHECK(has(complete_at_end(m, "an"), "ans"));
    std::vector<std::string> u = complete_at_end(m, "_");
    CHECK(has(u, "_"));
    CHECK(has(u, ("_" + std::to_string(m.submitted())).c_str()));
}

// Slice 4 (plan §41.7a): an object's members after `.` and `->`, stepping
// through the chain's types from a session name; a scope's after `::`.
TEST_CASE("completion: members after . and ->, and a scope's after ::")
{
    InteractiveSession c;
    REQUIRE(c.begin("--std=c17"));
    REQUIRE(c.submit("struct Inner { int depth; double width; };"));
    REQUIRE(c.submit("struct Point { int x; int y; struct Inner in; struct Point *next; };"));
    REQUIRE(c.submit("struct Point p = { 1, 2 };"));
    REQUIRE(c.submit("struct Point *pp = &p;"));
    size_t start = 0;
    std::vector<std::string> fields = { "in", "next", "x", "y" };
    CHECK(complete_at_end(c, "p.", &start) == fields);	// an empty word lists all
    CHECK(start == 2u);
    CHECK(complete_at_end(c, "pp->") == fields);
    CHECK(complete_at_end(c, "p.in.") == std::vector<std::string>{ "depth", "width" });
    CHECK(complete_at_end(c, "pp->next->in.w") == std::vector<std::string>{ "width" });
    CHECK(complete_at_end(c, "p->").empty());		// no pointer
    CHECK(complete_at_end(c, "pp.").empty());		// `.` on a pointer
    CHECK(complete_at_end(c, "p.nope.").empty());
    CHECK(complete_at_end(c, "f().").empty());		// an expression: not yet
    CHECK(complete_at_end(c, "a[0].").empty());

    InteractiveSession p;
    REQUIRE(p.begin("--std=c++17"));
    REQUIRE(p.submit("struct Base { int b1; void bm() {} };"));
    REQUIRE(p.submit("class Box : public Base { public: int w; int get() { return w; }"
		     " static int count; typedef int unit;"
		     " private: int secret; void hidden() {} };"));
    REQUIRE(p.submit("Box bx;"));
    REQUIRE(p.submit("Box &rb = bx;"));
    std::vector<std::string> box = { "b1", "bm", "count", "get", "w" };
    CHECK(complete_at_end(p, "bx.") == box);	// no private member, a base's
    CHECK(complete_at_end(p, "rb.") == box);	// a reference is its referent
    // After the class's name: its members too, and its nested types, not
    // its injected-class-name.
    std::vector<std::string> scope = { "b1", "bm", "count", "get", "unit", "w" };
    CHECK(complete_at_end(p, "Box::") == scope);
    REQUIRE(p.submit("namespace geometry { int area = 2; namespace deep { int z = 1; }"
		     " struct Shape { int s; }; int perimeter(int a) { return a; } }"));
    std::vector<std::string> geo = { "Shape", "area", "deep", "perimeter" };
    CHECK(complete_at_end(p, "geometry::") == geo);
    CHECK(complete_at_end(p, "geometry::deep::") == std::vector<std::string>{ "z" });
    REQUIRE(p.submit("enum class Color { Red, Green };"));
    CHECK(complete_at_end(p, "Color::") == std::vector<std::string>{ "Green", "Red" });
    REQUIRE(p.submit("#include <string>"));
    REQUIRE(p.submit("std::string str = \"abc\";"));
    CHECK(complete_at_end(p, "str.si") == std::vector<std::string>{ "size" });
    CHECK_FALSE(has(complete_at_end(p, "str."), "_M_dataplus"));	// reserved
    REQUIRE(p.submit("#include <vector>"));
    CHECK(complete_at_end(p, "std::vec") == std::vector<std::string>{ "vector" });
    // A namespace's names are what an entry writes: madc's registered
    // instantiations (`allocator_char`) are not.
    std::vector<std::string> st = complete_at_end(p, "std::alloc");
    CHECK(has(st, "allocator"));
    CHECK_FALSE(has(st, "allocator_char"));
    REQUIRE(p.submit("struct Pt { int x; int y; };"));
    REQUIRE(p.submit("Pt{3, 4}"));
    CHECK(complete_at_end(p, "ans.") == std::vector<std::string>{ "x", "y" });
    CHECK(complete_at_end(p, "_.") == std::vector<std::string>{ "x", "y" });
    // A qualified query parses its attempt, and leaves nothing of it.
    CHECK(complete_at_end(p, "namespace tmpns { int k; }\ntmpns::")
	  == std::vector<std::string>{ "k" });
    CHECK_FALSE(has(complete_at_end(p, "tmp"), "tmpns"));

    // madc: a var's script methods, and a module's namespace in a fresh
    // session (its fragment fills it when the attempt is parsed).
    InteractiveSession m;
    REQUIRE(m.begin("--std=madc"));
    CHECK(complete_at_end(m, "php::strto")
	  == (std::vector<std::string>{ "strtolower", "strtoupper" }));
    REQUIRE(m.submit("var total = { \"a\": 1 };"));
    std::vector<std::string> vm = complete_at_end(m, "total.");
    CHECK(has(vm, "count"));
    CHECK(has(vm, "is_string"));
    CHECK(has(vm, "substr"));
    REQUIRE(m.submit("total.count()"));
    CHECK(m.shown() == "1");
}

// §37 item 8 (plan §41.8a, slice 1): the command front and `%type`. A
// command is recognized at an entry's start, taken and numbered like any
// input, and `%type` parses its argument as an attempt that rolls back and
// never runs.
TEST_CASE("session commands: %help, %type, an unknown command, and what stays C")
{
    InteractiveSession c;
    REQUIRE(c.begin("--std=c17"));
    std::ostringstream err;
    c.program().error_stream = &err;
    REQUIRE(c.submit("int x = 10;"));
    REQUIRE(c.submit("int sq(int n) { return n * n; }"));
    REQUIRE(c.submit("int calls = 0;"));
    REQUIRE(c.submit("int bump(void) { return ++calls; }"));
    REQUIRE(c.submit("struct Point { int x; int y; };"));
    REQUIRE(c.submit("struct Point p = { 1, 2 };"));
    REQUIRE(c.submit("int arr[3];"));
    REQUIRE(c.submit("%help"));
    CHECK(c.shown().find("%type EXPR") != std::string::npos);
    CHECK(c.submitted() == 8u);			// a command takes REPL[N]
    struct { const char *entry, *type; } types[] = {
	{ "%type x", "int" },
	{ ":type x * 2.5", "double" },		// `:` is the alias (D13)
	{ "  %type sq(x)", "int" },		// blanks before it
	{ "%type &x", "int *" },
	{ "%type p", "struct Point" },
	{ "%type &p", "struct Point *" },
	{ "%type sq", "int (int)" },		// a function designator
	{ "%type arr", "int [3]" },
	{ "%type bump()", "int" },
    };
    for ( size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i )
    {
	CAPTURE(types[i].entry);
	REQUIRE(c.submit(types[i].entry));
	CHECK(c.shown() == types[i].type);
    }
    // Never run: bump's side effect is absent, and the next entry shows.
    REQUIRE(c.submit("calls"));
    CHECK(c.shown() == "0");
    // Refused: an undeclared name at the column typed, a statement, an
    // unknown command.
    CHECK_FALSE(c.submit("%type nope + 1"));
    CHECK(err.str().find(":1:7: ") != std::string::npos);
    // The recorded diagnostic keeps the token's end: the echo underlines
    // `nope` (gcc's ^~~~, D26).
    CHECK(err.str().find("\033[1;32m^~~~\033[m") != std::string::npos);
    CHECK_FALSE(c.submit("%type x;"));
    CHECK(err.str().find("%type takes an expression") != std::string::npos);
    CHECK_FALSE(c.submit("%nosuch"));
    CHECK(err.str().find("unknown command '%nosuch'") != std::string::npos);
    // An attempt leaves nothing: a declaration it parses is gone (a
    // declaration without its `;` shows its value, D10).
    REQUIRE(c.submit("%type int y = 5"));
    CHECK(c.shown() == "int");
    CHECK_FALSE(c.submit("y"));
    // What stays C: a global qualifier, and a continuation that starts
    // with `%name` (the entry's first line decides).
    REQUIRE(c.submit("int g(int a, int b)\n{ return a\n%b; }"));
    REQUIRE(c.submit("g(7, 4)"));
    CHECK(c.shown() == "3");

    // Completion: the command names, then the argument as an entry.
    size_t start = 0;
    CHECK(complete_at_end(c, "%ty", &start) == std::vector<std::string>{ "type" });
    CHECK(start == 1u);
    CHECK(complete_at_end(c, ":he") == std::vector<std::string>{ "help" });
    CHECK(complete_at_end(c, "%type bu", &start) == std::vector<std::string>{ "bump" });
    CHECK(start == 6u);

    InteractiveSession p;
    REQUIRE(p.begin("--std=c++17"));
    REQUIRE(p.submit("#include <string>"));
    REQUIRE(p.submit("std::string s = \"abc\";"));
    REQUIRE(p.submit("int (*fp)(int) = nullptr;"));
    REQUIRE(p.submit("int x = 1;"));
    REQUIRE(p.submit("%type s"));
    CHECK(p.shown() == "std::string");		// the source's name
    REQUIRE(p.submit("%type fp"));
    CHECK(p.shown() == "int (*)(int)");
    REQUIRE(p.submit("%type x == 2"));
    CHECK(p.shown() == "bool");
    REQUIRE(p.submit("::x"));			// C++'s global qualifier
    CHECK(p.shown() == "1");

    InteractiveSession m;
    REQUIRE(m.begin("--std=madc"));
    REQUIRE(m.submit("var v = 5;"));
    REQUIRE(m.submit("%type v"));
    CHECK(m.shown() == "var");			// the dialect's carrier
}

// The third prefix (plan §7f): cling's and Node's `.name` reaches the table
// `%` and `:` reach, and their own spellings are alias rows naming our
// commands under every prefix — `.L` is %load, `.q` and `.exit` %quit, `.?`
// %help — which %help names. %quit ends the session: ended() for a host that
// honors no payloads, the `quit` payload for one that does. What stays C:
// `.5`, and a designator's `.x` on a continuation line.
TEST_CASE("session commands: the `.` prefix, alias rows and %quit (§7f)")
{
    std::string lib = temp_source("int thrice(int v) { return 3 * v; }\n",
				  "madc_repl_dotlib");
    InteractiveSession c;
    REQUIRE(c.begin("--std=c17"));
    std::ostringstream err;
    c.program().error_stream = &err;
    REQUIRE(c.submit("int x = 10;"));
    REQUIRE(c.submit(".type x * 2.5"));
    CHECK(c.shown() == "double");
    REQUIRE(c.submit(".L " + lib));
    REQUIRE(c.submit("thrice(x)"));
    CHECK(c.shown() == "30");
    CHECK_FALSE(c.submit("%L " + lib + "x"));	// %L is .L: its FILE opens first
    CHECK(first_error(c) == "Failed to open file");
    const char *helps[] = { ".help", ".?", "%?", ":?" };
    for ( size_t i = 0; i < sizeof(helps) / sizeof(helps[0]); ++i )
    {
	CAPTURE(helps[i]);
	REQUIRE(c.submit(helps[i]));
	const std::string &h = c.shown();
	CHECK(h.find("list the session's commands (also .?)\n") != std::string::npos);
	CHECK(h.find("nothing runs (also .L)\n") != std::string::npos);
	CHECK(h.find("%quit") != std::string::npos);
	CHECK(h.find("end the session (also .q, .exit)\n") != std::string::npos);
    }
    CHECK_FALSE(c.submit(".nosuch"));
    CHECK(err.str().find("unknown command '.nosuch'") != std::string::npos);
    // What stays C.
    REQUIRE(c.submit(".5 + x"));
    CHECK(c.shown() == "10.5");
    REQUIRE(c.submit("struct P { int x; int y; };"));
    REQUIRE(c.submit("struct P q = {\n.x = 3,\n.y = 4 };"));
    REQUIRE(c.submit("q.y"));
    CHECK(c.shown() == "4");
    // Completion offers the aliases beside the names.
    size_t start = 0;
    CHECK(complete_at_end(c, ".ex", &start) == std::vector<std::string>{ "exit" });
    CHECK(start == 1u);
    CHECK(complete_at_end(c, ".q") == (std::vector<std::string>{ "q", "quit" }));
    CHECK(complete_at_end(c, "%L") == std::vector<std::string>{ "L" });
    // %quit: refused with an argument, else the session ends (status 0).
    int status = 7;
    CHECK_FALSE(c.ended(status));
    CHECK_FALSE(c.submit("%quit now"));
    CHECK(first_error(c) == "%quit takes no argument");
    CHECK_FALSE(c.ended(status));
    REQUIRE(c.submit(".q"));
    CHECK(c.ended(status));
    CHECK(status == 0);

    InteractiveSession h;
    REQUIRE(h.begin("--std=c17"));
    h.host_honors_payloads(true);
    REQUIRE(h.submit(".exit"));
    CHECK(h.payload() == madc::session_payload::quit);
    CHECK(h.payload_argv().empty());
    REQUIRE(h.submit(":L " + lib));
    CHECK(h.payload() == madc::session_payload::load);
    CHECK(h.payload_argv() == std::vector<std::string>{ lib });

    std::remove(lib.c_str());
}

// TEXT in a fresh temporary file, each `@` in it replaced by the file's stem
// (its name without the directory and an extension: the function %call
// calls); the path, the stem through `stem`.
static std::string temp_named_source(const char *text, const char *prefix,
				     std::string &stem)
{
    std::string path;
    int fd = madc::detail::make_temp_file(prefix, path);
    REQUIRE(fd >= 0);
    const size_t sep = path.find_last_of("/\\");
    stem = sep == std::string::npos ? path : path.substr(sep + 1);
    const size_t dot = stem.rfind('.');
    if ( dot != std::string::npos && dot > 0 )
	stem.erase(dot);
    std::string body;
    for ( const char *p = text; *p; ++p )
	body += *p == '@' ? stem : std::string(1, *p);
    CHECK(madc::detail::write_fd_without_sigpipe(fd, body.data(), body.size())
	  == (ssize_t)body.size());
    ::close(fd);
    return path;
}

// cling's `.x FILE(ARGS)` (plan §7f): FILE loads, then the function named
// after it is called with ARGS (none without them), its value shown and its
// diagnostics citing the columns typed; a FILE with no such function runs its
// own main (with no ARGS as %run -i runs it); one with neither is refused. A
// payload host gets FILE and the call's text, loads FILE, and call_file
// makes the call.
TEST_CASE("session commands: %call (.x) calls the function named after FILE (§7f)")
{
    std::string two, zero, bad, solo, withargs, none, host;
    std::string f2 = temp_named_source("int @(int a, int b) { return a * 10 + b; }\n",
				       "madc_repl_calltwo", two);
    std::string f0 = temp_named_source("int @(void) { return 7; }\n",
				       "madc_repl_callzero", zero);
    std::string fb = temp_named_source("int @(int a, int b) { return a + b; }\n",
				       "madc_repl_callbad", bad);
    std::string m1 = temp_named_source(
	"int solo_ran = 0;\n"
	"int main(int argc, char **argv) { solo_ran = argc + 40; return 0; }\n",
	"madc_repl_callsolo", solo);
    std::string m2 = temp_named_source(
	"int args_ran = 0;\n"
	"int main(int argc, char **argv) { args_ran = argc; return 5; }\n",
	"madc_repl_callargs", withargs);
    std::string nf = temp_named_source("int nothing_here = 1;\n",
				       "madc_repl_callnone", none);
    std::string fh = temp_named_source("int @(int a, int b) { return a * b; }\n",
				       "madc_repl_callhost", host);

    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    std::ostringstream err;
    s.program().error_stream = &err;
    REQUIRE(s.submit(".x " + f2 + "(4, 2)"));
    CHECK(s.shown() == "42");
    REQUIRE(s.submit(two + "(1, 1)"));		// its names stay
    CHECK(s.shown() == "11");
    REQUIRE(s.submit("%call " + f0));		// no ARGS: it takes none
    CHECK(s.shown() == "7");
    REQUIRE(s.submit(".x " + m1));		// FILE's main, as %run -i runs it
    REQUIRE(s.submit("solo_ran"));
    CHECK(s.shown() == "41");
    // Refused: the call's undeclared argument at the column typed, a FILE
    // with neither function, no FILE, and text after the call.
    err.str("");
    CHECK_FALSE(s.submit(".x " + fb + "(4, nope)"));
    CHECK(err.str().find(":1:" + std::to_string(3 + fb.size() + 5) + ": ")
	  != std::string::npos);
    CHECK(err.str().find("undeclared identifier 'nope'") != std::string::npos);
    CHECK_FALSE(s.submit(".x " + nf));
    CHECK(first_error(s) == "%call: " + nf + " defines no function " + none
			    + ", and no main");
    CHECK_FALSE(s.submit("%call"));
    CHECK(first_error(s) == "%call needs a FILE");
    CHECK_FALSE(s.submit("%call " + f2 + "(1, 2) + 3"));
    CHECK(first_error(s) == "%call's arguments end the line: %call FILE(ARGS)");

    // A second main is refused, so FILE's main with ARGS has its own session.
    InteractiveSession t;
    REQUIRE(t.begin("--std=c17"));
    REQUIRE(t.submit(".x " + m2 + "(3, 0)"));	// FILE's main, called with ARGS
    CHECK(t.shown() == "5");
    REQUIRE(t.submit("args_ran"));
    CHECK(t.shown() == "3");

    InteractiveSession h;
    REQUIRE(h.begin("--std=c17"));
    h.host_honors_payloads(true);
    REQUIRE(h.submit(".x " + fh + "(5, 6)"));
    CHECK(h.payload() == madc::session_payload::call);
    REQUIRE(h.payload_argv().size() == 2u);
    CHECK(h.payload_argv()[0] == fh);
    CHECK(h.payload_argv()[1] == std::string(3 + fh.size(), ' ') + "(5, 6)");
    CHECK(h.function(host.c_str()) == (void *)NULL);	// nothing loaded yet
    const std::vector<std::string> argv = h.payload_argv();
    REQUIRE(h.load(argv[0]));
    int status = -1;
    REQUIRE(h.call_file(argv[0], argv[1], &status));
    CHECK(h.shown() == "30");

    const std::string files[] = { f2, f0, fb, m1, m2, nf, fh };
    for ( const std::string &f : files )
	std::remove(f.c_str());
}

// `%build FILE [-o OUT]` (plan §7f; madcide's Build): FILE compiled to a
// native executable under the session's standard, OUT by default FILE
// without its extension; a refused FILE's diagnostics are the session's. A
// payload host gets FILE and OUT, and build_file builds the text it hands
// over (an editor's buffer), not the file.
TEST_CASE("session commands: %build FILE [-o OUT] (§7f)")
{
    std::string prog = temp_source("int main(void) { return 3; }\n", "madc_repl_build");
    std::string bad = temp_source("int main(void) { return nope; }\n",
				  "madc_repl_buildbad");
    const std::string out = prog + "_exe";
    const std::string named = prog + "_named.c";	// an extension to drop
    {
	std::ofstream f(named.c_str());
	f << "int main(void) { return 5; }\n";
    }

    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    std::ostringstream err;
    s.program().error_stream = &err;
    REQUIRE(s.submit("%build " + prog + " -o " + out));
    CHECK(s.shown() == "built " + out);
    int rc = std::system(out.c_str());
    CHECK(WEXITSTATUS(rc) == 3);
    std::remove(out.c_str());
    REQUIRE(s.submit(".build " + named));		// OUT: FILE without .c
    CHECK(s.shown() == "built " + prog + "_named");
    rc = std::system((prog + "_named").c_str());
    CHECK(WEXITSTATUS(rc) == 5);
    std::remove((prog + "_named").c_str());
    // Refused: a FILE with no extension and no -o, no FILE, -o without OUT,
    // and a FILE that does not compile (its diagnostics are the session's;
    // nothing is built).
    CHECK_FALSE(s.submit("%build " + prog));
    CHECK(first_error(s) == "%build: FILE has no extension to drop; name the"
			    " executable: %build FILE -o OUT");
    CHECK_FALSE(s.submit("%build"));
    CHECK(first_error(s) == "%build needs a FILE");
    CHECK_FALSE(s.submit("%build " + prog + " -o"));
    CHECK(first_error(s) == "%build takes one FILE and -o OUT");
    err.str("");
    CHECK_FALSE(s.submit("%build " + bad + " -o " + out));
    CHECK(err.str().find("undeclared identifier 'nope'") != std::string::npos);
    CHECK(first_error(s).find("nope") != std::string::npos);
    CHECK_FALSE(std::ifstream(out.c_str()).good());

    InteractiveSession h;
    REQUIRE(h.begin("--std=c17"));
    h.host_honors_payloads(true);
    REQUIRE(h.submit("%build " + named));
    CHECK(h.payload() == madc::session_payload::build);
    CHECK(h.payload_argv() == (std::vector<std::string>{ named, prog + "_named" }));
    CHECK_FALSE(std::ifstream((prog + "_named").c_str()).good());	// nothing built
    REQUIRE(h.build_file(named, "int main(void) { return 6; }\n", out));
    CHECK(h.shown() == "built " + out);
    rc = std::system(out.c_str());
    CHECK(WEXITSTATUS(rc) == 6);			// the text handed over

    const std::string files[] = { prog, bad, named, out };
    for ( const std::string &f : files )
	std::remove(f.c_str());
}

// `%open FILE` and `%edit NAME` (plan §7f, the IDE layer): the file in its
// host's editor — the `open` payload (FILE, then the line %edit names, from
// the bindings' origin) for a payload host, else the terminal's editor
// ($EDITOR) run by the session. A name an entry defined has no file.
TEST_CASE("session commands: %open FILE and %edit NAME (§7f)")
{
    std::string lib = temp_source("int seven = 7;\nint thrice(int v) { return 3 * v; }\n",
				  "madc_repl_editlib");
    InteractiveSession h;
    REQUIRE(h.begin("--std=c17"));
    h.host_honors_payloads(true);
    REQUIRE(h.submit("%open " + lib + "_new.c"));		// need not exist
    CHECK(h.payload() == madc::session_payload::open);
    CHECK(h.payload_argv() == std::vector<std::string>{ lib + "_new.c" });
    REQUIRE(h.load(lib));
    REQUIRE(h.submit("int x = 1;"));
    REQUIRE(h.submit("%edit thrice"));
    CHECK(h.payload() == madc::session_payload::open);
    CHECK(h.payload_argv() == (std::vector<std::string>{ lib, "2" }));
    REQUIRE(h.submit(".edit seven"));
    CHECK(h.payload_argv() == (std::vector<std::string>{ lib, "1" }));
    CHECK_FALSE(h.submit("%edit x"));
    CHECK(first_error(h) == "%edit: 'x' was defined in REPL[2], which is no file to open");
    CHECK_FALSE(h.submit("%edit nosuch"));
    CHECK(first_error(h) == "%edit: 'nosuch' is not a name the session defined");
    CHECK_FALSE(h.submit("%edit 1x"));
    CHECK(first_error(h) == "%edit takes a name");
    CHECK_FALSE(h.submit("%open"));
    CHECK(first_error(h) == "%open takes one FILE");

    // No payload host: the session runs the terminal's editor ($EDITOR).
    const char *was = getenv("EDITOR");
    const std::string saved = was ? was : "";
    setenv("EDITOR", "true", 1);
    InteractiveSession s;
    REQUIRE(s.begin("--std=c17"));
    REQUIRE(s.submit("%open " + lib));
    CHECK(s.payload() == madc::session_payload::none);
    REQUIRE(s.load(lib));
    REQUIRE(s.submit("%edit thrice"));
    if ( was )
	setenv("EDITOR", saved.c_str(), 1);
    else
	unsetenv("EDITOR");
    std::remove(lib.c_str());
}

// The one rule a host reads an entry's command by (an IDE answers its own
// commands before the session sees the entry): the word, its command (none
// for a word the session does not own) and the rest of the line.
TEST_CASE("session commands: command_of reads an entry as the session does (§7f)")
{
    std::string word, arg;
    InteractiveSession::Command code = InteractiveSession::Command::help;
    REQUIRE(InteractiveSession::command_of(".L lib.c", word, code, arg));
    CHECK(word == "L");
    CHECK(code == InteractiveSession::Command::load);
    CHECK(arg == "lib.c");
    REQUIRE(InteractiveSession::command_of("%hi  bob ann", word, code, arg));
    CHECK(word == "hi");
    CHECK(code == InteractiveSession::Command::none);
    CHECK(arg == "bob ann");
    REQUIRE(InteractiveSession::command_of(":quit", word, code, arg));
    CHECK(code == InteractiveSession::Command::quit);
    CHECK(arg.empty());
    REQUIRE(InteractiveSession::command_of("?x", word, code, arg));
    CHECK(code == InteractiveSession::Command::pinfo);
    CHECK(arg == "x");
    CHECK_FALSE(InteractiveSession::command_of("int x;", word, code, arg));
    CHECK_FALSE(InteractiveSession::command_of("%1", word, code, arg));
}

// Slice 2 (plan §41.8a): `?name` / `%pinfo name` describe what the session
// knows of a name, in IPython's fields, each overload with its location
// (Julia), from the walk completion reads: `?` describes a name exactly
// when Tab offers it typed whole.
TEST_CASE("session commands: ?name and %pinfo describe a name")
{
    InteractiveSession c;
    REQUIRE(c.begin("--std=c17"));
    std::ostringstream err;
    c.program().error_stream = &err;
    REQUIRE(c.submit("#include <sys/stat.h>"));
    REQUIRE(c.submit("int x = 10;"));
    REQUIRE(c.submit("int sq(int n) { return n * n; }"));
    REQUIRE(c.submit("struct Point { int x; int y; };"));
    REQUIRE(c.submit("typedef struct Point Pt;"));
    REQUIRE(c.submit("int arr[3];"));
    REQUIRE(c.submit("#define N 5"));
    REQUIRE(c.submit("#define SQ(a) ((a) * (a))"));
    REQUIRE(c.submit("int __hidden = 1;"));
    struct { const char *entry, *shown; } described[] = {
	{ "?sq", "Signature: int sq(int n)  @ REPL[3]:1\nType:      function" },
	{ "%pinfo sq", "Signature: int sq(int n)  @ REPL[3]:1\nType:      function" },
	{ "  ?x", "Type:      int\nDefined:   @ REPL[2]:1" },
	{ "?arr", "Type:      int [3]\nDefined:   @ REPL[6]:1" },
	{ "?Point", "Type:      struct Point\nDefined:   @ REPL[4]:1\n"
		    "Members:   int x\n           int y" },
	{ "?Pt", "Typedef:   struct Point\nDefined:   @ REPL[5]:1\n"
		 "Members:   int x\n           int y" },
	{ "?N", "Macro:     #define N 5" },
	{ "?SQ", "Macro:     #define SQ(a) ((a) * (a))" },
	{ "?while", "while is a keyword of C" },
    };
    for ( size_t i = 0; i < sizeof(described) / sizeof(described[0]); ++i )
    {
	CAPTURE(described[i].entry);
	REQUIRE(c.submit(described[i].entry));
	CHECK(c.shown() == described[i].shown);
    }
    CHECK(c.submitted() == 18u);		// each takes REPL[N]
    // A C tag and a function of one name: each, the tag first.
    REQUIRE(c.submit("?stat"));
    const std::string st = c.shown();
    CHECK(st.compare(0, 23, "Type:      struct stat\n") == 0);
    CHECK(st.find("\n\nSignature: int stat(const char *, struct stat *)") != std::string::npos);
    // `?` alone is %help (Julia, IPython).
    REQUIRE(c.submit("?"));
    CHECK(c.shown().find("%pinfo NAME") != std::string::npos);
    // Refused: an unknown name at the column typed, and what is no name.
    CHECK_FALSE(c.submit("?nosuchname"));
    CHECK(err.str().find(":1:2: ") != std::string::npos);
    CHECK(err.str().find("'nosuchname' is not declared") != std::string::npos);
    CHECK_FALSE(c.submit("?x+1"));
    CHECK(err.str().find("%pinfo takes a name") != std::string::npos);
    // `?` and Tab agree: a reserved name the user declared is offered for a
    // word shaped like one and described; the session's own never.
    const char *names[] = { "sq", "x", "__hidden", "__madc_entry_1", "nosuchname" };
    for ( size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i )
    {
	CAPTURE(names[i]);
	const bool tab = has(complete_at_end(c, names[i]), names[i]);
	CHECK(tab == c.submit(std::string("?") + names[i]));
    }
    // Completion: after `?` the name completes as an entry; %pi is a command.
    size_t start = 0;
    CHECK(has(complete_at_end(c, "?s", &start), "sq"));
    CHECK(start == 1u);
    CHECK(complete_at_end(c, "%pi") == std::vector<std::string>{ "pinfo" });

    // C++: an overload pair, each with its location; a class's public members
    // (a const method's qualifier, a static member; the private one not).
    InteractiveSession p;
    REQUIRE(p.begin("--std=c++17"));
    REQUIRE(p.submit("int sq(int n) { return n * n; }"));
    REQUIRE(p.submit("double sq(double d) { return d * d; }"));
    REQUIRE(p.submit("struct Box { int w; static int count; "
		     "int area() const { return w * w; } private: int secret; };"));
    REQUIRE(p.submit("namespace geometry { int dim = 3; }"));
    REQUIRE(p.submit("?sq"));
    CHECK(p.shown() == "Signature: int sq(int n)  @ REPL[1]:1\n"
		       "Signature: double sq(double d)  @ REPL[2]:1\n"
		       "Type:      function");
    REQUIRE(p.submit("?Box"));
    CHECK(p.shown() == "Type:      Box\nMembers:   int w\n"
		       "           static int count\n           int area() const");
    REQUIRE(p.submit("?geometry"));
    CHECK(p.shown() == "Type:      namespace");

    // madc: the carrier, and a compiler-implemented public, which its
    // fragment declares as a template (no invented signature).
    InteractiveSession m;
    REQUIRE(m.begin("--std=madc"));
    REQUIRE(m.submit("var v = 5;"));
    REQUIRE(m.submit("?v"));
    CHECK(m.shown() == "Type:      var\nDefined:   @ REPL[1]:1");
    REQUIRE(m.submit("?println"));
    CHECK(m.shown() == "Type:      function template");
}

// Where a keyword comes from (owner, 2026-09-27): the language whose first
// standard has it ("C", "C++"), else the standard it first arrived in, among
// the session's languages; "madc" only for the dialect's own.
TEST_CASE("?name: where a keyword comes from")
{
    struct { const char *std, *name, *shown; } keywords[] = {
	{ "--std=madc", "while", "while is a keyword of C" },
	{ "--std=madc", "for", "for is a keyword of C" },
	{ "--std=madc", "const", "const is a keyword of C89" },	// after K&R C
	{ "--std=madc", "class", "class is a keyword of C++" },
	{ "--std=madc", "inline", "inline is a keyword of C++" },	// C++98 before C99
	{ "--std=madc", "constexpr", "constexpr is a keyword of C++11" },
	{ "--std=madc", "defer", "defer is a keyword of madc" },
	{ "--std=c17", "restrict", "restrict is a keyword of C99" },
	{ "--std=c17", "_Thread_local", "_Thread_local is a keyword of C11" },
	{ "--std=c17", "operator", "operator is a keyword of C++" },	// one C lacks
	{ "--std=c++17", "thread_local", "thread_local is a keyword of C++11" },
	{ "--std=c++17", "while", "while is a keyword of C" },
    };
    for ( size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); ++i )
    {
	CAPTURE(keywords[i].std);
	CAPTURE(keywords[i].name);
	InteractiveSession s;
	REQUIRE(s.begin(keywords[i].std));
	REQUIRE(s.submit(std::string("?") + keywords[i].name));
	CHECK(s.shown() == keywords[i].shown);
    }
    // A type keyword is its type too.
    InteractiveSession c;
    REQUIRE(c.begin("--std=c17"));
    REQUIRE(c.submit("?int"));
    CHECK(c.shown() == "int is a keyword of C\n\nType:      int");
    // The gate: every keyword madc reserves, under each language, is in the
    // standards' lists, so `?` never falls back to saying nothing of it.
    const char *stds[] = { "--std=madc", "--std=c89", "--std=c17", "--std=c23",
			   "--std=c++98", "--std=c++17", "--std=c++26" };
    for ( size_t i = 0; i < sizeof(stds) / sizeof(stds[0]); ++i )
    {
	CAPTURE(stds[i]);
	InteractiveSession k;
	REQUIRE(k.begin(stds[i]));
	k.program().keyword_map.for_each_readonly([&](const char *key, TokenKeyword *const &) -> bool {
	    CAPTURE(key);
	    const bool listed = Program::keyword_origin(key) != NULL;
	    CHECK(listed);
	    return false;
	});
    }
}

// Plan §41.11a step 3d: the bindings owner, IPython's %whos and Chthonia's
// Symbols view. The session's own objects and functions only (an included
// header's, a reserved name and a result never), sorted by name, each with
// its type, its value in the show's row form (no pointer followed, text
// included; at most 16 elements of an aggregate, then `…`) and its origin.
// The values come from a quiet entry that takes no number.
namespace {

const madc::value *binding_row(const madc::value &rows, const char *name)
{
    if ( !rows.is_array() )
	return NULL;
    for ( const madc::value &r : rows.as_array() )
	if ( r.is_object() && r.as_object().at("name").as_string() == name )
	    return &r;
    return NULL;
}

std::string binding_field(const madc::value &rows, const char *name,
			  const char *field)
{
    const madc::value *r = binding_row(rows, name);
    if ( !r )
	return "<no row>";
    const madc::value &f = r->as_object().at(field);
    return f.is_string() ? f.as_string() : std::to_string(f.as_integer());
}

} // namespace

TEST_CASE("session bindings: %whos lists the names the session defined (C17)")
{
    InteractiveSession c;
    REQUIRE(c.begin("--std=c17"));
    REQUIRE(c.submit("%whos"));
    CHECK(c.shown() == "Interactive namespace is empty.");
    REQUIRE(c.submit("#include <stdio.h>"));
    REQUIRE(c.submit("int count = 3;"));
    REQUIRE(c.submit("int square(int n) { return n * n; }"));
    REQUIRE(c.submit("char *p = (char *)1;"));
    REQUIRE(c.submit("const char *greeting = \"hi\";"));
    REQUIRE(c.submit("int big[20] = { 1, 2, 3 };"));
    REQUIRE(c.submit("struct P { int x; int y; } pt = { 1, 2 };"));
    REQUIRE(c.submit("int __hidden = 1;"));
    REQUIRE(c.submit("count + 1"));	// a result: never a binding
    const unsigned before = c.submitted();
    madc::value rows;
    c.bindings(rows);
    CHECK(c.submitted() == before);	// the quiet entry takes no number
    std::string names;
    for ( const madc::value &r : rows.as_array() )
	names += r.as_object().at("name").as_string() + " ";
    CHECK(names == "big count greeting p pt square ");
    CHECK(binding_field(rows, "count", "kind") == std::to_string((int)madc::name_kind::object));
    CHECK(binding_field(rows, "square", "kind") == std::to_string((int)madc::name_kind::function));
    CHECK(binding_field(rows, "count", "type") == "int");
    CHECK(binding_field(rows, "count", "value") == "3");
    CHECK(binding_field(rows, "count", "file") == "REPL[3]");
    CHECK(binding_field(rows, "count", "line") == "1");
    CHECK(binding_field(rows, "square", "type") == "int (int)");
    CHECK(binding_field(rows, "square", "value").empty());
    CHECK(binding_field(rows, "square", "file") == "REPL[4]");
    // The row's value spells no type (its Type column does, gdb's `info
    // locals`). A pointer shows its address; a character pointer then its
    // text, read so that a wild one cannot crash the backend.
    CHECK(binding_field(rows, "p", "value") == "0x1 <unreadable>");
    const std::string g = binding_field(rows, "greeting", "value");
    CHECK(g.compare(0, 2, "0x") == 0);
    CHECK(g.size() > 5);
    CHECK(g.compare(g.size() - 5, 5, " \"hi\"") == 0);
    // An aggregate: 16 elements, then `…`.
    CHECK(binding_field(rows, "big", "type") == "int [20]");
    CHECK(binding_field(rows, "big", "value")
	  == "{ 1, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, … }");
    CHECK(binding_field(rows, "pt", "value") == "{ .x = 1, .y = 2 }");
    // %whos prints the same rows as a table.
    REQUIRE(c.submit("%whos"));
    const std::string table = c.shown();
    CHECK(table.compare(0, 4, "Name") == 0);
    CHECK(table.find("Origin") != std::string::npos);
    CHECK(table.find("\ncount     int           3") != std::string::npos);
    CHECK(table.find("REPL[4]:1") != std::string::npos);
    // The session goes on.
    REQUIRE(c.submit("count * 2"));
    CHECK(c.shown() == "6");
}

TEST_CASE("session bindings: C++ containers, a class the session wrote, a qualified typedef")
{
    InteractiveSession c;
    REQUIRE(c.begin("--std=c++17"));
    REQUIRE(c.submit("#include <vector>"));
    REQUIRE(c.submit("#include <string>"));
    REQUIRE(c.submit("std::vector<int> v = { 1, 2, 3 };"));
    REQUIRE(c.submit("std::string s = \"hello\";"));
    REQUIRE(c.submit("struct Box { int a[3]; long size() const { return 3; } "
		     "int operator[](long i) const { return a[i]; } };"));
    REQUIRE(c.submit("Box b = { { 7, 8, 9 } };"));
    REQUIRE(c.submit("std::vector<int> w = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, "
		     "11, 12, 13, 14, 15, 16, 17, 18, 19, 20 };"));
    REQUIRE(c.submit("static std::string t = \"x\";"));
    REQUIRE(c.submit("t += \"y\";"));
    REQUIRE(c.submit("#include <map>"));
    REQUIRE(c.submit("std::map<int, int> m = { { 2, 20 }, { 1, 10 } };"));
    madc::value rows;
    c.bindings(rows);
    // A container's row is its elements: the Type column names it.
    CHECK(binding_field(rows, "v", "type") == "std::vector<int>");
    CHECK(binding_field(rows, "v", "value") == "{ 1, 2, 3 }");
    CHECK(binding_field(rows, "w", "value")
	  == "{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, … }");
    CHECK(binding_field(rows, "m", "value") == "{ { 1, 10 }, { 2, 20 } }");
    // A class the session wrote a method of walks by its members: a row
    // calls no code the session wrote.
    CHECK(binding_field(rows, "b", "value") == "{ .a = { 7, 8, 9 } }");
    // B94: a qualified typedef's object is the entry's, not its header's.
    CHECK(binding_field(rows, "s", "type") == "std::string");
    CHECK(binding_field(rows, "s", "value") == "\"hello\"");
    CHECK(binding_field(rows, "s", "file") == "REPL[4]");
    CHECK(binding_field(rows, "t", "value") == "\"xy\"");
    REQUIRE(c.submit("?s"));
    CHECK(c.shown().find("Defined:   @ REPL[4]:1") != std::string::npos);
    // B94's silent case: a unit's static is the session's name.
    REQUIRE(c.submit("t"));
    CHECK(c.shown() == "\"xy\"");
}

// BUGS.md B97: a template instantiation's type is its template-id as g++ and
// clang++ write it, the arguments by their source names and the defaulted
// ones left out, in %type, %whos's rows and the show alike. The oracle (g++
// 13 / clang++ 18, an incomplete `show<decltype(x)>`'s diagnostic):
// std::vector<int>, std::map<int, long int> / std::map<int, long>,
// std::__cxx11::list<int> / std::list<int>, Box<int>, Box<int, 8>,
// Two<int>, Two<int, long int> / Two<int, long>, and std::vector<int> for a
// written default. madc writes the separator without a space, as the show
// always has.
TEST_CASE("%type spells a template instantiation as g++ and clang++ do (B97)")
{
    InteractiveSession c;
    REQUIRE(c.begin("--std=c++17"));
    REQUIRE(c.submit("#include <vector>"));
    REQUIRE(c.submit("#include <map>"));
    REQUIRE(c.submit("#include <list>"));
    REQUIRE(c.submit("#include <string>"));
    REQUIRE(c.submit("template <class T, int N = 4> struct Box { T v[N]; };"));
    REQUIRE(c.submit("template <class T, class U = T> struct Two { T a; U b; };"));
    REQUIRE(c.submit("std::vector<int> v = { 1, 2, 3 };"));
    REQUIRE(c.submit("std::map<int, long> m;"));
    REQUIRE(c.submit("std::list<int> l;"));
    REQUIRE(c.submit("std::vector<std::string> vs;"));
    REQUIRE(c.submit("Box<int> b;"));
    REQUIRE(c.submit("Box<int, 8> b8;"));
    REQUIRE(c.submit("Two<int> t1;"));
    REQUIRE(c.submit("Two<int, long> t2;"));
    REQUIRE(c.submit("std::vector<int, std::allocator<int>> v2;"));
    struct { const char *entry, *type; } types[] = {
	{ "%type v", "std::vector<int>" },
	{ "%type m", "std::map<int,long>" },
	{ "%type l", "std::list<int>" },
	{ "%type vs", "std::vector<std::string>" },
	{ "%type b", "Box<int>" },
	{ "%type b8", "Box<int,8>" },
	{ "%type t1", "Two<int>" },
	{ "%type t2", "Two<int,long>" },
	{ "%type v2", "std::vector<int>" },
	{ "%type &v", "std::vector<int> *" },
    };
    for ( size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i )
    {
	CAPTURE(types[i].entry);
	REQUIRE(c.submit(types[i].entry));
	CHECK(c.shown() == types[i].type);
    }
    madc::value rows;
    c.bindings(rows);
    CHECK(binding_field(rows, "v", "type") == "std::vector<int>");
    CHECK(binding_field(rows, "b8", "type") == "Box<int,8>");
    REQUIRE(c.submit("v"));
    CHECK(c.shown() == "std::vector<int>{ 1, 2, 3 }");	// the show agrees
}

TEST_CASE("session bindings: a madc var (madc)")
{
    InteractiveSession c;
    REQUIRE(c.begin("--std=madc"));
    REQUIRE(c.submit("var cfg = { \"a\": 1, \"b\": 2 };"));
    REQUIRE(c.submit("long total = 5;"));
    madc::value rows;
    c.bindings(rows);
    CHECK(binding_field(rows, "cfg", "type") == "var");
    CHECK(binding_field(rows, "cfg", "value") == "{ \"a\": 1, \"b\": 2 }");
    CHECK(binding_field(rows, "total", "value") == "5");
}

// BUGS.md B94: an object whose type is a qualified typedef name
// (`std::string`) is the unit's that declared it, as an unqualified one's
// is: `?` cites the entry, and a unit's static is a session name (plan
// §41.5a), so a later entry changes the one object.
TEST_CASE("a qualified typedef's object is the entry's own (B94)")
{
    InteractiveSession c;
    REQUIRE(c.begin("--std=c++17"));
    REQUIRE(c.submit("#include <string>"));
    REQUIRE(c.submit("std::string s = \"hello\";"));
    REQUIRE(c.submit("?s"));
    CHECK(c.shown().find("Defined:   @ REPL[2]:1") != std::string::npos);
    REQUIRE(c.submit("static std::string t = \"x\";"));
    REQUIRE(c.submit("t += \"y\";"));
    REQUIRE(c.submit("t"));
    CHECK(c.shown() == "\"xy\"");
}
