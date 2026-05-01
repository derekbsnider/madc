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
	engine.error_stream = &errbuf;
	std::unique_ptr<Program> prog = engine.create_program();

	CHECK_FALSE(prog->load_file("/tmp/madc_missing_file_should_not_exist_424242.mad"));
	CHECK(errbuf.str().find("Failed to open file") != std::string::npos);
	CHECK(errbuf.str().find("/tmp/madc_missing_file_should_not_exist_424242.mad") != std::string::npos);
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
	engine.input_stream = &inbuf;
	engine.output_stream = &outbuf;
	engine.error_stream = &errbuf;
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

	unlink(path.c_str());
    }

    TEST_CASE("Program execute runtime failures become diagnostics") {
	std::string path = write_temp_mad_source(
	    "madc_prog_no_main",
	    "int helper() { return 0; }\n");
	REQUIRE(!path.empty());

	std::ostringstream errbuf;
	MadcEngine engine;
	engine.error_stream = &errbuf;
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

	unlink(path.c_str());
    }
}
