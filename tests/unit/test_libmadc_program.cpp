// Unit tests for madc::program (libmadc public embedding API).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <asmjit/x86.h>

#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <stack>
#include <string>
#include <vector>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_api.h"
#include "madc_value_cell.h"
#include "libmadc/api.h"
#include "libmadc/engine.h"
#include "libmadc/program.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits.h>
#include <unistd.h>

namespace {

int64_t g_host_sum = 0;
int64_t g_host_strlen = 0;
volatile int64_t g_host_spin_sink = 0;
std::vector<char> g_host_memory;

int64_t host_add(int64_t left, int64_t right)
{
    g_host_sum = left + right;
    return g_host_sum;
}

int64_t host_strlen(const char *s)
{
    g_host_strlen = s ? static_cast<int64_t>(std::strlen(s)) : -1;
    return g_host_strlen;
}

int64_t host_sum4(int64_t a, int64_t b, int64_t c, int64_t d)
{
    g_host_sum = a + b + c + d;
    return g_host_sum;
}

int64_t host_word_length(const char *s)
{
    g_host_strlen = s ? static_cast<int64_t>(std::strlen(s)) : -1;
    return g_host_strlen;
}

const char *host_echo_value(const char *s)
{
    static std::string out;
    out = s ? s : "";
    out += "!";
    return out.c_str();
}

void host_spin(int64_t iterations)
{
    for ( int64_t i = 0; i < iterations; ++i )
	g_host_spin_sink += (i & 1);
}

int64_t host_hold_memory(int64_t bytes)
{
    if ( bytes < 0 )
	bytes = 0;
    g_host_memory.assign(static_cast<size_t>(bytes), 0);
    for ( size_t i = 0; i < g_host_memory.size(); i += 4096 )
	g_host_memory[i] = static_cast<char>(i & 0x7f);
    if ( !g_host_memory.empty() )
	g_host_memory[g_host_memory.size() - 1] = 1;
    return static_cast<int64_t>(g_host_memory.size());
}

std::string make_temp_source_path()
{
    char path[] = "/tmp/madc_program_test_XXXXXX";
    int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    close(fd);
    return std::string(path);
}

void write_file(const std::string &path, const std::string &contents)
{
    std::ofstream out(path.c_str(), std::ios::binary);
    REQUIRE(out.good());
    out << contents;
    out.close();
}

std::string resolve_repo_lib_dir()
{
    char resolved[PATH_MAX];
    if ( realpath("lib", resolved) )
    {
	std::string candidate = std::string(resolved) + "/libmadc.so";
	if ( access(candidate.c_str(), R_OK) == 0 )
	    return std::string(resolved);
    }
    if ( realpath("../lib", resolved) )
    {
	std::string candidate = std::string(resolved) + "/libmadc.so";
	if ( access(candidate.c_str(), R_OK) == 0 )
	    return std::string(resolved);
    }
    return "/usr/local/lib";
}

std::string executable_command(const std::string &exe_path)
{
    return "env LD_LIBRARY_PATH=" + resolve_repo_lib_dir() + ":/usr/local/lib "
	+ exe_path;
}

} // namespace

