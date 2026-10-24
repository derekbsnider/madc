// Unit tests for madc_modules — the module map and the ONE platform
// library-spelling owner (design: docs/plans/2026-09-06-ui-web-target-and-
// madcide-gui.md §3.1). `import name;`, `-l<name>` and the native lanes'
// link closure all spell a library through madc_module_library_spelling;
// these rows pin the rule per target OS so no call site ever needs a host
// #ifdef to know what image a name means.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;	// the prologue every tests/unit file uses
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <string>
#include "datadef.h"
#include "madc_modules.h"

TEST_CASE("module spelling: the platform rule for a bare library name")
{
	CHECK(madc_module_library_spelling("foo", TargetOS::Posix)   == "libfoo.so");
	CHECK(madc_module_library_spelling("foo", TargetOS::Darwin)  == "libfoo.dylib");
	CHECK(madc_module_library_spelling("foo", TargetOS::Windows) == "foo.dll");
}

TEST_CASE("module spelling: a registry row names the real runtime image per OS")
{
	CHECK(madc_module_library_spelling("m", TargetOS::Posix)   == "libm.so.6");
	CHECK(madc_module_library_spelling("m", TargetOS::Darwin)  == "libSystem.B.dylib");
	CHECK(madc_module_library_spelling("m", TargetOS::Windows) == "ucrtbase.dll");
	CHECK(madc_module_library_spelling("c", TargetOS::Posix)   == "libc.so.6");
	CHECK(madc_module_library_spelling("c", TargetOS::Darwin)  == "libSystem.B.dylib");
	CHECK(madc_module_library_spelling("c", TargetOS::Windows) == "ucrtbase.dll");
}

TEST_CASE("module spelling: a path or an already-spelled name passes verbatim (the -l contract)")
{
	CHECK(madc_module_library_spelling("/opt/x/libfoo.so", TargetOS::Posix) == "/opt/x/libfoo.so");
	CHECK(madc_module_library_spelling("libfoo.so", TargetOS::Posix)        == "libfoo.so");
	CHECK(madc_module_library_spelling("bar.dll", TargetOS::Windows)        == "bar.dll");
	CHECK(madc_module_library_spelling("libz.dylib", TargetOS::Darwin)      == "libz.dylib");
	// A path is a path on every target: either separator marks one.
	CHECK(madc_module_library_spelling("C:\\x\\foo", TargetOS::Windows)     == "C:\\x\\foo");
	CHECK(madc_module_library_spelling("./foo", TargetOS::Posix)            == "./foo");
	// A versioned soname is already spelled (libfoo.so.2 carries the suffix
	// inside the name); the rule must not wrap it as liblibfoo.so.2.so.
	CHECK(madc_module_library_spelling("libfoo.so.2", TargetOS::Posix)      == "libfoo.so.2");
}

TEST_CASE("module spelling: the target-default form follows madc_target_os")
{
	CHECK(madc_module_library_spelling("foo") == madc_module_library_spelling("foo", madc_target_os));
	CHECK(madc_module_library_spelling("m")   == madc_module_library_spelling("m", madc_target_os));
}

TEST_CASE("module rows: interface presence")
{
	const MadcModuleSpec *m = madc_module_find("m");
	REQUIRE(m != nullptr);
	CHECK(std::string(m->interface) == "math.h");
	CHECK(madc_module_library_spelling("foo", TargetOS::Posix) == "libfoo.so");
	CHECK(madc_module_library_spelling("foo", TargetOS::Darwin) == "libfoo.dylib");
	CHECK(madc_module_library_spelling("foo", TargetOS::Windows) == "foo.dll");
	const MadcModuleSpec *c = madc_module_find("c");
	REQUIRE(c != nullptr);
	CHECK(c->interface == nullptr);
	const MadcModuleSpec *none = madc_module_find("no-such-module");
	CHECK(none == nullptr);
	const MadcModuleSpec *empty = madc_module_find("");
	CHECK(empty == nullptr);
}

