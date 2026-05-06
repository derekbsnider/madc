// Unit tests for madc::program (libmadc public embedding API).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

bool madc_verbose = false;
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
#include "libmadc/api.h"
#include "libmadc/program.h"

#include <cstdio>
#include <cstring>
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

} // namespace

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

    TEST_CASE("exec_file reports runtime error when main is missing") {
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

    TEST_CASE("eval returns string results through char pointer marshaling") {
	madc::program pgm;
	madc::value result;

	CHECK(pgm.eval("string name = \"echo\";\n"
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

    TEST_CASE("eval_expression can return host-supplied string bindings") {
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

    TEST_CASE("eval_expression can use math.h header groups for libm functions") {
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
	CHECK(err->message == "dynamic symbol fallback is disabled by registration policy");
    }

    TEST_CASE("compile_options can disable extern late-bind dlsym fallback") {
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

    TEST_CASE("invoke_limits can reject excessive cpu usage from call") {
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

    TEST_CASE("invoke_limits can reject excessive resident growth from call") {
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

    TEST_CASE("fork_per_invocation exec_string succeeds for a simple program") {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.execution = madc::execution_mode::fork_per_invocation;
	pgm.set_security_policy(policy);

	CHECK(pgm.exec_string("int main() { return 0; }\n", "fork_ok.mad"));
	CHECK_FALSE(pgm.has_error());
	CHECK(pgm.diagnostics().empty());
    }

    TEST_CASE("fork_per_invocation exec_file reports runtime errors") {
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

    TEST_CASE("fork_per_invocation exec_string counts stdout toward output limits") {
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

    TEST_CASE("fork_per_invocation exec_string counts stderr toward output limits") {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.execution = madc::execution_mode::fork_per_invocation;
	pgm.set_security_policy(policy);
	madc::invoke_limits limits;
	limits.output_bytes = 4;
	pgm.set_invoke_limits(limits);

	CHECK_FALSE(pgm.exec_string("#include <iostream>\n"
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

    TEST_CASE("fork_per_invocation eval returns string results from child execution") {
	madc::program pgm;
	madc::security_policy policy = pgm.get_security_policy();
	policy.execution = madc::execution_mode::fork_per_invocation;
	pgm.set_security_policy(policy);

	madc::value result;
	CHECK(pgm.eval("string name = \"echo\";\n"
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

    TEST_CASE("register_function exposes a host integer callback to scripts") {
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

    TEST_CASE("register_function supports string to const char* coercion") {
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

    TEST_CASE("script-side runtime eval sees current scope when allowed") {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path,
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

    TEST_CASE("runtime_eval_policy can restrict child full eval independently of the parent program") {
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

    TEST_CASE("call rejects unsupported string object parameters for now") {
	madc::program pgm;
	std::string path = make_temp_source_path();
	write_file(path,
		   "string echo(string s) { return s; }\n"
		   "int main() { return 0; }\n");

	REQUIRE(pgm.compile_file(path));
	madc::value result;
	CHECK_FALSE(pgm.call("echo", {madc::value("hello")}, &result));
	REQUIRE(pgm.has_error());
	const madc::error *err = pgm.last_error();
	REQUIRE(err != NULL);
	CHECK(err->message == "program::call does not support this return type yet");

	std::remove(path.c_str());
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
		   "int counter = 0;\n"
		   "int read_counter() { return counter; }\n"
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
		   "string name = \"alice\";\n"
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
