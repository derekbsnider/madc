#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define MADC_UNIT_TEST
#include "doctest.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

thread_local bool madc_verbose = false;
#define DBG(x) do { } while (0)

#include "../../src/madc_posix_io.h"

using madc::detail::host_path_dirname;
using madc::detail::host_path_basename;
using madc::detail::host_path_within;
using madc::detail::host_path_absolute;
using madc::detail::host_path_join;
using madc::detail::host_path_expand_home;

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

TEST_CASE("host_path_absolute: a leading separator names its own root")
{
	CHECK(host_path_absolute("/usr/include"));
	CHECK(host_path_absolute("/"));
	CHECK_FALSE(host_path_absolute(""));
	CHECK_FALSE(host_path_absolute("inc"));
	CHECK_FALSE(host_path_absolute("./inc"));
	CHECK_FALSE(host_path_absolute("../inc"));
}

#ifdef _WIN32
TEST_CASE("host_path_absolute: a drive or either separator on a Windows host")
{
	// madc::canonical_path's spelling of a project's include directory:
	// a '/'-only test joined it onto the manifest's own directory.
	CHECK(host_path_absolute("Z:\\workspace\\madc\\tmp\\inc"));
	CHECK(host_path_absolute("C:/src/demo.c"));
	CHECK(host_path_absolute("c:demo.c"));		// drive-relative: its own drive
	CHECK(host_path_absolute("\\\\server\\share"));	// UNC
	CHECK(host_path_absolute("\\rooted"));
	CHECK_FALSE(host_path_absolute("1:x"));
	CHECK_FALSE(host_path_absolute("inc\\x.h"));
}
#else
TEST_CASE("host_path_absolute: a drive spelling is a relative name on POSIX")
{
	CHECK_FALSE(host_path_absolute("C:\\src"));
	CHECK_FALSE(host_path_absolute("\\rooted"));
}
#endif

TEST_CASE("host_path_join: a relative path under a directory, one separator")
{
	CHECK(host_path_join("/work/proj", "src/a.c") == "/work/proj/src/a.c");
	CHECK(host_path_join("/work/proj/", "a.c") == "/work/proj/a.c");
	CHECK(host_path_join("/", "a.c") == "/a.c");
	CHECK(host_path_join("/work", "/etc/x") == "/etc/x");	// absolute stays
	CHECK(host_path_join("/work", "") == "");
	CHECK(host_path_join("", "a.c") == "a.c");
	CHECK(host_path_join(".", "a.c") == "a.c");		// never a ./ twin
#ifdef _WIN32
	CHECK(host_path_join("Z:\\work\\", "a.c") == "Z:\\work\\a.c");
	CHECK(host_path_join("Z:\\work", "C:\\x") == "C:\\x");
#endif
}

TEST_CASE("host_path_type_of: nothing, a file, a directory")
{
	using madc::detail::host_path_type;
	using madc::detail::host_path_type_of;
	CHECK(host_path_type_of("") == host_path_type::none);
	CHECK(host_path_type_of("no/such/path/at/all") == host_path_type::none);
	CHECK(host_path_type_of(".") == host_path_type::directory);
	const char *made = "test_posix_io_type_of.tmp";
	FILE *f = fopen(made, "w");
	REQUIRE(f);
	fclose(f);
	CHECK(host_path_type_of(made) == host_path_type::file);
	remove(made);
	CHECK(host_path_type_of(made) == host_path_type::none);
}

TEST_CASE("host_path_expand_home: ~ and ~/x against the home, nothing else")
{
	std::string out;
#ifdef _WIN32
	const char *home = getenv("USERPROFILE");
#else
	const char *home = getenv("HOME");
#endif
	REQUIRE(home);
	CHECK(host_path_expand_home("~/notes.txt", out));
	CHECK(out == std::string(home) + "/notes.txt");
	CHECK(host_path_expand_home("~", out));
	CHECK(out == home);
	CHECK_FALSE(host_path_expand_home("~user/x", out));	// not ours
	CHECK_FALSE(host_path_expand_home("a/~/b", out));
	CHECK_FALSE(host_path_expand_home("", out));
}

TEST_CASE("map_exec_region_at: placed exactly, refused when taken, writable on request")
{
	// An address far from the image, the heap and the default mapping area.
	void *want = (void *)(uintptr_t)0x300000000000ULL;
	const std::size_t len = 4096;
	void *p = madc::detail::map_exec_region_at(want, len);
	REQUIRE(p == want);
	CHECK(madc::detail::map_exec_region_at(want, len) == nullptr);	// taken
	REQUIRE(madc::detail::protect_exec_region(p, len, true) == 0);
	*(volatile unsigned char *)p = 0xc3;
	CHECK(*(volatile unsigned char *)p == 0xc3);
	CHECK(madc::detail::protect_exec_region(p, len, false) == 0);
	CHECK(*(volatile unsigned char *)p == 0xc3);
	CHECK(madc::detail::unmap_exec_region(p, len) == 0);
}

// The one extension / stem rule (check-one-path-stem.sh): the extension
// follows the last dot of the FINAL component; a dot in a directory and a
// leading dot are none.
TEST_CASE("host_path_drop_extension / host_path_stem: the final component's last dot")
{
	using madc::detail::host_path_drop_extension;
	using madc::detail::host_path_stem;
	CHECK(host_path_drop_extension("foo.c") == "foo");
	CHECK(host_path_drop_extension("dir.d/foo.c") == "dir.d/foo");
	CHECK(host_path_drop_extension("dir.d/foo") == "dir.d/foo");
	CHECK(host_path_drop_extension("../tool") == "../tool");	// was "."
	CHECK(host_path_drop_extension(".bashrc") == ".bashrc");
	CHECK(host_path_drop_extension("dir/.hidden") == "dir/.hidden");
	CHECK(host_path_drop_extension("a.tar.gz") == "a.tar");
	CHECK(host_path_stem("dir/foo.tar.gz") == "foo.tar");
	CHECK(host_path_stem("foo") == "foo");
	CHECK(host_path_stem("dir.d/") == "");
#ifdef _WIN32
	CHECK(host_path_stem("C:\\src\\demo.c") == "demo");
	CHECK(host_path_drop_extension("C:\\v1.2\\tool") == "C:\\v1.2\\tool");
#else
	CHECK(host_path_stem("dir\\demo.c") == "dir\\demo");	// '\' is a name character
#endif
}

// held_file: one process at a time holds a file (discovery's ownership and
// liveness). ORACLE: flock(2) — an exclusive lock on one open file refuses a
// second open file's non-blocking LOCK_EX with EWOULDBLOCK, in this process
// as in any other (flock locks belong to the open file, not the process).
TEST_CASE("held_file: one holder at a time; release(true) removes the file")
{
    const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
	+ "/madc_held_file_unit.json";
    std::remove(path.c_str());
    CHECK_FALSE(madc::detail::host_file_held(path));	// no file: no holder
    madc::detail::held_file a, b;
    REQUIRE(a.open_held(path));
    CHECK(a.held());
    CHECK(madc::detail::host_file_held(path));
    CHECK_FALSE(b.open_held(path));			// held elsewhere
    CHECK(a.replace("{\"x\":1}\n"));
    a.release();
    CHECK_FALSE(madc::detail::host_file_held(path));	// let go: no live holder
    REQUIRE(b.open_held(path));
    b.release(true);
    FILE *gone = std::fopen(path.c_str(), "r");
    CHECK(gone == NULL);					// removed with the hold
    if ( gone )
	std::fclose(gone);
}