TEST_CASE("spelled library: any target's suffix marks an already-spelled name")
{
	CHECK(madc_spelled_library_p("libc.so.6"));
	CHECK(madc_spelled_library_p("libfoo.so"));
	CHECK(madc_spelled_library_p("libSystem.B.dylib"));
	CHECK(madc_spelled_library_p("ucrtbase.dll"));
	CHECK(!madc_spelled_library_p("libc++"));	// a darwin cover stem
	CHECK(!madc_spelled_library_p("libsystem_"));
	CHECK(!madc_spelled_library_p("m"));
	// Per target: a Mach-O writer asking "the target's user libraries" must
	// not see the Linux-hosted cross madc's own ELF cover set as Darwin's.
	CHECK(madc_spelled_library_p("libfoo.dylib", TargetOS::Darwin));
	CHECK(!madc_spelled_library_p("libm.so.6", TargetOS::Darwin));
	CHECK(!madc_spelled_library_p("libstdc++.so.6", TargetOS::Darwin));
	CHECK(madc_spelled_library_p("libm.so.6", TargetOS::Posix));
	CHECK(!madc_spelled_library_p("ucrtbase.dll", TargetOS::Posix));
	CHECK(madc_spelled_library_p("ucrtbase.dll", TargetOS::Windows));
}

TEST_CASE("dso suffix per OS")
{
	CHECK(std::string(madc_target_dso_suffix(TargetOS::Posix))   == ".so");
	CHECK(std::string(madc_target_dso_suffix(TargetOS::Darwin))  == ".dylib");
	CHECK(std::string(madc_target_dso_suffix(TargetOS::Windows)) == ".dll");
}

TEST_CASE("module open: an unopenable spelling reports an error and NULL")
{
	std::string err;
	void *h = madc_module_open("libmadc-no-such-module-xyz.so", err);
	CHECK(h == nullptr);
	CHECK(!err.empty());
}

// The module list a unit carries (__madc_module_deps): one writer, one
// reader. A link entry is its spelling; a madc module's, its reference ('@',
// 'g' or '-', its name, '=', its file).
TEST_CASE("module list: the entry spellings")
{
	std::string t;
	madc_module_deps_append(t, "libm.so.6");
	madc_module_deps_append(t, madc_module_reference("ui_web", "/x/ui_web.so", true));
	madc_module_deps_append(t, madc_module_reference("git", "/x/git.so", false));
	CHECK(t == std::string("libm.so.6\0@gui_web=/x/ui_web.so\0@-git=/x/git.so\0", 48));
}

// Opening the list: a library or a module that will not open is the import's
// error; an empty list opens.
TEST_CASE("module list: open")
{
	std::string err;
	CHECK(madc_module_deps_open("", NULL, err));
	CHECK(err.empty());
	std::string r;
	madc_module_deps_append(r, "libmadc-no-such-module-xyz.so");
	CHECK_FALSE(madc_module_deps_open(r.c_str(), r.c_str() + r.size(), err));
	CHECK(err.find("import: cannot load 'libmadc-no-such-module-xyz.so'") == 0);
	std::string m;
	madc_module_deps_append(m, madc_module_reference("nosuch", "/no/such/dir/nosuch.so", false));
	m.push_back('\0');	// the emitted table's closing NUL: NULL bounds it
	CHECK_FALSE(madc_module_deps_open(m.c_str(), NULL, err));
	CHECK(err == "import: no module named 'nosuch'");
}

// A reference opens the file it records while that file is there; a file that
// is no madc module is refused with the reason (the import's own check).
TEST_CASE("module reference: the recorded file, described")
{
	std::string err;
	CHECK(madc_module_reference("git", "/x/git.so", false) == "@-git=/x/git.so");
	CHECK(madc_module_reference("ui_web", "/x/ui_web.so", true) == "@gui_web=/x/ui_web.so");
	CHECK(!madc_module_open_reference("git", err));
	CHECK(err.find("is not a module reference") != std::string::npos);
	CHECK(!madc_module_open_reference("@-nosuch=", err));
	CHECK(err == "import: no module named 'nosuch'");
}
