// Unit tests for the configuration-file layer (forest-carriers S6): parsing,
// the strict diagnostics, path resolution, and the lookup chain's shape.
//
// Two layers are covered. madc::config_* is madc's SCHEMA (its five keys), and
// madc::cfg::config_file is the schema-blind READER shared with madcdat and any
// madcdis-based consumer — the last suite below drives it with a different
// application name and different keys, which is the only test that actually
// proves it is reusable rather than merely generic-shaped.
//
// How the values then win or lose against the command line and the environment
// (the CLI > env > madc.ini > baked rule) is integration-gated by
// scripts/forest_config_gate.sh, because the precedence lives at the CLI layer
// where the settings land, not in either of these layers.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <stdlib.h>
#include <unistd.h>

#include "madc_config.h"
#include "madc_config_file.h"

namespace {

// Fixture files follow the existing unit-test convention: /tmp/madc_<what>_<pid>.
std::string write_ini(const std::string &tag, const std::string &body)
{
	std::ostringstream p;
	p << "/tmp/madc_cfg_" << tag << "_" << (long)getpid() << ".ini";
	std::string path = p.str();
	std::ofstream f(path.c_str());
	f << body;
	f.close();
	return path;
}

}   // namespace

TEST_SUITE("madc.ini reader") {

TEST_CASE("every key parses, with comments, a section header and quotes") {
	std::string path = write_ini("all",
		"# a comment\n"
		"; another comment\n"
		"\n"
		"[madc]\n"
		"std = c99\n"
		"forest = /abs/groves.msnap\n"
		"include = /abs/one\n"
		"include = /abs/two\n"
		"cpu-limit = 90\n"
		"mem-limit = \"8192\"\n");
	madc::config_settings cfg;
	std::ostringstream err;
	REQUIRE(madc::config_parse_file(path, cfg, err));
	CHECK(err.str().empty());
	CHECK(cfg.source_path == path);
	CHECK(cfg.has_std);
	CHECK(cfg.std_option == "c99");
	CHECK(cfg.forest == "/abs/groves.msnap");
	REQUIRE(cfg.include_dirs.size() == 2);
	// Order is the file's order: the caller appends them after the -I dirs.
	CHECK(cfg.include_dirs[0] == "/abs/one");
	CHECK(cfg.include_dirs[1] == "/abs/two");
	CHECK(cfg.has_cpu_limit);
	CHECK(cfg.cpu_limit_secs == 90);
	CHECK(cfg.has_mem_limit);
	CHECK(cfg.mem_limit_mb == 8192);	// quotes are syntax, not content
	unlink(path.c_str());
}

// The has_* flags exist for exactly this: "the file said 0" must be
// distinguishable from "the file said nothing", or a configured 0 (= disable
// the guard) would be indistinguishable from the baked default.
TEST_CASE("an explicit 0 is a value, not an absence") {
	std::string path = write_ini("zero", "cpu-limit = 0\nmem-limit = 0\n");
	madc::config_settings cfg;
	std::ostringstream err;
	REQUIRE(madc::config_parse_file(path, cfg, err));
	CHECK(cfg.has_cpu_limit);
	CHECK(cfg.cpu_limit_secs == 0);
	CHECK(cfg.has_mem_limit);
	CHECK(cfg.mem_limit_mb == 0);
	unlink(path.c_str());
}

TEST_CASE("keys are case-insensitive, values are not") {
	std::string path = write_ini("case", "STD = c99\nMem-Limit = 512\n");
	madc::config_settings cfg;
	std::ostringstream err;
	REQUIRE(madc::config_parse_file(path, cfg, err));
	CHECK(cfg.std_option == "c99");
	CHECK(cfg.mem_limit_mb == 512);
	unlink(path.c_str());
}

TEST_CASE("a repeated scalar takes the last value; include appends") {
	std::string path = write_ini("dup",
		"std = c99\nstd = c11\ninclude = /a\ninclude = /b\n");
	madc::config_settings cfg;
	std::ostringstream err;
	REQUIRE(madc::config_parse_file(path, cfg, err));
	CHECK(cfg.std_option == "c11");
	REQUIRE(cfg.include_dirs.size() == 2);
	CHECK(cfg.include_dirs[0] == "/a");
	unlink(path.c_str());
}

// A relative path in a config file means "relative to the config file" — a
// system-wide /etc/madc.ini naming `forest = groves.msnap` cannot sensibly mean
// something in whatever directory madc happened to be started from.
TEST_CASE("relative paths resolve against the config file's own directory") {
	std::string path = write_ini("rel", "forest = groves.msnap\ninclude = inc\n");
	madc::config_settings cfg;
	std::ostringstream err;
	REQUIRE(madc::config_parse_file(path, cfg, err));
	CHECK(cfg.forest == "/tmp/groves.msnap");
	REQUIRE(cfg.include_dirs.size() == 1);
	CHECK(cfg.include_dirs[0] == "/tmp/inc");
	unlink(path.c_str());
}

TEST_CASE("a leading ~/ expands against HOME; an absolute path is untouched") {
	setenv("HOME", "/home/tester", 1);
	std::string path = write_ini("tilde", "forest = ~/g.msnap\ninclude = /abs/x\n");
	madc::config_settings cfg;
	std::ostringstream err;
	REQUIRE(madc::config_parse_file(path, cfg, err));
	CHECK(cfg.forest == "/home/tester/g.msnap");
	CHECK(cfg.include_dirs[0] == "/abs/x");
	unlink(path.c_str());
}

// STRICT by design: a config file is the user's own file, so half-applying it
// is the silent-degradation failure this project refuses. Every diagnostic
// names the file, the line, and what was wrong.
TEST_CASE("an unknown key is an error naming the key, the line and the accepted set") {
	std::string path = write_ini("unknown", "std = c99\nfrost = /x\n");
	madc::config_settings cfg;
	std::ostringstream err;
	CHECK(!madc::config_parse_file(path, cfg, err));
	std::string msg = err.str();
	CHECK(msg.find("unknown key 'frost'") != std::string::npos);
	CHECK(msg.find(":2:") != std::string::npos);
	CHECK(msg.find("std, forest, include, cpu-limit, mem-limit") != std::string::npos);
	unlink(path.c_str());
}

TEST_CASE("a line with no '=' is an error") {
	std::string path = write_ini("noeq", "std c99\n");
	madc::config_settings cfg;
	std::ostringstream err;
	CHECK(!madc::config_parse_file(path, cfg, err));
	CHECK(err.str().find("expected 'key = value'") != std::string::npos);
	unlink(path.c_str());
}

TEST_CASE("an empty value is an error, not an empty setting") {
	std::string path = write_ini("empty", "std =\n");
	madc::config_settings cfg;
	std::ostringstream err;
	CHECK(!madc::config_parse_file(path, cfg, err));
	CHECK(err.str().find("empty value") != std::string::npos);
	unlink(path.c_str());
}

// Trailing junk is refused rather than silently truncated: `mem-limit = 8G`
// must say so, not arm an 8 MB guard.
TEST_CASE("a non-numeric limit is an error") {
	std::string path = write_ini("badint", "mem-limit = 8G\n");
	madc::config_settings cfg;
	std::ostringstream err;
	CHECK(!madc::config_parse_file(path, cfg, err));
	CHECK(err.str().find("mem-limit needs a whole number") != std::string::npos);
	unlink(path.c_str());

	path = write_ini("badcpu", "cpu-limit = -5\n");
	madc::config_settings cfg2;
	std::ostringstream err2;
	CHECK(!madc::config_parse_file(path, cfg2, err2));
	CHECK(err2.str().find("cpu-limit needs a whole number") != std::string::npos);
	unlink(path.c_str());
}

TEST_CASE("a foreign section is an error; [madc] is accepted") {
	std::string path = write_ini("section", "[clang]\nstd = c99\n");
	madc::config_settings cfg;
	std::ostringstream err;
	CHECK(!madc::config_parse_file(path, cfg, err));
	CHECK(err.str().find("unknown section [clang]") != std::string::npos);
	unlink(path.c_str());

	path = write_ini("unterm", "[madc\nstd = c99\n");
	madc::config_settings cfg2;
	std::ostringstream err2;
	CHECK(!madc::config_parse_file(path, cfg2, err2));
	CHECK(err2.str().find("unterminated section header") != std::string::npos);
	unlink(path.c_str());
}

TEST_CASE("a file that cannot be read is an error naming it") {
	madc::config_settings cfg;
	std::ostringstream err;
	CHECK(!madc::config_parse_file("/tmp/madc_cfg_does_not_exist.ini", cfg, err));
	CHECK(err.str().find("cannot read config file") != std::string::npos);
}

// The chain's ORDER is the contract: the project-local file first, then the
// user's, then the system's — and the first EXISTING one wins outright, so a
// user config never partially overlays a system one.
TEST_CASE("the search chain is project, then user, then system") {
	setenv("XDG_CONFIG_HOME", "/xdg", 1);
	std::vector<std::string> paths = madc::config_search_paths();
	REQUIRE(paths.size() >= 2);
	CHECK(paths[0] == "madc.ini");
	CHECK(paths[1] == "/xdg/madc/madc.ini");
#ifdef MADC_SYSCONFDIR
	REQUIRE(paths.size() == 3);
	CHECK(paths[2] == std::string(MADC_SYSCONFDIR) + "/madc.ini");
#endif
	unsetenv("XDG_CONFIG_HOME");
	setenv("HOME", "/home/tester", 1);
	paths = madc::config_search_paths();
	REQUIRE(paths.size() >= 2);
	CHECK(paths[1] == "/home/tester/.config/madc/madc.ini");
}

// --config=<file> is the whole search and MUST load: a named file that gets
// ignored is the same failure as a named forest container that gets ignored.
// (With the configure axis off it refuses instead, naming the build option.)
TEST_CASE("an explicitly named file that does not exist is an error") {
	madc::config_settings cfg;
	std::ostringstream err;
	CHECK(!madc::config_load("/tmp/madc_cfg_absent.ini", cfg, err));
	CHECK(!err.str().empty());
}

TEST_CASE("config_file_supported reports the configure axis") {
#ifdef MADC_ENABLE_CONFIG_FILE
	CHECK(madc::config_file_supported());
#else
	CHECK(!madc::config_file_supported());
#endif
}

}

