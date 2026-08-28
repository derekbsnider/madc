#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define MADC_UNIT_TEST
#include "doctest.h"

#include <string>

thread_local bool madc_verbose = false;
#define DBG(x) do { } while (0)

#include "../../src/madc_posix_io.h"

using madc::detail::host_path_dirname;
using madc::detail::host_path_basename;

TEST_CASE("host_path_dirname/basename: POSIX separators on every host")
{
	CHECK(host_path_dirname("tools/madcide/madcide.mad") == "tools/madcide/");
	CHECK(host_path_basename("tools/madcide/madcide.mad") == "madcide.mad");
	CHECK(host_path_dirname("plain.mad") == "");
	CHECK(host_path_basename("plain.mad") == "plain.mad");
	CHECK(host_path_dirname("/root.mad") == "/");
	CHECK(host_path_basename("/root.mad") == "root.mad");
	CHECK(host_path_dirname("") == "");
	CHECK(host_path_basename("") == "");
}

#ifdef _WIN32
TEST_CASE("host_path_dirname/basename: '\\' separates on a Windows host")
{
	// The owner's first genuine-Windows madcide launch: cmd tab-completion
	// spells the source path with '\', and the include chain must derive
	// the source directory from it (relative #include resolution).
	CHECK(host_path_dirname("tools\\madcide\\madcide.mad") == "tools\\madcide\\");
	CHECK(host_path_basename("tools\\madcide\\madcide.mad") == "madcide.mad");
	// Mixed spellings appear once the include chain concatenates '/'.
	CHECK(host_path_dirname("tools\\madcide/madcide_core.inc") == "tools\\madcide/");
	CHECK(host_path_basename("C:\\src\\demo.c") == "demo.c");
}
#else
TEST_CASE("host_path_dirname/basename: '\\' is a filename character on POSIX")
{
	CHECK(host_path_dirname("dir\\notasep.mad") == "");
	CHECK(host_path_basename("dir\\notasep.mad") == "dir\\notasep.mad");
}
#endif
