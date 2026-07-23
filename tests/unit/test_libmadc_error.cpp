// Unit tests for madc::error (libmadc public embedding API).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <sstream>
#include <stack>
#include <string>
#include <vector>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "libmadc/error.h"

#include <memory>
#include <string>

using madc::error;

TEST_SUITE("madc::error") {

    TEST_CASE("default error is unknown error with empty location") {
	error e;
	CHECK(e.level == error::severity::error);
	CHECK(e.stage == error::phase::unknown);
	CHECK(e.file.empty());
	CHECK(e.line == 0);
	CHECK(e.column == 0);
    }

    TEST_CASE("explicit error stores fields and formats string") {
	error e(error::severity::warning, error::phase::parser,
		"watch this", "/tmp/test.mad", 7, 9);
	CHECK(e.level == error::severity::warning);
	CHECK(e.stage == error::phase::parser);
	CHECK(e.to_string() == "/tmp/test.mad:7:9: warning: watch this");
    }

    TEST_CASE("severity_name and phase_name cover every enumerator") {
	CHECK(std::string(error::severity_name(error::severity::warning)) == "warning");
	CHECK(std::string(error::severity_name(error::severity::error))   == "error");
	CHECK(std::string(error::phase_name(error::phase::unknown))  == "unknown");
	CHECK(std::string(error::phase_name(error::phase::lexer))    == "lexer");
	CHECK(std::string(error::phase_name(error::phase::parser))   == "parser");
	CHECK(std::string(error::phase_name(error::phase::compiler)) == "compiler");
	CHECK(std::string(error::phase_name(error::phase::runtime))  == "runtime");
    }

    TEST_CASE("equality compares all fields") {
	error a(error::severity::error, error::phase::compiler, "boom", "f.mad", 1, 2);
	error b(error::severity::error, error::phase::compiler, "boom", "f.mad", 1, 2);
	error c(error::severity::warning, error::phase::compiler, "boom", "f.mad", 1, 2);
	CHECK(a == b);
	CHECK(a != c);
    }

    TEST_CASE("program diagnostics convert into public madc errors") {
	MadcEngine engine;
	std::unique_ptr<Program> prog = engine.create_program();
	prog->report_warning(Program::DiagnosticPhase::lexer,
			     "bad include",
			     "/tmp/a.mad",
			     3,
			     4);
	prog->report_error(Program::DiagnosticPhase::runtime,
			   "main missing",
			   "/tmp/a.mad",
			   10,
			   1);

	std::vector<error> errs = madc::make_errors_from_program_diagnostics(*prog);
	REQUIRE(errs.size() == 2);
	CHECK(errs[0].level == error::severity::warning);
	CHECK(errs[0].stage == error::phase::lexer);
	CHECK(errs[0].message == "bad include");
	CHECK(errs[1].level == error::severity::error);
	CHECK(errs[1].stage == error::phase::runtime);
	CHECK(errs[1].to_string() == "/tmp/a.mad:10:1: error: main missing");
    }
}