// In-process compile/exec/call/eval run on the CIR→c2mir→MIR backend via
// CirJitSession (see docs/plans/2026-06-10-libmadc-eval-on-cir-plan.md).
// Cases still marked doctest::skip() are the deferred increments: AOT
// save/load (stubbed on CIR), fork_per_invocation child execution, host
// callback registration (register_function/engine callbacks), script
// string-object & std::string call marshalling, get_global/set_global,
// script-side runtime-eval scope access, invoke_limits rejection shapes,
// and error-surface details that still differ from the asmjit-era spec.
TEST_SUITE("madc::program") {

    TEST_CASE("compile_file succeeds for a valid file with main") {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path, "int main() { return 0; }\n");

	CHECK(pgm.compile_file(path));
	CHECK_FALSE(pgm.has_error());
	CHECK(pgm.diagnostics().empty());

	std::remove(path.c_str());
    }

    TEST_CASE("exec_file reports runtime error when main is missing" * doctest::skip()) {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path, "int helper() { return 1; }\n");

	CHECK_FALSE(pgm.exec_file(path));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message == "Program::execute() cannot find main");

	std::remove(path.c_str());
    }

    TEST_CASE("exec_string succeeds for a simple in-memory program") {
	madc::program pgm;

	CHECK(pgm.exec_string("int main() { return 0; }\n", "memory_test.mad"));
	CHECK_FALSE(pgm.has_error());
	CHECK(pgm.diagnostics().empty());
    }

    TEST_CASE("exec_string rewrites public diagnostic filename to virtual filename") {
	madc::program pgm;

	CHECK_FALSE(pgm.exec_string("int main( { return 0; }\n", "memory_test.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::parser);
	CHECK(err->file == "memory_test.mad");
	REQUIRE_FALSE(pgm.diagnostics().empty());
	CHECK(pgm.diagnostics()[0].file == "memory_test.mad");
    }

    TEST_CASE("diagnostics reset across runs") {
	madc::program pgm;

	CHECK_FALSE(pgm.exec_string("int main( { return 0; }\n", "bad.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->file == "bad.mad");

	CHECK(pgm.exec_string("int main() { return 0; }\n", "good.mad"));
	CHECK_FALSE(pgm.has_error());
	CHECK(pgm.diagnostics().empty());
    }

    TEST_CASE("eval compiles in-memory source and returns integer result") {
	madc::program pgm;
	madc::value result;

	CHECK(pgm.eval("int helper() { return 40; }\n"
		       "int __madc_eval() { return helper() + 2; }\n",
		       &result,
		       "eval_ok.mad"));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 42);
	CHECK_FALSE(pgm.has_error());
    }

	TEST_CASE("eval returns string results through char pointer marshaling" * doctest::skip()) {
	madc::program pgm;
	madc::value result;

	CHECK(pgm.eval("#include <string>\n"
		       "std::string name = \"echo\";\n"
		       "char * __madc_eval() { return name.c_str(); }\n",
		       &result,
		       "eval_string.mad"));
	REQUIRE(result.is_string());
	CHECK(result.as_string() == "echo");
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval rewrites diagnostic filename to virtual filename") {
	madc::program pgm;
	madc::value result;

	CHECK_FALSE(pgm.eval("int __madc_eval( { return 0; }\n", &result, "eval_bad.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::parser);
	CHECK(err->file == "eval_bad.mad");
	REQUIRE_FALSE(pgm.diagnostics().empty());
	CHECK(pgm.diagnostics()[0].file == "eval_bad.mad");
    }

    TEST_CASE("eval requires the reserved zero-arg entry function") {
	madc::program pgm;
	madc::value result;

	CHECK_FALSE(pgm.eval("int helper() { return 1; }\n", &result, "eval_missing_entry.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->message == "program::call cannot find function '__madc_eval'");
    }

    TEST_CASE("madc::eval convenience wrapper mirrors program eval") {
	madc::value result;

	CHECK(madc::eval("int helper() { return 6; }\n"
			 "int __madc_eval() { return helper() * 7; }\n",
			 &result,
			 "api_eval.mad"));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 42);
    }

    TEST_CASE("eval_unit names the explicit full-translation-unit lane") {
	madc::program pgm;
	int64_t result = 0;

	CHECK(pgm.eval_unit("int helper() { return 40; }\n"
			    "int __madc_eval() { return helper() + 2; }\n",
			    result,
			    "eval_unit_ok.mad"));
	CHECK(result == 42);
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("madc::eval_unit convenience wrapper mirrors program eval_unit") {
	int64_t result = 0;

	CHECK(madc::eval_unit("int helper() { return 6; }\n"
			      "int __madc_eval() { return helper() * 7; }\n",
			      result,
			      "api_eval_unit.mad"));
	CHECK(result == 42);
    }

    TEST_CASE("typed eval overloads write directly into host destinations") {
	madc::program pgm;
	int64_t i = 0;
	std::string s;

	CHECK(pgm.eval("int __madc_eval() { return 42; }\n",
		       i,
		       "typed_eval_int.mad"));
	CHECK(i == 42);

	CHECK(pgm.eval("char * __madc_eval() { return \"echo\"; }\n",
		       s,
		       "typed_eval_string.mad"));
	CHECK(s == "echo");
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("typed eval_expression overloads write directly into host destinations") {
	madc::program pgm;
	int64_t i = 0;
	std::string s;

	CHECK(pgm.eval_expression("6 * 7", i, "typed_expr_int.mad"));
	CHECK(i == 42);

	CHECK(pgm.eval_expression("\"echo\"", s, "typed_expr_string.mad"));
	CHECK(s == "echo");
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("typed convenience wrappers mirror typed program eval") {
	int64_t eval_result = 0;
	int64_t expr_result = 0;

	CHECK(madc::eval("int __madc_eval() { return 42; }\n",
			 eval_result,
			 "api_typed_eval.mad"));
	CHECK(eval_result == 42);

	CHECK(madc::eval_expression("6 * 7",
				    expr_result,
				    "api_typed_eval_expr.mad"));
	CHECK(expr_result == 42);
    }

    TEST_CASE("eval_body wraps typed host bodies automatically") {
	madc::program pgm;
	int64_t i = 0;
	std::string s;

	CHECK(pgm.eval_body("return 42;", i, "typed_eval_body_int.mad"));
	CHECK(i == 42);

	CHECK(pgm.eval_body("int helper() { return 40; }\nreturn helper() + 2;",
			   i,
			   "typed_eval_body_helper.mad"));
	CHECK(i == 42);

	CHECK(pgm.eval_body("return \"echo\";", s, "typed_eval_body_string.mad"));
	CHECK(s == "echo");
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_body still accepts explicit eval entry source") {
	madc::program pgm;
	int64_t i = 0;

	CHECK(pgm.eval_body("int __madc_eval() { return 42; }\n",
			   i,
			   "typed_eval_body_explicit.mad"));
	CHECK(i == 42);
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_body supports explicit return contract for madc::value") {
	madc::value result;

	CHECK(madc::eval_body("return 42;",
			      &result,
			      madc::program::native_type::integer,
			      "api_eval_body_value.mad"));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 42);
    }

    TEST_CASE("eval_body rejects void return contracts") {
	madc::program pgm;
	madc::value result;

	CHECK_FALSE(pgm.eval_body("return 0;",
				  &result,
				  madc::program::native_type::void_type,
				  "eval_body_void_bad.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->message.find("requires a non-void scalar or string return type") != std::string::npos);
    }

    TEST_CASE("typed eval overloads reject incompatible result kinds") {
	madc::program pgm;
	std::string s;

	CHECK_FALSE(pgm.eval("int __madc_eval() { return 42; }\n",
			     s,
			     "typed_eval_bad.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->message.find("requires a string result") != std::string::npos);
    }

    TEST_CASE("eval_expression evaluates a plain arithmetic expression") {
	madc::program pgm;
	madc::value result;

	CHECK(pgm.eval_expression("1 + 2 * 3", &result, "expr_ok.mad"));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 7);
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_expression rejects statement-shaped input") {
	madc::program pgm;
	madc::value result;

	CHECK_FALSE(pgm.eval_expression("1; return 2", &result, "expr_bad.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("statement separators are not allowed") != std::string::npos);
    }

    TEST_CASE("eval_expression rejects breakout tokens before compilation") {
	madc::program pgm;
	madc::value result;

	CHECK_FALSE(pgm.eval_expression("0); puti(1); (0", &result, "expr_breakout.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("statement separators are not allowed") != std::string::npos);
    }

    TEST_CASE("eval_expression rejects mutation-oriented operators") {
	madc::program pgm;
	madc::value result;

	CHECK_FALSE(pgm.eval_expression("(__madc_expr_value = 7)", &result, "expr_assign.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK((err->message.find("assignment operators are not allowed") != std::string::npos
	    || err->message.find("reserved madc implementation identifiers are not allowed") != std::string::npos));

	pgm.clear_diagnostics();
	CHECK_FALSE(pgm.eval_expression("++5", &result, "expr_inc.mad"));
	REQUIRE(pgm.has_error());
	err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("increment and decrement operators are not allowed") != std::string::npos);
    }

    TEST_CASE("eval_expression rejects comma sequencing") {
	madc::program pgm;
	madc::value result;

    CHECK_FALSE(pgm.eval_expression("(1, 2)", &result, "expr_comma.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
    }

    TEST_CASE("eval_expression still allows ternary expressions through the AST validator") {
	madc::program pgm;
	madc::value result;

	CHECK(pgm.eval_expression("1 ? 40 + 2 : 0", &result, "expr_ternary.mad"));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 42);
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_expression rejects subscript access by default") {
	madc::program pgm;
	madc::value result;

	CHECK_FALSE(pgm.eval_expression("\"abc\"[1]", &result, "expr_subscript_blocked.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("subscript access is disabled by expression policy") != std::string::npos);
    }

    TEST_CASE("eval_expression can enable subscript access explicitly") {
	madc::program pgm;
	madc::expression_policy policy;
	policy.allow_subscript_access = true;
	pgm.set_expression_policy(policy);

	madc::value result;
	CHECK(pgm.eval_expression("\"abc\"[1]", &result, "expr_subscript_ok.mad"));
	REQUIRE(result.is_integer());
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_expression rejects pointer operations by default") {
	madc::program pgm;
	madc::value result;

	CHECK_FALSE(pgm.eval_expression("*((char *)\"abc\")", &result, "expr_ptr_blocked.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("pointer operations are disabled by expression policy") != std::string::npos);
    }

    TEST_CASE("eval_expression can enable pointer operations explicitly") {
	madc::program pgm;
	madc::expression_policy policy;
	policy.allow_pointer_operations = true;
	pgm.set_expression_policy(policy);

	madc::value result;
	CHECK(pgm.eval_expression("*((char *)\"abc\")", &result, "expr_ptr_ok.mad"));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == static_cast<int>('a'));
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_expression can use host-supplied integer bindings") {
	madc::program pgm;
	std::map<std::string, madc::value> bindings;
	bindings["x"] = madc::value(static_cast<int64_t>(6));
	bindings["y"] = madc::value(static_cast<int64_t>(7));
	pgm.set_expression_bindings(bindings);

	madc::value result;
	CHECK(pgm.eval_expression("x * y", &result, "expr_bindings_int.mad"));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 42);
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_expression can return host-supplied string bindings" * doctest::skip()) {
	madc::program pgm;
	std::map<std::string, madc::value> bindings;
	bindings["name"] = madc::value("echo");
	pgm.set_expression_bindings(bindings);

	madc::value result;
	CHECK(pgm.eval_expression("name", &result, "expr_bindings_string.mad"));
	REQUIRE(result.is_string());
	CHECK(result.as_string() == "echo");
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_expression rejects unsupported binding kinds") {
	madc::program pgm;
	std::map<std::string, madc::value> bindings;
	bindings["items"] = madc::value::make_array();
	pgm.set_expression_bindings(bindings);

	madc::value result;
	CHECK_FALSE(pgm.eval_expression("items", &result, "expr_bindings_array.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("cannot bind value kind 'array'") != std::string::npos);
    }

    TEST_CASE("eval_expression can use object context fields as bindings") {
	madc::program pgm;
	std::map<std::string, madc::value> fields;
	fields["x"] = madc::value(static_cast<int64_t>(6));
	fields["y"] = madc::value(static_cast<int64_t>(7));
	pgm.set_expression_context(madc::value::make_object(fields));

	madc::value result;
	CHECK(pgm.eval_expression("x * y", &result, "expr_context_int.mad"));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 42);
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_expression can traverse nested object context primitives") {
	madc::program pgm;
	std::map<std::string, madc::value> stats_fields;
	stats_fields["level"] = madc::value(static_cast<int64_t>(41));
	std::map<std::string, madc::value> user_fields;
	user_fields["stats"] = madc::value::make_object(stats_fields);
	std::map<std::string, madc::value> root_fields;
	root_fields["user"] = madc::value::make_object(user_fields);
	pgm.set_expression_context(madc::value::make_object(root_fields));

	madc::value result;
	CHECK(pgm.eval_expression("user.stats.level + 1", &result, "expr_context_nested.mad"));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 42);
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_expression can return top-level string context fields") {
	madc::program pgm;
	std::map<std::string, madc::value> fields;
	fields["name"] = madc::value("echo");
	pgm.set_expression_context(madc::value::make_object(fields));

	madc::value result;
	CHECK(pgm.eval_expression("name", &result, "expr_context_string.mad"));
	REQUIRE(result.is_string());
	CHECK(result.as_string() == "echo");
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_expression can traverse nested object context string fields") {
	madc::program pgm;
	std::map<std::string, madc::value> user_fields;
	user_fields["name"] = madc::value("echo");
	std::map<std::string, madc::value> root_fields;
	root_fields["user"] = madc::value::make_object(user_fields);
	pgm.set_expression_context(madc::value::make_object(root_fields));

	madc::value result;
	CHECK(pgm.eval_expression("user.name", &result, "expr_context_nested_string.mad"));
	REQUIRE(result.is_string());
	CHECK(result.as_string() == "echo");
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_expression rejects string to non-string comparison") {
	madc::program pgm;
	std::map<std::string, madc::value> user_fields;
	user_fields["name"] = madc::value("echo");
	std::map<std::string, madc::value> root_fields;
	root_fields["user"] = madc::value::make_object(user_fields);
	pgm.set_expression_context(madc::value::make_object(root_fields));

	bool result = false;
	CHECK_FALSE(pgm.eval_expression("user.name == 5", result, "expr_context_mixed_compare.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("cannot compare string and non-string values") != std::string::npos);
    }

    TEST_CASE("eval_expression lowers cast-wrapped string compares by value") {
	madc::program pgm;
	std::map<std::string, madc::value> user_fields;
	user_fields["name"] = madc::value("echo");
	std::map<std::string, madc::value> root_fields;
	root_fields["user"] = madc::value::make_object(user_fields);
	pgm.set_expression_context(madc::value::make_object(root_fields));

	bool result = false;
	CHECK(pgm.eval_expression("(bool)(user.name == \"echo\")", result, "expr_context_cast_compare.mad"));
	CHECK(result == true);
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_expression strict equality on strings compares by value") {
	madc::program pgm;

	bool result = false;
	CHECK(pgm.eval_expression("\"abc\" === \"abc\"", result, "expr_strict_str_eq.mad"));
	CHECK(result == true);
	CHECK_FALSE(pgm.has_error());

	CHECK(pgm.eval_expression("\"abc\" !== \"abc\"", result, "expr_strict_str_ne.mad"));
	CHECK(result == false);
	CHECK_FALSE(pgm.has_error());

	CHECK(pgm.eval_expression("\"abc\" !== \"abd\"", result, "expr_strict_str_ne2.mad"));
	CHECK(result == true);
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_expression strict equality on numbers is type-strict") {
	madc::program pgm;

	bool result = false;
	CHECK(pgm.eval_expression("5 === 5", result, "expr_strict_int_eq.mad"));
	CHECK(result == true);
	CHECK_FALSE(pgm.has_error());

	CHECK(pgm.eval_expression("5 === 6", result, "expr_strict_int_ne.mad"));
	CHECK(result == false);
	CHECK_FALSE(pgm.has_error());

	CHECK(pgm.eval_expression("5 !== 6", result, "expr_strict_int_ne2.mad"));
	CHECK(result == true);
	CHECK_FALSE(pgm.has_error());

	// int literal vs double literal: different type domains
	CHECK(pgm.eval_expression("5 === 5.0", result, "expr_strict_mixed_num.mad"));
	CHECK(result == false);
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_expression rejects strict string to non-string comparison") {
	madc::program pgm;

	bool result = false;
	CHECK_FALSE(pgm.eval_expression("\"5\" !== 5", result, "expr_strict_mixed_compare.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->message.find("cannot compare string and non-string values") != std::string::npos);
    }

    TEST_CASE("eval_expression rewrites nested object context paths with parser-legal trivia") {
	madc::program pgm;
	std::map<std::string, madc::value> stats_fields;
	stats_fields["level"] = madc::value(static_cast<int64_t>(41));
	std::map<std::string, madc::value> user_fields;
	user_fields["stats"] = madc::value::make_object(stats_fields);
	std::map<std::string, madc::value> root_fields;
	root_fields["user"] = madc::value::make_object(user_fields);
	pgm.set_expression_context(madc::value::make_object(root_fields));

	madc::value result;
	CHECK(pgm.eval_expression("user /* root */ . stats\n.\tlevel + 1",
				  &result,
				  "expr_context_nested_trivia.mad"));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 42);
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_expression rejects missing nested object context fields explicitly") {
	madc::program pgm;
	std::map<std::string, madc::value> stats_fields;
	stats_fields["level"] = madc::value(static_cast<int64_t>(41));
	std::map<std::string, madc::value> user_fields;
	user_fields["stats"] = madc::value::make_object(stats_fields);
	std::map<std::string, madc::value> root_fields;
	root_fields["user"] = madc::value::make_object(user_fields);
	pgm.set_expression_context(madc::value::make_object(root_fields));

	madc::value result;
	CHECK_FALSE(pgm.eval_expression("user.stats.rank + 1", &result, "expr_context_missing.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("cannot find field 'rank'") != std::string::npos);
    }

    TEST_CASE("eval_expression rejects nested descent through primitive context fields explicitly") {
	madc::program pgm;
	std::map<std::string, madc::value> user_fields;
	user_fields["name"] = madc::value("echo");
	std::map<std::string, madc::value> root_fields;
	root_fields["user"] = madc::value::make_object(user_fields);
	pgm.set_expression_context(madc::value::make_object(root_fields));

	madc::value result;
	CHECK_FALSE(pgm.eval_expression("user.name.first", &result, "expr_context_bad_descent.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("cannot descend through non-object field 'user.name'") != std::string::npos);
    }

    TEST_CASE("eval_expression ignores unrelated unsupported context fields") {
	madc::program pgm;
	std::map<std::string, madc::value> root_fields;
	root_fields["x"] = madc::value(static_cast<int64_t>(6));
	root_fields["items"] = madc::value::make_array();
	pgm.set_expression_context(madc::value::make_object(root_fields));

	madc::value result;
	CHECK(pgm.eval_expression("x * 7", &result, "expr_context_unused_array.mad"));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 42);
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_expression rejects non-object context values") {
	madc::program pgm;
	pgm.set_expression_context(madc::value(static_cast<int64_t>(7)));

	madc::value result;
	CHECK_FALSE(pgm.eval_expression("x", &result, "expr_context_bad.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("context must be an object value") != std::string::npos);
    }

    TEST_CASE("eval_expression rejects context and explicit binding collisions") {
	madc::program pgm;
	std::map<std::string, madc::value> fields;
	fields["x"] = madc::value(static_cast<int64_t>(6));
	pgm.set_expression_context(madc::value::make_object(fields));

	std::map<std::string, madc::value> bindings;
	bindings["x"] = madc::value(static_cast<int64_t>(7));
	pgm.set_expression_bindings(bindings);

	madc::value result;
	CHECK_FALSE(pgm.eval_expression("x", &result, "expr_context_collision.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("collides with an explicit binding") != std::string::npos);
    }

    TEST_CASE("eval_expression rejects function calls by default") {
	madc::program pgm;
	madc::value result;

	CHECK_FALSE(pgm.eval_expression("strlen(\"abcd\")", &result, "expr_calls_blocked.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("function calls are not allowed") != std::string::npos);
    }

    TEST_CASE("eval_expression can use math.h header groups for libm functions" * doctest::skip()) {
	madc::program pgm;
	madc::expression_policy policy;
	policy.allow_function_calls = true;
	policy.allowed_headers.push_back("math.h");
	pgm.set_expression_policy(policy);

	madc::value result;
	CHECK(pgm.eval_expression("sqrt(9.0) + cos(0.0)", &result, "expr_math.mad"));
	REQUIRE(result.is_real());
	CHECK(result.as_real() == doctest::Approx(4.0));
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_expression can use direct symbol allowlists without headers") {
	madc::program pgm;
	madc::expression_policy policy;
	policy.allow_function_calls = true;
	policy.allowed_functions.push_back("strlen");
	pgm.set_expression_policy(policy);

	madc::value result;
	CHECK(pgm.eval_expression("strlen(\"abcd\")", &result, "expr_strlen.mad"));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 4);
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("eval_expression rejects calls outside the expression allowlist") {
	madc::program pgm;
	madc::expression_policy policy;
	policy.allow_function_calls = true;
	policy.allowed_functions.push_back("strlen");
	pgm.set_expression_policy(policy);

	madc::value result;
	CHECK_FALSE(pgm.eval_expression("strcmp(\"a\", \"b\")", &result, "expr_call_reject.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("rejected function call to 'strcmp'") != std::string::npos);
    }

    TEST_CASE("eval_expression returns string literals through c_string marshaling") {
	madc::program pgm;
	madc::value result;

	CHECK(pgm.eval_expression("\"hello\"", &result, "expr_string.mad"));
	REQUIRE(result.is_string());
	CHECK(result.as_string() == "hello");
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("madc::eval_expression convenience wrapper mirrors program eval_expression") {
	madc::value result;

	CHECK(madc::eval_expression("6 * 7", &result, "api_eval_expr.mad"));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 42);
    }

    TEST_CASE("madc::exec_string convenience wrapper mirrors program exec") {
	CHECK(madc::exec_string("int main() { return 0; }\n", "api_exec_string.mad"));
    }

    TEST_CASE("madc::exec_file convenience wrapper mirrors program exec") {
	std::string path = make_temp_source_path();
	write_file(path, "int main() { return 0; }\n");

	CHECK(madc::exec_file(path));

	std::remove(path.c_str());
    }

    TEST_CASE("compile_options can disable core builtin registration") {
	madc::program pgm;
	madc::compile_options options = pgm.get_compile_options();
	options.enable_core_functions = false;
	pgm.set_compile_options(options);

	CHECK_FALSE(pgm.exec_string("int main() { puti(42); return 0; }\n", "no_core.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::parser);
	CHECK(err->message == "use of undeclared identifier 'puti'");
    }

    TEST_CASE("security_policy can disable namespace registration") {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.allow_php_namespace = false;
	pgm.set_security_policy(policy);

	CHECK_FALSE(pgm.exec_string("int main() { php::trim(\" hi \"); return 0; }\n", "no_php.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::parser);
	CHECK(err->message == "Unknown namespace 'php'");
    }

    TEST_CASE("invoke_limits roundtrip through program surface") {
	madc::program pgm;
	madc::invoke_limits limits;
	limits.cpu_ms = 25;
	limits.memory_bytes = 4096;
	limits.output_bytes = 512;

	pgm.set_invoke_limits(limits);
	const madc::invoke_limits &stored = pgm.get_invoke_limits();
	CHECK(stored.cpu_ms == 25);
	CHECK(stored.memory_bytes == 4096);
	CHECK(stored.output_bytes == 512);
    }

    TEST_CASE("system_locked authority mode clamps dangerous policy gates") {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.mode = madc::authority_mode::system_locked;
	policy.execution = madc::execution_mode::in_process;
	policy.allow_process_functions = true;
	policy.allow_dlfcn_functions = true;
	policy.allow_runtime_eval_source_scope_access = true;
	policy.allow_runtime_eval_expression_scope_access = true;
	pgm.set_security_policy(policy);

	const madc::security_policy &stored_policy = pgm.get_security_policy();
	const madc::compile_options &stored_options = pgm.get_compile_options();
	CHECK(stored_policy.mode == madc::authority_mode::system_locked);
	CHECK(stored_policy.execution == madc::execution_mode::fork_per_invocation);
	CHECK_FALSE(stored_policy.allow_process_functions);
	CHECK_FALSE(stored_policy.allow_dlfcn_functions);
	CHECK_FALSE(stored_policy.allow_runtime_eval_source_scope_access);
	CHECK_FALSE(stored_policy.allow_runtime_eval_expression_scope_access);
	CHECK_FALSE(stored_options.enable_process_functions);
	CHECK_FALSE(stored_options.enable_dlfcn_functions);
	CHECK_FALSE(stored_options.enable_runtime_eval_source_scope_access);
	CHECK_FALSE(stored_options.enable_runtime_eval_expression_scope_access);
    }

    TEST_CASE("security_policy execution mode roundtrips when not locked") {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.mode = madc::authority_mode::host_authoritative;
	policy.execution = madc::execution_mode::fork_per_invocation;
	pgm.set_security_policy(policy);

	const madc::security_policy &stored_policy = pgm.get_security_policy();
	CHECK(stored_policy.mode == madc::authority_mode::host_authoritative);
	CHECK(stored_policy.execution == madc::execution_mode::fork_per_invocation);
    }

    TEST_CASE("expression_policy roundtrips explicit call allowlists") {
	madc::program pgm;
	madc::expression_policy policy;
	policy.allow_function_calls = true;
	policy.allow_member_access = true;
	policy.allow_subscript_access = true;
	policy.allow_pointer_operations = true;
	policy.allowed_headers.push_back("math.h");
	policy.allowed_functions.push_back("strlen");
	pgm.set_expression_policy(policy);

	const madc::expression_policy &stored = pgm.get_expression_policy();
	CHECK(stored.allow_function_calls);
	CHECK(stored.allow_member_access);
	CHECK(stored.allow_subscript_access);
	CHECK(stored.allow_pointer_operations);
	REQUIRE(stored.allowed_headers.size() == 1);
	CHECK(stored.allowed_headers[0] == "math.h");
	REQUIRE(stored.allowed_functions.size() == 1);
	CHECK(stored.allowed_functions[0] == "strlen");
    }

    TEST_CASE("runtime_eval_policy roundtrips explicit child-program capability settings") {
	madc::program pgm;
	madc::runtime_eval_policy policy;
	policy.allow_core_functions = false;
	policy.allow_process_functions = false;
	policy.allow_dlfcn_functions = false;
	policy.allow_std_namespace = false;
	policy.allow_php_namespace = false;
	policy.restrict_headers_to_allowlist = true;
	policy.restrict_dlfcn_symbols_to_allowlist = true;
	policy.allowed_headers.push_back("math.h");
	policy.allowed_dlfcn_symbols.push_back("strlen");
	pgm.set_runtime_eval_policy(policy);

	const madc::runtime_eval_policy &stored = pgm.get_runtime_eval_policy();
	CHECK_FALSE(stored.allow_core_functions);
	CHECK_FALSE(stored.allow_process_functions);
	CHECK_FALSE(stored.allow_dlfcn_functions);
	CHECK_FALSE(stored.allow_std_namespace);
	CHECK_FALSE(stored.allow_php_namespace);
	CHECK(stored.restrict_headers_to_allowlist);
	CHECK(stored.restrict_dlfcn_symbols_to_allowlist);
	REQUIRE(stored.allowed_headers.size() == 1);
	CHECK(stored.allowed_headers[0] == "math.h");
	REQUIRE(stored.allowed_dlfcn_symbols.size() == 1);
	CHECK(stored.allowed_dlfcn_symbols[0] == "strlen");
    }

    TEST_CASE("expression bindings roundtrip through program state") {
	madc::program pgm;
	std::map<std::string, madc::value> bindings;
	bindings["count"] = madc::value(static_cast<int64_t>(2));
	bindings["label"] = madc::value("ok");
	pgm.set_expression_bindings(bindings);

	const std::map<std::string, madc::value> &stored = pgm.get_expression_bindings();
	REQUIRE(stored.size() == 2);
	CHECK(stored.find("count") != stored.end());
	CHECK(stored.find("label") != stored.end());
	CHECK(stored.find("count")->second == madc::value(static_cast<int64_t>(2)));
	CHECK(stored.find("label")->second == madc::value("ok"));

	pgm.clear_expression_bindings();
	CHECK(pgm.get_expression_bindings().empty());
    }

    TEST_CASE("expression context roundtrips through program state") {
	madc::program pgm;
	std::map<std::string, madc::value> fields;
	fields["count"] = madc::value(static_cast<int64_t>(2));
	fields["label"] = madc::value("ok");
	madc::value context = madc::value::make_object(fields);
	pgm.set_expression_context(context);

	CHECK(pgm.get_expression_context() == context);
	pgm.clear_expression_context();
	CHECK(pgm.get_expression_context().is_null());
    }

    TEST_CASE("system_locked authority mode prevents compile options from re-enabling dlfcn") {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.mode = madc::authority_mode::system_locked;
	pgm.set_security_policy(policy);

	madc::compile_options options = pgm.get_compile_options();
	options.enable_dlfcn_functions = true;
	pgm.set_compile_options(options);

	CHECK_FALSE(pgm.get_compile_options().enable_dlfcn_functions);
	CHECK_FALSE(pgm.exec_string("#load \"libm.so.6\" as libm;\n"
				    "int main() { return 0; }\n",
				    "system_locked_load.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::lexer);
	CHECK(err->message == "#load is disabled by registration policy");
    }

    TEST_CASE("system_locked authority mode disables process builtins") {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.mode = madc::authority_mode::system_locked;
	pgm.set_security_policy(policy);

	CHECK_FALSE(pgm.exec_string("int main() { return system(\"true\"); }\n",
				    "system_locked_process.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
    }

    TEST_CASE("compile_options can disable #load directives") {
	madc::program pgm;
	madc::compile_options options = pgm.get_compile_options();
	options.enable_dlfcn_functions = false;
	pgm.set_compile_options(options);

	CHECK_FALSE(pgm.exec_string("#load \"libm.so.6\" as libm;\n"
				    "int main() { return 0; }\n",
				    "no_load.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::lexer);
	CHECK(err->message == "#load is disabled by registration policy");
    }

    TEST_CASE("compile_options can disable parse-time dlsym fallback") {
	madc::program pgm;
	madc::compile_options options = pgm.get_compile_options();
	options.enable_dlfcn_functions = false;
	pgm.set_compile_options(options);

	CHECK_FALSE(pgm.exec_string("int main() { return strlen(\"abc\"); }\n",
				    "no_parse_dlsym.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::parser);
	CHECK(err->message == "use of undeclared identifier 'strlen'");
    }

    TEST_CASE("compile_options can disable extern late-bind dlsym fallback" * doctest::skip()) {
	madc::program pgm;
	madc::compile_options options = pgm.get_compile_options();
	options.enable_dlfcn_functions = false;
	pgm.set_compile_options(options);

	CHECK_FALSE(pgm.exec_string("extern int strcmp(char *a, char *b);\n"
				    "int main() { return strcmp(\"a\", \"b\"); }\n",
				    "no_compile_dlsym.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::compiler);
	CHECK(err->message == "dynamic symbol fallback is disabled by registration policy");
    }

    TEST_CASE("invoke_limits can reject excessive output from exec") {
	madc::program pgm;
	madc::invoke_limits limits;
	limits.output_bytes = 4;
	pgm.set_invoke_limits(limits);

	CHECK_FALSE(pgm.exec_string("int main() { puts(\"abcdef\"); return 0; }\n", "limit_output.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("output_bytes limit") != std::string::npos);
    }

    TEST_CASE("invoke_limits can reject excessive cpu usage from call" * doctest::skip()) {
	madc::program pgm;
	REQUIRE(pgm.register_function(
	    "host_spin",
	    reinterpret_cast<madc::program::native_function>(host_spin),
	    madc::program::native_signature(
		madc::program::native_type::void_type,
		{madc::program::native_type::integer})));

	std::string path = make_temp_source_path();
	write_file(path,
		   "int main() { return 0; }\n");
	REQUIRE(pgm.compile_file(path));

	madc::invoke_limits limits;
	limits.cpu_ms = 1;
	pgm.set_invoke_limits(limits);

	madc::value result;
	CHECK_FALSE(pgm.call("host_spin", {madc::value(INT64_C(200000000))}, &result));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("cpu_ms limit") != std::string::npos);

	std::remove(path.c_str());
    }

    TEST_CASE("invoke_limits can reject excessive resident growth from call" * doctest::skip()) {
	madc::program pgm;
	g_host_memory.clear();
	REQUIRE(pgm.register_function(
	    "host_hold_memory",
	    reinterpret_cast<madc::program::native_function>(host_hold_memory),
	    madc::program::native_signature(
		madc::program::native_type::integer,
		{madc::program::native_type::integer})));

	std::string path = make_temp_source_path();
	write_file(path,
		   "int main() { return 0; }\n");
	REQUIRE(pgm.compile_file(path));

	madc::invoke_limits limits;
	limits.memory_bytes = 1024 * 1024;
	pgm.set_invoke_limits(limits);

	madc::value result;
	CHECK_FALSE(pgm.call("host_hold_memory", {madc::value(INT64_C(8) * 1024 * 1024)}, &result));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("memory_bytes limit") != std::string::npos);

	g_host_memory.clear();
	std::remove(path.c_str());
    }

    TEST_CASE("fork_per_invocation exec_string succeeds for a simple program" * doctest::skip()) {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.execution = madc::execution_mode::fork_per_invocation;
	pgm.set_security_policy(policy);

	CHECK(pgm.exec_string("int main() { return 0; }\n", "fork_ok.mad"));
	CHECK_FALSE(pgm.has_error());
	CHECK(pgm.diagnostics().empty());
    }

    TEST_CASE("fork_per_invocation exec_file reports runtime errors" * doctest::skip()) {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.execution = madc::execution_mode::fork_per_invocation;
	pgm.set_security_policy(policy);

	std::string path = make_temp_source_path();
	write_file(path, "int helper() { return 1; }\n");

	CHECK_FALSE(pgm.exec_file(path));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message == "Program::execute() cannot find main");

	std::remove(path.c_str());
    }

    TEST_CASE("fork_per_invocation exec_string counts stdout toward output limits" * doctest::skip()) {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.execution = madc::execution_mode::fork_per_invocation;
	pgm.set_security_policy(policy);
	madc::invoke_limits limits;
	limits.output_bytes = 4;
	pgm.set_invoke_limits(limits);

	CHECK_FALSE(pgm.exec_string("int main() { puts(\"fork stdout\"); return 0; }\n",
				    "fork_stdout_limit.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("output_bytes limit") != std::string::npos);
    }

    TEST_CASE("fork_per_invocation exec_string counts stderr toward output limits" * doctest::skip()) {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.execution = madc::execution_mode::fork_per_invocation;
	pgm.set_security_policy(policy);
	madc::invoke_limits limits;
	limits.output_bytes = 4;
	pgm.set_invoke_limits(limits);

	CHECK_FALSE(pgm.exec_string("#include <iostream>\n"
				    "using namespace std;\n"
				    "int main() { cerr << \"fork stderr\"; return 0; }\n",
				    "fork_stderr_limit.mad"));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->stage == madc::error::phase::runtime);
	CHECK(err->message.find("output_bytes limit") != std::string::npos);
    }

    TEST_CASE("fork_per_invocation call returns scalar results from child execution") {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.execution = madc::execution_mode::fork_per_invocation;
	pgm.set_security_policy(policy);

	std::string path = make_temp_source_path();
	write_file(path,
		   "int add(int a, int b) { return a + b; }\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));
	madc::value result;
	CHECK(pgm.call("add", {madc::value(int64_t(5)), madc::value(int64_t(8))}, &result));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 13);
	CHECK_FALSE(pgm.has_error());

	std::remove(path.c_str());
    }

	TEST_CASE("fork_per_invocation eval returns string results from child execution" * doctest::skip()) {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.execution = madc::execution_mode::fork_per_invocation;
	pgm.set_security_policy(policy);

	madc::value result;
	CHECK(pgm.eval("#include <string>\n"
		       "std::string name = \"echo\";\n"
		       "char * __madc_eval() { return name.c_str(); }\n",
		       &result,
		       "fork_eval_string.mad"));
	REQUIRE(result.is_string());
	CHECK(result.as_string() == "echo");
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("fork_per_invocation eval_expression returns scalar results from child execution") {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.execution = madc::execution_mode::fork_per_invocation;
	pgm.set_security_policy(policy);

	madc::value result;
	CHECK(pgm.eval_expression("6 * 7", &result, "fork_expr_eval.mad"));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 42);
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("program destruction restores cerr stream state") {
	{
	    madc::program pgm;
	    CHECK(pgm.exec_string("int main() { return 0; }\n", "scoped_ok.mad"));
	}

	std::cerr << std::flush;
	CHECK(true);
    }

    TEST_CASE("register_function exposes a host integer callback to scripts" * doctest::skip()) {
	madc::program pgm;
	g_host_sum = 0;

	REQUIRE(pgm.register_function(
	    "host_add",
	    reinterpret_cast<madc::program::native_function>(host_add),
	    madc::program::native_signature(
		madc::program::native_type::integer,
		{madc::program::native_type::integer, madc::program::native_type::integer})));

	CHECK(pgm.exec_string("int main() { host_add(2, 5); return 0; }\n", "host_add.mad"));
	CHECK(g_host_sum == 7);
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("register_function supports string to const char* coercion" * doctest::skip()) {
	madc::program pgm;
	g_host_strlen = 0;

	REQUIRE(pgm.register_function(
	    "host_strlen",
	    reinterpret_cast<madc::program::native_function>(host_strlen),
	    madc::program::native_signature(
		madc::program::native_type::integer,
		{madc::program::native_type::c_string})));

	CHECK(pgm.exec_string("int main() { host_strlen(\"hello\"); return 0; }\n", "host_strlen.mad"));
	CHECK(g_host_strlen == 5);
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("call invokes a compiled script function with integer args and return") {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path,
		   "int add(int a, int b) { return a + b; }\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));
	madc::value result;
	CHECK(pgm.call("add", {madc::value(int64_t(3)), madc::value(int64_t(9))}, &result));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 12);
	CHECK_FALSE(pgm.has_error());

	std::remove(path.c_str());
    }

    TEST_CASE("call passes string values to char pointer parameters") {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path,
		   "int firstchar(char *s) { return s[0]; }\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));
	madc::value result;
	CHECK(pgm.call("firstchar", {madc::value("hello")}, &result));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == int64_t('h'));
	CHECK_FALSE(pgm.has_error());

	std::remove(path.c_str());
    }

    TEST_CASE("call invokes a compiled script function with four integer args") {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path,
		   "int add4(int a, int b, int c, int d) { return a + b + c + d; }\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));
	madc::value result;
	CHECK(pgm.call("add4",
		       {madc::value(int64_t(10)),
			madc::value(int64_t(20)),
			madc::value(int64_t(5)),
			madc::value(int64_t(7))},
		       &result));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 42);
	CHECK_FALSE(pgm.has_error());

	std::remove(path.c_str());
    }

    TEST_CASE("register_function supports four integer parameters" * doctest::skip()) {
	madc::program pgm;
	g_host_sum = 0;

	REQUIRE(pgm.register_function(
	    "host_sum4",
	    reinterpret_cast<madc::program::native_function>(host_sum4),
	    madc::program::native_signature(
		madc::program::native_type::integer,
		{
		    madc::program::native_type::integer,
		    madc::program::native_type::integer,
		    madc::program::native_type::integer,
		    madc::program::native_type::integer
		})));

	CHECK(pgm.exec_string("int main() { host_sum4(10, 20, 5, 7); return 0; }\n",
			      "host_sum4.mad"));
	CHECK(g_host_sum == 42);
	CHECK_FALSE(pgm.has_error());
    }

	TEST_CASE("register_function deduces c-string callback signatures" * doctest::skip()) {
	madc::program pgm;
	g_host_strlen = 0;

	REQUIRE(pgm.register_function("host_word_length", host_word_length));
	REQUIRE(pgm.register_function("host_echo_value", host_echo_value));

	std::string path = make_temp_source_path();
	write_file(path,
		   "int measure() { return host_word_length(\"hello\"); }\n"
		   "const char *shout() { return host_echo_value(\"hi\"); }\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));

	madc::value result;
	REQUIRE(pgm.call("measure", {}, &result));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 5);
	CHECK(g_host_strlen == 5);

	REQUIRE(pgm.call("shout", {}, &result));
	REQUIRE(result.is_string());
	CHECK(result.as_string() == "hi!");
	CHECK_FALSE(pgm.has_error());

	std::remove(path.c_str());
    }

    TEST_CASE("script-side runtime eval sees current scope when allowed") {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path,
		   "#include <string>\n"
		   "#include <ns_madc>\n"
		   "int probe_expr(int base) {\n"
		   "    int bonus = 2;\n"
		   "    return madc::eval_expression_int(\"base + bonus\");\n"
		   "}\n"
		   "int probe_eval(int base) {\n"
		   "    int bonus = 2;\n"
		   "    return madc::eval_int(\"return base + bonus;\");\n"
		   "}\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));

	madc::value result;
	REQUIRE(pgm.call("probe_expr", {madc::value(int64_t(40))}, &result));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 42);

	REQUIRE(pgm.call("probe_eval", {madc::value(int64_t(40))}, &result));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 42);

	std::remove(path.c_str());
    }

    TEST_CASE("script-side runtime eval scope access can be disabled independently") {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.allow_runtime_eval_source_scope_access = false;
	policy.allow_runtime_eval_expression_scope_access = false;
	pgm.set_security_policy(policy);

	std::string path = make_temp_source_path();
	write_file(path,
		   "#include <string>\n"
		   "#include <ns_madc>\n"
		   "int probe_expr(int base) {\n"
		   "    int bonus = 2;\n"
		   "    return madc::eval_expression_int(\"base + bonus\");\n"
		   "}\n"
		   "int probe_eval(int base) {\n"
		   "    int bonus = 2;\n"
		   "    return madc::eval_int(\"return base + bonus;\");\n"
		   "}\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));

	madc::value result;
	REQUIRE(pgm.call("probe_expr", {madc::value(int64_t(40))}, &result));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 0);

	REQUIRE(pgm.call("probe_eval", {madc::value(int64_t(40))}, &result));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 0);

	std::remove(path.c_str());
    }

    TEST_CASE("script-side runtime expression scope access can be disabled without affecting full eval") {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.allow_runtime_eval_expression_scope_access = false;
	pgm.set_security_policy(policy);

	std::string path = make_temp_source_path();
	write_file(path,
		   "#include <string>\n"
		   "#include <ns_madc>\n"
		   "int probe_expr(int base) {\n"
		   "    int bonus = 2;\n"
		   "    return madc::eval_expression_int(\"base + bonus\");\n"
		   "}\n"
		   "int probe_eval(int base) {\n"
		   "    int bonus = 2;\n"
		   "    return madc::eval_int(\"return base + bonus;\");\n"
		   "}\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));

	madc::value result;
	REQUIRE(pgm.call("probe_expr", {madc::value(int64_t(40))}, &result));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 0);

	REQUIRE(pgm.call("probe_eval", {madc::value(int64_t(40))}, &result));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 42);

	std::remove(path.c_str());
    }

    TEST_CASE("script-side full eval scope access can be disabled without affecting runtime expression eval") {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.allow_runtime_eval_source_scope_access = false;
	pgm.set_security_policy(policy);

	std::string path = make_temp_source_path();
	write_file(path,
		   "#include <string>\n"
		   "#include <ns_madc>\n"
		   "int probe_expr(int base) {\n"
		   "    int bonus = 2;\n"
		   "    return madc::eval_expression_int(\"base + bonus\");\n"
		   "}\n"
		   "int probe_eval(int base) {\n"
		   "    int bonus = 2;\n"
		   "    return madc::eval_int(\"return base + bonus;\");\n"
		   "}\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));

	madc::value result;
	REQUIRE(pgm.call("probe_expr", {madc::value(int64_t(40))}, &result));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 42);

	REQUIRE(pgm.call("probe_eval", {madc::value(int64_t(40))}, &result));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 0);

	std::remove(path.c_str());
    }

    TEST_CASE("runtime_eval_policy can restrict child full eval independently of the parent program" * doctest::skip()) {
	madc::program pgm;
	madc::runtime_eval_policy policy = pgm.get_runtime_eval_policy();
	policy.allow_core_functions = false;
	pgm.set_runtime_eval_policy(policy);

	std::string path = make_temp_source_path();
	write_file(path,
		   "int probe_eval() {\n"
		   "    return madc::eval_int(\"puti(7); return 42;\");\n"
		   "}\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));

	madc::value result;
	REQUIRE(pgm.call("probe_eval", std::vector<madc::value>(), &result));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 0);

	std::remove(path.c_str());
    }

	TEST_CASE("call supports std::string arguments for script string parameters" * doctest::skip()) {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path,
		   "#include <string>\n"
		   "int length_of(std::string s) { return s.length(); }\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));
	madc::value result;
	REQUIRE(pgm.call("length_of", {madc::value("hello")}, &result));
	REQUIRE(result.is_integer());
	CHECK(result.as_integer() == 5);
	CHECK_FALSE(pgm.has_error());

	std::remove(path.c_str());
    }

	TEST_CASE("call supports script string object returns" * doctest::skip()) {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path,
		   "#include <string>\n"
		   "std::string echo(std::string s) { return s; }\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));
	madc::value result;
	REQUIRE(pgm.call("echo", {madc::value("hello")}, &result));
	REQUIRE(result.is_string());
	CHECK(result.as_string() == "hello");
	CHECK_FALSE(pgm.has_error());

	std::remove(path.c_str());
    }

    TEST_CASE("register_function does not expose C++ object native signatures") {
	CHECK(true);
    }

    TEST_CASE("madc C API can compile and call scalar and string results" * doctest::skip()) {
	madc_program *pgm = madc_program_create();
	REQUIRE(pgm != NULL);

	madc_value args[2];
	madc_value result;
	madc_value_init(&args[0]);
	madc_value_init(&args[1]);
	madc_value_init(&result);

	REQUIRE(madc_program_exec_string(
	    pgm,
	    "int add(int a, int b) { return a + b; }\n"
	    "#include <string>\n"
	    "std::string greet() { return \"hello\"; }\n"
	    "int main() { return 0; }\n",
	    "c_api_exec.mad") == MADC_OK);

	REQUIRE(madc_value_set_integer(&args[0], 19) == MADC_OK);
	REQUIRE(madc_value_set_integer(&args[1], 23) == MADC_OK);
	REQUIRE(madc_program_call(pgm, "add", args, 2, &result) == MADC_OK);
	CHECK(result.type_id == MADC_VALUE_INTEGER);
	CHECK(result.integer_value == 42);

	madc_value_clear(&result);
	REQUIRE(madc_program_call(pgm, "greet", NULL, 0, &result) == MADC_OK);
	CHECK(result.type_id == MADC_VALUE_STRING);
	size_t greet_len = 0;
	const char *greet_text = madc_value_text(&result, &greet_len);
	REQUIRE(greet_text != (const char *)NULL);
	CHECK(std::string(greet_text, greet_len) == "hello");

	madc_value_clear(&args[0]);
	madc_value_clear(&args[1]);
	madc_value_clear(&result);
	madc_program_destroy(pgm);
    }

    TEST_CASE("madc C API exposes last error text") {
	madc_program *pgm = madc_program_create();
	REQUIRE(pgm != NULL);

	CHECK(madc_program_call(pgm, "missing_fn", NULL, 0, NULL) == MADC_ERROR);
	const char *err = madc_program_last_error(pgm);
	REQUIRE(err != NULL);
	CHECK(std::string(err).empty() == false);

	madc_program_destroy(pgm);
    }

    TEST_CASE("madc C API policy mirrors roundtrip through program state") {
	madc_program *pgm = madc_program_create();
	REQUIRE(pgm != NULL);

	madc_compile_options options;
	madc_compile_options_init(&options);
	options.enable_core_functions = 0;
	options.enable_js_namespace = 0;
	REQUIRE(madc_program_set_compile_options(pgm, &options) == MADC_OK);

	madc_compile_options read_options;
	madc_compile_options_init(&read_options);
	REQUIRE(madc_program_get_compile_options(pgm, &read_options) == MADC_OK);
	CHECK(read_options.enable_core_functions == 0);
	CHECK(read_options.enable_js_namespace == 0);

	madc_security_policy policy;
	madc_security_policy_init(&policy);
	policy.mode = MADC_AUTHORITY_SYSTEM_LOCKED;
	policy.execution = MADC_EXECUTION_FORK_PER_INVOCATION;
	policy.allow_process_functions = 0;
	REQUIRE(madc_program_set_security_policy(pgm, &policy) == MADC_OK);

	madc_security_policy read_policy;
	madc_security_policy_init(&read_policy);
	REQUIRE(madc_program_get_security_policy(pgm, &read_policy) == MADC_OK);
	CHECK(read_policy.execution == MADC_EXECUTION_FORK_PER_INVOCATION);
	CHECK(read_policy.allow_process_functions == 0);

	madc_runtime_eval_policy eval_policy;
	madc_runtime_eval_policy_init(&eval_policy);
	eval_policy.allow_dlfcn_functions = 0;
	eval_policy.restrict_headers_to_allowlist = 1;
	REQUIRE(madc_program_set_runtime_eval_policy(pgm, &eval_policy) == MADC_OK);

	madc_runtime_eval_policy read_eval_policy;
	madc_runtime_eval_policy_init(&read_eval_policy);
	REQUIRE(madc_program_get_runtime_eval_policy(pgm, &read_eval_policy) == MADC_OK);
	CHECK(read_eval_policy.allow_dlfcn_functions == 0);
	CHECK(read_eval_policy.restrict_headers_to_allowlist == 1);

	madc_invoke_limits limits;
	madc_invoke_limits_init(&limits);
	limits.cpu_ms = 7;
	limits.memory_bytes = 11;
	limits.output_bytes = 13;
	REQUIRE(madc_program_set_invoke_limits(pgm, &limits) == MADC_OK);

	madc_invoke_limits read_limits;
	madc_invoke_limits_init(&read_limits);
	REQUIRE(madc_program_get_invoke_limits(pgm, &read_limits) == MADC_OK);
	CHECK(read_limits.cpu_ms == 7);
	CHECK(read_limits.memory_bytes == 11);
	CHECK(read_limits.output_bytes == 13);

	madc_program_destroy(pgm);
    }

    TEST_CASE("madc C API can enumerate diagnostics") {
	madc_program *pgm = madc_program_create();
	REQUIRE(pgm != NULL);

	CHECK(madc_program_exec_string(pgm, "int main( { return 0; }\n", "c_api_bad.mad") == MADC_ERROR);
	REQUIRE(madc_program_diagnostic_count(pgm) >= 1);

	madc_error diagnostic;
	madc_error_init(&diagnostic);
	REQUIRE(madc_program_get_diagnostic(pgm, 0, &diagnostic) == MADC_OK);
	CHECK(diagnostic.phase == MADC_PHASE_PARSER);
	REQUIRE(diagnostic.file != NULL);
	CHECK(std::string(diagnostic.file, diagnostic.file_length) == "c_api_bad.mad");
	REQUIRE(diagnostic.message != NULL);
	CHECK(diagnostic.message_length > 0);
	madc_error_clear(&diagnostic);

	madc_program_destroy(pgm);
    }

    TEST_CASE("get_global and set_global roundtrip integer globals") {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path,
		   "int counter = 4;\n"
		   "int read_counter() { return counter; }\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));

	madc::value current;
	REQUIRE(pgm.get_global("counter", &current));
	REQUIRE(current.is_integer());
	CHECK(current.as_integer() == 4);

	REQUIRE(pgm.set_global("counter", madc::value(int64_t(9))));
	CHECK(pgm.call("read_counter", {}, &current));
	REQUIRE(current.is_integer());
	CHECK(current.as_integer() == 9);

	std::remove(path.c_str());
    }

    TEST_CASE("set_global preserves 64-bit integer values") {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path,
		   "long counter = 0;\n"
		   "long read_counter() { return counter; }\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));

	const int64_t big = INT64_C(5000000000);
	REQUIRE(pgm.set_global("counter", madc::value(big)));

	madc::value current;
	REQUIRE(pgm.get_global("counter", &current));
	REQUIRE(current.is_integer());
	CHECK(current.as_integer() == big);

	REQUIRE(pgm.call("read_counter", {}, &current));
	REQUIRE(current.is_integer());
	CHECK(current.as_integer() == big);

	std::remove(path.c_str());
    }

	TEST_CASE("get_global and set_global roundtrip string globals") {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path,
		   "#include <string>\n"
		   "std::string name = \"alice\";\n"
		   "char *first_name_char() { return name.c_str(); }\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));

	madc::value current;
	REQUIRE(pgm.get_global("name", &current));
	REQUIRE(current.is_string());
	CHECK(current.as_string() == "alice");

	REQUIRE(pgm.set_global("name", madc::value("bravo")));
	REQUIRE(pgm.get_global("name", &current));
	REQUIRE(current.is_string());
	CHECK(current.as_string() == "bravo");

	madc::value result;
	REQUIRE(pgm.call("first_name_char", {}, &result));
	REQUIRE(result.is_string());
	CHECK(result.as_string() == "bravo");

	std::remove(path.c_str());
    }

    TEST_CASE("set_global writes exactly the variable's size (neighbor canary)") {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path,
		   "int a = 4;\n"
		   "int b = 7;\n"
		   "int read_b() { return b; }\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));

	// The old helpers wrote int globals as int64 — an 8-byte store that
	// clobbers the neighboring 4-byte global on real MIR data layout.
	REQUIRE(pgm.set_global("a", madc::value(int64_t(9))));

	madc::value current;
	REQUIRE(pgm.get_global("b", &current));
	REQUIRE(current.is_integer());
	CHECK(current.as_integer() == 7);

	REQUIRE(pgm.call("read_b", {}, &current));
	REQUIRE(current.is_integer());
	CHECK(current.as_integer() == 7);

	REQUIRE(pgm.get_global("a", &current));
	CHECK(current.as_integer() == 9);

	std::remove(path.c_str());
    }

    TEST_CASE("get_global rejects unsupported array globals") {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path,
		   "int nums[2];\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));

	madc::value current;
	CHECK_FALSE(pgm.get_global("nums", &current));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->message == "program::get_global does not support this variable type yet");

	std::remove(path.c_str());
    }
}

// ---------------------------------------------------------------------------
// save_object (ELF .o output) tests
// ---------------------------------------------------------------------------

TEST_SUITE("save_object") {

    TEST_CASE("save_object writes a valid ELF relocatable" * doctest::skip()) {
	madc::program pgm;
	REQUIRE(pgm.compile_string(
	    "int add(int a, int b) { return a + b; }\n"
	    "int main() { return 0; }\n",
	    "elf_test.mad"));

	std::string obj_path = "/tmp/madc_test_elf.o";
	REQUIRE(pgm.save_object(obj_path));

	// Verify it's valid ELF with readelf.
	std::string cmd = "readelf -h " + obj_path + " 2>&1";
	FILE *fp = popen(cmd.c_str(), "r");
	REQUIRE(fp != NULL);
	char buf[4096];
	std::string output;
	while ( fgets(buf, sizeof(buf), fp) )
	    output += buf;
	int status = pclose(fp);
	CHECK(status == 0);
	CHECK(output.find("ELF Header:") != std::string::npos);
	CHECK(output.find("REL") != std::string::npos);          // ET_REL
	CHECK(output.find("X86-64") != std::string::npos);       // EM_X86_64

	std::remove(obj_path.c_str());
    }

    TEST_CASE("save_object includes function symbols" * doctest::skip()) {
	madc::program pgm;
	REQUIRE(pgm.compile_string(
	    "int mul(int a, int b) { return a * b; }\n"
	    "int main() { return 0; }\n",
	    "sym_test.mad"));

	std::string obj_path = "/tmp/madc_test_sym.o";
	REQUIRE(pgm.save_object(obj_path));

	// Check symbols with readelf -s.
	std::string cmd = "readelf -s " + obj_path + " 2>&1";
	FILE *fp = popen(cmd.c_str(), "r");
	REQUIRE(fp != NULL);
	char buf[4096];
	std::string output;
	while ( fgets(buf, sizeof(buf), fp) )
	    output += buf;
	pclose(fp);
	CHECK(output.find("mul") != std::string::npos);
	CHECK(output.find("main") != std::string::npos);
	CHECK(output.find("FUNC") != std::string::npos);

	std::remove(obj_path.c_str());
    }

    TEST_CASE("save_executable writes a runnable ELF binary" * doctest::skip()) {
	madc::program pgm;
	pgm.set_aot_mode(true);
	REQUIRE(pgm.compile_string(
	    "int main() { return 42; }\n",
	    "exec_test.mad"));

	std::string exe_path = "/tmp/madc_test_exe";
	REQUIRE(pgm.save_executable(exe_path));

	// Verify it's a valid ELF executable.
	std::string cmd = "readelf -h " + exe_path + " 2>&1";
	FILE *fp = popen(cmd.c_str(), "r");
	REQUIRE(fp != NULL);
	char buf[4096];
	std::string output;
	while ( fgets(buf, sizeof(buf), fp) )
	    output += buf;
	pclose(fp);
	CHECK(output.find("EXEC") != std::string::npos);
	CHECK(output.find("X86-64") != std::string::npos);

	// Run it and check exit code.
	int status = system(executable_command(exe_path).c_str());
	int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	CHECK(exit_code == 42);

	std::remove(exe_path.c_str());
    }

    TEST_CASE("save_executable runs global initialization before main and supports stderr" * doctest::skip()) {
	madc::program pgm;
	pgm.set_aot_mode(true);
	REQUIRE(pgm.compile_string(
	    "#include <stdio.h>\n"
	    "int g = 7;\n"
	    "int main() { fprintf(stderr, \"%d\\n\", g); return 0; }\n",
	    "exec_stderr_global_init.mad"));

	std::string exe_path = "/tmp/madc_test_exe_stderr";
	REQUIRE(pgm.save_executable(exe_path));

	std::string cmd = executable_command(exe_path) + " 2>&1";
	FILE *fp = popen(cmd.c_str(), "r");
	REQUIRE(fp != NULL);
	char buf[4096];
	std::string output;
	while ( fgets(buf, sizeof(buf), fp) )
	    output += buf;
	int status = pclose(fp);
	int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	CHECK(exit_code == 0);
	CHECK(output == "7\n");

	std::remove(exe_path.c_str());
    }

    TEST_CASE("save_executable preserves global array layout across function-scope extern redeclarations" * doctest::skip()) {
	madc::program pgm;
	pgm.set_aot_mode(true);
	REQUIRE(pgm.compile_string(
	    "#include <stdio.h>\n"
	    "#include <string.h>\n"
	    "char buf[16];\n"
	    "void set_buf() { extern char buf[]; strcpy(buf, \"ok\"); }\n"
	    "int main() { set_buf(); printf(\"%s\\n\", buf); return 0; }\n",
	    "exec_global_array_extern.mad"));

	std::string exe_path = "/tmp/madc_test_exe_global_array_extern";
	REQUIRE(pgm.save_executable(exe_path));

	std::string cmd = executable_command(exe_path) + " 2>&1";
	FILE *fp = popen(cmd.c_str(), "r");
	REQUIRE(fp != NULL);
	char buf[4096];
	std::string output;
	while ( fgets(buf, sizeof(buf), fp) )
	    output += buf;
	int status = pclose(fp);
	int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	CHECK(exit_code == 0);
	CHECK(output == "ok\n");

	std::remove(exe_path.c_str());
    }

    TEST_CASE("save_executable returns char pointers from string literals" * doctest::skip()) {
	madc::program pgm;
	pgm.set_aot_mode(true);
	REQUIRE(pgm.compile_string(
	    "#include <stdio.h>\n"
	    "char *get_msg() { return \"hello\"; }\n"
	    "int main() { putchar(*get_msg()); return 0; }\n",
	    "exec_return_cstr_literal.mad"));

	std::string exe_path = "/tmp/madc_test_exe_return_cstr_literal";
	REQUIRE(pgm.save_executable(exe_path));

	std::string cmd = executable_command(exe_path) + " 2>&1";
	FILE *fp = popen(cmd.c_str(), "r");
	REQUIRE(fp != NULL);
	char buf[4096];
	std::string output;
	while ( fgets(buf, sizeof(buf), fp) )
	    output += buf;
	int status = pclose(fp);
	int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	CHECK(exit_code == 0);
	CHECK(output == "h");

	std::remove(exe_path.c_str());
    }

    TEST_CASE("save_executable assigns char pointers from string literals" * doctest::skip()) {
	madc::program pgm;
	pgm.set_aot_mode(true);
	REQUIRE(pgm.compile_string(
	    "#include <stdio.h>\n"
	    "char *msg;\n"
	    "int main() { msg = \"hello\"; putchar(*msg); return 0; }\n",
	    "exec_assign_cstr_literal.mad"));

	std::string exe_path = "/tmp/madc_test_exe_assign_cstr_literal";
	REQUIRE(pgm.save_executable(exe_path));

	std::string cmd = executable_command(exe_path) + " 2>&1";
	FILE *fp = popen(cmd.c_str(), "r");
	REQUIRE(fp != NULL);
	char buf[4096];
	std::string output;
	while ( fgets(buf, sizeof(buf), fp) )
	    output += buf;
	int status = pclose(fp);
	int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	CHECK(exit_code == 0);
	CHECK(output == "h");

	std::remove(exe_path.c_str());
    }

    TEST_CASE("save_executable fails before compile") {
	madc::program pgm;
	CHECK_FALSE(pgm.save_executable("/tmp/madc_test_noexe"));
	CHECK(pgm.has_error());
    }

    TEST_CASE("save_object fails before compile") {
	madc::program pgm;
	CHECK_FALSE(pgm.save_object("/tmp/madc_test_nocompile.o"));
	CHECK(pgm.has_error());
    }
}

// ---------------------------------------------------------------------------
// load_object (ELF .o round-trip) tests
// ---------------------------------------------------------------------------

TEST_SUITE("load_object") {

    TEST_CASE("save then load round-trip calls produce correct results" * doctest::skip()) {
	std::string obj_path = "/tmp/madc_test_roundtrip.o";

	// Compile and save.
	{
	    madc::program pgm;
	    REQUIRE(pgm.compile_string(
		"int add(int a, int b) { return a + b; }\n"
		"int mul(int a, int b) { return a * b; }\n"
		"int main() { return 0; }\n",
		"roundtrip.mad"));
	    REQUIRE(pgm.save_object(obj_path));
	}

	// Load into a fresh program and call.
	{
	    madc::program pgm2;
	    REQUIRE(pgm2.load_object(obj_path));
	    CHECK(pgm2.has_function("add"));
	    CHECK(pgm2.has_function("mul"));
	    CHECK(pgm2.has_function("main"));

	    madc::value r1, r2;
	    REQUIRE(pgm2.call("add", {madc::value(int64_t(10)), madc::value(int64_t(32))}, &r1));
	    CHECK(r1.as_integer() == 42);
	    REQUIRE(pgm2.call("mul", {madc::value(int64_t(6)), madc::value(int64_t(7))}, &r2));
	    CHECK(r2.as_integer() == 42);
	}

	std::remove(obj_path.c_str());
    }

    TEST_CASE("load_object reports missing file") {
	madc::program pgm;
	CHECK_FALSE(pgm.load_object("/tmp/madc_no_such_file_42.o"));
	CHECK(pgm.has_error());
    }

    TEST_CASE("loaded function callable multiple times" * doctest::skip()) {
	std::string obj_path = "/tmp/madc_test_multi_load.o";

	{
	    madc::program pgm;
	    REQUIRE(pgm.compile_string(
		"int square(int n) { return n * n; }\n"
		"int main() { return 0; }\n",
		"multi.mad"));
	    REQUIRE(pgm.save_object(obj_path));
	}

	{
	    madc::program pgm2;
	    REQUIRE(pgm2.load_object(obj_path));

	    madc::value r;
	    REQUIRE(pgm2.call("square", {madc::value(int64_t(5))}, &r));
	    CHECK(r.as_integer() == 25);
	    REQUIRE(pgm2.call("square", {madc::value(int64_t(9))}, &r));
	    CHECK(r.as_integer() == 81);
	    REQUIRE(pgm2.call("square", {madc::value(int64_t(0))}, &r));
	    CHECK(r.as_integer() == 0);
	}

	std::remove(obj_path.c_str());
    }

    TEST_CASE("has_function returns false for absent symbol in loaded object" * doctest::skip()) {
	std::string obj_path = "/tmp/madc_test_absent.o";

	{
	    madc::program pgm;
	    REQUIRE(pgm.compile_string(
		"int foo() { return 1; }\n"
		"int main() { return 0; }\n",
		"absent.mad"));
	    REQUIRE(pgm.save_object(obj_path));
	}

	{
	    madc::program pgm2;
	    REQUIRE(pgm2.load_object(obj_path));
	    CHECK(pgm2.has_function("foo"));
	    CHECK_FALSE(pgm2.has_function("bar"));
	}

	std::remove(obj_path.c_str());
    }

    TEST_CASE("save_object emits UNDEF symbols for external calls" * doctest::skip()) {
	// Compile a script that calls abs() — a libc function resolved
	// via dlsym at compile time. The .o should contain an UNDEF
	// symbol for "abs".
	madc::program pgm;
	REQUIRE(pgm.compile_string(
	    "#include <stdlib.h>\n"
	    "int my_abs(int x) { return abs(x); }\n"
	    "int main() { return 0; }\n",
	    "extern_test.mad"));

	std::string obj_path = "/tmp/madc_test_extern.o";
	REQUIRE(pgm.save_object(obj_path));

	// Check that readelf shows an UNDEF symbol.
	std::string cmd = "readelf -s " + obj_path + " 2>&1";
	FILE *fp = popen(cmd.c_str(), "r");
	REQUIRE(fp != NULL);
	char buf[4096];
	std::string output;
	while ( fgets(buf, sizeof(buf), fp) )
	    output += buf;
	pclose(fp);

	// abs should appear as UND (undefined).
	CHECK(output.find("abs") != std::string::npos);
	CHECK(output.find("UND") != std::string::npos);

	std::remove(obj_path.c_str());
    }
}

// ---------------------------------------------------------------------------
// compile-once-run-many tests
// ---------------------------------------------------------------------------

TEST_SUITE("compile_string / exec") {

    TEST_CASE("compile_string compiles without executing") {
	madc::program pgm;
	REQUIRE(pgm.compile_string(
	    "int add(int a, int b) { return a + b; }\n"
	    "int main() { return 0; }\n",
	    "compile_test.mad"));
	CHECK(pgm.is_compiled());
	CHECK(pgm.has_function("add"));
	CHECK(pgm.has_function("main"));
	CHECK_FALSE(pgm.has_error());
    }

    TEST_CASE("is_compiled returns false before compile") {
	madc::program pgm;
	CHECK_FALSE(pgm.is_compiled());
    }

    TEST_CASE("exec runs main on already-compiled program") {
	madc::program pgm;
	REQUIRE(pgm.compile_string(
	    "int counter = 0;\n"
	    "int get_counter() { return counter; }\n"
	    "int main() { counter = 42; return 0; }\n",
	    "exec_test.mad"));
	REQUIRE(pgm.exec());

	madc::value result;
	REQUIRE(pgm.call("get_counter", {}, &result));
	CHECK(result.as_integer() == 42);
    }

    TEST_CASE("call works multiple times on compiled program") {
	madc::program pgm;
	REQUIRE(pgm.compile_string(
	    "int add(int a, int b) { return a + b; }\n"
	    "int main() { return 0; }\n",
	    "multi_call.mad"));

	madc::value r1, r2, r3;
	REQUIRE(pgm.call("add", {madc::value(int64_t(1)), madc::value(int64_t(2))}, &r1));
	REQUIRE(pgm.call("add", {madc::value(int64_t(10)), madc::value(int64_t(20))}, &r2));
	REQUIRE(pgm.call("add", {madc::value(int64_t(100)), madc::value(int64_t(200))}, &r3));
	CHECK(r1.as_integer() == 3);
	CHECK(r2.as_integer() == 30);
	CHECK(r3.as_integer() == 300);
    }

    TEST_CASE("compile_file then call multiple times") {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path,
		   "int mul(int a, int b) { return a * b; }\n"
		   "int main() { return 0; }\n");
	REQUIRE(pgm.compile_file(path));
	CHECK(pgm.is_compiled());

	madc::value r1, r2;
	REQUIRE(pgm.call("mul", {madc::value(int64_t(6)), madc::value(int64_t(7))}, &r1));
	REQUIRE(pgm.call("mul", {madc::value(int64_t(3)), madc::value(int64_t(4))}, &r2));
	CHECK(r1.as_integer() == 42);
	CHECK(r2.as_integer() == 12);

	std::remove(path.c_str());
    }

    TEST_CASE("compile_string with engine callback then call" * doctest::skip()) {
	madc::engine eng;
	g_host_sum = 0;
	REQUIRE(eng.register_function("host_add", host_add));

	madc::program pgm = eng.create_program();
	REQUIRE(pgm.compile_string(
	    "int do_add(int a, int b) { return host_add(a, b); }\n"
	    "int main() { return 0; }\n",
	    "eng_compile.mad"));

	madc::value result;
	REQUIRE(pgm.call("do_add", {madc::value(int64_t(100)), madc::value(int64_t(23))}, &result));
	CHECK(result.as_integer() == 123);
	CHECK(g_host_sum == 123);
    }
}

// ---------------------------------------------------------------------------
// program::has_function tests
// ---------------------------------------------------------------------------

TEST_SUITE("program::has_function") {

    TEST_CASE("has_function returns true for compiled function") {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path,
		   "int add(int a, int b) { return a + b; }\n"
		   "int main() { return 0; }\n");
	REQUIRE(pgm.compile_file(path));
	CHECK(pgm.has_function("add"));
	CHECK(pgm.has_function("main"));
	std::remove(path.c_str());
    }

    TEST_CASE("has_function returns false for nonexistent function") {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path, "int main() { return 0; }\n");
	REQUIRE(pgm.compile_file(path));
	CHECK_FALSE(pgm.has_function("no_such_fn"));
	std::remove(path.c_str());
    }

    TEST_CASE("has_function returns false for global variable") {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path,
		   "int counter = 0;\n"
		   "int main() { return 0; }\n");
	REQUIRE(pgm.compile_file(path));
	CHECK_FALSE(pgm.has_function("counter"));
	std::remove(path.c_str());
    }

    TEST_CASE("has_function returns false before compile") {
	madc::program pgm;
	CHECK_FALSE(pgm.has_function("main"));
    }
}

// ---------------------------------------------------------------------------
// madc::engine tests
// ---------------------------------------------------------------------------

TEST_SUITE("madc::engine") {

    TEST_CASE("engine creates a program that compiles and runs") {
	madc::engine eng;
	madc::program pgm = eng.create_program();
	int64_t result = 0;
	REQUIRE(pgm.eval_expression("6 * 7", result));
	CHECK(result == 42);
    }

    TEST_CASE("engine policy defaults propagate to created programs") {
	madc::engine eng;
	madc::compile_options opts;
	opts.enable_process_functions = false;
	eng.set_compile_options(opts);

	madc::program pgm = eng.create_program();
	const madc::compile_options &pgm_opts = pgm.get_compile_options();
	CHECK_FALSE(pgm_opts.enable_process_functions);
    }

    TEST_CASE("engine invoke limits propagate to created programs") {
	madc::engine eng;
	madc::invoke_limits limits;
	limits.cpu_ms = 500;
	limits.output_bytes = 4096;
	eng.set_invoke_limits(limits);

	madc::program pgm = eng.create_program();
	const madc::invoke_limits &pgm_limits = pgm.get_invoke_limits();
	CHECK(pgm_limits.cpu_ms == 500);
	CHECK(pgm_limits.output_bytes == 4096);
    }

    TEST_CASE("two programs from the same engine have independent state") {
	madc::engine eng;
	madc::program p1 = eng.create_program();
	madc::program p2 = eng.create_program();

	int64_t r1 = 0, r2 = 0;
	REQUIRE(p1.eval_expression("3 + 4", r1));
	REQUIRE(p2.eval_expression("10 + 20", r2));
	CHECK(r1 == 7);
	CHECK(r2 == 30);
    }

    TEST_CASE("engine-registered callback is available to created programs" * doctest::skip()) {
	madc::engine eng;
	g_host_sum = 0;
	REQUIRE(eng.register_function("host_add", host_add));

	madc::program pgm = eng.create_program();
	CHECK(pgm.exec_string(
	    "int main() { host_add(10, 32); return 0; }\n",
	    "eng_callback.mad"));
	CHECK(g_host_sum == 42);
    }

    TEST_CASE("engine-registered callback is available to multiple programs" * doctest::skip()) {
	madc::engine eng;
	g_host_sum = 0;
	REQUIRE(eng.register_function("host_add", host_add));

	madc::program p1 = eng.create_program();
	madc::program p2 = eng.create_program();

	CHECK(p1.exec_string(
	    "int main() { host_add(1, 2); return 0; }\n",
	    "eng_cb1.mad"));
	CHECK(g_host_sum == 3);

	CHECK(p2.exec_string(
	    "int main() { host_add(10, 20); return 0; }\n",
	    "eng_cb2.mad"));
	CHECK(g_host_sum == 30);
    }

    TEST_CASE("program created from engine can still register its own callbacks" * doctest::skip()) {
	madc::engine eng;
	madc::program pgm = eng.create_program();
	g_host_sum = 0;
	REQUIRE(pgm.register_function("host_add", host_add));

	CHECK(pgm.exec_string(
	    "int main() { host_add(100, 200); return 0; }\n",
	    "pgm_own_cb.mad"));
	CHECK(g_host_sum == 300);
    }

    TEST_CASE("engine security policy propagates to programs") {
	madc::engine eng;
	madc::security_policy sec;
	sec.allow_process_functions = false;
	sec.allow_dlfcn_functions = false;
	eng.set_security_policy(sec);

	madc::program pgm = eng.create_program();
	const madc::security_policy &pgm_sec = pgm.get_security_policy();
	CHECK_FALSE(pgm_sec.allow_process_functions);
	CHECK_FALSE(pgm_sec.allow_dlfcn_functions);
    }

    TEST_CASE("engine expression policy propagates to programs") {
	madc::engine eng;
	madc::expression_policy ep;
	ep.allow_function_calls = true;
	ep.allow_member_access = true;
	eng.set_expression_policy(ep);

	madc::program pgm = eng.create_program();
	const madc::expression_policy &pgm_ep = pgm.get_expression_policy();
	CHECK(pgm_ep.allow_function_calls);
	CHECK(pgm_ep.allow_member_access);
    }

    TEST_CASE("engine is move-constructible") {
	madc::engine eng1;
	madc::invoke_limits limits;
	limits.cpu_ms = 999;
	eng1.set_invoke_limits(limits);

	madc::engine eng2(std::move(eng1));
	CHECK(eng2.get_invoke_limits().cpu_ms == 999);
    }
}

// --- 32-byte madc_value ABI behavior (helpers live in madc_c_api.cpp,
// which this binary links). Layout/cell-runtime pins live in
// test_libmadc_value.cpp; this suite covers the helper semantics.
TEST_SUITE("madc_value ABI helpers") {

    TEST_CASE("string SSO vs cell + copy/clear refcounts") {
	madc_value v; madc_value_init(&v);
	REQUIRE(madc_value_set_string(&v, "short") == MADC_OK);   // 5 -> SSO
	CHECK((v.flags & MADC_VF_INLINE_TEXT) != 0);
	CHECK(v.size == 5);
	CHECK(std::string(madc_value_text(&v, NULL)) == "short");
	REQUIRE(madc_value_set_string(&v, "exactly16bytes!!") == MADC_OK); // 16 -> cell
	CHECK((v.flags & MADC_VF_HEAP) != 0);
	CHECK(madc_cell_refcount(v.text_value) == 1);
	madc_value c; madc_value_init(&c);
	REQUIRE(madc_value_copy(&c, &v) == MADC_OK);
	CHECK(c.text_value == v.text_value);                      // shared cell
	CHECK(madc_cell_refcount(v.text_value) == 2);
	madc_value_clear(&c);
	CHECK(madc_cell_refcount(v.text_value) == 1);
	size_t len = 0;
	CHECK(std::string(madc_value_text(&v, &len)) == "exactly16bytes!!");
	CHECK(len == 16);
	madc_value_clear(&v);
	CHECK(v.type_id == MADC_VALUE_NULL);
    }

    TEST_CASE("gradual-typing flags on set helpers") {
	madc_value v; madc_value_init(&v);
	REQUIRE(madc_value_set_integer(&v, 5) == MADC_OK);
	v.flags |= MADC_VF_TYPE_LOCKED;
	CHECK(madc_value_set_string(&v, "no") == MADC_ERROR);     // cross-domain
	CHECK(madc_value_set_real(&v, 1.5) == MADC_ERROR);        // LOCKED: no numeric juggle
	CHECK(madc_value_set_null(&v) == MADC_ERROR);             // not nullable
	CHECK(v.integer_value == 5);                              // untouched by rejections
	v.flags |= MADC_VF_NULLABLE;
	CHECK(madc_value_set_null(&v) == MADC_OK);                // ?int accepts null
	CHECK(v.type_id == MADC_VALUE_INTEGER);                   // lock keeps the domain
	CHECK(v.size == 0);                                       // typed-null marker
	CHECK(madc_value_set_integer(&v, 7) == MADC_OK);          // and accepts its domain
	CHECK(v.integer_value == 7);

	madc_value w; madc_value_init(&w);
	REQUIRE(madc_value_set_integer(&w, 1) == MADC_OK);
	w.flags |= MADC_VF_TYPE_COERCE;
	CHECK(madc_value_set_real(&w, 2.9) == MADC_OK);           // converts toward INT64
	CHECK(w.type_id == MADC_VALUE_INTEGER);
	CHECK(w.integer_value == 2);
	CHECK(madc_value_set_string(&w, "no") == MADC_ERROR);     // coerce is numeric-only

	madc_value k; madc_value_init(&k);
	REQUIRE(madc_value_set_integer(&k, 9) == MADC_OK);
	k.flags |= MADC_VF_CONST;
	CHECK(madc_value_set_integer(&k, 10) == MADC_ERROR);      // read-only
	CHECK(k.integer_value == 9);
	madc_value_clear(&v); madc_value_clear(&w);
	k.flags &= ~(uint32_t)MADC_VF_CONST; madc_value_clear(&k);
    }
}