// The reader is SCHEMA-BLIND, and this suite is the proof: a second consumer
// with its own application name and its own keys — nothing madc-specific —
// gets the same lookup rule, path semantics and strict diagnostics. If someone
// later welds madc's schema back into the reader, these cases fail.
TEST_SUITE("schema-blind reader reuse") {

TEST_CASE("a different application name drives the whole lookup chain") {
	setenv("XDG_CONFIG_HOME", "/xdg", 1);
	std::vector<std::string> paths =
		madc::cfg::config_file("madcdat").search_paths();
	REQUIRE(paths.size() >= 2);
	CHECK(paths[0] == "madcdat.ini");
	CHECK(paths[1] == "/xdg/madcdat/madcdat.ini");
#ifdef MADC_SYSCONFDIR
	REQUIRE(paths.size() == 3);
	CHECK(paths[2] == std::string(MADC_SYSCONFDIR) + "/madcdat.ini");
#endif
	unsetenv("XDG_CONFIG_HOME");
}

TEST_CASE("a consumer's own keys parse, including kinds madc does not use") {
	std::string path = write_ini("dat",
		"[madcdat]\n"
		"backend = sqlite3\n"
		"store = data/madcdat.db\n"
		"peer = alpha\n"
		"peer = beta\n"
		"cache-mb = 256\n"
		"federation = yes\n");
	std::string backend, store;
	std::vector<std::string> peers;
	unsigned long cache = 0;
	bool federate = false, saw_backend = false, saw_cache = false;
	madc::cfg::config_file cf("madcdat");
	cf.accept_text("backend", backend, &saw_backend);
	cf.accept_path("store", store);
	// peer NAMES, not paths: a text list is taken verbatim, where a path
	// list would (correctly) resolve each value against the file's directory.
	cf.accept_text_list("peer", peers);
	cf.accept_count("cache-mb", cache, &saw_cache);
	cf.accept_flag("federation", federate);
	std::ostringstream err;
	REQUIRE(cf.parse_file(path, err));
	CHECK(err.str().empty());
	CHECK(saw_backend);
	CHECK(backend == "sqlite3");
	CHECK(store == "/tmp/data/madcdat.db");	// resolved against the file's dir
	REQUIRE(peers.size() == 2);
	CHECK(peers[0] == "alpha");
	CHECK(peers[1] == "beta");
	CHECK(saw_cache);
	CHECK(cache == 256);
	CHECK(federate);
	unlink(path.c_str());
}

TEST_CASE("boolean spellings, and a bad one is refused") {
	std::string path = write_ini("bool", "federation = off\n");
	bool federate = true;
	madc::cfg::config_file cf("madcdat");
	cf.accept_flag("federation", federate);
	std::ostringstream err;
	REQUIRE(cf.parse_file(path, err));
	CHECK(!federate);
	unlink(path.c_str());

	path = write_ini("bool2", "federation = maybe\n");
	bool f2 = false;
	madc::cfg::config_file cf2("madcdat");
	cf2.accept_flag("federation", f2);
	std::ostringstream err2;
	CHECK(!cf2.parse_file(path, err2));
	CHECK(err2.str().find("federation needs yes/no") != std::string::npos);
	unlink(path.c_str());
}

// The strict diagnostics belong to the reader, so a consumer inherits them
// without writing any: the accepted-key list is generated from what IT
// registered, and the section it accepts is ITS application name.
TEST_CASE("diagnostics name the consumer's own schema, not madc's") {
	std::string path = write_ini("datbad", "backend = x\nstd = c99\n");
	std::string backend;
	madc::cfg::config_file cf("madcdat");
	cf.accept_text("backend", backend);
	std::ostringstream err;
	CHECK(!cf.parse_file(path, err));
	std::string msg = err.str();
	// `std` is a madc key; this consumer never registered it, so it is
	// unknown here — and the accepted list is the consumer's, not madc's.
	CHECK(msg.find("unknown key 'std'") != std::string::npos);
	CHECK(msg.find("accepted: backend") != std::string::npos);
	CHECK(msg.find("forest") == std::string::npos);
	unlink(path.c_str());

	path = write_ini("datsect", "[madc]\nbackend = x\n");
	madc::cfg::config_file cf2("madcdat");
	cf2.accept_text("backend", backend);
	std::ostringstream err2;
	CHECK(!cf2.parse_file(path, err2));
	CHECK(err2.str().find("only [madcdat] is accepted") != std::string::npos);
	unlink(path.c_str());
}

}
