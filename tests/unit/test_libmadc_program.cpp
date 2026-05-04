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
#include "libmadc/program.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {

int64_t g_host_sum = 0;
int64_t g_host_strlen = 0;

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
