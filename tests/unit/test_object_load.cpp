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

// Two import targets deliberately invisible to dlsym (static, not in the
// dynamic symbol table): at emit time madc's sentinel resolver hands BOTH
// the same &undef_sentinel value, so the address pool's (value, item) slot
// keying is what keeps their .mir.addrpool slots — and relocations — apart.
static int r6_ext_a_impl() { return 111; }
static int r6_ext_b_impl() { return 222; }

void *two_import_resolver(const char *name, void *)
{
    if (std::strcmp(name, "r6_test_ext_a") == 0)
	return (void *)&r6_ext_a_impl;
    if (std::strcmp(name, "r6_test_ext_b") == 0)
	return (void *)&r6_ext_b_impl;
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

    TEST_CASE("multi-object link: cross-object calls, data, and dup detection") {
	// TU a: defines `scale` and calls b's `bonus` (undefined here);
	// TU b: defines `bonus` and reads a's `scale` (undefined here).
	// Circular cross-object references — resolvable only by merging.
	std::string a_path = emit_object(
	    "int scale = 6;\n"
	    "int bonus(int);\n"
	    "int amul(int x) { return bonus(x) * scale; }\n"
	    "int main(int argc, char **argv) { return amul(argc + 5); }\n");
	std::string b_path = emit_object(
	    "extern int scale;\n"
	    "int bonus(int x) { return x + scale; }\n");

	std::vector<std::string> paths;
	paths.push_back(a_path);
	paths.push_back(b_path);

	// Run lane (merge in memory, load, enter main):
	// main(1) = amul(6) = bonus(6) * 6 = (6 + 6) * 6 = 72.
	char *oargv[] = { (char *)a_path.c_str(), NULL };
	CHECK(madc_cir_run_objects(paths, 1, oargv) == 72);

	// ld -r lane: the merged relocatable loads and runs like any .o.
	std::string r_path =
	    write_temp("/tmp/madc_unit_objload_XXXXXX.o", 2, "");
	REQUIRE(!r_path.empty());
	std::vector<std::string> user_libs;
	REQUIRE(madc_cir_link_objects(paths, mnkRelocatable, r_path.c_str(),
				      user_libs) == 0);
	char *rargv[] = { (char *)r_path.c_str(), NULL };
	CHECK(madc_cir_run_object(r_path.c_str(), 1, rargv) == 72);

	// Executable lane: the merged capture through the PIE emitter.
	std::string x_path =
	    write_temp("/tmp/madc_unit_objload_XXXXXX.bin", 4, "");
	REQUIRE(!x_path.empty());
	REQUIRE(madc_cir_link_objects(paths, mnkPieExecutable,
				      x_path.c_str(), user_libs) == 0);
	{
	    std::ifstream f(x_path.c_str(), std::ios::binary);
	    std::string img((std::istreambuf_iterator<char>(f)),
			    std::istreambuf_iterator<char>());
	    REQUIRE(img.size() > 4);
	    CHECK(img.compare(0, 4, "\177ELF") == 0);
	}

	// Duplicate strong definition: linking a with itself must fail
	// loudly (stderr names the symbol; here we assert the rc).
	std::vector<std::string> dup;
	dup.push_back(a_path);
	dup.push_back(a_path);
	std::string d_path =
	    write_temp("/tmp/madc_unit_objload_XXXXXX.o", 2, "");
	REQUIRE(!d_path.empty());
	CHECK(madc_cir_link_objects(dup, mnkRelocatable, d_path.c_str(),
				    user_libs) != 0);

	std::remove(a_path.c_str());
	std::remove(b_path.c_str());
	std::remove(r_path.c_str());
	std::remove(x_path.c_str());
	std::remove(d_path.c_str());
    }

    TEST_CASE("two same-sentinel imports keep distinct pool slots (R6)") {
	// Both prototypes are unresolvable at emit (static impls above are
	// not dlsym-visible), so the sentinel resolver hands them one shared
	// VALUE; the object must still carry one relocated slot per import.
	std::string obj_path = emit_object(
	    "int r6_test_ext_a();\n"
	    "int r6_test_ext_b();\n"
	    "int combine() { return r6_test_ext_a() * 1000 + r6_test_ext_b(); }\n"
	    "int main() { return 0; }\n");

	std::ifstream f(obj_path.c_str(), std::ios::binary);
	REQUIRE(f.good());
	std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
				std::istreambuf_iterator<char>());
	f.close();
	REQUIRE(!bytes.empty());

	char err[256] = "";
	MIR_object_loaded_t lo = MIR_object_load(bytes.data(), bytes.size(),
						 two_import_resolver, NULL,
						 err, sizeof err);
	if (!lo)
	    MESSAGE("MIR_object_load: " << err);
	REQUIRE(lo != NULL);

	int (*combine)() = (int (*)())MIR_object_loaded_sym(lo, "combine");
	REQUIRE(combine != NULL);
	CHECK(combine() == 111222); // a fused slot would give 111111 or 222222

	MIR_object_loaded_unload(lo);
	std::remove(obj_path.c_str());
    }
}
