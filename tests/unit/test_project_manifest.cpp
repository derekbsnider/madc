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

TEST_CASE("read_project_manifest: single TU, command string") {
	std::string json =
		"[{\"directory\":\"/proj\",\"file\":\"a.c\","
		"\"command\":\"gcc -c -DFOO -DBAR=2 -I/proj/inc -std=c89 a.c -o a.o\"}]";
	std::string path = write_tmp("cc_single.json", json);
	ProjectManifest m; std::string err;
	REQUIRE(read_project_manifest(path, m, err));
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

TEST_CASE("read_project_manifest: arguments array + two TUs") {
	std::string json =
		"[{\"directory\":\"/p\",\"file\":\"a.c\","
		" \"arguments\":[\"gcc\",\"-c\",\"-DA\",\"a.c\"]},"
		" {\"directory\":\"/p\",\"file\":\"b.c\","
		" \"arguments\":[\"gcc\",\"-c\",\"-DB\",\"b.c\"]}]";
	std::string path = write_tmp("cc_two.json", json);
	ProjectManifest m; std::string err;
	REQUIRE(read_project_manifest(path, m, err));
	REQUIRE(m.tus.size() == 2);
	CHECK(m.tus[0].file == "/p/a.c");
	CHECK(m.tus[1].file == "/p/b.c");
	REQUIRE(m.tus[0].defines.size() == 1);
	CHECK(m.tus[0].defines[0] == "A");
	CHECK(m.tus[1].defines[0] == "B");
	CHECK(m.entry == "main");
}

TEST_CASE("read_project_manifest: malformed input is rejected") {
	ProjectManifest m; std::string err;

	std::string bad = write_tmp("cc_bad.json", "[ this is not json ");
	CHECK_FALSE(read_project_manifest(bad, m, err));
	CHECK_FALSE(err.empty());

	// An object WITHOUT "tus" is not a manifest (the native shape's one
	// required key) — junk must not read as an empty project.
	err.clear();
	std::string notarr = write_tmp("cc_obj.json", "{\"x\":1}");
	CHECK_FALSE(read_project_manifest(notarr, m, err));
	CHECK_FALSE(err.empty());

	err.clear();
	std::string notjson = write_tmp("cc_scalar.json", "42");
	CHECK_FALSE(read_project_manifest(notjson, m, err));
	CHECK_FALSE(err.empty());
}

// ---- the NATIVE object shape (owner ruling 2026-08-31): what madcide
// writes; --project reads it interchangeably with the cc.json import.

TEST_CASE("read_project_manifest: native object — bare-string TUs, entry, "
	  "output; paths resolve against the manifest's directory") {
	std::string json =
		"{\"tus\":[\"a.mad\",\"sub/b.mad\"],"
		"\"entry\":\"start\",\"output\":\"prog\"}";
	std::string path = write_tmp("native_basic.prj.json", json);
	ProjectManifest m; std::string err;
	REQUIRE(read_project_manifest(path, m, err));
	REQUIRE(m.tus.size() == 2);
	CHECK(m.tus[0].file == "/tmp/a.mad");
	CHECK(m.tus[0].working_dir == "/tmp");
	CHECK(m.tus[1].file == "/tmp/sub/b.mad");
	CHECK(m.entry == "start");
	CHECK(m.output_name == "prog");
}

TEST_CASE("read_project_manifest: native object — TU objects carry the "
	  "ProjectTU fields; unknown keys pass through untouched") {
	std::string json =
		"{\"tus\":[{\"file\":\"a.c\",\"directory\":\"/p\","
		"\"defines\":[\"FOO\",\"BAR=2\"],\"include_dirs\":[\"inc\"],"
		"\"std\":\"c89\",\"stdlib\":\"libc++\",\"future_key\":1}],"
		"\"commands\":[{\"name\":\"later\"}]}";
	std::string path = write_tmp("native_tu.prj.json", json);
	ProjectManifest m; std::string err;
	REQUIRE(read_project_manifest(path, m, err));
	REQUIRE(m.tus.size() == 1);
	CHECK(m.tus[0].file == "/p/a.c");
	CHECK(m.tus[0].working_dir == "/p");
	REQUIRE(m.tus[0].defines.size() == 2);
	CHECK(m.tus[0].defines[0] == "FOO");
	CHECK(m.tus[0].defines[1] == "BAR=2");
	REQUIRE(m.tus[0].include_dirs.size() == 1);
	CHECK(m.tus[0].include_dirs[0] == "/p/inc");
	CHECK(m.tus[0].std_option == "c89");
	CHECK(m.tus[0].stdlib_option == "libc++");
	CHECK(m.entry == "main");	// the default stands when unspecified
}

TEST_CASE("read_project_manifest: a manifest in the cwd adds no ./ prefix "
	  "(buffer paths compare by spelling — ./x and x must not open twice)") {
	FILE *f = fopen("madc_unit_cwd.prj.json", "wb");
	REQUIRE(f != nullptr);
	fputs("{\"tus\":[\"a.mad\"]}", f);
	fclose(f);
	ProjectManifest m; std::string err;
	REQUIRE(read_project_manifest("madc_unit_cwd.prj.json", m, err));
	REQUIRE(m.tus.size() == 1);
	CHECK(m.tus[0].file == "a.mad");
	CHECK(m.tus[0].working_dir == ".");
	std::remove("madc_unit_cwd.prj.json");
}

TEST_CASE("read_project_manifest: native object — empty tus is a valid "
	  "(empty) project; a missing file field is rejected") {
	ProjectManifest m; std::string err;
	std::string empty = write_tmp("native_empty.prj.json", "{\"tus\":[]}");
	REQUIRE(read_project_manifest(empty, m, err));
	CHECK(m.tus.empty());

	ProjectManifest m2;
	std::string nofile = write_tmp("native_nofile.prj.json",
				       "{\"tus\":[{\"directory\":\"/p\"}]}");
	CHECK_FALSE(read_project_manifest(nofile, m2, err));
	CHECK_FALSE(err.empty());
}
