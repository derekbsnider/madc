/* madc_cir.cpp — CIR translation: TokenBase AST → c2mir node_t tree.
 *
 * Walks the madc parser's TokenBase tree and builds the equivalent
 * c2mir node_t tree, which can then be fed to c2mir's type checker
 * and MIR generator.
 */

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
#include <stdlib.h>
#include <dlfcn.h>


#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_cir.h"
#include "cir_builder.h"
#include "cir_emit_c.h"

extern "C" {
#include "c2mir/c2mir_api.h"
#include "mir-gen.h"
}

extern thread_local bool madc_verbose;
extern thread_local int madc_opt_level;

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

c2m_ctx_t cir_init(MIR_context_t mir_ctx, bool debug_p)
{
    struct c2mir_options *opts = new struct c2mir_options();
    memset(opts, 0, sizeof(*opts));
    opts->message_file = stderr;
    // debug_p makes c2mir_compile_tree print the POST-check tree (after
    // do_context) to message_file — same stage as `c2m -d`, for diffing.
    opts->debug_p = debug_p ? 1 : 0;
    return c2mir_init_compile(mir_ctx, opts);
}

int cir_compile(MIR_context_t mir_ctx, c2m_ctx_t c2m, node_t tree,
		const char *module_name)
{
    return c2mir_compile_tree(mir_ctx, c2m, tree, module_name);
}

void cir_finish(c2m_ctx_t c2m)
{
    c2mir_finish_compile(c2m);
}

// -----------------------------------------------------------------------
// Import resolver for MIR linking — finds C library symbols via dlsym
// -----------------------------------------------------------------------

static void *cir_import_resolver(const char *name)
{
    void *addr = dlsym(RTLD_DEFAULT, name);
    if (!addr)
	DBG(std::cerr << "cir_import_resolver: unresolved: " << name << std::endl);
    return addr;
}

// -----------------------------------------------------------------------
// JIT symbolization for the crash handler
// -----------------------------------------------------------------------
//
// The MIR module whose functions are currently JIT-executing. Set just
// before main() is invoked so the crash handler (madc.cpp) can map a
// faulting machine-code address back to the MIR function name + offset —
// MIR generates each function lazily, recording its code range in
// machine_code / machine_code_len, so a function on the live call stack has
// a resolvable range. This is the first brick of madc's own debugger.
static MIR_module_t g_jit_module = NULL;

// Resolve a code address to "func+0xoff [JIT]". Returns 1 on success. Only
// reads pointer-chasing DLIST fields and writes via snprintf — acceptable
// from a crashing signal handler (the process is dying regardless).
extern "C" int madc_jit_symbolize(void *addr, char *out, unsigned long n)
{
	if (!g_jit_module || !out || n == 0) return 0;
	char *a = (char *)addr;
	for (MIR_item_t item = DLIST_HEAD(MIR_item_t, g_jit_module->items);
	     item != NULL; item = DLIST_NEXT(MIR_item_t, item)) {
		if (item->item_type != MIR_func_item || !item->u.func) continue;
		char *mc = (char *)item->u.func->machine_code;
		size_t len = item->u.func->machine_code_len;
		if (mc && len && a >= mc && a < mc + len) {
			snprintf(out, n, "%s+0x%lx [JIT]", item->u.func->name,
				 (unsigned long)(a - mc));
			return 1;
		}
	}
	return 0;
}

// -----------------------------------------------------------------------
// Full CIR pipeline: parse → translate → compile → JIT execute
// -----------------------------------------------------------------------

