// Unit tests for the frozen-forest discovery failure policy (forest-carriers
// S3): RegistrationPolicy::forest_missing_policy. The three policies react to
// a discovery chain that ends with NO usable container — the unit-test binary
// is unpacked (self-image arm misses), has no <exe>.forest sidecar, and every
// case unsets MADC_FOREST, so the chain is guaranteed empty here.
// The discovery ARMS themselves (sidecar, env, ordering, loud junk/explicit
// misses) are integration-gated by scripts/forest_sidecar_gate.sh.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <map>
#include <list>
#include <vector>
#include <queue>
#include <stack>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

// Global instances are defined in parser.cpp (linked via TESTOBJ)

TEST_SUITE("forest missing policy") {

TEST_CASE("silent_fallback: chain miss returns NULL with no notice") {
	unsetenv("MADC_FOREST");
	MadcEngine engine;
	std::unique_ptr<Program> prog = engine.create_program();
	std::ostringstream err;
	prog->error_stream = &err;
	prog->registration_policy.enable_forest_bind = true;
	CHECK(prog->ensure_bind_forest() == (CirFrozenForest *)NULL);
	CHECK(err.str().empty());
}

TEST_CASE("loud_fallback: chain miss returns NULL after one notice") {
	unsetenv("MADC_FOREST");
	MadcEngine engine;
	std::unique_ptr<Program> prog = engine.create_program();
	std::ostringstream err;
	prog->error_stream = &err;
	prog->registration_policy.enable_forest_bind = true;
	prog->registration_policy.forest_missing_policy =
		Program::RegistrationPolicy::ForestPolicy::loud_fallback;
	CHECK(prog->ensure_bind_forest() == (CirFrozenForest *)NULL);
	CHECK(err.str().find("no frozen forest found") != std::string::npos);
	// The open attempt is one-shot (bind_forest_tried): a second call
	// neither re-probes nor re-notices.
	std::string first = err.str();
	CHECK(prog->ensure_bind_forest() == (CirFrozenForest *)NULL);
	CHECK(err.str() == first);
}

TEST_CASE("strict_require: chain miss is a hard error") {
	unsetenv("MADC_FOREST");
	MadcEngine engine;
	std::unique_ptr<Program> prog = engine.create_program();
	prog->registration_policy.enable_forest_bind = true;
	prog->registration_policy.forest_missing_policy =
		Program::RegistrationPolicy::ForestPolicy::strict_require;
	// Throw prints to stderr (never DBG-gated) then raises std::exception.
	CHECK_THROWS_AS(prog->ensure_bind_forest(), std::exception);
}

TEST_CASE("strict_require: explicit --forest-bind path miss is a hard error") {
	unsetenv("MADC_FOREST");
	MadcEngine engine;
	std::unique_ptr<Program> prog = engine.create_program();
	prog->registration_policy.enable_forest_bind = true;
	prog->forest_bind_path = "tmp/definitely-absent.msnap";
	prog->registration_policy.forest_missing_policy =
		Program::RegistrationPolicy::ForestPolicy::strict_require;
	CHECK_THROWS_AS(prog->ensure_bind_forest(), std::exception);
}

// The chain-end fallback's config-mismatch matrix (a container WAS seen but
// its producer std/-D differs — the multi-dialect fall-through). loud must
// stay SILENT on it (the packed CLI compiling a C file against its
// C++-parsed corpus is the everyday case; the expect_quiet suite tests with
// --std= fixtures pin this end-to-end), while strict still hard-errors.
TEST_CASE("loud_fallback: config mismatch is the silent multi-dialect fall-through") {
	MadcEngine engine;
	std::unique_ptr<Program> prog = engine.create_program();
	std::ostringstream err;
	prog->error_stream = &err;
	prog->registration_policy.forest_missing_policy =
		Program::RegistrationPolicy::ForestPolicy::loud_fallback;
	prog->forest_missing_fallback(/*config_mismatch=*/true);
	CHECK(err.str().empty());
	prog->forest_missing_fallback(/*config_mismatch=*/false);
	CHECK(err.str().find("no frozen forest found") != std::string::npos);
}

// The library-image arm (forest-carriers S4) keys off image IDENTITY: in a
// monolithic link — this unit binary, and bin/madc by default — libmadc's code
// lives in the executable itself, so the arm must resolve to the very path
// arm 1 already probed and be skipped. (The shared shape, where the two
// differ, is integration-gated by scripts/forest_library_gate.sh.)
TEST_CASE("library image resolves to the executable in a monolithic link") {
	std::string exe = madc_self_exe_path();
	std::string lib = madc_self_lib_path();
	CHECK(!exe.empty());
	CHECK(!lib.empty());
	CHECK(lib == exe);
}

TEST_CASE("strict_require: config mismatch is still a hard error") {
	MadcEngine engine;
	std::unique_ptr<Program> prog = engine.create_program();
	prog->registration_policy.forest_missing_policy =
		Program::RegistrationPolicy::ForestPolicy::strict_require;
	CHECK_THROWS_AS(prog->forest_missing_fallback(true), std::exception);
	CHECK_THROWS_AS(prog->forest_missing_fallback(false), std::exception);
}

}
