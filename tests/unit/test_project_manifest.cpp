#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include "doctest.h"
#include "madc_project.h"
#include "libmadc/sysinfo.h"	// madc::sys.version: the running release
#include "../../src/madc_posix_io.h"	// host_path_make_parents: the writer's tree
#include <cstdio>
#include <fstream>
#include <sstream>

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

TEST_CASE("read_project_manifest: a field of the wrong JSON type is refused, "
	  "never thrown") {
	// value("file", std::string()) throws type_error.302 on a number: it
	// aborted `madc --project`.
	ProjectManifest m; std::string err;
	std::string native = write_tmp("native_badtype.prj.json",
				       "{\"tus\":[{\"file\":3}]}");
	CHECK_FALSE(read_project_manifest(native, m, err));
	CHECK(err.find("wrong type") != std::string::npos);
	ProjectManifest c; err.clear();
	std::string cc = write_tmp("cc_badtype.json",
				   "[{\"directory\":7,\"file\":\"a.c\"}]");
	CHECK_FALSE(read_project_manifest(cc, c, err));
	CHECK(err.find("wrong type") != std::string::npos);
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
	CHECK(m.tus[0].working_dir == "/tmp/");	// host_path_dirname's spelling
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

#ifdef _WIN32
TEST_CASE("read_project_manifest: a manifest named with '\\' resolves its "
	  "TUs against its own directory (the host's separators)") {
	// A '/'-only split read "." for this path: the TUs resolved against
	// the cwd instead of the manifest's directory.
	std::string path = write_tmp("native_bs.prj.json", "{\"tus\":[\"a.mad\"]}");
	for (char &c : path)
		if (c == '/') c = '\\';
	ProjectManifest m; std::string err;
	REQUIRE(read_project_manifest(path, m, err));
	REQUIRE(m.tus.size() == 1);
	CHECK(m.tus[0].file == "\\tmp\\a.mad");
}
#endif

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

TEST_CASE("read_project_manifest: native object — the project kind: absent = "
	  "console, \"gui\" reads, an unknown word refuses loud") {
	// madcide polish P2c: the kind decides the Windows executable's
	// SUBSYSTEM (gui = no console at start, gcc's -mwindows) and where an
	// IDE sends the program's output when it runs it.
	ProjectManifest m; std::string err;
	std::string plain = write_tmp("native_kind_none.prj.json", "{\"tus\":[]}");
	REQUIRE(read_project_manifest(plain, m, err));
	CHECK(m.kind == ProjectKind::console);

	ProjectManifest g;
	std::string gui = write_tmp("native_kind_gui.prj.json",
				    "{\"tus\":[\"a.mad\"],\"kind\":\"gui\"}");
	REQUIRE(read_project_manifest(gui, g, err));
	CHECK(g.kind == ProjectKind::gui);
	CHECK(std::string(project_kind_name(g.kind)) == "gui");

	ProjectManifest c;
	std::string con = write_tmp("native_kind_console.prj.json",
				    "{\"tus\":[],\"kind\":\"console\"}");
	REQUIRE(read_project_manifest(con, c, err));
	CHECK(c.kind == ProjectKind::console);
	CHECK(std::string(project_kind_name(c.kind)) == "console");

	ProjectManifest bad;
	std::string unk = write_tmp("native_kind_bad.prj.json",
				    "{\"tus\":[],\"kind\":\"service\"}");
	CHECK_FALSE(read_project_manifest(unk, bad, err));
	CHECK(err.find("kind") != std::string::npos);
	ProjectManifest notstr;
	std::string num = write_tmp("native_kind_num.prj.json",
				    "{\"tus\":[],\"kind\":2}");
	CHECK_FALSE(read_project_manifest(num, notstr, err));

	ProjectKind k;
	CHECK(project_kind_from_name("gui", k));
	CHECK(k == ProjectKind::gui);
	CHECK_FALSE(project_kind_from_name("GUI", k));	// the spelling is the table's
}

TEST_CASE("read_project_manifest: native object — the icon: absent = none, a "
	  "name resolves against the manifest's directory, a non-name refuses") {
	// The program's Windows icon (.ico): a PE image carries it as its
	// icon resources (madc_pe_icon.h).
	ProjectManifest m; std::string err;
	std::string plain = write_tmp("native_icon_none.prj.json", "{\"tus\":[]}");
	REQUIRE(read_project_manifest(plain, m, err));
	CHECK(m.icon.empty());

	ProjectManifest i;
	std::string named = write_tmp("native_icon.prj.json",
				      "{\"tus\":[],\"icon\":\"app.ico\"}");
	REQUIRE(read_project_manifest(named, i, err));
	CHECK(i.icon == "/tmp/app.ico");

	ProjectManifest abs;
	std::string absolute = write_tmp("native_icon_abs.prj.json",
					 "{\"tus\":[],\"icon\":\"/x/app.ico\"}");
	REQUIRE(read_project_manifest(absolute, abs, err));
	CHECK(abs.icon == "/x/app.ico");

	ProjectManifest empty;
	std::string e = write_tmp("native_icon_empty.prj.json",
				  "{\"tus\":[],\"icon\":\"\"}");
	CHECK_FALSE(read_project_manifest(e, empty, err));
	CHECK(err.find("icon") != std::string::npos);
	ProjectManifest notstr;
	std::string num = write_tmp("native_icon_num.prj.json",
				    "{\"tus\":[],\"icon\":7}");
	CHECK_FALSE(read_project_manifest(num, notstr, err));
	CHECK(err.find("icon") != std::string::npos);
}

TEST_CASE("read_project_manifest: native object — \"madc\" is the oldest madc "
	  "release that builds the project: absent = any, an older madc refuses "
	  "naming both versions, a non-version refuses") {
	// The running version is madc::sys.version (the build's VERSION).
	const std::string running = madc::sys.version;
	ProjectManifest m; std::string err;
	std::string plain = write_tmp("native_madc_none.prj.json", "{\"tus\":[]}");
	REQUIRE(read_project_manifest(plain, m, err));
	CHECK(m.madc_min.empty());

	ProjectManifest same;
	std::string eq = write_tmp("native_madc_same.prj.json",
				   "{\"tus\":[],\"madc\":\"" + running + "\"}");
	REQUIRE(read_project_manifest(eq, same, err));
	CHECK(same.madc_min == running);

	// Fields compare as numbers: 0.99.0 is older than 0.101.0.
	ProjectManifest older;
	std::string old = write_tmp("native_madc_older.prj.json",
				    "{\"tus\":[],\"madc\":\"0.99.0\"}");
	CHECK(read_project_manifest(old, older, err));

	ProjectManifest newer;
	std::string fut = write_tmp("native_madc_newer.prj.json",
				    "{\"tus\":[],\"madc\":\"999999.0.0\"}");
	CHECK_FALSE(read_project_manifest(fut, newer, err));
	CHECK(err == "this project needs madc 999999.0.0 or newer (this is madc "
		     + running + ")");

	const char *bad[] = { "\"1.2\"", "\"1.2.3.4\"", "\"v1.2.3\"", "\"1.2.x\"",
			      "\"\"", "\" 1.2.3\"", "7", "null" };
	for (const char *b : bad) {
		ProjectManifest r;
		std::string f = write_tmp("native_madc_bad.prj.json",
					  std::string("{\"tus\":[],\"madc\":") + b + "}");
		CHECK_FALSE(read_project_manifest(f, r, err));
		CHECK(err == "\"madc\" must be a release version (major.minor.patch)");
	}
}

// ---- the manifest's one writer (split arc 1B.8): madcide's bytes —
// js::stringify(proj, 2) and a newline, keys sorted.

static std::string slurp(const std::string &path) {
	std::ifstream f(path.c_str(), std::ios::binary);
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static void touch(const std::string &path) {
	madc::detail::host_path_make_parents(path);
	FILE *f = fopen(path.c_str(), "wb");
	REQUIRE(f != nullptr);
	fputs("int main(void) { return 0; }\n", f);
	fclose(f);
}

TEST_CASE("project_manifest_create: madcide's empty project; an existing "
	  "file is refused") {
	std::string path = "/tmp/madc_test_writer_new.prj.json";
	std::remove(path.c_str());
	std::string err;
	REQUIRE(project_manifest_create(path, err));
	CHECK(slurp(path) == "{\n  \"entry\": \"main\",\n  \"tus\": []\n}\n");
	CHECK_FALSE(project_manifest_create(path, err));
	CHECK(err.find("exists") != std::string::npos);
	ProjectManifest m;
	REQUIRE(read_project_manifest(path, m, err));
	CHECK(m.tus.empty());
	CHECK(m.entry == "main");
	std::remove(path.c_str());
}

TEST_CASE("project_manifest_add_tu / remove_tu: TUs named relative to the "
	  "manifest's directory below it, canonical outside it; every other key "
	  "kept") {
	const std::string dir = "/tmp/madc_test_writer";
	const std::string path = dir + "/w.prj.json";
	const std::string a = dir + "/a.c", b = dir + "/sub/b.c";
	const std::string out = "/tmp/madc_test_writer_out.c";
	touch(a);
	touch(b);
	touch(out);
	{
		FILE *f = fopen(path.c_str(), "wb");
		REQUIRE(f != nullptr);
		fputs("{\"commands\":[{\"name\":\"later\"}],"
		      "\"tus\":[{\"file\":\"o.c\",\"defines\":[\"X\"]}],"
		      "\"entry\":\"main\"}", f);
		fclose(f);
	}
	std::string err;
	REQUIRE(project_manifest_add_tu(path, a, err));
	REQUIRE(project_manifest_add_tu(path, dir + "/sub/../sub/b.c", err));
	REQUIRE(project_manifest_add_tu(path, out, err));
	CHECK(slurp(path) ==
	      "{\n"
	      "  \"commands\": [\n"
	      "    {\n"
	      "      \"name\": \"later\"\n"
	      "    }\n"
	      "  ],\n"
	      "  \"entry\": \"main\",\n"
	      "  \"tus\": [\n"
	      "    {\n"
	      "      \"defines\": [\n"
	      "        \"X\"\n"
	      "      ],\n"
	      "      \"file\": \"o.c\"\n"
	      "    },\n"
	      "    \"a.c\",\n"
	      "    \"sub/b.c\",\n"
	      "    \"/tmp/madc_test_writer_out.c\"\n"
	      "  ]\n"
	      "}\n");
	ProjectManifest m;
	REQUIRE(read_project_manifest(path, m, err));
	REQUIRE(m.tus.size() == 4);
	CHECK(m.tus[1].file == a);
	CHECK(m.tus[2].file == b);
	CHECK(m.tus[3].file == out);

	// Refused, the file as it was: a TU already (by another spelling), no
	// such file, an entry no TU names.
	const std::string before = slurp(path);
	CHECK_FALSE(project_manifest_add_tu(path, dir + "/./a.c", err));
	CHECK(err.find("already") != std::string::npos);
	CHECK_FALSE(project_manifest_add_tu(path, dir + "/nosuch.c", err));
	CHECK(err.find("no such file") != std::string::npos);
	CHECK_FALSE(project_manifest_remove_tu(path, dir + "/nosuch.c", err));
	CHECK(err.find("not in the project") != std::string::npos);
	CHECK(slurp(path) == before);

	// Removed: by its path; a TU entry whole, its fields with it; a file
	// gone from the disk by the manifest's own spelling.
	REQUIRE(project_manifest_remove_tu(path, b, err));
	REQUIRE(project_manifest_remove_tu(path, dir + "/o.c", err));
	std::remove(a.c_str());
	REQUIRE(project_manifest_remove_tu(path, "a.c", err));
	CHECK(slurp(path) ==
	      "{\n"
	      "  \"commands\": [\n"
	      "    {\n"
	      "      \"name\": \"later\"\n"
	      "    }\n"
	      "  ],\n"
	      "  \"entry\": \"main\",\n"
	      "  \"tus\": [\n"
	      "    \"/tmp/madc_test_writer_out.c\"\n"
	      "  ]\n"
	      "}\n");
	std::remove(b.c_str());
	std::remove(out.c_str());
	std::remove(path.c_str());
	std::remove((dir + "/sub").c_str());
	std::remove(dir.c_str());
}

TEST_CASE("project_manifest writers: a compile_commands.json import and a "
	  "mistyped field are refused") {
	std::string cc = write_tmp("writer_cc.json",
				   "[{\"directory\":\"/p\",\"file\":\"a.c\"}]");
	std::string err;
	CHECK_FALSE(project_manifest_remove_tu(cc, "/p/a.c", err));
	CHECK(err.find("read-only") != std::string::npos);
	std::string bad = write_tmp("writer_bad.prj.json",
				    "{\"tus\":[{\"file\":3}]}");
	CHECK_FALSE(project_manifest_remove_tu(bad, "/p/a.c", err));
	CHECK(err.find("wrong type") != std::string::npos);
	std::remove(cc.c_str());
	std::remove(bad.c_str());
}

TEST_CASE("project_default_output: the manifest's output, else its path less "
	  ".prj.json, else less its extension") {
	ProjectManifest m;
	CHECK(project_default_output("/p/x.prj.json", m) == "/p/x");
	CHECK(project_default_output("/p/build.json", m) == "/p/build");
	CHECK(project_default_output("x.prj.json", m) == "x");
	m.output_name = "prog";
	CHECK(project_default_output("/p/x.prj.json", m) == "/p/prog");
	m.output_name = "/abs/prog";
	CHECK(project_default_output("/p/x.prj.json", m) == "/abs/prog");
}