// Common translation-unit lowering: translate the parsed Program to a
// cir_node tree, run the validity gate, c2mir-compile it, and return the
// produced MIR module. This is the single implementation of the
// "Program → MIR module" path shared by madc_cir_execute (single-file run)
// and the multi-TU project engine.
//
// The CirBuilder owns the node arena backing the returned module, so it must
// OUTLIVE the module (through cir_compile()/MIR_gen()). It is heap-allocated
// here and handed back via `out_builder`; the caller owns it and must
// `delete` it after run/teardown. On any failure the builder is destroyed
// here, `out_builder` is left null, and nullptr is returned.
//
// The dump flags reproduce madc_cir_execute's behavior exactly:
//   dump_nodes   — dump pre-check nodes, then continue compiling.
//   dump_tree    — dump pre-check tree, then continue compiling.
//   dump_checked — run c2mir's checker, dump POST-check tree, then STOP
//                  (it mutates the tree). `out_stop` is set true and nullptr
//                  is returned with no error; the caller returns 0.
// The project engine passes all three flags false and ignores out_stop.
static MIR_module_t build_tu_module(MIR_context_t ctx, c2m_ctx_t c2m,
				    Program *prog, const char *source_name,
				    bool dump_tree, bool dump_nodes,
				    bool dump_checked,
				    CirBuilder *&out_builder, bool &out_stop)
{
    out_builder = NULL;
    out_stop = false;

    // CirBuilder (cir_node) is the sole backend. The builder owns its node
    // arena and must outlive cir_compile()/MIR_gen() — hence it is returned to
    // the caller. (The legacy static cir_translate() path was removed; it had
    // drifted from this backend — e.g. mislowering backward `goto` loops — and
    // a stale unit test pointed at it once hung the suite. One backend, no A/B.)
    CirBuilder *builder = new CirBuilder(c2m);
    node_t tree = builder->translate_module(prog);
    if (!tree) {
	fprintf(stderr, "%s: tree build failed\n", source_name);
	delete builder;
	return NULL;
    }

    if (dump_nodes)
	cir_dump_nodes(stderr, tree);

    if (dump_tree) {
	fprintf(stderr, "=== CIR TREE (pre-check) ===\n");
	c2mir_dump_tree(c2m, stderr, tree);
	fprintf(stderr, "=== END CIR TREE ===\n");
    }

    // Validity gate: never hand c2mir a tree containing error/incomplete
    // nodes (the builder emits them where it can't translate a construct).
    if (int nerr = cir_report_errors(stderr, tree)) {
	fprintf(stderr, "%s: %d untranslatable node(s); not compiling\n",
		source_name, nerr);
	delete builder;
	return NULL;
    }

    // --dump-cir-checked: run c2mir's checker (do_context) and dump the
    // POST-check tree to stderr — same stage as `c2m -d`, for diffing. This
    // mutates the tree, so we stop here rather than compile it.
    if (dump_checked) {
	fprintf(stderr, "=== CIR TREE (post-check) ===\n");
	c2mir_dump_tree_checked(c2m, stderr, tree);
	fprintf(stderr, "=== END CIR TREE ===\n");
	delete builder;
	out_stop = true;
	return NULL;
    }

    if (!cir_compile(ctx, c2m, tree, source_name)) {
	fprintf(stderr, "%s: cir_compile failed\n", source_name);
	delete builder;
	return NULL;
    }

    MIR_module_t mod = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx));
    if (!mod) {
	fprintf(stderr, "%s: no module produced\n", source_name);
	delete builder;
	return NULL;
    }

    out_builder = builder;
    return mod;
}

int madc_cir_execute(Program *prog, const char *source_name,
		     int user_argc, char **user_argv,
		     bool dump_tree, bool dump_nodes, bool dump_checked)
{
    MIR_context_t ctx = MIR_init();
    c2mir_init(ctx);
    MIR_gen_init(ctx);
    MIR_gen_set_optimize_level(ctx, (unsigned)madc_opt_level);

    c2m_ctx_t c2m = cir_init(ctx, dump_checked);
    if (!c2m) {
	fprintf(stderr, "madc_cir_execute: cir_init failed\n");
	MIR_gen_finish(ctx);
	c2mir_finish(ctx);
	MIR_finish(ctx);
	return -1;
    }

    CirBuilder *builder = NULL;
    bool stop = false;
    MIR_module_t mod = build_tu_module(ctx, c2m, prog, source_name,
				       dump_tree, dump_nodes, dump_checked,
				       builder, stop);
    if (!mod) {
	// dump_checked: dumped the post-check tree and stopped (success, 0).
	// Otherwise build_tu_module already printed the diagnostic.
	cir_finish(c2m);
	c2mir_finish(ctx);
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
	return stop ? 0 : -1;
    }

    MIR_load_module(ctx, mod);
    MIR_link(ctx, MIR_set_gen_interface, cir_import_resolver);

    void *code = nullptr;
    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, mod->items);
	 item != nullptr; item = DLIST_NEXT(MIR_item_t, item)) {
	if (item->item_type == MIR_func_item &&
	    strcmp(item->u.func->name, "main") == 0) {
	    code = MIR_gen(ctx, item);
	    break;
	}
    }

    if (!code) {
	fprintf(stderr, "madc_cir_execute: main() not found\n");
	cir_finish(c2m);
	c2mir_finish(ctx);
	MIR_gen_finish(ctx);
	MIR_finish(ctx);
	delete builder;
	return -1;
    }

    // Expose the module to the crash handler for JIT symbolization.
    g_jit_module = mod;
    int result = ((int (*)(int, char **))code)(user_argc, user_argv);
    g_jit_module = NULL;

    cir_finish(c2m);
    MIR_gen_finish(ctx);
    c2mir_finish(ctx);
    MIR_finish(ctx);
    delete builder;

    return result;
}

// Build the cir_node tree and render it as C source (no compile/run).
// Used by `--emit=c11|mc11`.
int madc_cir_emit(Program *prog, const char *source_name, FILE *out,
		  CirEmitLang lang)
{
    (void)source_name;
    MIR_context_t ctx = MIR_init();
    c2mir_init(ctx);

    c2m_ctx_t c2m = cir_init(ctx, /*debug_p=*/false);
    if (!c2m) {
	fprintf(stderr, "madc_cir_emit: cir_init failed\n");
	c2mir_finish(ctx);
	MIR_finish(ctx);
	return -1;
    }

    CirBuilder builder(c2m);
    node_t tree = builder.translate_module(prog);
    if (!tree) {
	fprintf(stderr, "madc_cir_emit: tree build failed\n");
	cir_finish(c2m);
	c2mir_finish(ctx);
	MIR_finish(ctx);
	return -1;
    }

    cir_emit_c(out, tree, lang);

    cir_finish(c2m);
    c2mir_finish(ctx);
    MIR_finish(ctx);
    return 0;
}
