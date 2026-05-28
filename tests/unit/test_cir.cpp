/* test_cir.cpp — Unit tests for CIR translation (TokenBase → c2mir node_t → MIR) */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <list>
#include <queue>
#include <iostream>
#include <sstream>
#include <fstream>
#include <stdint.h>
#include <asmjit/x86.h>

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

extern "C" {
#include "mir.h"
#include "mir-gen.h"
#include "c2mir/c2mir.h"
#include "c2mir/c2mir_api.h"
}

#include "../src/madc_cir.h"

// Helper: tokenize+parse source, translate to CIR, compile+run, return result
static int64_t cir_run(const char *source) {
    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer(source, "<test>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));

    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);

    node_t tree = cir_translate(c2m, prog.get());
    REQUIRE(tree != nullptr);

    int ok = cir_compile(mir_ctx, c2m, tree, "test_mod");
    REQUIRE(ok == 1);

    MIR_module_t mod = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(mir_ctx));
    MIR_load_module(mir_ctx, mod);
    MIR_link(mir_ctx, MIR_set_interp_interface, NULL);

    MIR_item_t func_item = NULL;
    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, mod->items);
	 item != NULL;
	 item = DLIST_NEXT(MIR_item_t, item)) {
	if (item->item_type == MIR_func_item) {
	    if (strcmp(item->u.func->name, "main") == 0) {
		func_item = item;
	    }
	}
    }
    if (!func_item) {
	// Debug: dump what's in the module
	MIR_output_module(mir_ctx, stderr, mod);
    }
    REQUIRE(func_item != nullptr);

    MIR_val_t val;
    MIR_interp(mir_ctx, func_item, &val, 0);
    int64_t result = val.i;

    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);
    return result;
}

TEST_CASE("CIR: return literal") {
    CHECK(cir_run("int main() { return 42; }") == 42);
    CHECK(cir_run("int main() { return 0; }") == 0);
    CHECK(cir_run("int main() { return -1; }") == -1);
}

TEST_CASE("CIR: arithmetic expressions") {
    CHECK(cir_run("int main() { return 2 + 3; }") == 5);
    CHECK(cir_run("int main() { return 10 - 3; }") == 7);
    CHECK(cir_run("int main() { return 6 * 7; }") == 42);
    CHECK(cir_run("int main() { return 100 / 10; }") == 10);
    CHECK(cir_run("int main() { return 17 % 5; }") == 2);
    CHECK(cir_run("int main() { return 2 + 3 * 4; }") == 14);
}

TEST_CASE("CIR: local variables") {
    CHECK(cir_run("int main() { int x; x = 10; return x; }") == 10);
    CHECK(cir_run("int main() { int x; x = 10; return x + 1; }") == 11);
    CHECK(cir_run("int main() { int a; int b; a = 3; b = 4; return a + b; }") == 7);
}

TEST_CASE("CIR: function with parameters") {
    CHECK(cir_run(
	"int add(int a, int b) { return a + b; }\n"
	"int main() { return add(3, 4); }"
    ) == 7);
}

TEST_CASE("CIR: if/else") {
    CHECK(cir_run(
	"int main() { int x; x = 10; if (x > 5) return 1; return 0; }"
    ) == 1);
    CHECK(cir_run(
	"int main() { int x; x = 3; if (x > 5) return 1; else return 0; }"
    ) == 0);
}

TEST_CASE("CIR: while loop") {
    CHECK(cir_run(
	"int main() { int i; int sum; i = 0; sum = 0;\n"
	"  while (i < 10) { sum = sum + i; i = i + 1; }\n"
	"  return sum; }"
    ) == 45);
}

TEST_CASE("CIR: for loop") {
    CHECK(cir_run(
	"int main() { int sum; int i; sum = 0;\n"
	"  for (i = 1; i <= 10; i = i + 1) sum = sum + i;\n"
	"  return sum; }"
    ) == 55);
}
