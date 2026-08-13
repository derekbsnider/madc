// Unit tests for datadef.h: DataType enum, varflag_t, DataDef type system

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
#include <sys/stat.h>
#include <syslog.h>
#include <stdio.h>
#include <unistd.h>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

// Global instances are defined in parser.cpp (linked via TESTOBJ)

// Local DataDefSTRUCT fixture for the struct tests. This used to be the
// compiler-global `ddTESTSTRUCT` built-in (removed as legacy cruft — the
// teststruct.mad integration test now defines its own struct); the unit tests
// only need a representative struct to exercise DataDefSTRUCT, so build one
// here. A function-local static (not a file-scope global) so it is constructed
// on first use — after the `ddCHARptr`/`ddINT`/`ddUINT8` globals it references
// (defined in parser.cpp, a different TU) are initialized — avoiding a
// static-init-order crash.
static DataDefSTRUCT &test_struct()
{
	static DataDefSTRUCT s("teststruct",
	{
		{"name", &ddCHARptr},
		{"id",   &ddINT},
		{"age",  &ddUINT8},
		{"sex",  &ddUINT8}
	});
	return s;
}

TEST_SUITE("DataType enum") {
    TEST_CASE("primitive types have expected values") {
        CHECK((int)DataType::dtVOID == 0);
        CHECK((int)DataType::dtBOOL == 1);
        CHECK(DataType::dtINT == DataType::dtINT32);
        CHECK(DataType::dtCHAR == DataType::dtINT8);
    }

    // The pointer/reference tag-arithmetic cases (dt*ptr == base+10000,
    // dt*ref == base+20000, and the rtPtr/rtRef/rtDePtr/rtDeRef macro
    // inverses) were removed with the encoding itself (tag-arithmetic
    // retirement). Derivation is now the DataDefPTR/DataDefREF/DataDefCONST
    // object graph — see the is_cstr() and DataDefPTR/REF suites and
    // is_pointer()/is_reference()/rawtype() for the structural contract.
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
    TEST_CASE("class-pattern provenance follows object birth and copy") {
	struct CaptureFlagRestore {
	    bool saved;
	    CaptureFlagRestore() : saved(madc_class_pattern_capture_active) {}
	    ~CaptureFlagRestore() { madc_class_pattern_capture_active = saved; }
	} restore;
	madc_class_pattern_capture_active = false;
	DataDef stable("stable", 4, DataType::dtINT32);
	stable.set_canonical_spelling("ns::stable");
	stable.canonical_swept = true;
	stable.type_id = 77;
	DataDef stable_copy(stable);
	CHECK_FALSE(stable.speculative_class_capture);
	CHECK_FALSE(stable_copy.speculative_class_capture);

	madc_class_pattern_capture_active = true;
	DataDef speculative("speculative", 4, DataType::dtINT32);
	DataDef copied_stable(stable);
	CHECK(speculative.speculative_class_capture);
	CHECK(copied_stable.speculative_class_capture);
	CHECK(copied_stable.name == stable.name);
	CHECK(copied_stable.size == stable.size);
	CHECK(copied_stable.type() == stable.type());
	CHECK(copied_stable.canonical_cpp_spelling() == "ns::stable");
	CHECK(copied_stable.canonical_swept);
	CHECK(copied_stable.type_id == 77);

	madc_class_pattern_capture_active = false;
	DataDef copied_speculative(speculative);
	CHECK(copied_speculative.speculative_class_capture);
	stable = speculative;
	CHECK_FALSE(stable.speculative_class_capture);
	CHECK(stable.name == "speculative");
	CHECK(stable.size == speculative.size);
	CHECK(stable.type() == speculative.type());
	speculative = stable;
	CHECK(speculative.speculative_class_capture);

	DataDefCLASS stable_class("StableClass", 0, DataType::dtRESERVED);
	madc_class_pattern_capture_active = true;
	DataDefCLASS speculative_class(stable_class);
	CHECK(speculative_class.speculative_class_capture);
	madc_class_pattern_capture_active = false;
	DataDefCLASS stable_destination("Destination", 0,
	    DataType::dtRESERVED);
	stable_destination = speculative_class;
	CHECK_FALSE(stable_destination.speculative_class_capture);
    }

    TEST_CASE("ddINT is numeric and integer") {
        CHECK(ddINT.is_numeric());
        CHECK(ddINT.is_integer());
        CHECK(!ddINT.is_real());
        CHECK(!ddINT.is_object());
    }

    TEST_CASE("ddDOUBLE is numeric and real but not integer") {
        CHECK(ddDOUBLE.is_numeric());
        CHECK(ddDOUBLE.is_real());
        CHECK(!ddDOUBLE.is_integer());
    }

    TEST_CASE("DataDefCLASS is object, not numeric") {
        DataDefCLASS cls("Probe", 0, DataType::dtRESERVED);
        CHECK(cls.is_object());
        CHECK(!cls.is_numeric());
        CHECK(!cls.is_integer());
    }

    TEST_CASE("unsigned types are detected correctly") {
        CHECK(ddUINT8.is_unsigned());
        CHECK(ddUINT32.is_unsigned());
        CHECK(!ddINT.is_unsigned());
        CHECK(!ddINT32.is_unsigned());
    }

    TEST_CASE("struct type has btStruct basetype") {
        CHECK(test_struct().basetype() == BaseType::btStruct);
        CHECK(test_struct().is_struct());
        CHECK(!test_struct().is_numeric());
    }

    TEST_CASE("rawtype strips pointer and reference derivation (structural)") {
        // Derivation is the object graph now (no tag bands / rt* macros):
        // DataDefPTR/REF forward rawtype() to base_type.
        DataDefPTR ptr_int(ddINT);   // int*
        DataDefREF ref_int(ddINT);   // int&
        CHECK(ptr_int.rawtype() == DataType::dtINT);
        CHECK(ref_int.rawtype() == DataType::dtINT);
        CHECK(ddINT.rawtype()   == DataType::dtINT);
        CHECK(ptr_int.is_pointer());
        CHECK(ref_int.is_reference());
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
        CHECK(test_struct().members.size() == 4);
        CHECK(test_struct().members[0].first == "name");
        CHECK(test_struct().members[1].first == "id");
        CHECK(test_struct().members[2].first == "age");
        CHECK(test_struct().members[3].first == "sex");
    }

    TEST_CASE("member offsets are sequential") {
        std::string name_s = "name", id_s = "id", age_s = "age";
        CHECK(test_struct().m_offset(name_s) == 0);
        CHECK(test_struct().m_offset(id_s) == (ssize_t)sizeof(char *));
        CHECK(test_struct().m_offset(age_s) == (ssize_t)(sizeof(char *) + 4));
    }

    TEST_CASE("m_type returns correct DataDef pointer") {
        std::string id_s = "id";
        DataDef *t = test_struct().m_type(id_s);
        CHECK(t != nullptr);
        CHECK(t->is_integer());
    }

    TEST_CASE("m_offset returns -1 for unknown member") {
        std::string unknown = "nonexistent";
        CHECK(test_struct().m_offset(unknown) == -1);
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
    // DEFERRED: eval/exec reimplements on CIR→c2mir→MIR (+ REPL); see task
    TEST_CASE("separate Program instances do not leak typedefs or macros" * doctest::skip()) {
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

    // DEFERRED: eval/exec reimplements on CIR→c2mir→MIR (+ REPL); see task
    TEST_CASE("Engine builtin registry can be extended before parse" * doctest::skip()) {
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

    // DEFERRED: eval/exec reimplements on CIR→c2mir→MIR (+ REPL); see task
    TEST_CASE("Engine namespace registry can be extended before parse" * doctest::skip()) {
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

    // DEFERRED: eval/exec reimplements on CIR→c2mir→MIR (+ REPL); see task
    TEST_CASE("MadcEngine seeds program policy and registries" * doctest::skip()) {
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

    // DEFERRED: eval/exec reimplements on CIR→c2mir→MIR (+ REPL); see task
    TEST_CASE("Program load_file runs tokenize parse and compile" * doctest::skip()) {
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

    TEST_CASE("Program does not inject std iostream globals") {
	std::string path = write_temp_mad_source(
	    "madc_prog_no_iostream_globals",
	    "int main() { return 0; }\n");
	REQUIRE(!path.empty());

	MadcEngine engine;
	std::unique_ptr<Program> prog = engine.create_program();

	TokenProgram *tp = prog->tokenize(path.c_str());
	REQUIRE(tp != NULL);
	REQUIRE(prog->parse(tp));
	std::string cout_name = "cout";
	std::string cin_name = "cin";
	std::string cerr_name = "cerr";
	Variable *cout_var = prog->findVariable(cout_name);
	Variable *cin_var = prog->findVariable(cin_name);
	Variable *cerr_var = prog->findVariable(cerr_name);
	CHECK(cout_var == NULL);
	CHECK(cin_var == NULL);
	CHECK(cerr_var == NULL);
	std::map<std::string, variable_map_t>::iterator std_it =
	    prog->namespace_map.find("std");
	REQUIRE(std_it != prog->namespace_map.end());
	CHECK(std_it->second.find("cout") == std_it->second.end());
	CHECK(std_it->second.find("cin") == std_it->second.end());
	CHECK(std_it->second.find("cerr") == std_it->second.end());

	unlink(path.c_str());
    }

    // DEFERRED: eval/exec reimplements on CIR→c2mir→MIR (+ REPL); see task
    TEST_CASE("Program execute runtime failures become diagnostics" * doctest::skip()) {
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

	// DEFERRED: eval/exec reimplements on CIR→c2mir→MIR (+ REPL); see task
	TEST_CASE("MadcEngine stream helpers capture script output" * doctest::skip()) {
	std::string path = write_temp_mad_source(
	    "madc_prog_capture_output",
	    "#include <string>\n"
	    "int main() { std::string s = \"ok\"; puti(42); printstr(s); return 0; }\n");
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

    // DEFERRED: eval/exec reimplements on CIR→c2mir→MIR (+ REPL); see task
    TEST_CASE("MadcEngine buffer helpers capture and clear output" * doctest::skip()) {
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

    // DEFERRED: eval/exec reimplements on CIR→c2mir→MIR (+ REPL); see task
    TEST_CASE("MadcEngine tee output duplicates to primary and buffer" * doctest::skip()) {
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

    TEST_CASE("MadcEngine maps every LogLevel to a distinct syslog priority") {
	CHECK(MadcEngine::syslog_priority_for(MadcEngine::LogLevel::emerg)  == LOG_EMERG);
	CHECK(MadcEngine::syslog_priority_for(MadcEngine::LogLevel::alert)  == LOG_ALERT);
	CHECK(MadcEngine::syslog_priority_for(MadcEngine::LogLevel::crit)   == LOG_CRIT);
	CHECK(MadcEngine::syslog_priority_for(MadcEngine::LogLevel::err)    == LOG_ERR);
	CHECK(MadcEngine::syslog_priority_for(MadcEngine::LogLevel::warn)   == LOG_WARNING);
	CHECK(MadcEngine::syslog_priority_for(MadcEngine::LogLevel::notice) == LOG_NOTICE);
	CHECK(MadcEngine::syslog_priority_for(MadcEngine::LogLevel::info)   == LOG_INFO);
	CHECK(MadcEngine::syslog_priority_for(MadcEngine::LogLevel::debug)  == LOG_DEBUG);
    }

    TEST_CASE("MadcEngine enable/disable syslog sink toggles state") {
	MadcEngine engine;
	CHECK(engine.syslog_active == false);
	engine.disable_syslog_sink();
	CHECK(engine.syslog_active == false);

	engine.enable_syslog_sink("madc-unit-test");
	CHECK(engine.syslog_active == true);
	CHECK(engine.syslog_ident == "madc-unit-test");
	CHECK(engine.syslog_option == LOG_PID);
	CHECK(engine.syslog_facility == LOG_USER);

	engine.enable_syslog_sink("madc-unit-test-2", LOG_CONS, LOG_LOCAL0);
	CHECK(engine.syslog_ident == "madc-unit-test-2");
	CHECK(engine.syslog_option == LOG_CONS);
	CHECK(engine.syslog_facility == LOG_LOCAL0);

	engine.disable_syslog_sink();
	CHECK(engine.syslog_active == false);
    }

    TEST_CASE("MadcEngine file sink writes formatted lines to disk") {
	const std::string path = "/tmp/madc_log_sink_test.log";
	::unlink(path.c_str());

	MadcEngine engine;
	engine.log_to_error_stream = false;
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;
	REQUIRE(engine.enable_file_sink(path));

	engine.write_log(MadcEngine::LogLevel::warn, "first line");
	engine.write_log(MadcEngine::LogLevel::info, "second line");
	engine.disable_file_sink();

	std::ifstream in(path);
	std::string contents((std::istreambuf_iterator<char>(in)),
			     std::istreambuf_iterator<char>());
	CHECK(contents.find("[warn] first line")  != std::string::npos);
	CHECK(contents.find("[info] second line") != std::string::npos);
	::unlink(path.c_str());
    }

    TEST_CASE("MadcEngine file sink appends across enable cycles") {
	const std::string path = "/tmp/madc_log_sink_append.log";
	::unlink(path.c_str());

	MadcEngine engine_a;
	engine_a.log_to_error_stream = false;
	engine_a.log_timestamps = false;
	engine_a.log_level_prefixes = true;
	REQUIRE(engine_a.enable_file_sink(path));
	engine_a.write_log(MadcEngine::LogLevel::warn, "before");
	engine_a.disable_file_sink();

	MadcEngine engine_b;
	engine_b.log_to_error_stream = false;
	engine_b.log_timestamps = false;
	engine_b.log_level_prefixes = true;
	REQUIRE(engine_b.enable_file_sink(path));
	engine_b.write_log(MadcEngine::LogLevel::err, "after");
	engine_b.disable_file_sink();

	std::ifstream in(path);
	std::string contents((std::istreambuf_iterator<char>(in)),
			     std::istreambuf_iterator<char>());
	CHECK(contents.find("[warn] before") != std::string::npos);
	CHECK(contents.find("[err] after")   != std::string::npos);
	::unlink(path.c_str());
    }

    TEST_CASE("MadcEngine file sink re-enable does not duplicate writes") {
	const std::string path = "/tmp/madc_log_sink_reenable.log";
	::unlink(path.c_str());

	MadcEngine engine;
	engine.log_to_error_stream = false;
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;
	REQUIRE(engine.enable_file_sink(path));
	engine.write_log(MadcEngine::LogLevel::warn, "first");
	engine.disable_file_sink();
	REQUIRE(engine.enable_file_sink(path));
	engine.write_log(MadcEngine::LogLevel::warn, "second");
	engine.disable_file_sink();

	std::ifstream in(path);
	std::vector<std::string> lines;
	std::string line;
	while ( std::getline(in, line) )
	    lines.push_back(line);
	REQUIRE(lines.size() == 2);
	CHECK(lines[0].find("[warn] first")  != std::string::npos);
	CHECK(lines[1].find("[warn] second") != std::string::npos);
	::unlink(path.c_str());
    }

    TEST_CASE("MadcEngine file sink rotates when size exceeds max_bytes") {
	const std::string path = "/tmp/madc_log_sink_rotate.log";
	::unlink(path.c_str());
	::unlink((path + ".1").c_str());
	::unlink((path + ".2").c_str());
	::unlink((path + ".3").c_str());

	MadcEngine engine;
	engine.log_to_error_stream = false;
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;
	REQUIRE(engine.enable_file_sink(path, 60, 3));

	for ( int i = 0; i < 12; ++i )
	    engine.write_log(MadcEngine::LogLevel::warn, std::string("line ") + std::to_string(i));
	engine.disable_file_sink();

	std::ifstream rotated(path + ".1");
	std::string rotated_contents((std::istreambuf_iterator<char>(rotated)),
				     std::istreambuf_iterator<char>());
	CHECK(!rotated_contents.empty());
	CHECK(rotated_contents.find("[warn] line") != std::string::npos);

	std::ifstream live(path);
	std::string live_contents((std::istreambuf_iterator<char>(live)),
				  std::istreambuf_iterator<char>());
	CHECK(live_contents.size() <= 60);

	::unlink(path.c_str());
	::unlink((path + ".1").c_str());
	::unlink((path + ".2").c_str());
	::unlink((path + ".3").c_str());
    }

    TEST_CASE("MadcEngine file sink rotation respects max_files cap") {
	const std::string path = "/tmp/madc_log_sink_capcap.log";
	for ( int i = 0; i <= 6; ++i )
	    ::unlink((path + (i ? "." + std::to_string(i) : "")).c_str());

	MadcEngine engine;
	engine.log_to_error_stream = false;
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;
	REQUIRE(engine.enable_file_sink(path, 40, 2));

	for ( int i = 0; i < 30; ++i )
	    engine.write_log(MadcEngine::LogLevel::warn, std::string("entry ") + std::to_string(i));
	engine.disable_file_sink();

	struct stat st;
	CHECK(::stat((path + ".1").c_str(), &st) == 0);
	CHECK(::stat((path + ".2").c_str(), &st) == 0);
	CHECK(::stat((path + ".3").c_str(), &st) != 0);

	for ( int i = 0; i <= 6; ++i )
	    ::unlink((path + (i ? "." + std::to_string(i) : "")).c_str());
    }

    TEST_CASE("MadcEngine json_escape handles quote, backslash, control, and unicode-low chars") {
	CHECK(MadcEngine::json_escape("plain")             == "plain");
	CHECK(MadcEngine::json_escape("with \"quote\"")    == "with \\\"quote\\\"");
	CHECK(MadcEngine::json_escape("back\\slash")       == "back\\\\slash");
	CHECK(MadcEngine::json_escape("a\nb")              == "a\\nb");
	CHECK(MadcEngine::json_escape("a\tb")              == "a\\tb");
	CHECK(MadcEngine::json_escape(std::string("\x01")) == "\\u0001");
    }

    TEST_CASE("MadcEngine format_json_log_line emits level + message fields") {
	MadcEngine engine;
	engine.log_timestamps = false;
	const std::string line = engine.format_json_log_line(MadcEngine::LogLevel::warn, "disk \"full\"");
	CHECK(line == "{\"level\":\"warn\",\"message\":\"disk \\\"full\\\"\"}");
    }

    TEST_CASE("MadcEngine format_json_log_line includes ts when timestamps enabled") {
	MadcEngine engine;
	engine.log_timestamps = true;
	const std::string line = engine.format_json_log_line(MadcEngine::LogLevel::info, "hi");
	CHECK(line.find("\"ts\":\"") != std::string::npos);
	CHECK(line.find("\"level\":\"info\"")  != std::string::npos);
	CHECK(line.find("\"message\":\"hi\"")  != std::string::npos);
    }

    TEST_CASE("MadcEngine json sink writes one JSON object per log call") {
	const std::string path = "/tmp/madc_log_sink_json.log";
	::unlink(path.c_str());

	MadcEngine engine;
	engine.log_to_error_stream = false;
	engine.log_timestamps = false;
	REQUIRE(engine.enable_json_sink(path));

	engine.write_log(MadcEngine::LogLevel::warn, "disk full");
	engine.write_log(MadcEngine::LogLevel::err,  "tab\there");
	engine.disable_json_sink();

	std::ifstream in(path);
	std::vector<std::string> lines;
	std::string line;
	while ( std::getline(in, line) )
	    lines.push_back(line);
	REQUIRE(lines.size() == 2);
	CHECK(lines[0] == "{\"level\":\"warn\",\"message\":\"disk full\"}");
	CHECK(lines[1] == "{\"level\":\"err\",\"message\":\"tab\\there\"}");

	::unlink(path.c_str());
    }

    TEST_CASE("MadcEngine json sink re-enable does not duplicate writes") {
	const std::string path = "/tmp/madc_log_sink_json_reenable.log";
	::unlink(path.c_str());

	MadcEngine engine;
	engine.log_to_error_stream = false;
	engine.log_timestamps = false;
	REQUIRE(engine.enable_json_sink(path));
	engine.write_log(MadcEngine::LogLevel::warn, "one");
	engine.disable_json_sink();
	REQUIRE(engine.enable_json_sink(path));
	engine.write_log(MadcEngine::LogLevel::warn, "two");
	engine.disable_json_sink();

	std::ifstream in(path);
	std::vector<std::string> lines;
	std::string line;
	while ( std::getline(in, line) )
	    lines.push_back(line);
	REQUIRE(lines.size() == 2);
	CHECK(lines[0] == "{\"level\":\"warn\",\"message\":\"one\"}");
	CHECK(lines[1] == "{\"level\":\"warn\",\"message\":\"two\"}");

	::unlink(path.c_str());
    }

    TEST_CASE("madc level streams flow through JSON sink alongside file sink") {
	const std::string text = "/tmp/madc_log_sink_dual_text.log";
	const std::string json = "/tmp/madc_log_sink_dual_json.log";
	::unlink(text.c_str());
	::unlink(json.c_str());

	MadcEngine engine;
	engine.log_to_error_stream = false;
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;
	REQUIRE(engine.enable_file_sink(text));
	REQUIRE(engine.enable_json_sink(json));
	engine.bind_log_streams();

	madc::warn << "shared payload " << 42 << std::endl;

	MadcEngine::unbind_log_streams();
	engine.disable_json_sink();
	engine.disable_file_sink();

	std::ifstream in_text(text);
	std::string text_contents((std::istreambuf_iterator<char>(in_text)),
				  std::istreambuf_iterator<char>());
	std::ifstream in_json(json);
	std::string json_contents((std::istreambuf_iterator<char>(in_json)),
				  std::istreambuf_iterator<char>());
	CHECK(text_contents.find("[warn] shared payload 42") != std::string::npos);
	CHECK(json_contents.find("\"level\":\"warn\"") != std::string::npos);
	CHECK(json_contents.find("\"message\":\"shared payload 42\"") != std::string::npos);

	::unlink(text.c_str());
	::unlink(json.c_str());
    }

    TEST_CASE("MadcEngine::Config defaults match field-by-field initial state") {
	MadcEngine::Config cfg;
	CHECK(cfg.threshold == MadcEngine::LogLevel::debug);
	CHECK(cfg.timestamps == false);
	CHECK(cfg.level_prefixes == true);
	CHECK(cfg.error_stream == true);
	CHECK(cfg.file_sink == false);
	CHECK(cfg.file_max_files == 5);
	CHECK(cfg.syslog_sink == false);
	CHECK(cfg.syslog_ident == "madc");
	CHECK(cfg.json_sink == false);
    }

    TEST_CASE("MadcEngine apply_log_config wires file + JSON sinks declaratively") {
	const std::string text = "/tmp/madc_log_cfg_text.log";
	const std::string json = "/tmp/madc_log_cfg_json.log";
	::unlink(text.c_str());
	::unlink(json.c_str());

	MadcEngine engine;
	MadcEngine::Config cfg;
	cfg.threshold      = MadcEngine::LogLevel::warn;
	cfg.error_stream   = false;
	cfg.file_sink      = true;
	cfg.file_path      = text;
	cfg.file_max_bytes = 0;
	cfg.json_sink      = true;
	cfg.json_path      = json;
	REQUIRE(engine.apply_log_config(cfg));

	CHECK(engine.log_threshold == MadcEngine::LogLevel::warn);
	CHECK(engine.log_to_error_stream == false);
	CHECK(engine.file_sink_active == true);
	CHECK(engine.json_sink_active == true);

	engine.write_log(MadcEngine::LogLevel::warn, "kept");
	engine.write_log(MadcEngine::LogLevel::info, "filtered");
	engine.disable_file_sink();
	engine.disable_json_sink();

	std::ifstream in_text(text);
	std::string text_contents((std::istreambuf_iterator<char>(in_text)),
				  std::istreambuf_iterator<char>());
	std::ifstream in_json(json);
	std::string json_contents((std::istreambuf_iterator<char>(in_json)),
				  std::istreambuf_iterator<char>());
	CHECK(text_contents.find("[warn] kept") != std::string::npos);
	CHECK(text_contents.find("filtered")    == std::string::npos);
	CHECK(json_contents.find("\"level\":\"warn\"") != std::string::npos);
	CHECK(json_contents.find("filtered")    == std::string::npos);

	::unlink(text.c_str());
	::unlink(json.c_str());
    }

    TEST_CASE("MadcEngine apply_log_config disables omitted sinks idempotently") {
	const std::string path = "/tmp/madc_log_cfg_disable.log";
	::unlink(path.c_str());

	MadcEngine engine;
	REQUIRE(engine.enable_file_sink(path));
	CHECK(engine.file_sink_active == true);

	MadcEngine::Config cfg;
	REQUIRE(engine.apply_log_config(cfg));
	CHECK(engine.file_sink_active == false);
	CHECK(engine.json_sink_active == false);
	CHECK(engine.syslog_active == false);

	::unlink(path.c_str());
    }

    TEST_CASE("MadcEngine apply_log_config re-apply does not duplicate sink output") {
	const std::string text = "/tmp/madc_log_cfg_reapply_text.log";
	const std::string json = "/tmp/madc_log_cfg_reapply_json.log";
	::unlink(text.c_str());
	::unlink(json.c_str());

	MadcEngine engine;
	MadcEngine::Config cfg;
	cfg.error_stream = false;
	cfg.file_sink = true;
	cfg.file_path = text;
	cfg.json_sink = true;
	cfg.json_path = json;
	REQUIRE(engine.apply_log_config(cfg));
	REQUIRE(engine.apply_log_config(cfg));

	engine.write_log(MadcEngine::LogLevel::warn, "once");
	engine.disable_file_sink();
	engine.disable_json_sink();

	std::ifstream text_in(text);
	std::vector<std::string> text_lines;
	std::string line;
	while ( std::getline(text_in, line) )
	    text_lines.push_back(line);
	std::ifstream json_in(json);
	std::vector<std::string> json_lines;
	while ( std::getline(json_in, line) )
	    json_lines.push_back(line);

	REQUIRE(text_lines.size() == 1);
	REQUIRE(json_lines.size() == 1);
	CHECK(text_lines[0].find("[warn] once") != std::string::npos);
	CHECK(json_lines[0] == "{\"level\":\"warn\",\"message\":\"once\"}");

	::unlink(text.c_str());
	::unlink(json.c_str());
    }

    TEST_CASE("MadcEngine apply_log_config returns false when a sink path is unwritable") {
	MadcEngine engine;
	MadcEngine::Config cfg;
	cfg.file_sink = true;
	cfg.file_path = "/proc/this/path/cannot/exist/madc.log";
	const bool ok = engine.apply_log_config(cfg);
	CHECK(ok == false);
	CHECK(engine.file_sink_active == false);
    }

    TEST_CASE("MadcEngine reopen_log_file picks up an externally rotated path") {
	const std::string path = "/tmp/madc_log_sink_reopen.log";
	::unlink(path.c_str());

	MadcEngine engine;
	engine.log_to_error_stream = false;
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;
	REQUIRE(engine.enable_file_sink(path));
	engine.write_log(MadcEngine::LogLevel::warn, "before rotate");

	const std::string rotated = path + ".prev";
	::rename(path.c_str(), rotated.c_str());
	engine.reopen_log_file();
	engine.write_log(MadcEngine::LogLevel::warn, "after rotate");
	engine.disable_file_sink();

	std::ifstream prev(rotated);
	std::string prev_contents((std::istreambuf_iterator<char>(prev)),
				  std::istreambuf_iterator<char>());
	std::ifstream live(path);
	std::string live_contents((std::istreambuf_iterator<char>(live)),
				  std::istreambuf_iterator<char>());
	CHECK(prev_contents.find("before rotate") != std::string::npos);
	CHECK(live_contents.find("after rotate")  != std::string::npos);
	CHECK(live_contents.find("before rotate") == std::string::npos);

	::unlink(path.c_str());
	::unlink(rotated.c_str());
    }

    TEST_CASE("MadcEngine file sink rejects unwritable paths and stays inactive") {
	MadcEngine engine;
	engine.log_to_error_stream = false;
	const bool ok = engine.enable_file_sink("/proc/this/path/cannot/exist/madc.log");
	CHECK(ok == false);
	CHECK(engine.file_sink_active == false);
    }

    TEST_CASE("madc level streams flow through file sink") {
	const std::string path = "/tmp/madc_log_sink_facade.log";
	::unlink(path.c_str());

	MadcEngine engine;
	engine.log_to_error_stream = false;
	engine.log_timestamps = false;
	engine.log_level_prefixes = true;
	REQUIRE(engine.enable_file_sink(path));
	engine.bind_log_streams();

	madc::warn  << "via "   << "facade " << 1 << std::endl;
	madc::debug << "trace " << 0xff      << std::endl;

	MadcEngine::unbind_log_streams();
	engine.disable_file_sink();

	std::ifstream in(path);
	std::string contents((std::istreambuf_iterator<char>(in)),
			     std::istreambuf_iterator<char>());
	CHECK(contents.find("[warn] via facade 1") != std::string::npos);
	CHECK(contents.find("[debug] trace 255")   != std::string::npos);
	::unlink(path.c_str());
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

TEST_SUITE("DataDef::same_representation (=== type-domain identity)") {
    TEST_CASE("integers: size+signedness; tags are the representation") {
        DataDef i32("int", 4, DataType::dtINT32);
        DataDef u32("uint32_t", 4, DataType::dtUINT32);
        DataDef u8("uint8_t", 1, DataType::dtUINT8);
        DataDef i64a("long", 8, DataType::dtINT64);
        DataDef i64b("long long", 8, DataType::dtINT64);
        DataDef ch("char", 1, DataType::dtCHAR);
        DataDef i8("int8_t", 1, DataType::dtINT8);
        CHECK(!i32.same_representation(u32));   // signedness
        CHECK(!u32.same_representation(u8));    // size
        CHECK(i64a.same_representation(i64b));  // long === long long
        CHECK(ch.same_representation(i8));      // char == int8 on this target
        CHECK(!ch.same_representation(u8));
        CHECK(i32.same_representation(i32));
    }
    TEST_CASE("kinds: bool/float/int are distinct domains") {
        DataDef b("bool", 1, DataType::dtBOOL);
        DataDef u8("uint8_t", 1, DataType::dtUINT8);
        DataDef f("float", 4, DataType::dtFLOAT);
        DataDef d("double", 8, DataType::dtDOUBLE);
        DataDef i32("int", 4, DataType::dtINT32);
        CHECK(!b.same_representation(u8));
        CHECK(!f.same_representation(d));
        CHECK(!i32.same_representation(d));
        CHECK(d.same_representation(d));
    }
    TEST_CASE("enums are their own domain") {
        DataDefENUM color("Color");
        DataDefENUM color2("Color");
        DataDefENUM fruit("Fruit");
        DataDef i32("int", 4, DataType::dtINT32);
        CHECK(!color.same_representation(i32));   // enum vs int: false
        CHECK(!i32.same_representation(color));
        CHECK(!color.same_representation(fruit)); // different enums
        CHECK(color.same_representation(color2)); // same enum name
    }
    TEST_CASE("pointers recurse on the pointee; refs compare as referee") {
        DataDef i32("int", 4, DataType::dtINT32);
        DataDef u32("uint32_t", 4, DataType::dtUINT32);
        DataDefPTR pi(i32);
        DataDefPTR pi2(i32);
        DataDefPTR pu(u32);
        DataDefREF ri(i32);
        CHECK(pi.same_representation(pi2));
        CHECK(!pi.same_representation(pu));    // pointee signedness
        CHECK(!pi.same_representation(i32));   // ptr vs non-ptr
        CHECK(ri.same_representation(i32));    // T& reads as T
    }
    TEST_CASE("double pointers: char** is structurally distinct from char and char*") {
        // char** = DataDefPTR(DataDefPTR(char)). With the tag encoding retired,
        // same_representation recurses on base_type, so the old numeric-band
        // collision (char**'s tag landing in the bare-reference range) is gone —
        // the distinction is purely structural now.
        // The wraps go through DataDef& (as the parser does via DataDef*) —
        // DataDefPTR(pc) directly would pick the COPY ctor, not a wrap.
        DataDef ch("char", 1, DataType::dtCHAR);
        DataDefPTR pc(ch);                              // char*
        DataDefPTR pp(static_cast<DataDef &>(pc));      // char**
        DataDefPTR pc2(ch);
        DataDefPTR pp2(static_cast<DataDef &>(pc2));    // an identical char**
        CHECK(pp.is_pointer());
        CHECK(pp.rawtype() == DataType::dtCHAR);        // recurses to the scalar
        CHECK(!pp.same_representation(ch));    // char** vs char
        CHECK(!ch.same_representation(pp));
        CHECK(!pc.same_representation(pp));    // char* vs char**
        CHECK(!pp.same_representation(pc));
        CHECK(pp.same_representation(pp2));    // identical char** chains
    }
    TEST_CASE("is_cstr(): structural char* detection (char*, const char*, char&)") {
        DataDef ch("char", 1, DataType::dtCHAR);
        DataDef i32("int", 4, DataType::dtINT32);
        DataDefPTR pc(ch);                              // char*
        DataDefPTR pp(static_cast<DataDef &>(pc));      // char**
        DataDefCONST cc(ch);                            // const char
        DataDefPTR pcc(static_cast<DataDef &>(cc));     // const char*
        DataDefCONST ccp(static_cast<DataDef &>(pc));   // char* const
        DataDefPTR pi(i32);                             // int*
        DataDefPTR pv(ddVOID);                          // void*
        DataDefREF rc(ch);                              // char& (lowers as char*)
        // Matches char* and its const-qualified variants; a reference lowers as
        // the pointer so it matches too (preserving the old tag behaviour).
        CHECK(pc.is_cstr());
        CHECK(pcc.is_cstr());
        CHECK(ccp.is_cstr());
        CHECK(rc.is_cstr());
        // Excludes non-pointers, char**, and other pointer types.
        CHECK(!ch.is_cstr());
        CHECK(!pp.is_cstr());
        CHECK(!pi.is_cstr());
        CHECK(!pv.is_cstr());
        // (The old `is_cstr() == (type() == dtCHARptr)` cross-checks were
        // removed with the dtCHARptr tag itself — the explicit expected values
        // above ARE the structural contract that replaced the tag compare.)
    }
    TEST_CASE("function signatures: return + params + varargs; fptr targets") {
        DataDef i32("int", 4, DataType::dtINT32);
        DataDef u32("uint32_t", 4, DataType::dtUINT32);
        DataDef d("double", 8, DataType::dtDOUBLE);

        FuncDef f1(i32);            // int(int, double)
        f1.parameters.push_back(&i32);
        f1.parameters.push_back(&d);
        FuncDef f2(i32);            // int(int, double)
        f2.parameters.push_back(&i32);
        f2.parameters.push_back(&d);
        FuncDef f3(i32);            // int(uint32_t, double)
        f3.parameters.push_back(&u32);
        f3.parameters.push_back(&d);
        FuncDef f4(i32);            // int(int)
        f4.parameters.push_back(&i32);
        FuncDef f5(i32);            // int(int, double, ...)
        f5.parameters.push_back(&i32);
        f5.parameters.push_back(&d);
        f5.is_varargs = true;

        CHECK(f1.same_representation(f2));   // identical signature
        CHECK(!f1.same_representation(f3));  // differing param type
        CHECK(!f1.same_representation(f4));  // differing param count
        CHECK(!f1.same_representation(f5));  // varargs-ness differs

        DataDefFPTR p1(&f1);
        DataDefFPTR p2(&f1);
        CHECK(p1.same_representation(p2));   // same target FuncDef

        // A NULL param slot matches only a NULL slot, and a both-NULL
        // slot must NOT exempt the remaining params from checking.
        FuncDef g1(i32);            // int(NULL, int)
        g1.parameters.push_back(NULL);
        g1.parameters.push_back(&i32);
        FuncDef g2(i32);            // int(NULL, double)
        g2.parameters.push_back(NULL);
        g2.parameters.push_back(&d);
        FuncDef g3(i32);            // int(NULL, int)
        g3.parameters.push_back(NULL);
        g3.parameters.push_back(&i32);
        CHECK(!g1.same_representation(g2));  // both-NULL continues; later mismatch
        CHECK(g1.same_representation(g3));   // both-NULL + matching rest
        CHECK(!g1.same_representation(f1));  // one-NULL vs typed param
    }
    TEST_CASE("C arrays compare element + count") {
        DataDef i32("int", 4, DataType::dtINT32);
        DataDef u32("uint32_t", 4, DataType::dtUINT32);
        DataDefCArray a4(i32, "int[4]", 4);
        DataDefCArray b4(i32, "int[4]", 4);
        DataDefCArray a5(i32, "int[5]", 5);
        DataDefCArray u4(u32, "uint32_t[4]", 4);
        CHECK(a4.same_representation(b4));
        CHECK(!a4.same_representation(a5));
        CHECK(!a4.same_representation(u4));
    }
}

TEST_SUITE("type table (typeid) identity layer") {

    // The slot numbers are ABI (design doc §7): once eval package C ships,
    // these constants are frozen, exactly like the manglings in test_mangle.
    TEST_CASE("primitive slot constants are pinned ABI") {
        CHECK(MADC_TYPEID_INVALID == 0);
        CHECK(MADC_TYPEID_VOID == 1);
        CHECK(MADC_TYPEID_VOID_REF == 2);
        CHECK(MADC_TYPEID_BOOL == 3);
        CHECK(MADC_TYPEID_CHAR == 4);
        CHECK(MADC_TYPEID_INT == 5);
        CHECK(MADC_TYPEID_INT8 == 6);
        CHECK(MADC_TYPEID_INT16 == 7);
        CHECK(MADC_TYPEID_INT24 == 8);
        CHECK(MADC_TYPEID_INT32 == 9);
        CHECK(MADC_TYPEID_INT64 == 10);
        CHECK(MADC_TYPEID_UINT8 == 11);
        CHECK(MADC_TYPEID_UINT16 == 12);
        CHECK(MADC_TYPEID_UINT24 == 13);
        CHECK(MADC_TYPEID_UINT32 == 14);
        CHECK(MADC_TYPEID_UINT64 == 15);
        CHECK(MADC_TYPEID_FLOAT == 16);
        CHECK(MADC_TYPEID_DOUBLE == 17);
        CHECK(MADC_TYPEID_LONG_DOUBLE == 18);
        CHECK(MADC_TYPEID_INT128 == 19);
        CHECK(MADC_TYPEID_UINT128 == 20);
        CHECK(MADC_TYPEID_COMPLEX_FLOAT == 21);
        CHECK(MADC_TYPEID_COMPLEX_DOUBLE == 22);
        CHECK(MADC_TYPEID_MAX_ALIGN_T == 23);
        CHECK(MADC_TYPEID_LPSTR == 24);
        CHECK(MADC_TYPEID_VOID_PTR == 25);
        CHECK(MADC_TYPEID_CHAR_PTR == 26);
        CHECK(MADC_TYPEID_INT_PTR == 27);
        CHECK(MADC_TYPEID_INT32_PTR == 28);
        CHECK(MADC_TYPEID_ARRAY == 29);
        CHECK(MADC_TYPEID_AUTO == 30);
        CHECK(MADC_TYPEID_TEXT == 31);
        CHECK(MADC_TYPEID_BYTES == 32);
        CHECK(MADC_TYPEID_OBJECT == 33);
        CHECK(MADC_TYPEID_BUILTIN_VA_LIST == 34);
        CHECK(MADC_TYPEID_PLATFORM_LONG == 35);
        CHECK(MADC_TYPEID_PLATFORM_ULONG == 36);
        CHECK(MADC_TYPEID_PRIMITIVE_LAST == 36);
        CHECK(MADC_TYPEID_PRIMITIVE_LAST < MADC_TYPEID_PRIMITIVE_END);
        CHECK(MADC_TYPEID_PRIMITIVE_END == 0x100);
        CHECK(MADC_TYPEID_SYSTEM_BASE == 0x100);
        CHECK(MADC_TYPEID_PROJECT_BASE == 0x01000000u);
    }

    TEST_CASE("primitive stamping round-trips slot <-> global DataDef") {
        madc_stamp_primitive_type_ids();
        CHECK(ddVOID.type_id == MADC_TYPEID_VOID);
        CHECK(ddINT.type_id == MADC_TYPEID_INT);
        CHECK(ddUINT64.type_id == MADC_TYPEID_UINT64);
        CHECK(ddDOUBLE.type_id == MADC_TYPEID_DOUBLE);
        CHECK(ddCHARptr.type_id == MADC_TYPEID_CHAR_PTR);
        CHECK(madc_primitive_for_slot(MADC_TYPEID_INT) == &ddINT);
        CHECK(madc_primitive_for_slot(MADC_TYPEID_AUTO) == &ddAUTO);
        // every backed slot stamps to exactly its own number
        for ( uint32_t s = 1; s <= MADC_TYPEID_PRIMITIVE_LAST; ++s )
        {
            DataDef *dd = madc_primitive_for_slot(s);
            if ( dd )
                CHECK(dd->type_id == s);
        }
        // P0: 128-bit integer slots are backed (16-byte, integer, align 16)
        CHECK(madc_primitive_for_slot(MADC_TYPEID_INT128) == &ddINT128);
        CHECK(madc_primitive_for_slot(MADC_TYPEID_UINT128) == &ddUINT128);
        CHECK(ddINT128.size == 16);
        CHECK(ddUINT128.size == 16);
        CHECK(ddINT128.alignment() == 16);
        CHECK(ddINT128.is_integer());
        CHECK(ddUINT128.is_integer());
        CHECK(!ddINT128.is_real());
        CHECK(!ddINT128.is_unsigned());
        CHECK(ddUINT128.is_unsigned());
        // the compiler-owned SysV va_list: struct __madc_va_list_tag[1]
        {
            DataDef *va = madc_primitive_for_slot(MADC_TYPEID_BUILTIN_VA_LIST);
            REQUIRE(va != (DataDef *)NULL);
            CHECK(va->type_id == MADC_TYPEID_BUILTIN_VA_LIST);
            CHECK(va->size == 24);
        }
        // Platform long/ulong (task #46b): target-shaped accessors. On LP64
        // they ARE ddINT64/ddUINT64 (slots 10/15) and the pinned slots 35/36
        // resolve NULL — stamping them would steal the i64 slots. On LLP64
        // they are the distinct 4-byte int-ranked dds named for the mangler.
        {
            struct ModelGuard {
                TargetDataModel saved;
                ModelGuard() : saved(madc_target_data_model) {}
                ~ModelGuard() { madc_target_data_model = saved; }
            } guard;
            madc_target_data_model = TargetDataModel::LP64;
            CHECK(dd_platform_long() == &ddINT64);
            CHECK(dd_platform_ulong() == &ddUINT64);
            CHECK(dd_platform_wchar() == &ddINT32);
            CHECK(madc_primitive_for_slot(MADC_TYPEID_PLATFORM_LONG) == (DataDef *)NULL);
            CHECK(madc_primitive_for_slot(MADC_TYPEID_PLATFORM_ULONG) == (DataDef *)NULL);
            madc_target_data_model = TargetDataModel::LLP64;
            DataDef *pl = dd_platform_long();
            DataDef *pu = dd_platform_ulong();
            REQUIRE(pl != (DataDef *)NULL);
            REQUIRE(pu != (DataDef *)NULL);
            CHECK(pl != &ddINT64);
            CHECK(pu != &ddUINT64);
            CHECK(pl->size == 4);
            CHECK(pu->size == 4);
            CHECK(pl->name == "long");
            CHECK(pu->name == "unsigned long");
            CHECK(pl->is_integer());
            CHECK(pu->is_integer());
            CHECK(!pl->is_unsigned());
            CHECK(pu->is_unsigned());
            // subclass typeid exempts them from the scalar desugar — the
            // NAME ("long" -> 'l') stays authoritative in mangled symbols
            CHECK(pl->mangle_scalar_spelling() == "");
            CHECK(pu->mangle_scalar_spelling() == "");
            CHECK(madc_primitive_for_slot(MADC_TYPEID_PLATFORM_LONG) == pl);
            CHECK(madc_primitive_for_slot(MADC_TYPEID_PLATFORM_ULONG) == pu);
            CHECK(dd_platform_wchar() == &ddUINT16);
        }
        // reserved-but-unbacked slots resolve NULL until their P0 slice lands
        CHECK(madc_primitive_for_slot(MADC_TYPEID_LONG_DOUBLE) == (DataDef *)NULL);
        // dynamic value kinds have no compiler DataDef
        CHECK(madc_primitive_for_slot(MADC_TYPEID_TEXT) == (DataDef *)NULL);
        CHECK(madc_primitive_for_slot(MADC_TYPEID_OBJECT) == (DataDef *)NULL);
        // out-of-segment queries resolve NULL
        CHECK(madc_primitive_for_slot(MADC_TYPEID_INVALID) == (DataDef *)NULL);
        CHECK(madc_primitive_for_slot(MADC_TYPEID_PRIMITIVE_END) == (DataDef *)NULL);
    }

    TEST_CASE("project segment: lazy stamp, memoization, round trip") {
        Program pgm;
        DataDef a("UserTypeA", 4, DataType::dtINT);
        DataDef b("UserTypeB", 8, DataType::dtINT64);

        CHECK(a.type_id == 0);                          // unregistered until asked
        uint32_t ida = pgm.type_id_for(&a);
        CHECK(ida == MADC_TYPEID_PROJECT_BASE);         // first project id
        CHECK(pgm.type_id_for(&a) == ida);              // memoized via the stamp
        CHECK(pgm.type_from_id(ida) == &a);             // round trip
        CHECK(pgm.type_id_for(&b) == MADC_TYPEID_PROJECT_BASE + 1);
        CHECK(pgm.type_from_id(MADC_TYPEID_PROJECT_BASE + 1) == &b);

        // primitives resolve through the slot table
        madc_stamp_primitive_type_ids();
        CHECK(pgm.type_from_id(MADC_TYPEID_CHAR) == &ddCHAR);
        CHECK(pgm.type_id_for(&ddCHAR) == MADC_TYPEID_CHAR);    // no re-stamp

        // defensive NULLs: invalid, reserved system segment, foreign/unknown ids
        CHECK(pgm.type_from_id(MADC_TYPEID_INVALID) == (DataDef *)NULL);
        CHECK(pgm.type_from_id(MADC_TYPEID_SYSTEM_BASE + 5) == (DataDef *)NULL);
        CHECK(pgm.type_from_id(MADC_TYPEID_PROJECT_BASE + 99) == (DataDef *)NULL);
        CHECK(pgm.type_id_for((DataDef *)NULL) == MADC_TYPEID_INVALID);
    }

    TEST_CASE("class registration journal rolls back registries and type ids") {
        Program pgm;
        pgm.datatype_map.set_pool(&pgm.type_name_pool);
        pgm.namespace_datatype_map.set_pool(&pgm.namespace_name_pool);
        pgm.template_map.set_pool(&pgm.template_name_pool);
        pgm.partial_spec_map.set_pool(&pgm.template_name_pool);
        pgm.template_alias_map.set_pool(&pgm.template_name_pool);
        pgm.var_template_map.set_pool(&pgm.template_name_pool);
        pgm.fn_template_map.set_pool(&pgm.template_name_pool);
        pgm.fn_template_decl_map.set_pool(&pgm.template_name_pool);
        pgm.fn_template_instantiated_vars.set_pool(&pgm.template_name_pool);

        DataDefCLASS original("JournalOriginal", 0, DataType::dtRESERVED);
        DataDefCLASS temporary("JournalTemporary", 0, DataType::dtRESERVED);
        TokenDataType original_type("JournalOriginal", original);
        TokenDataType temporary_type("JournalTemporary", temporary);
        pgm.struct_map.set("JournalKey", &original);
        pgm.datatype_map["JournalKey"] = &original_type;
        pgm.forest_arena_enabled = true;

        {
            Program::ClassRegistrationJournal journal(pgm);
            CHECK(pgm.class_registration_taps_muted);
            CHECK_FALSE(pgm.forest_arena_enabled);
            pgm.struct_map.set("JournalKey", &temporary);
            pgm.struct_map.set("TemporaryKey", &temporary);
            pgm.datatype_map["JournalKey"] = &temporary_type;
            pgm.datatype_map["TemporaryKey"] = &temporary_type;
            CHECK(pgm.type_id_for(&temporary) == MADC_TYPEID_PROJECT_BASE);
        }

        CHECK_FALSE(pgm.class_registration_taps_muted);
        CHECK(pgm.forest_arena_enabled);
        CHECK(pgm.struct_map.find("JournalKey")->second == &original);
        CHECK(pgm.struct_map.find("TemporaryKey") == pgm.struct_map.end());
        CHECK(*pgm.datatype_map.find("JournalKey") == &original_type);
        CHECK(pgm.datatype_map.find("TemporaryKey") == pgm.datatype_map.end());
        CHECK(temporary.type_id == 0);
        CHECK(pgm.project_types.size() == 0);

        {
            Program::ClassRegistrationJournal journal(pgm);
            pgm.struct_map.set("CommittedKey", &temporary);
            journal.commit();
        }
        CHECK(pgm.struct_map.find("CommittedKey")->second == &temporary);
        CHECK_FALSE(pgm.class_registration_taps_muted);
        CHECK(pgm.forest_arena_enabled);
    }

    TEST_CASE("derived-type id API: pointer/reference/const round-trip + idempotent") {
        Program pgm;
        madc_stamp_primitive_type_ids();

        uint32_t int_id = pgm.type_id_for(&ddINT);
        CHECK(int_id == MADC_TYPEID_INT);

        // pointer-to(int): idempotent, reverse-resolves to the canonical
        // DataDefPTR — the SAME object the compiler's getPointerType returns.
        uint32_t p1 = pgm.derived_type_id(DerivedKind::dkPointer, int_id);
        uint32_t p2 = pgm.derived_type_id(DerivedKind::dkPointer, int_id);
        CHECK(p1 != MADC_TYPEID_INVALID);
        CHECK(p1 == p2);                                       // idempotent
        DataDef *pd = pgm.type_from_id(p1);
        CHECK(pd != (DataDef *)NULL);
        CHECK(pd->is_pointer());
        CHECK(pd == pgm.getPointerType(&ddINT));               // one source of truth
        // int* is the well-known global ddINTptr — a primitive slot, not a
        // freshly-minted project id.
        CHECK(pd == &ddINTptr);
        CHECK(p1 == MADC_TYPEID_INT_PTR);

        // reference-to(int) and const(int): idempotent, canonical
        uint32_t r1 = pgm.derived_type_id(DerivedKind::dkReference, int_id);
        CHECK(r1 == pgm.derived_type_id(DerivedKind::dkReference, int_id));
        CHECK(pgm.type_from_id(r1) == (DataDef *)pgm.getReferenceType(&ddINT));

        uint32_t c1 = pgm.derived_type_id(DerivedKind::dkConst, int_id);
        CHECK(c1 == pgm.derived_type_id(DerivedKind::dkConst, int_id));
        CHECK(pgm.type_from_id(c1) == (DataDef *)pgm.getConstType(&ddINT));

        // distinct kinds yield distinct ids
        CHECK(p1 != r1);
        CHECK(r1 != c1);
        CHECK(p1 != c1);

        // unknown operand id -> invalid
        CHECK(pgm.derived_type_id(DerivedKind::dkPointer,
                                  MADC_TYPEID_PROJECT_BASE + 999) == MADC_TYPEID_INVALID);
    }
}

TEST_SUITE("DataDefTemplateParam (unresolved template parameter T)") {
    TEST_CASE("identity: is_template_param true, every other predicate false") {
        DataDefTemplateParam t("T", 0);

        CHECK(t.is_template_param());
        CHECK(t.basetype() == BaseType::btTemplateParam);
        CHECK(t.name == "T");
        CHECK(t.param_index == 0u);
        CHECK(t.size == 0u);
        CHECK(t.rawtype() == DataType::dtVOID);
        CHECK(t.reftype() == RefType::rtValue);

        // A placeholder is none of these — the inherited predicates must all
        // answer false so type-system consumers tolerate it without crashing
        // or mis-classifying it.
        CHECK_FALSE(t.is_numeric());
        CHECK_FALSE(t.is_integer());
        CHECK_FALSE(t.is_real());
        CHECK_FALSE(t.is_unsigned());
        CHECK_FALSE(t.is_pointer());
        CHECK_FALSE(t.is_reference());
        CHECK_FALSE(t.is_const());
        CHECK_FALSE(t.is_struct());
        CHECK_FALSE(t.is_object());
        CHECK_FALSE(t.is_function());
        CHECK_FALSE(t.is_complex());
        CHECK_FALSE(t.is_simd());
        CHECK_FALSE(t.is_member_pointer());
    }

    TEST_CASE("param_index carries the parameter position") {
        DataDefTemplateParam t0("T", 0);
        DataDefTemplateParam t1("U", 1);
        DataDefTemplateParam t2("V", 7);
        CHECK(t0.param_index == 0u);
        CHECK(t1.param_index == 1u);
        CHECK(t2.param_index == 7u);
        CHECK(t1.name == "U");
    }

    TEST_CASE("a non-placeholder type answers is_template_param false") {
        CHECK_FALSE(ddINT.is_template_param());
        CHECK_FALSE(ddCHAR.is_template_param());
    }
}
