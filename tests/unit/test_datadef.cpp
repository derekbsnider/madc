// Unit tests for datadef.h: DataType enum, varflag_t, DataDef type system

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <iostream>
#include <sstream>
#include <string>
#include <map>
#include <list>
#include <vector>
#include <queue>
#include <stack>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <asmjit/x86.h>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

// Global instances are defined in parser.cpp (linked via TESTOBJ)

TEST_SUITE("DataType enum") {
    TEST_CASE("primitive types have expected values") {
        CHECK((int)DataType::dtVOID == 0);
        CHECK((int)DataType::dtBOOL == 1);
        CHECK(DataType::dtINT == DataType::dtINT64);
        CHECK(DataType::dtCHAR == DataType::dtINT8);
    }

    TEST_CASE("pointer variants are base + 10000") {
        CHECK((uint32_t)DataType::dtINTptr == (uint32_t)DataType::dtINT + 10000);
        CHECK((uint32_t)DataType::dtSTRINGptr == (uint32_t)DataType::dtSTRING + 10000);
    }

    TEST_CASE("reference variants are base + 20000") {
        CHECK((uint32_t)DataType::dtINTref == (uint32_t)DataType::dtINT + 20000);
        CHECK((uint32_t)DataType::dtSTRINGref == (uint32_t)DataType::dtSTRING + 20000);
    }

    TEST_CASE("rtPtr/rtDePtr macros are inverses") {
        DataType t = DataType::dtINT32;
        CHECK(rtDePtr(rtPtr(t)) == t);
    }

    TEST_CASE("rtRef/rtDeRef macros are inverses") {
        DataType t = DataType::dtINT32;
        CHECK(rtDeRef(rtRef(t)) == t);
    }
}

TEST_SUITE("varflag_t") {
    TEST_CASE("flags can be combined with OR") {
        uint16_t flags = vfLOCAL | vfSTACK | vfMODIFIED;
        CHECK((flags & vfLOCAL) != 0);
        CHECK((flags & vfSTACK) != 0);
        CHECK((flags & vfMODIFIED) != 0);
        CHECK((flags & vfREGISTER) == 0);
    }

    TEST_CASE("vfREGISTER flag can be set and tested") {
        uint16_t flags = vfLOCAL | vfREGISTER;
        CHECK((flags & vfREGISTER) != 0);
        CHECK((flags & vfMODIFIED) == 0);
    }

    TEST_CASE("vfMODIFIED can be cleared") {
        uint16_t flags = vfMODIFIED | vfLOCAL;
        flags &= ~vfMODIFIED;
        CHECK((flags & vfMODIFIED) == 0);
        CHECK((flags & vfLOCAL) != 0);
    }
}

TEST_SUITE("DataDef type queries") {
    TEST_CASE("ddINT is numeric and integer") {
        CHECK(ddINT.is_numeric());
        CHECK(ddINT.is_integer());
        CHECK(!ddINT.is_real());
        CHECK(!ddINT.is_string());
        CHECK(!ddINT.is_object());
    }

    TEST_CASE("ddDOUBLE is numeric and real but not integer") {
        CHECK(ddDOUBLE.is_numeric());
        CHECK(ddDOUBLE.is_real());
        CHECK(!ddDOUBLE.is_integer());
    }

    TEST_CASE("ddSTRING is string and object, not numeric") {
        CHECK(ddSTRING.is_string());
        CHECK(ddSTRING.is_object());
        CHECK(!ddSTRING.is_numeric());
        CHECK(!ddSTRING.is_integer());
    }

    TEST_CASE("unsigned types are detected correctly") {
        CHECK(ddUINT8.is_unsigned());
        CHECK(ddUINT32.is_unsigned());
        CHECK(!ddINT.is_unsigned());
        CHECK(!ddINT32.is_unsigned());
    }

    TEST_CASE("struct type has btStruct basetype") {
        CHECK(ddTESTSTRUCT.basetype() == BaseType::btStruct);
        CHECK(ddTESTSTRUCT.is_struct());
        CHECK(!ddTESTSTRUCT.is_numeric());
    }

    TEST_CASE("rawtype strips pointer and reference variants") {
        DataType ptr_int = rtPtr(DataType::dtINT);
        DataType ref_int = rtRef(DataType::dtINT);
        CHECK(DataDef::rawtype(ptr_int) == DataType::dtINT);
        CHECK(DataDef::rawtype(ref_int) == DataType::dtINT);
        CHECK(DataDef::rawtype(DataType::dtINT) == DataType::dtINT);
    }

    TEST_CASE("reftype detection") {
        CHECK(ddINT.reftype() == RefType::rtValue);
    }

    TEST_CASE("is_compatible: same type is compatible") {
        CHECK(ddINT.is_compatible(ddINT));
        CHECK(ddINT.is_compatible(ddINT32));  // both numeric
    }

    TEST_CASE("is_compatible: numeric types are compatible with each other") {
        CHECK(ddINT8.is_compatible(ddINT64));
        CHECK(ddUINT32.is_compatible(ddINT16));
    }
}

