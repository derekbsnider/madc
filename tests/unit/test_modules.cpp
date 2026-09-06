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
	const MadcModuleSpec *c = madc_module_find("c");
	REQUIRE(c != nullptr);
	CHECK(c->interface == nullptr);
	const MadcModuleSpec *none = madc_module_find("no-such-module");
	CHECK(none == nullptr);
	const MadcModuleSpec *empty = madc_module_find("");
	CHECK(empty == nullptr);
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
