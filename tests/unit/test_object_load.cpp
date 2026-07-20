// Unit test for the AOT R4b .o-as-precompiled-cache lane: madc emits a
// relocatable object (madc_cir_emit_native, mnkObject) and this process
// loads it back through the fork's in-process ET_REL loader
// (MIR_object_load) — map, relocate, resolve, call. The whole product path
// runs in-repo with no external toolchain even at test time.

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
#include "../src/madc_cir.h"
#include "mir-debug.h"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>

// The one-shot CLI flag madc_cir_emit_native sets (defined weak in
// madc_globals.cpp); reset after each emission so later cases in this
// binary run in normal JIT mode.
extern thread_local bool madc_object_mode;

namespace {

std::string write_temp(const char *suffix_tmpl, int suffix_len,
		       const char *contents)
{
    std::string tmpl = suffix_tmpl;
    std::vector<char> path(tmpl.begin(), tmpl.end());
    path.push_back('\0');
    int fd = mkstemps(path.data(), suffix_len);
    if (fd < 0)
	return std::string();
    if (contents && *contents) {
	ssize_t len = (ssize_t)std::strlen(contents);
	if (write(fd, contents, len) != len) {
	    close(fd);
	    return std::string();
	}
    }
    close(fd);
    return std::string(path.data());
}

// Emit src to a fresh .o via the production emit entry; returns the .o path.
std::string emit_object(const char *src)
{
    std::string src_path = write_temp("/tmp/madc_unit_objload_XXXXXX.mad", 4,
				      src);
    REQUIRE(!src_path.empty());
    std::string obj_path = write_temp("/tmp/madc_unit_objload_XXXXXX.o", 2,
				      "");
    REQUIRE(!obj_path.empty());

    MadcEngine engine;
    std::unique_ptr<Program> prog = engine.create_program();
    TokenProgram *tp = prog->tokenize(src_path.c_str());
    REQUIRE(tp != NULL);
    REQUIRE(prog->parse(tp));

    std::vector<std::string> user_libs;
    int rc = madc_cir_emit_native(prog.get(), src_path.c_str(), mnkObject,
				  obj_path.c_str(), user_libs);
    madc_object_mode = false;
    REQUIRE(rc == 0);
    std::remove(src_path.c_str());
    return obj_path;
}

// This binary embeds libmadc.a with -rdynamic, so dlsym(RTLD_DEFAULT)
// reaches libc, the madc runtime, and the mir builtin exports alike —
// the same reach the CLI resolver has.
void *dlsym_resolver(const char *name, void *)
{
    return dlsym(RTLD_DEFAULT, name);
}

} // namespace

TEST_SUITE("madc_cir_run_object") {

    TEST_CASE(".o loads in-process and its functions execute") {
	std::string obj_path = emit_object(
	    "int mul_base = 6;\n"
	    "int madd(int a, int b) { return a + b; }\n"
	    "int mmul(int a) { return a * mul_base; }\n"
	    "int main() { return 0; }\n");

	std::ifstream f(obj_path.c_str(), std::ios::binary);
	REQUIRE(f.good());
	std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
				std::istreambuf_iterator<char>());
	f.close();
	REQUIRE(!bytes.empty());

	char err[256] = "";
	MIR_object_loaded_t lo = MIR_object_load(bytes.data(), bytes.size(),
						 dlsym_resolver, NULL,
						 err, sizeof err);
	if (!lo)
	    MESSAGE("MIR_object_load: " << err);
	REQUIRE(lo != NULL);

	int (*madd)(int, int)
	    = (int (*)(int, int))MIR_object_loaded_sym(lo, "madd");
	int (*mmul)(int) = (int (*)(int))MIR_object_loaded_sym(lo, "mmul");
	REQUIRE(madd != NULL);
	REQUIRE(mmul != NULL);
	CHECK(madd(20, 22) == 42);
	CHECK(mmul(20) == 120);	// global data through an ABS64 .data reloc
	CHECK((MIR_object_loaded_sym(lo, "no_such_symbol") == NULL));

	// Safe here: nothing in the object registered atexit handlers.
	MIR_object_loaded_unload(lo);
	std::remove(obj_path.c_str());
    }

    TEST_CASE("madc_cir_run_object runs main and returns its exit status") {
	std::string obj_path = emit_object(
	    "int main() { return 42; }\n");

	char *oargv[] = { (char *)obj_path.c_str(), NULL };
	CHECK(madc_cir_run_object(obj_path.c_str(), 1, oargv) == 42);
	std::remove(obj_path.c_str());
    }
}
