// Unit test for the AOT -shared lane (madc_cir_emit_native, mnkShared):
// MIR assembles an ET_DYN shared object directly — no external toolchain —
// and this process consumes it via dlopen/dlsym, so the whole product path
// is exercised in-repo with no host cc/ld even at test time.

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

} // namespace

TEST_SUITE("madc_cir_emit_native") {

    TEST_CASE("-shared emits a dlopen-consumable ET_DYN shared object") {
	std::string src_path = write_temp("/tmp/madc_unit_shared_XXXXXX.mad", 4,
					  "int mul_base = 6;\n"
					  "int madd(int a, int b) { return a + b; }\n"
					  "int mmul(int a) { return a * mul_base; }\n");
	REQUIRE(!src_path.empty());
	std::string so_path = write_temp("/tmp/madc_unit_shared_XXXXXX.so", 3, "");
	REQUIRE(!so_path.empty());

	{
	    MadcEngine engine;
	    std::unique_ptr<Program> prog = engine.create_program();
	    TokenProgram *tp = prog->tokenize(src_path.c_str());
	    REQUIRE(tp != NULL);
	    REQUIRE(prog->parse(tp));

	    std::vector<std::string> user_libs;
	    int rc = madc_cir_emit_native(prog.get(), src_path.c_str(),
					  mnkShared, so_path.c_str(), user_libs);
	    madc_object_mode = false;
	    REQUIRE(rc == 0);
	}

	// RTLD_NOW forces eager application of every .rela.dyn entry —
	// R_X86_64_RELATIVE internals and imported-symbol slots alike.
	// RTLD_DEEPBIND: this binary statically embeds libmadc.a with
	// -rdynamic, so the .so's DT_NEEDED libmadc.so.0 would otherwise
	// bind its initializers to the executable's interposed symbols and
	// run them against mixed per-image state (split-brain hang in the
	// lexer intern table). Deep binding keeps the loaded subtree
	// self-consistent; a plain (non-madc-static) host needs no flag.
	void *h = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
	if (!h)
	    MESSAGE("dlopen: " << dlerror());
	REQUIRE(h != NULL);
	int (*madd)(int, int) = (int (*)(int, int))dlsym(h, "madd");
	int (*mmul)(int) = (int (*)(int))dlsym(h, "mmul");
	REQUIRE(madd != NULL);
	REQUIRE(mmul != NULL);
	CHECK(madd(20, 22) == 42);
	CHECK(mmul(20) == 120);	// module data through a RELATIVE reloc
	dlclose(h);

	std::remove(src_path.c_str());
	std::remove(so_path.c_str());
    }
}