TEST_SUITE("DataDefSTRUCT") {
    TEST_CASE("teststruct has expected members and size") {
        CHECK(ddTESTSTRUCT.members.size() == 4);
        CHECK(ddTESTSTRUCT.members[0].first == "name");
        CHECK(ddTESTSTRUCT.members[1].first == "id");
        CHECK(ddTESTSTRUCT.members[2].first == "age");
        CHECK(ddTESTSTRUCT.members[3].first == "sex");
    }

    TEST_CASE("member offsets are sequential") {
        std::string name_s = "name", id_s = "id", age_s = "age";
        CHECK(ddTESTSTRUCT.m_offset(name_s) == 0);
        CHECK(ddTESTSTRUCT.m_offset(id_s) == (ssize_t)sizeof(std::string));
        CHECK(ddTESTSTRUCT.m_offset(age_s) == (ssize_t)(sizeof(std::string) + 8));
    }

    TEST_CASE("m_type returns correct DataDef pointer") {
        std::string id_s = "id";
        DataDef *t = ddTESTSTRUCT.m_type(id_s);
        CHECK(t != nullptr);
        CHECK(t->is_integer());
    }

    TEST_CASE("m_offset returns -1 for unknown member") {
        std::string unknown = "nonexistent";
        CHECK(ddTESTSTRUCT.m_offset(unknown) == -1);
    }
}

TEST_SUITE("Variable") {
    TEST_CASE("Variable set/get for integer types") {
        Variable v("test_var", ddINT32, 1);
        v.set(42);
        CHECK(v.get<int32_t>() == 42);
        v.set(-1);
        CHECK(v.get<int32_t>() == -1);
    }

    TEST_CASE("Variable inc/dec") {
        Variable v("counter", ddINT32, 1);
        v.set(10);
        v.inc();
        CHECK(v.get<int32_t>() == 11);
        v.dec();
        CHECK(v.get<int32_t>() == 10);
    }

    TEST_CASE("Variable modified flag") {
        Variable v("x", ddINT, 1);
        CHECK((v.flags & vfMODIFIED) == 0);
        v.modified();
        CHECK((v.flags & vfMODIFIED) != 0);
    }

    TEST_CASE("Variable cmp for integers") {
        Variable v("y", ddINT32, 1);
        v.set(99);
        CHECK(v.cmp(99));
        CHECK(!v.cmp(100));
    }
}

static std::string write_temp_mad_source(const char *tag, const char *source)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/%s_%ld.mad", tag, (long)getpid());
    FILE *fp = fopen(path, "w");
    if ( !fp )
	return "";
    fputs(source, fp);
    fclose(fp);
    return std::string(path);
}

static void register_test_host_namespace(Program &pgm)
{
    Variable *var = pgm.addFunction(
	"__host_putchar",
	datatype_vec_t{DataType::dtINT, DataType::dtINT},
	(fVOIDFUNC)putchar);
    if ( var )
	pgm.namespace_map["host"]["putchar"] = var;
}

