#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define MADC_UNIT_TEST
#include "doctest.h"

#include <string>

thread_local bool madc_verbose = false;
#define DBG(x) do { } while (0)

#include "../../src/madc_posix_io.h"

using madc::detail::host_path_dirname;
using madc::detail::host_path_basename;
using madc::detail::host_path_within;

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

TEST_CASE("host_path_within: a separator must follow the directory, a sibling is outside")
{
	size_t at = 0;
	CHECK(host_path_within("/usr/include/", "/usr/include/stdio.h", &at));
	CHECK(at == 13);
	CHECK(host_path_within("/usr/include", "/usr/include/sys/stat.h", &at));
	CHECK(at == 13);
	// the sibling a bare prefix compare also matched
	CHECK_FALSE(host_path_within("/usr/lib/gcc/13/include", "/usr/lib/gcc/13/include-fixed/limits.h"));
	// the directory itself, and an empty directory, contain nothing
	CHECK_FALSE(host_path_within("/usr/include/", "/usr/include/"));
	CHECK_FALSE(host_path_within("/usr/include", "/usr/include"));
	CHECK_FALSE(host_path_within("", "/usr/include/stdio.h"));
	// the root keeps its one separator
	CHECK(host_path_within("/", "/etc/hosts", &at));
	CHECK(at == 1);
	CHECK_FALSE(host_path_within("/usr/include/", "/usr/local/include/x.h"));
}

#ifdef _WIN32
TEST_CASE("host_path_within: the canonicalizer's '\\' separates on a Windows host")
{
	// GetFullPathName spells every separator '\\': a system include dir and
	// a header under it, as canonical_path_for_compare returns them (B167).
	size_t at = 0;
	CHECK(host_path_within("Z:\\usr\\x86_64-w64-mingw32\\include",
			       "Z:\\usr\\x86_64-w64-mingw32\\include\\wtypes.h", &at));
	CHECK(at == 34);
	CHECK(host_path_within("Z:\\usr\\include\\", "Z:\\usr\\include\\pshpack1.h"));
	CHECK(host_path_within("Z:\\", "Z:\\usr", &at));
	CHECK(at == 3);
	CHECK(host_path_within("C:\\src", "C:\\src/demo.c"));	// a mixed spelling
	CHECK_FALSE(host_path_within("C:\\src", "C:\\src2\\demo.c"));
}
#else
TEST_CASE("host_path_within: '\\' is a filename character on POSIX")
{
	CHECK_FALSE(host_path_within("/repo", "/repo\\file"));
	CHECK(host_path_within("/repo", "/repo/a\\b", nullptr));
}
#endif
