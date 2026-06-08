#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include "doctest.h"
#include "madc_project.h"
#include <cstdio>

// Helper: write a temp file, return its path (in /tmp, unique per test name).
static std::string write_tmp(const char *name, const std::string &content) {
	std::string path = std::string("/tmp/madc_test_") + name;
	FILE *f = fopen(path.c_str(), "wb");
	REQUIRE(f != nullptr);
	fwrite(content.data(), 1, content.size(), f);
	fclose(f);
	return path;
}

TEST_CASE("read_compile_commands: single TU, command string") {
	std::string json =
		"[{\"directory\":\"/proj\",\"file\":\"a.c\","
		"\"command\":\"gcc -c -DFOO -DBAR=2 -I/proj/inc -std=c89 a.c -o a.o\"}]";
	std::string path = write_tmp("cc_single.json", json);
	ProjectManifest m; std::string err;
	REQUIRE(read_compile_commands(path, m, err));
	REQUIRE(m.tus.size() == 1);
	CHECK(m.tus[0].working_dir == "/proj");
	CHECK(m.tus[0].file == "/proj/a.c");
	CHECK(m.tus[0].std_option == "c89");
	REQUIRE(m.tus[0].defines.size() == 2);
	CHECK(m.tus[0].defines[0] == "FOO");
	CHECK(m.tus[0].defines[1] == "BAR=2");
	REQUIRE(m.tus[0].include_dirs.size() == 1);
	CHECK(m.tus[0].include_dirs[0] == "/proj/inc");
}

TEST_CASE("read_compile_commands: arguments array + two TUs") {
	std::string json =
		"[{\"directory\":\"/p\",\"file\":\"a.c\","
		" \"arguments\":[\"gcc\",\"-c\",\"-DA\",\"a.c\"]},"
		" {\"directory\":\"/p\",\"file\":\"b.c\","
		" \"arguments\":[\"gcc\",\"-c\",\"-DB\",\"b.c\"]}]";
	std::string path = write_tmp("cc_two.json", json);
	ProjectManifest m; std::string err;
	REQUIRE(read_compile_commands(path, m, err));
	REQUIRE(m.tus.size() == 2);
	CHECK(m.tus[0].file == "/p/a.c");
	CHECK(m.tus[1].file == "/p/b.c");
	REQUIRE(m.tus[0].defines.size() == 1);
	CHECK(m.tus[0].defines[0] == "A");
	CHECK(m.tus[1].defines[0] == "B");
	CHECK(m.entry == "main");
}

TEST_CASE("read_compile_commands: malformed input is rejected") {
	ProjectManifest m; std::string err;

	std::string bad = write_tmp("cc_bad.json", "[ this is not json ");
	CHECK_FALSE(read_compile_commands(bad, m, err));
	CHECK_FALSE(err.empty());

	err.clear();
	std::string notarr = write_tmp("cc_obj.json", "{\"x\":1}");
	CHECK_FALSE(read_compile_commands(notarr, m, err));
	CHECK_FALSE(err.empty());
}