TEST_SUITE("Program isolation") {
    TEST_CASE("separate Program instances do not leak typedefs or macros") {
	std::string good_path = write_temp_mad_source(
	    "madc_prog_good",
	    "#define ANSWER 42\n"
	    "typedef int myint;\n"
	    "int main() { myint x = ANSWER; return 0; }\n");
	REQUIRE(!good_path.empty());

	std::string bad_path = write_temp_mad_source(
	    "madc_prog_bad",
	    "int main() { myint x = ANSWER; return 0; }\n");
	REQUIRE(!bad_path.empty());

	MadcEngine engine;
	std::unique_ptr<Program> good_prog = engine.create_program();
	TokenProgram *good_tp = good_prog->tokenize(good_path.c_str());
	CHECK(good_tp != nullptr);
	REQUIRE(good_tp != nullptr);
	CHECK(good_prog->parse(good_tp));
	CHECK(good_prog->compile());

	std::unique_ptr<Program> bad_prog = engine.create_program();
	TokenProgram *bad_tp = bad_prog->tokenize(bad_path.c_str());
	CHECK(bad_tp != nullptr);
	REQUIRE(bad_tp != nullptr);
	CHECK_FALSE(bad_prog->parse(bad_tp));

	unlink(good_path.c_str());
	unlink(bad_path.c_str());
    }

    TEST_CASE("separate engines can expose different builtin registration policies") {
	std::string path = write_temp_mad_source(
	    "madc_prog_builtins",
	    "int main() { puti(42); return 0; }\n");
	REQUIRE(!path.empty());

	MadcEngine default_engine;
	std::unique_ptr<Program> default_prog = default_engine.create_program();
	TokenProgram *default_tp = default_prog->tokenize(path.c_str());
	CHECK(default_tp != nullptr);
	REQUIRE(default_tp != nullptr);
	CHECK(default_prog->parse(default_tp));

	MadcEngine restricted_engine;
	restricted_engine.registration_policy.enable_core_functions = false;
	std::unique_ptr<Program> restricted_prog = restricted_engine.create_program();
	TokenProgram *restricted_tp = restricted_prog->tokenize(path.c_str());
	CHECK(restricted_tp != nullptr);
	REQUIRE(restricted_tp != nullptr);
	CHECK_FALSE(restricted_prog->parse(restricted_tp));

	unlink(path.c_str());
    }

    TEST_CASE("Engine builtin registry can be extended before parse") {
	std::string path = write_temp_mad_source(
	    "madc_prog_host_builtin",
	    "int main() { host_putchar(65); return 0; }\n");
	REQUIRE(!path.empty());

	MadcEngine engine;
	engine.populate_default_registries();
	engine.builtin_registry.add_core_function(
	    "host_putchar",
	    datatype_vec_t{DataType::dtINT, DataType::dtINT},
	    (fVOIDFUNC)putchar);
	std::unique_ptr<Program> prog = engine.create_program();

	TokenProgram *tp = prog->tokenize(path.c_str());
	CHECK(tp != nullptr);
	REQUIRE(tp != nullptr);
	CHECK(prog->parse(tp));
	CHECK(prog->compile());

	unlink(path.c_str());
    }

    TEST_CASE("Engine namespace registry can be extended before parse") {
	std::string path = write_temp_mad_source(
	    "madc_prog_host_namespace",
	    "int main() { host::putchar(65); return 0; }\n");
	REQUIRE(!path.empty());

	MadcEngine engine;
	engine.populate_default_registries();
	engine.namespace_registry.add_namespace("host", register_test_host_namespace);
	std::unique_ptr<Program> prog = engine.create_program();

	TokenProgram *tp = prog->tokenize(path.c_str());
	CHECK(tp != nullptr);
	REQUIRE(tp != nullptr);
	CHECK(prog->parse(tp));
	CHECK(prog->compile());

	unlink(path.c_str());
    }

    TEST_CASE("legacy bare Program still self-seeds default builtins") {
	std::string path = write_temp_mad_source(
	    "madc_prog_legacy_default",
	    "int main() { puti(42); return 0; }\n");
	REQUIRE(!path.empty());

	Program prog;
	TokenProgram *tp = prog.tokenize(path.c_str());
	CHECK(tp != nullptr);
	REQUIRE(tp != nullptr);
	CHECK(prog.parse(tp));

	unlink(path.c_str());
    }

    TEST_CASE("MadcEngine seeds program policy and registries") {
	std::string restricted_path = write_temp_mad_source(
	    "madc_engine_restricted",
	    "int main() { puti(42); return 0; }\n");
	REQUIRE(!restricted_path.empty());

	std::string host_path = write_temp_mad_source(
	    "madc_engine_hostns",
	    "int main() { host::putchar(65); return 0; }\n");
	REQUIRE(!host_path.empty());

	MadcEngine engine;
	engine.populate_default_registries();
	engine.registration_policy.enable_core_functions = false;
	engine.namespace_registry.add_namespace("host", register_test_host_namespace);

	std::unique_ptr<Program> restricted_prog = engine.create_program();
	TokenProgram *restricted_tp = restricted_prog->tokenize(restricted_path.c_str());
	CHECK(restricted_tp != nullptr);
	REQUIRE(restricted_tp != nullptr);
	CHECK_FALSE(restricted_prog->parse(restricted_tp));

	engine.registration_policy.enable_core_functions = true;
	std::unique_ptr<Program> host_prog = engine.create_program();
	TokenProgram *host_tp = host_prog->tokenize(host_path.c_str());
	CHECK(host_tp != nullptr);
	REQUIRE(host_tp != nullptr);
	CHECK(host_prog->parse(host_tp));
	CHECK(host_prog->compile());

	unlink(restricted_path.c_str());
	unlink(host_path.c_str());
    }

    TEST_CASE("Program load_file runs tokenize parse and compile") {
	std::string path = write_temp_mad_source(
	    "madc_prog_load_file",
	    "int main() { return 0; }\n");
	REQUIRE(!path.empty());

	MadcEngine engine;
	std::unique_ptr<Program> prog = engine.create_program();
	CHECK(prog->load_file(path.c_str()));
	CHECK_FALSE(prog->last_error.has_error);
	CHECK(prog->diagnostics.empty());
	prog->report_warning(Program::DiagnosticPhase::runtime, "test warning");
	REQUIRE(prog->diagnostics.size() == 1);
	CHECK(prog->diagnostics[0].severity == Program::DiagnosticSeverity::warning);
	CHECK(prog->diagnostics[0].phase == Program::DiagnosticPhase::runtime);
	CHECK(prog->diagnostics[0].message == "test warning");

	unlink(path.c_str());
    }

    TEST_CASE("Program captures structured error info for tokenize failure") {
	MadcEngine engine;
	std::unique_ptr<Program> prog = engine.create_program();
	CHECK_FALSE(prog->load_file("/tmp/madc_missing_file_should_not_exist_424242.mad"));
	CHECK(prog->last_error.has_error);
	REQUIRE(prog->diagnostics.size() == 1);
	CHECK(prog->diagnostics[0].severity == Program::DiagnosticSeverity::error);
	CHECK(prog->diagnostics[0].phase == Program::DiagnosticPhase::lexer);
	CHECK(prog->diagnostics[0].message == "Failed to open file");
	CHECK(prog->last_error.message == "Failed to open file");
	CHECK(prog->last_error.file == "/tmp/madc_missing_file_should_not_exist_424242.mad");
    }

    TEST_CASE("Program captures structured error info for parse failure") {
	std::string path = write_temp_mad_source(
	    "madc_prog_parse_error",
	    "int main() { missing_symbol = 1; return 0; }\n");
	REQUIRE(!path.empty());

	MadcEngine engine;
	std::unique_ptr<Program> prog = engine.create_program();
	CHECK_FALSE(prog->load_file(path.c_str()));
	CHECK(prog->last_error.has_error);
	REQUIRE(prog->diagnostics.size() == 1);
	CHECK(prog->diagnostics[0].severity == Program::DiagnosticSeverity::error);
	CHECK(prog->diagnostics[0].phase == Program::DiagnosticPhase::parser);
	CHECK(prog->last_error.file == path);
	CHECK(prog->last_error.message.find("undeclared identifier") != std::string::npos);
	CHECK(prog->diagnostics[0].message.find("undeclared identifier") != std::string::npos);
	CHECK(prog->last_error.line > 0);
	CHECK(prog->last_error.column > 0);

	unlink(path.c_str());
    }

    TEST_CASE("Program formats diagnostics from structured state") {
	MadcEngine engine;
	std::unique_ptr<Program> prog = engine.create_program();
	std::ostringstream os;

	prog->report_warning(Program::DiagnosticPhase::runtime, "watch this");
	const Program::Diagnostic *diag = prog->last_diagnostic();
	REQUIRE(diag != NULL);
	CHECK(std::string(prog->diagnostic_phase_name(diag->phase)) == "runtime");
	prog->print_last_diagnostic(os, "(test suffix)");
	CHECK(os.str().find("warning:") != std::string::npos);
	CHECK(os.str().find("watch this") != std::string::npos);
	CHECK(os.str().find("(test suffix)") != std::string::npos);
    }

    TEST_CASE("Program diagnostics honor engine error stream") {
	std::ostringstream errbuf;
	MadcEngine engine;
	engine.bind_error_stream(errbuf);
	std::unique_ptr<Program> prog = engine.create_program();

	CHECK_FALSE(prog->load_file("/tmp/madc_missing_file_should_not_exist_424242.mad"));
	CHECK(errbuf.str().find("Failed to open file") != std::string::npos);
	CHECK(errbuf.str().find("/tmp/madc_missing_file_should_not_exist_424242.mad") != std::string::npos);
	engine.reset_standard_streams();
    }

    TEST_CASE("Program lazy iostream globals honor engine streams") {
	std::string path = write_temp_mad_source(
	    "madc_prog_iostream_streams",
	    "#include <iostream>\nint main() { return 0; }\n");
	REQUIRE(!path.empty());

	std::istringstream inbuf("hello");
	std::ostringstream outbuf;
	std::ostringstream errbuf;
	MadcEngine engine;
	engine.bind_input_stream(inbuf);
	engine.bind_output_stream(outbuf);
	engine.bind_error_stream(errbuf);
	std::unique_ptr<Program> prog = engine.create_program();

	TokenProgram *tp = prog->tokenize(path.c_str());
	REQUIRE(tp != NULL);
	REQUIRE(prog->parse(tp));
	Variable *cout_var = prog->lazy_resolve("cout");
	Variable *cin_var = prog->lazy_resolve("cin");
	Variable *cerr_var = prog->lazy_resolve("cerr");
	REQUIRE(cout_var != NULL);
	REQUIRE(cin_var != NULL);
	REQUIRE(cerr_var != NULL);
	REQUIRE(cout_var->data != NULL);
	REQUIRE(cin_var->data != NULL);
	REQUIRE(cerr_var->data != NULL);
	CHECK(((std::ostream *)cout_var->data)->rdbuf() == outbuf.rdbuf());
	CHECK(((std::istream *)cin_var->data)->rdbuf() == inbuf.rdbuf());
	CHECK(((std::ostream *)cerr_var->data)->rdbuf() == errbuf.rdbuf());

	engine.reset_standard_streams();
	unlink(path.c_str());
    }

    TEST_CASE("Program execute runtime failures become diagnostics") {
	std::string path = write_temp_mad_source(
	    "madc_prog_no_main",
	    "int helper() { return 0; }\n");
	REQUIRE(!path.empty());

	std::ostringstream errbuf;
	MadcEngine engine;
	engine.bind_error_stream(errbuf);
	std::unique_ptr<Program> prog = engine.create_program();
	CHECK(prog->load_file(path.c_str()));
	CHECK_FALSE(prog->last_error.has_error);
	CHECK(prog->diagnostics.empty());

	prog->execute();
	CHECK(prog->last_error.has_error);
	REQUIRE(prog->diagnostics.size() == 1);
	CHECK(prog->diagnostics[0].severity == Program::DiagnosticSeverity::error);
	CHECK(prog->diagnostics[0].phase == Program::DiagnosticPhase::runtime);
	CHECK(prog->diagnostics[0].message == "Program::execute() cannot find main");
	CHECK(errbuf.str().find("cannot find main") != std::string::npos);

	engine.reset_standard_streams();
	unlink(path.c_str());
    }

    TEST_CASE("MadcEngine stream helpers capture script output") {
	std::string path = write_temp_mad_source(
	    "madc_prog_capture_output",
	    "int main() { string s = \"ok\"; puti(42); printstr(s); return 0; }\n");
	REQUIRE(!path.empty());

	std::ostringstream outbuf;
	MadcEngine engine;
	engine.bind_output_stream(outbuf);
	std::unique_ptr<Program> prog = engine.create_program();
	CHECK(prog->load_file(path.c_str()));
	prog->execute();
	CHECK(outbuf.str().find("42") != std::string::npos);
	CHECK(outbuf.str().find("ok") != std::string::npos);

	engine.reset_standard_streams();
	unlink(path.c_str());
    }

    TEST_CASE("MadcEngine buffer helpers capture and clear output") {
	std::string path = write_temp_mad_source(
	    "madc_prog_buffer_capture",
	    "int main() { puti(7); return 0; }\n");
	REQUIRE(!path.empty());

	MadcEngine engine;
	engine.capture_output_to_buffer();
	CHECK(engine.has_output_buffer());
	std::unique_ptr<Program> prog = engine.create_program();
	CHECK(prog->load_file(path.c_str()));
	prog->execute();
	CHECK(engine.output_buffer_str().find("7") != std::string::npos);
	engine.clear_output_buffer();
	CHECK(engine.output_buffer_str().empty());

	engine.reset_standard_streams();
	unlink(path.c_str());
    }

    TEST_CASE("MadcEngine buffer helpers capture error diagnostics") {
	MadcEngine engine;
	engine.capture_error_to_buffer();
	CHECK(engine.has_error_buffer());
	std::unique_ptr<Program> prog = engine.create_program();

	CHECK_FALSE(prog->load_file("/tmp/madc_missing_file_should_not_exist_424242.mad"));
	CHECK(engine.error_buffer_str().find("Failed to open file") != std::string::npos);
	engine.clear_error_buffer();
	CHECK(engine.error_buffer_str().empty());

	engine.reset_standard_streams();
    }

    TEST_CASE("MadcEngine tee output duplicates to primary and buffer") {
	std::string path = write_temp_mad_source(
	    "madc_prog_tee_output",
	    "int main() { puti(9); return 0; }\n");
	REQUIRE(!path.empty());

	std::ostringstream primary;
	MadcEngine engine;
	engine.bind_output_stream(primary);
	engine.tee_output_to_buffer();
	std::unique_ptr<Program> prog = engine.create_program();
	CHECK(prog->load_file(path.c_str()));
	prog->execute();
	CHECK(primary.str().find("9") != std::string::npos);
	CHECK(engine.output_buffer_str().find("9") != std::string::npos);

	engine.reset_standard_streams();
	unlink(path.c_str());
    }

    TEST_CASE("MadcEngine tee error duplicates to primary and buffer") {
	std::ostringstream primary;
	MadcEngine engine;
	engine.bind_error_stream(primary);
	engine.tee_error_to_buffer();
	std::unique_ptr<Program> prog = engine.create_program();

	CHECK_FALSE(prog->load_file("/tmp/madc_missing_file_should_not_exist_424242.mad"));
	CHECK(primary.str().find("Failed to open file") != std::string::npos);
	CHECK(engine.error_buffer_str().find("Failed to open file") != std::string::npos);

	engine.reset_standard_streams();
    }

    TEST_CASE("MadcEngine formats levelled log messages") {
	MadcEngine engine;
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;
	CHECK(engine.format_log_message(MadcEngine::LogLevel::warn, "heads up") == "[warn] heads up");
	CHECK(std::string(engine.log_level_name(MadcEngine::LogLevel::crit)) == "crit");
    }

    TEST_CASE("MadcEngine writes levelled logs to error buffer") {
	MadcEngine engine;
	engine.capture_error_to_buffer();
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;

	engine.write_log(MadcEngine::LogLevel::err, "disk is full");
	CHECK(engine.error_buffer_str().find("[err] disk is full") != std::string::npos);

	engine.reset_standard_streams();
    }

    TEST_CASE("MadcEngine log timestamps are optional") {
	MadcEngine engine;
	engine.log_timestamps = true;
	engine.log_level_prefixes = true;
	std::string formatted = engine.format_log_message(MadcEngine::LogLevel::info, "hello");
	CHECK(formatted.find("[info] hello") != std::string::npos);
	CHECK(formatted.size() > std::string("[info] hello").size());
    }

    TEST_CASE("madc::warn ostream writes through bound engine") {
	MadcEngine engine;
	engine.capture_error_to_buffer();
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;
	engine.bind_log_streams();

	madc::warn << "disk " << "almost " << 95 << "% full" << std::endl;
	CHECK(engine.error_buffer_str().find("[warn] disk almost 95% full") != std::string::npos);

	MadcEngine::unbind_log_streams();
	engine.reset_standard_streams();
    }

    TEST_CASE("madc level streams flush only on newline or sync") {
	MadcEngine engine;
	engine.capture_error_to_buffer();
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;
	engine.bind_log_streams();

	madc::info << "partial line, no newline yet";
	CHECK(engine.error_buffer_str().empty());
	madc::info << " ... finished\n";
	CHECK(engine.error_buffer_str().find("[info] partial line, no newline yet ... finished") != std::string::npos);

	MadcEngine::unbind_log_streams();
	engine.reset_standard_streams();
    }

    TEST_CASE("madc level streams cover every LogLevel") {
	MadcEngine engine;
	engine.capture_error_to_buffer();
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;
	engine.bind_log_streams();

	madc::emerg  << "lvl emerg" << std::endl;
	madc::alert  << "lvl alert" << std::endl;
	madc::crit   << "lvl crit"  << std::endl;
	madc::err    << "lvl err"   << std::endl;
	madc::warn   << "lvl warn"  << std::endl;
	madc::notice << "lvl notice" << std::endl;
	madc::info   << "lvl info"  << std::endl;
	madc::debug  << "lvl debug" << std::endl;

	const std::string buf = engine.error_buffer_str();
	CHECK(buf.find("[emerg] lvl emerg")   != std::string::npos);
	CHECK(buf.find("[alert] lvl alert")   != std::string::npos);
	CHECK(buf.find("[crit] lvl crit")     != std::string::npos);
	CHECK(buf.find("[err] lvl err")       != std::string::npos);
	CHECK(buf.find("[warn] lvl warn")     != std::string::npos);
	CHECK(buf.find("[notice] lvl notice") != std::string::npos);
	CHECK(buf.find("[info] lvl info")     != std::string::npos);
	CHECK(buf.find("[debug] lvl debug")   != std::string::npos);

	MadcEngine::unbind_log_streams();
	engine.reset_standard_streams();
    }

    TEST_CASE("madc level stream sync flushes pending partial line") {
	MadcEngine engine;
	engine.capture_error_to_buffer();
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;
	engine.bind_log_streams();

	madc::warn << "no newline here";
	madc::warn.flush();
	CHECK(engine.error_buffer_str().find("[warn] no newline here") != std::string::npos);

	MadcEngine::unbind_log_streams();
	engine.reset_standard_streams();
    }

    TEST_CASE("MadcEngine log_threshold filters write_log") {
	MadcEngine engine;
	engine.capture_error_to_buffer();
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;
	engine.log_threshold = MadcEngine::LogLevel::warn;

	engine.write_log(MadcEngine::LogLevel::emerg,  "must pass emerg");
	engine.write_log(MadcEngine::LogLevel::warn,   "must pass warn");
	engine.write_log(MadcEngine::LogLevel::notice, "must drop notice");
	engine.write_log(MadcEngine::LogLevel::info,   "must drop info");
	engine.write_log(MadcEngine::LogLevel::debug,  "must drop debug");

	const std::string buf = engine.error_buffer_str();
	CHECK(buf.find("must pass emerg") != std::string::npos);
	CHECK(buf.find("must pass warn")  != std::string::npos);
	CHECK(buf.find("must drop notice") == std::string::npos);
	CHECK(buf.find("must drop info")   == std::string::npos);
	CHECK(buf.find("must drop debug")  == std::string::npos);

	engine.reset_standard_streams();
    }

    TEST_CASE("madc level streams honor engine log_threshold") {
	MadcEngine engine;
	engine.capture_error_to_buffer();
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;
	engine.log_threshold = MadcEngine::LogLevel::err;
	engine.bind_log_streams();

	madc::crit  << "kept crit"   << std::endl;
	madc::err   << "kept err"    << std::endl;
	madc::warn  << "drop warn"   << std::endl;
	madc::info  << "drop info"   << std::endl;
	madc::debug << "drop debug " << 42 << std::endl;

	const std::string buf = engine.error_buffer_str();
	CHECK(buf.find("kept crit") != std::string::npos);
	CHECK(buf.find("kept err")  != std::string::npos);
	CHECK(buf.find("drop warn")  == std::string::npos);
	CHECK(buf.find("drop info")  == std::string::npos);
	CHECK(buf.find("drop debug") == std::string::npos);

	MadcEngine::unbind_log_streams();
	engine.reset_standard_streams();
    }

    TEST_CASE("MadcEngine fans out write_log to additional sinks") {
	MadcEngine engine;
	engine.capture_error_to_buffer();
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;

	std::vector<std::pair<MadcEngine::LogLevel, std::string>> recorded;
	engine.add_log_sink([&recorded](MadcEngine::LogLevel lvl, const std::string &msg) {
	    recorded.push_back({lvl, msg});
	});

	engine.write_log(MadcEngine::LogLevel::warn, "fanout works");
	engine.write_log(MadcEngine::LogLevel::info, "info too");

	CHECK(engine.error_buffer_str().find("[warn] fanout works") != std::string::npos);
	CHECK(engine.error_buffer_str().find("[info] info too")    != std::string::npos);
	REQUIRE(recorded.size() == 2);
	CHECK(recorded[0].first  == MadcEngine::LogLevel::warn);
	CHECK(recorded[0].second == "fanout works");
	CHECK(recorded[1].first  == MadcEngine::LogLevel::info);
	CHECK(recorded[1].second == "info too");

	engine.reset_standard_streams();
    }

    TEST_CASE("MadcEngine sinks receive original message and respect threshold") {
	MadcEngine engine;
	engine.capture_error_to_buffer();
	engine.log_threshold = MadcEngine::LogLevel::warn;

	int sink_calls = 0;
	engine.add_log_sink([&sink_calls](MadcEngine::LogLevel, const std::string &) {
	    ++sink_calls;
	});

	engine.write_log(MadcEngine::LogLevel::crit,   "critical");
	engine.write_log(MadcEngine::LogLevel::warn,   "warning");
	engine.write_log(MadcEngine::LogLevel::info,   "info filtered");
	engine.write_log(MadcEngine::LogLevel::debug,  "debug filtered");

	CHECK(sink_calls == 2);

	engine.reset_standard_streams();
    }

    TEST_CASE("MadcEngine log_to_error_stream toggle silences default sink") {
	MadcEngine engine;
	engine.capture_error_to_buffer();
	engine.log_to_error_stream = false;

	int sink_calls = 0;
	engine.add_log_sink([&sink_calls](MadcEngine::LogLevel, const std::string &) {
	    ++sink_calls;
	});

	engine.write_log(MadcEngine::LogLevel::warn, "no error stream");
	CHECK(engine.error_buffer_str().empty());
	CHECK(sink_calls == 1);

	engine.log_to_error_stream = true;
	engine.write_log(MadcEngine::LogLevel::warn, "and back");
	CHECK(engine.error_buffer_str().find("[warn] and back") != std::string::npos);
	CHECK(sink_calls == 2);

	engine.reset_standard_streams();
    }

    TEST_CASE("MadcEngine clear_log_sinks removes registered sinks") {
	MadcEngine engine;
	engine.log_to_error_stream = false;

	int sink_calls = 0;
	engine.add_log_sink([&sink_calls](MadcEngine::LogLevel, const std::string &) {
	    ++sink_calls;
	});
	engine.write_log(MadcEngine::LogLevel::warn, "first");
	CHECK(sink_calls == 1);

	engine.clear_log_sinks();
	engine.write_log(MadcEngine::LogLevel::warn, "second");
	CHECK(sink_calls == 1);
    }

    TEST_CASE("madc level streams reach registered sinks unchanged") {
	MadcEngine engine;
	engine.log_to_error_stream = false;
	engine.bind_log_streams();

	std::vector<std::pair<MadcEngine::LogLevel, std::string>> recorded;
	engine.add_log_sink([&recorded](MadcEngine::LogLevel lvl, const std::string &msg) {
	    recorded.push_back({lvl, msg});
	});

	madc::warn  << "facade " << "to " << "sink"  << std::endl;
	madc::err   << "errline " << 7 << std::endl;

	REQUIRE(recorded.size() == 2);
	CHECK(recorded[0].first  == MadcEngine::LogLevel::warn);
	CHECK(recorded[0].second == "facade to sink");
	CHECK(recorded[1].first  == MadcEngine::LogLevel::err);
	CHECK(recorded[1].second == "errline 7");

	MadcEngine::unbind_log_streams();
    }

    TEST_CASE("madc level streams flush correctly after threshold raised at runtime") {
	MadcEngine engine;
	engine.capture_error_to_buffer();
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;
	engine.log_threshold = MadcEngine::LogLevel::err;
	engine.bind_log_streams();

	madc::debug << "should not appear" << std::endl;
	CHECK(engine.error_buffer_str().find("should not appear") == std::string::npos);

	engine.log_threshold = MadcEngine::LogLevel::debug;
	madc::debug << "now visible" << std::endl;
	CHECK(engine.error_buffer_str().find("[debug] now visible") != std::string::npos);

	MadcEngine::unbind_log_streams();
	engine.reset_standard_streams();
    }

    TEST_CASE("madc level streams route through engine error sink even when error captured later") {
	MadcEngine engine;
	engine.bind_log_streams();
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;

	engine.capture_error_to_buffer();
	madc::err << "captured after bind" << std::endl;
	CHECK(engine.error_buffer_str().find("[err] captured after bind") != std::string::npos);

	MadcEngine::unbind_log_streams();
	engine.reset_standard_streams();
    }
}
