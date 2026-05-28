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

TEST_CASE("CIR: int main() { return 42; }") {
    // 1. Tokenize + parse via madc's original parser
    auto prog = std::make_shared<Program>();
    TokenProgram *tp = prog->tokenize_buffer("int main() { return 42; }", "<test>");
    REQUIRE(tp != nullptr);
    REQUIRE(prog->parse(tp));

    // 2. Initialize MIR + c2mir
    MIR_context_t mir_ctx = MIR_init();
    c2mir_init(mir_ctx);
    c2m_ctx_t c2m = cir_init(mir_ctx);
    REQUIRE(c2m != nullptr);

    // 3. Translate TokenBase AST → c2mir node_t
    node_t tree = cir_translate(c2m, prog.get());
    REQUIRE(tree != nullptr);

    // 4. Type-check + generate MIR
    int ok = cir_compile(mir_ctx, c2m, tree, "test_module");
    REQUIRE(ok == 1);

    // 5. JIT and execute
    MIR_module_t mod = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(mir_ctx));
    MIR_load_module(mir_ctx, mod);
    MIR_link(mir_ctx, MIR_set_interp_interface, NULL);

    MIR_item_t func_item = NULL;
    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, mod->items);
	 item != NULL;
	 item = DLIST_NEXT(MIR_item_t, item)) {
	if (item->item_type == MIR_func_item) {
	    func_item = item;
	    break;
	}
    }
    REQUIRE(func_item != nullptr);

    MIR_val_t val;
    MIR_interp(mir_ctx, func_item, &val, 0);
    CHECK(val.i == 42);

    // 6. Cleanup
    cir_finish(c2m);
    c2mir_finish(mir_ctx);
    MIR_finish(mir_ctx);
}
