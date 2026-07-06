/* madc_cir.cpp — CIR translation: TokenBase AST → c2mir node_t tree.
 *
 * Walks the madc parser's TokenBase tree and builds the equivalent
 * c2mir node_t tree, which can then be fed to c2mir's type checker
 * and MIR generator.
 */

#include <algorithm>
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
#include <stdarg.h>
#include <setjmp.h>
#include <dlfcn.h>
#include <chrono>


#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_cir.h"
#include "madc_project.h"
#include "cir_builder.h"
#include "cir_emit_c.h"
#include "cir_freeze.h"

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

// Host-callback registrations of the Program being linked: the synthesized
// trampolines (synth_host_trampoline) call __madc_host_cb_<k> imports whose
// addresses exist only in this table — dlsym can never find them. Set around
// MIR_link by the session/one-shot build paths (same single-threaded session
// discipline as the fatal-containment state below).
static thread_local const std::vector<Program::HostCallbackReg> *cir_active_host_regs = NULL;

static void *cir_import_resolver(const char *name)
{
    if (cir_active_host_regs)
	for (const Program::HostCallbackReg &r : *cir_active_host_regs)
	    if (r.entry && r.import_sym == name)
		return (void *)r.entry;
    void *addr = dlsym(RTLD_DEFAULT, name);
    if (!addr)
	DBG(std::cerr << "cir_import_resolver: unresolved: " << name << std::endl);
    return addr;
}

// Diagnostic: report every MIR import that no loaded module defines and the
// resolver can't satisfy — i.e. the symbols MIR_link will reject as "import of
// undefined item". MIR aborts on the FIRST such item and truncates its (huge,
// mangled) name in the error, so this lists ALL of them, untruncated. Invoked
// from the link-error handler. This is the missing MIR-side visibility: a
// tsubst body that references an un-instantiated/un-emitted symbol shows up here
// by exact name, turning "guess and rebuild" into a targeted fix.
static void cir_dump_undefined_imports(MIR_context_t ctx)
{
    DLIST (MIR_module_t) *mods = MIR_get_module_list(ctx);
    if (!mods) return;
    std::set<std::string> defined;
    for (MIR_module_t m = DLIST_HEAD(MIR_module_t, *mods); m;
	 m = DLIST_NEXT(MIR_module_t, m))
	for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it;
	     it = DLIST_NEXT(MIR_item_t, it)) {
	    switch (it->item_type) {
	    case MIR_func_item: case MIR_data_item: case MIR_ref_data_item:
	    case MIR_lref_data_item: case MIR_expr_data_item: case MIR_bss_item:
		if (const char *nm = MIR_item_name(ctx, it)) defined.insert(nm);
		break;
	    default: break;
	    }
	}
    std::set<std::string> reported;
    for (MIR_module_t m = DLIST_HEAD(MIR_module_t, *mods); m;
	 m = DLIST_NEXT(MIR_module_t, m))
	for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it;
	     it = DLIST_NEXT(MIR_item_t, it)) {
	    if (it->item_type != MIR_import_item || !it->u.import_id) continue;
	    std::string nm = it->u.import_id;
	    if (defined.count(nm) || reported.count(nm)) continue;
	    if (cir_import_resolver(nm.c_str())) continue;   // dlsym/host-resolvable
	    reported.insert(nm);
	    fprintf(stderr, "  undefined MIR import: %s\n", nm.c_str());
	}
    fprintf(stderr, "  [%zu undefined import(s) total]\n", reported.size());
}

// -----------------------------------------------------------------------
// MIR fatal-error containment
// -----------------------------------------------------------------------
//
// MIR's default error handler exit(1)s the PROCESS — fatal for an embedding
// host (libmadc): a bad module (e.g. an unresolved import at link time) must
// surface as a diagnostic, not kill the application. MIR_error_func_t is
// declared noreturn, so the supported recovery shape is longjmp back to the
// armed call site; the frames jumped across are plain C (mir.c), so no C++
// destructors are skipped.
static thread_local jmp_buf cir_mir_error_jmp;
static thread_local bool cir_mir_error_armed = false;
static thread_local char cir_mir_error_text[512];

static void MIR_NO_RETURN cir_mir_error(MIR_error_type_t error_type,
					const char *format, ...)
{
    (void)error_type;
    va_list ap;
    va_start(ap, format);
    vsnprintf(cir_mir_error_text, sizeof(cir_mir_error_text), format, ap);
    va_end(ap);
    if (cir_mir_error_armed)
	longjmp(cir_mir_error_jmp, 1);
    // Unarmed (a fatal outside a guarded region): keep MIR's fail-fast
    // default so the error is never silently swallowed.
    fprintf(stderr, "MIR fatal error: %s\n", cir_mir_error_text);
    exit(1);
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

// Translate the parsed Program to its module tree with throw containment.
// translate_module materializes deferred/lazy template bodies on demand
// (parse_deferred_lazy_body). Those re-parses use the parser's Throw
// mechanism, which prints the diagnostic to stderr AND throws. An escaping
// throw here would terminate the process (uncaught -> std::terminate). Catch
// it: the error was already reported, so fail cleanly instead. On failure
// the builder is destroyed, out_builder left NULL, and NULL returned; on
// success the caller owns the returned builder (it backs the tree's arena).
static node_t cir_translate_guarded(c2m_ctx_t c2m, Program *prog,
				    const char *source_name,
				    CirBuilder *&out_builder)
{
    out_builder = NULL;
    CirBuilder *builder = new CirBuilder(c2m);
    node_t tree = NULL;
    auto _cir_t0 = std::chrono::steady_clock::now();	// --show-stats: CIR build
    try {
	tree = builder->translate_module(prog);
    } catch (const std::exception &e) {
	fprintf(stderr, "%s: tree build failed (%s)\n", source_name, e.what());
	delete builder;
	return NULL;
    } catch (...) {
	fprintf(stderr, "%s: tree build failed (compile error in instantiated template body)\n",
		source_name);
	delete builder;
	return NULL;
    }
    if (prog)
	prog->_cir_build_seconds += std::chrono::duration<double>(
	    std::chrono::steady_clock::now() - _cir_t0).count();
    if (!tree) {
	fprintf(stderr, "%s: tree build failed\n", source_name);
	delete builder;
	return NULL;
    }
    out_builder = builder;
    return tree;
}

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
    CirBuilder *builder = NULL;
    node_t tree = cir_translate_guarded(c2m, prog, source_name, builder);
    if (!tree)
	return NULL;

    // Phase-1 proof hook (two-tree / materialize-from-AST): when
    // MADC_XTEST_CIR_COPY is set, compile a DEEP COPY of the whole module tree
    // instead of the original — exercising copy_cir_subtree across the entire
    // suite. The validity gate, c2mir compile, and --emit=c11 below then all run
    // on the copy; byte-identical output proves the copy is faithful. Env-gated,
    // off by default, not user-facing (an xtest hook, like the dump_* flags).
    static const bool xtest_cir_copy = getenv("MADC_XTEST_CIR_COPY") != NULL;
    if (xtest_cir_copy)
	tree = builder->copy_cir_subtree(CIR_NODE(tree))->as_node();

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

    auto _c2m_t0 = std::chrono::steady_clock::now();	// --show-stats: c2mir compile
    bool _c2m_ok = cir_compile(ctx, c2m, tree, source_name);
    if (prog)
	prog->_c2mir_seconds += std::chrono::duration<double>(
	    std::chrono::steady_clock::now() - _c2m_t0).count();
    if (!_c2m_ok) {
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

    // Debug: MADC_DUMP_MIR=1 dumps the textual MIR (proto/func signatures) to
    // stderr before link/run — for inspecting generated function ABI.
    if (getenv("MADC_DUMP_MIR"))
	MIR_output(ctx, stderr);

    out_builder = builder;
    return mod;
}

CirJitSession::CirJitSession()
    : ctx(NULL), c2m(NULL), builder(NULL), forest(NULL), mod(NULL)
{
}

CirJitSession::~CirJitSession()
{
    teardown();
}

void CirJitSession::teardown()
{
    // The proven madc_cir_execute order; the builder / forest own the node
    // storage backing the module, so they are deleted last.
    if (c2m) cir_finish(c2m);
    if (ctx) {
	MIR_gen_finish(ctx);
	c2mir_finish(ctx);
	MIR_finish(ctx);
    }
    delete builder;
    delete forest;
    ctx = NULL;
    c2m = NULL;
    builder = NULL;
    forest = NULL;
    mod = NULL;
    gen_cache.clear();
}

bool CirJitSession::init_contexts(const char *source_name, bool dump_checked)
{
    ctx = MIR_init();
    MIR_set_error_func(ctx, cir_mir_error);
    c2mir_init(ctx);
    MIR_gen_init(ctx);
    MIR_gen_set_optimize_level(ctx, (unsigned)madc_opt_level);

    c2m = cir_init(ctx, dump_checked);
    if (!c2m) {
	fprintf(stderr, "%s: cir_init failed\n", source_name);
	teardown();
	return false;
    }
    return true;
}

bool CirJitSession::load_and_link(const char *source_name, Program *prog)
{
    if (setjmp(cir_mir_error_jmp)) {
	// A MIR fatal (e.g. "import of undefined item") longjmp'd back here.
	cir_mir_error_armed = false;
	fprintf(stderr, "%s: MIR error: %s\n", source_name, cir_mir_error_text);
	// MIR aborts on the first undefined import and truncates its name; list
	// them all, untruncated (host-regs still set for accurate resolution).
	cir_dump_undefined_imports(ctx);
	cir_active_host_regs = NULL;
	teardown();
	return false;
    }
    cir_mir_error_armed = true;
    MIR_load_module(ctx, mod);
    cir_active_host_regs = prog ? &prog->host_callback_regs : NULL;
    MIR_link(ctx, MIR_set_gen_interface, cir_import_resolver);
    cir_active_host_regs = NULL;
    cir_mir_error_armed = false;
    return true;
}

bool CirJitSession::build(Program *prog, const char *source_name,
			  bool dump_tree, bool dump_nodes, bool dump_checked,
			  bool *dump_stop)
{
    if (dump_stop) *dump_stop = false;
    if (ctx) teardown();   // a rebuild starts from a clean context

    if (!init_contexts(source_name, dump_checked))
	return false;

    bool stop = false;
    mod = build_tu_module(ctx, c2m, prog, source_name,
			  dump_tree, dump_nodes, dump_checked,
			  builder, stop);
    if (!mod) {
	// dump_checked: dumped the post-check tree and stopped (not an
	// error). Otherwise build_tu_module printed the diagnostic.
	if (dump_stop) *dump_stop = stop;
	teardown();
	return false;
    }

    return load_and_link(source_name, prog);
}

bool CirJitSession::build_frozen(const void *image, size_t image_len,
				 const char *module_name)
{
    if (ctx) teardown();   // a rebuild starts from a clean context

    if (!init_contexts(module_name, /*dump_checked=*/false))
	return false;

    forest = new CirFrozenForest();
    if (!forest->open(image, image_len, c2m)) {
	teardown();
	return false;
    }

    // Recreate the freezing process's link environment (#load / -l dlopens)
    // BEFORE materialize + link, so import resolution sees the same symbols.
    for (size_t i = 0; i < forest->libs().size(); ++i) {
	const std::string &lib = forest->libs()[i];
	if (!dlopen(lib.c_str(), RTLD_LAZY | RTLD_GLOBAL)) {
	    fprintf(stderr, "madc: frozen forest needs %s: %s\n",
		    lib.c_str(), dlerror());
	    teardown();
	    return false;
	}
    }

    cir_node *root = forest->root();
    if (!root) {
	fprintf(stderr, "%s: forest root failed to materialize\n", module_name);
	teardown();
	return false;
    }

    // The same validity gate as a live build: never hand c2mir a tree
    // containing error/incomplete nodes.
    if (int nerr = cir_report_errors(stderr, root->as_node())) {
	fprintf(stderr, "%s: %d untranslatable node(s) in frozen tree; not compiling\n",
		module_name, nerr);
	teardown();
	return false;
    }

    if (!cir_compile(ctx, c2m, root->as_node(), module_name)) {
	fprintf(stderr, "%s: cir_compile failed\n", module_name);
	teardown();
	return false;
    }
    mod = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx));
    if (!mod) {
	fprintf(stderr, "%s: no module produced\n", module_name);
	teardown();
	return false;
    }
    if (getenv("MADC_DUMP_MIR"))
	MIR_output(ctx, stderr);

    return load_and_link(module_name, /*prog=*/NULL);
}

void *CirJitSession::function_code(const char *emitted_name)
{
    if (!mod || !emitted_name || !emitted_name[0]) return NULL;
    std::map<std::string, void *>::iterator gi = gen_cache.find(emitted_name);
    if (gi != gen_cache.end()) return gi->second;
    if (setjmp(cir_mir_error_jmp)) {
	// A MIR fatal during lazy codegen longjmp'd back here.
	cir_mir_error_armed = false;
	fprintf(stderr, "%s: MIR codegen error: %s\n", emitted_name,
		cir_mir_error_text);
	return NULL;
    }
    cir_mir_error_armed = true;
    void *code = NULL;
    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, mod->items);
	 item != nullptr; item = DLIST_NEXT(MIR_item_t, item)) {
	if (item->item_type == MIR_func_item &&
	    strcmp(item->u.func->name, emitted_name) == 0) {
	    code = MIR_gen(ctx, item);
	    break;
	}
    }
    cir_mir_error_armed = false;
    if (code) gen_cache[emitted_name] = code;
    return code;
}

void *CirJitSession::data_address(const char *emitted_name)
{
    if (!mod || !emitted_name || !emitted_name[0]) return NULL;
    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, mod->items);
	 item != nullptr; item = DLIST_NEXT(MIR_item_t, item)) {
	const char *name = NULL;
	switch (item->item_type) {
	    case MIR_data_item:      name = item->u.data->name; break;
	    case MIR_bss_item:       name = item->u.bss->name; break;
	    case MIR_ref_data_item:  name = item->u.ref_data->name; break;
	    case MIR_expr_data_item: name = item->u.expr_data->name; break;
	    default: break;
	}
	if (name != NULL && strcmp(name, emitted_name) == 0)
	    return item->addr;
    }
    return NULL;
}

int CirJitSession::run_main(int argc, char **argv, bool *ok, double *out_secs)
{
    void *code = function_code("main");
    if (ok) *ok = (code != NULL);
    if (!code) return -1;
    // Expose the module to the crash handler for JIT symbolization.
    g_jit_module = mod;
    auto _ex_t0 = std::chrono::steady_clock::now();	// --show-stats: execution
    int result = ((int (*)(int, char **))code)(argc, argv);
    if (out_secs)
	*out_secs = std::chrono::duration<double>(
	    std::chrono::steady_clock::now() - _ex_t0).count();
    g_jit_module = NULL;
    return result;
}

int madc_cir_execute(Program *prog, const char *source_name,
		     int user_argc, char **user_argv,
		     bool dump_tree, bool dump_nodes, bool dump_checked)
{
    CirJitSession session;
    bool stop = false;
    if (!session.build(prog, source_name, dump_tree, dump_nodes,
		       dump_checked, &stop))
	return stop ? 0 : -1;

    bool ok = false;
    int result = session.run_main(user_argc, user_argv, &ok, &prog->_exec_seconds);
    if (!ok) {
	fprintf(stderr, "madc_cir_execute: main() not found\n");
	return -1;
    }
    return result;
}

// --- B4a: grove payload v2 fill (pack-time recording -> forest payloads) ---
// The bridge between the Program's lex/parse-time pack recording and the
// container format: buckets the token stream per unit (absolute _buf order —
// the same index space the decl recorder's cursor positions live in),
// converts global decl boundaries to unit-local slices, encodes PP-export
// deltas, include edges, the branch-macro set, and the canonical unit order.
// Lives HERE (not in cir_freeze.cpp) so the container layer stays
// Program-blind (design doc 2026-07-04 §2).
static void cir_forest_fill_pack_payloads(Program *prog, cir_frozen_forest &f)
{
    if (!prog || !prog->pack_recording)
	return;
    madc::dis::intern_table *pool = TokenBase::_active_strpool;
    if (!pool)
	return;

    // Unit lookup by NAME CONTENT: partition units interned into the live
    // pool; recording keys are lexer-interned pointers — only the spelling
    // is shared.
    std::map<std::string, uint32_t> unit_idx;
    for (size_t u = 0; u < f.units.size(); ++u)
	unit_idx[pool->c_str(f.units[u].unit_name_id)] = (uint32_t)u;
    auto ensure_unit = [&](const std::string &name) -> uint32_t {
	std::map<std::string, uint32_t>::iterator it = unit_idx.find(name);
	if (it != unit_idx.end())
	    return it->second;
	f.units.push_back(cir_forest_unit());
	f.units.back().unit_name_id = pool->intern(name);
	uint32_t idx = (uint32_t)(f.units.size() - 1);
	unit_idx[name] = idx;
	return idx;
    };

    // 1. Bucket the token stream per unit. unit_of/local_of are parallel to
    //    the absolute stream index (== the recorder's cursor positions).
    size_t n = (size_t)(prog->tokens.end() - prog->tokens.begin());
    std::vector<uint32_t> unit_of(n, 0xffffffffu), local_of(n, 0);
    std::vector<std::vector<TokenBase *> > unit_toks;
    const char *last_file = NULL;
    uint32_t last_unit = 0;
    size_t i = 0;
    for (TokenStream::const_iterator it = prog->tokens.begin();
	 it != prog->tokens.end(); ++it, ++i) {
	TokenBase *tb = *it;
	if (!tb || !tb->file)
	    continue;	// unbucketable: a range containing it flags SPANS_UNITS
	uint32_t u;
	if (tb->file == last_file)
	    u = last_unit;
	else {
	    u = ensure_unit(tb->file);
	    last_file = tb->file;
	    last_unit = u;
	}
	if (unit_toks.size() < f.units.size())
	    unit_toks.resize(f.units.size());
	unit_of[i] = u;
	local_of[i] = (uint32_t)unit_toks[u].size();
	unit_toks[u].push_back(tb);
    }

    // 2. Decl index: global [begin,end) -> (unit, local slice).
    for (size_t d = 0; d < prog->pack_decls.size(); ++d) {
	const Program::PackDeclEntry &e = prog->pack_decls[d];
	if (e.begin >= n || e.end > n || e.end <= e.begin)
	    continue;
	uint32_t u = unit_of[e.begin];
	if (u == 0xffffffffu)
	    continue;
	uint32_t aux = e.aux;
	uint32_t last_local = local_of[e.begin];
	for (size_t j = e.begin; j < e.end; ++j) {
	    if (unit_of[j] == u)
		last_local = local_of[j];
	    else
		aux |= Program::PACK_DECL_SPANS_UNITS;
	}
	cir_forest_decl_entry de;
	de.name_id     = pool->intern(e.name);
	de.kind        = e.kind;
	de.slice_begin = local_of[e.begin];
	de.slice_end   = last_local + 1;
	de.aux         = aux;
	f.units[u].decl_index.push_back(de);
    }

    // 3. PP-export deltas, in directive order (tombstones included).
    for (std::map<const char *, std::vector<Program::PackMacroEvent> >::const_iterator
	     pe = prog->pack_pp_exports.begin();
	 pe != prog->pack_pp_exports.end(); ++pe) {
	uint32_t u = ensure_unit(pe->first);
	for (size_t k = 0; k < pe->second.size(); ++k) {
	    const Program::PackMacroEvent &ev = pe->second[k];
	    cir_forest_pp_event h;
	    h.name_id   = pool->intern(ev.name);
	    h.tag_flags = ev.tag;
	    const std::string &body =
		ev.tag == Program::PackMacroEvent::peDefineFn ? ev.macro.body
							      : ev.value;
	    h.body_id = body.empty() ? 0 : pool->intern(body);
	    h.variadic_param_id = 0;
	    h.nparams = 0;
	    if (ev.tag == Program::PackMacroEvent::peDefineFn) {
		if (ev.macro.variadic)
		    h.tag_flags |= CIR_FOREST_PP_VARIADIC;
		if (!ev.macro.variadic_param.empty())
		    h.variadic_param_id = pool->intern(ev.macro.variadic_param);
		h.nparams = (uint32_t)ev.macro.params.size();
	    }
	    std::vector<uint32_t> &out = f.units[u].pp_events;
	    out.push_back(h.name_id);
	    out.push_back(h.tag_flags);
	    out.push_back(h.body_id);
	    out.push_back(h.variadic_param_id);
	    out.push_back(h.nparams);
	    if (ev.tag == Program::PackMacroEvent::peDefineFn)
		for (size_t p = 0; p < ev.macro.params.size(); ++p)
		    out.push_back(pool->intern(ev.macro.params[p]));
	}
    }

    // 4. Include edges (directory unit indices, include order). Targets are
    //    ensured FIRST — ensure_unit may reallocate f.units.
    for (std::map<const char *, std::vector<const char *> >::const_iterator
	     ee = prog->pack_unit_edges.begin();
	 ee != prog->pack_unit_edges.end(); ++ee) {
	uint32_t u = ensure_unit(ee->first);
	std::vector<uint32_t> tgts;
	for (size_t k = 0; k < ee->second.size(); ++k)
	    tgts.push_back(ensure_unit(ee->second[k]));
	f.units[u].edges = tgts;
    }

    // 5. Branch-relevant macro names (sorted ids for reproducible bytes).
    for (std::set<std::string>::const_iterator bm = prog->pack_branch_macros.begin();
	 bm != prog->pack_branch_macros.end(); ++bm)
	f.branch_macros.push_back(pool->intern(*bm));
    std::sort(f.branch_macros.begin(), f.branch_macros.end());

    // 6. Canonical unit order = first-tokenization order (the pack driver's
    //    include list IS the canonical system order; design doc §6).
    for (size_t k = 0; k < prog->pack_unit_order.size(); ++k)
	f.canon_order.push_back(ensure_unit(prog->pack_unit_order[k]));

    // 7. Serialize per-unit token slices last (unit set is final now).
    unit_toks.resize(f.units.size());
    for (size_t u = 0; u < f.units.size(); ++u) {
	if (unit_toks[u].empty())
	    continue;
	madc_pch::serialize_token_seq(unit_toks[u], f.units[u].token_payload);
	f.units[u].token_count = (uint32_t)unit_toks[u].size();
    }
}

// Serialize the project type table into f.type_records + f.type_payload (Phase 6;
// 2026-06-12 type-table design §6.4 "forest type-refs serialize as ids + table
// segments"). Each file-scope typedef, each named struct/union, and each named
// non-polymorphic class becomes ONE cir_forest_type_record; the aggregate's data
// members ride the type_payload stream as VERBATIM cir_forest_type_member records
// (offset / count / access / origin / bitfield are the values the parser already
// computed, loaded as-is on the far side), and a class's direct bases ride it as
// cir_forest_type_base records (subobject offset / access / is_primary, verbatim).
// Because layout is loaded, never re-derived, unnamed-bitfield gaps, #pragma pack,
// reverse storage, and base subobject offsets all survive for free. Pointer fields
// are ids: a member/base type is a typeid (a pinned primitive slot, or the
// aggregate's own record id) swizzled back to DataDef* at load. top_decls is
// definition order, so a value-member or base aggregate is recorded before the
// aggregate that uses it. A class is skipped WHOLE — bind then cleanly lacks it (a
// loud error at the use site) rather than mis-linking — when it carries state this
// slice does not yet serialize: a vtable (polymorphic), a union layout, a virtual
// or not-yet-recorded base, or a member of an unserializable type (pointer / fnptr
// / anon aggregate). Binding a class's methods to their real Itanium symbols is a
// follow-on; this slice covers the data layout (the type), not method dispatch.
// The container stays Program-blind; this is the Program->payload bridge (the same
// seam as cir_forest_fill_pack_payloads). Names intern into the ACTIVE pool that
// cir_forest_write serializes.
// Ensure `dd` — a type reached as a member / method-param / method-return /
// typedef-underlying of a serialized aggregate — will RESOLVE on load, recording a
// derived-type record (pointer / reference / const) for it if needed, transitively.
// A pinned primitive (incl. void*/char*/int*) resolves via madc_type_from_id; an
// already-`recorded` aggregate (or `self`, the aggregate being recorded right now,
// so a self-referential `Node *next` works) resolves via the load swizzle map; a
// derived type gets its OWN record (kind + ref0 = operand typeid, no member payload)
// after its operand is ensured — the SAME "serialize the table entry, pointer field
// as an id, swizzle on load" discipline as a typedef record, one mechanism widened
// (NOT a parallel format). Returns false — the caller then bails/skips — when the
// type is an UNRECORDED aggregate (the outer fixpoint retries it) or an opaque type
// with no reconstructable content. Any derived record emitted for a type whose
// aggregate later bails is a harmless orphan (deduped by `recorded`, unreferenced on
// load); records are order-independent (load reconstructs derived types in a
// fixpoint), so emitting mid-serialize is safe.
static bool cir_forest_record_derived(DataDef *dd, DataDef *self,
				      madc::dis::intern_table &pool,
				      std::set<DataDef *> &recorded,
				      cir_frozen_forest &f)
{
	if (!dd)
		return false;
	uint32_t tid = madc_type_id_for(dd);
	if (!tid)
		return false;
	if (tid < MADC_TYPEID_PRIMITIVE_END)	// pinned primitive (void*/char*/int* included)
		return true;
	if (dd == self || recorded.count(dd))	// self-reference, or already has a record
		return true;

	// A derived type over an operand. DataDefREF IS-A DataDefPTR, so test the
	// reference first; anything that is none of these is an unrecorded aggregate
	// (fixpoint retries) or opaque (not serializable here).
	uint32_t kind;
	DataDef *operand;
	if (DataDefREF *rf = dynamic_cast<DataDefREF *>(dd)) {
		kind = CIR_TYPEK_REFERENCE; operand = rf->base_type;
	} else if (DataDefPTR *pt = dynamic_cast<DataDefPTR *>(dd)) {
		kind = CIR_TYPEK_POINTER;   operand = pt->base_type;
	} else if (DataDefCONST *cs = dynamic_cast<DataDefCONST *>(dd)) {
		kind = CIR_TYPEK_CONST;     operand = cs->base_type;
	} else {
		return false;
	}
	if (!operand || !cir_forest_record_derived(operand, self, pool, recorded, f))
		return false;			// operand not (yet) serializable

	cir_forest_type_record r;
	memset(&r, 0, sizeof(r));
	r.type_id = tid;
	r.kind    = kind;
	r.name_id = pool.intern(dd->name);
	r.ref0    = madc_type_id_for(operand);
	f.type_records.push_back(r);
	recorded.insert(dd);
	return true;
}

// Serialize sdd's data members into `payload` as VERBATIM cir_forest_type_member
// records (offset / count / access / origin / bitfield are the parser's computed
// values, loaded as-is on the far side). A member type rides as a typeid: a pinned
// primitive, an aggregate already in `recorded`, or a derived type (pointer /
// reference / const) recorded on demand by cir_forest_record_derived. Returns false
// (leaving `payload` partial, which the caller discards) if any member type is not
// yet serializable — so the caller skips the whole aggregate rather than mis-linking.
// Shared by the struct and class paths; `self` is the aggregate being recorded (for
// a self-referential pointer) and `f` receives any derived records.
static bool cir_forest_serialize_members(DataDefSTRUCT *sdd,
					 madc::dis::intern_table &pool,
					 std::set<DataDef *> &recorded,
					 cir_frozen_forest &f,
					 std::vector<uint32_t> &payload)
{
	for (size_t m = 0; m < sdd->members.size(); ++m) {
		DataDef *mdd = sdd->members[m].second;
		if (!mdd)
			return false;
		if (!cir_forest_record_derived(mdd, sdd, pool, recorded, f))
			return false;
		uint32_t mtid = madc_type_id_for(mdd);
		cir_forest_type_member tm;
		memset(&tm, 0, sizeof(tm));
		tm.name_id = pool.intern(sdd->members[m].first);
		tm.type_id = mtid;
		tm.offset  = (uint32_t)(m < sdd->member_offsets.size() ? sdd->member_offsets[m] : 0);
		tm.count   = (uint32_t)(m < sdd->member_counts.size() ? sdd->member_counts[m] : 1);
		if (tm.count == 0) tm.count = 1;
		tm.access  = m < sdd->member_access.size() ? sdd->member_access[m] : 0u;
		tm.origin  = m < sdd->member_origin.size() ? (int32_t)sdd->member_origin[m] : -1;
		if (m < sdd->member_bitfields.size()
		    && sdd->member_bitfields[m].is_bitfield) {
			const DataDefSTRUCT::BitFieldInfo &bf = sdd->member_bitfields[m];
			tm.bf_flags = 1u | (bf.is_unsigned ? 2u : 0u)
				    | (bf.reverse_storage ? 4u : 0u);
			tm.bf_bit_offset     = (uint32_t)bf.bit_offset;
			tm.bf_bit_width      = (uint32_t)bf.bit_width;
			tm.bf_storage_offset = (uint32_t)bf.storage_offset;
			tm.bf_storage_size   = (uint32_t)bf.storage_size;
		}
		madc::dis::pod_append(payload, tm);
	}
	return true;
}

// Serialize a class's direct bases into `bpayload` as VERBATIM cir_forest_type_base
// records (subobject offset / access / is_primary). Returns false when the class
// carries base state this slice does not serialize — a virtual base, or a base not
// yet in `recorded` — so the caller skips the whole class.
static bool cir_forest_serialize_bases(DataDefCLASS *cdd,
				       const std::set<DataDef *> &recorded,
				       std::vector<uint32_t> &bpayload)
{
	for (size_t b = 0; b < cdd->bases.size(); ++b) {
		const BaseSpec &bs = cdd->bases[b];
		if (bs.is_virtual || !bs.base)
			return false;
		uint32_t bid = madc_type_id_for(bs.base);
		if (!(bid && recorded.count(bs.base)))
			return false;
		cir_forest_type_base tb;
		memset(&tb, 0, sizeof(tb));
		tb.base_type_id = bid;
		tb.offset       = (uint32_t)bs.offset;
		tb.flags        = bs.is_primary ? 2u : 0u;	// is_virtual bailed above
		tb.access       = bs.access;
		madc::dis::pod_append(bpayload, tb);
	}
	return true;
}

// Build ONE cir_forest_type_record for aggregate sdd (cdd == sdd for a class, NULL
// for a plain struct/union) and append it + its member/base payload to f. Returns
// false WITHOUT touching f when a member (or, for a class, a base) type is not yet
// recorded — the caller retries it in a later fixpoint round, or skips it. On
// success sdd is inserted into `recorded`.
// Append cdd's non-virtual method DECLARATIONS to f.type_payload — the fixed-stride
// records first, then each method's explicit-param typeids — and report the slice
// via method_begin/method_count. Slice 1 covers PLAIN named methods only: ctors /
// dtors / operators / template methods are skipped (individually, not bailing the
// class), as is any method whose return or an explicit param is not a serializable
// typeid (a primitive or an already-recorded aggregate). The hidden __this (param 0
// of a non-static method) is NOT written — load rebuilds it as a pointer to the
// class. All serialized classes are non-polymorphic (the freeze bails on a vtable),
// so every method here is non-virtual.
static void cir_forest_append_methods(DataDefCLASS *cdd, madc::dis::intern_table &pool,
				      std::set<DataDef *> &recorded,
				      cir_frozen_forest &f,
				      uint32_t &method_begin, uint32_t &method_count)
{
	const size_t MSTRIDE = madc::dis::pod_words<cir_forest_type_method>();
	// A param / return type resolves like a member: primitive, recorded aggregate,
	// or a derived type recorded on demand (self = cdd, for a method taking/returning
	// its own class by pointer/reference).
	auto serializable = [&](DataDef *dd) -> bool {
		return cir_forest_record_derived(dd, cdd, pool, recorded, f);
	};
	std::vector<cir_forest_type_method> recs;
	std::vector<std::vector<uint32_t> > params;	// explicit param typeids per method
	for (size_t i = 0; i < cdd->methods.size(); ++i) {
		Variable *mv = cdd->methods[i];
		if (!mv)
			continue;
		FuncDef *fd = dynamic_cast<FuncDef *>(mv->type);
		if (!fd)
			continue;
		const std::string &disp = fd->method_display_name;
		if (disp.empty())
			continue;
		if (disp == cdd->name || disp[0] == '~'
		    || disp.compare(0, 8, "operator") == 0)
			continue;			// ctor / dtor / operator: later slice
		if (fd->is_member_template || !fd->template_param_names.empty())
			continue;			// template method: later slice
		bool is_static = (mv->flags & vfSTATIC) != 0;
		if (!is_static && fd->parameters.empty())
			continue;			// no __this slot -> malformed, skip
		if (!serializable(&fd->returns))
			continue;
		size_t p0 = is_static ? 0 : 1;		// skip the hidden __this
		std::vector<uint32_t> pt;
		bool ok = true;
		for (size_t p = p0; p < fd->parameters.size(); ++p) {
			if (!serializable(fd->parameters[p])) { ok = false; break; }
			pt.push_back(madc_type_id_for(fd->parameters[p]));
		}
		if (!ok)
			continue;
		cir_forest_type_method m;
		memset(&m, 0, sizeof(m));
		m.name_id        = pool.intern(mv->name);
		m.display_id     = pool.intern(disp);
		m.ret_type_id    = madc_type_id_for(&fd->returns);
		m.emit_symbol_id = fd->emit_symbol.empty() ? 0 : pool.intern(fd->emit_symbol);
		m.flags = (fd->is_const_method ? CIR_METHF_CONST : 0u)
			| (fd->is_varargs ? CIR_METHF_VARARGS : 0u)
			| (fd->is_void_params ? CIR_METHF_VOIDPARAMS : 0u)
			| (is_static ? CIR_METHF_STATIC : 0u);
		m.param_count = (uint32_t)pt.size();
		// INLINE vs LIBRARY: if the AST holds a func-def for this method's mangled
		// symbol, its body is Tree-1 content — record where so bind copies it into
		// the consumer's Tree-2 on use. No func-def => LIBRARY method (body in a
		// .so): leave body location zero, load keeps it declaration-only.
		std::map<std::string, std::pair<uint32_t, uint32_t> >::const_iterator bl =
			f.funcdef_locs.find(mv->name);
		if (bl != f.funcdef_locs.end()) {
			m.flags |= CIR_METHF_HAS_BODY;
			m.body_unit = bl->second.first;
			m.body_idx  = bl->second.second;
		}
		recs.push_back(m);
		params.push_back(pt);
	}
	method_begin = (uint32_t)f.type_payload.size();
	method_count = (uint32_t)recs.size();
	// Records first, then the param runs; each record's param_begin is the ABSOLUTE
	// type_payload offset of its run (records occupy method_begin .. param_base).
	uint32_t param_base = method_begin + method_count * (uint32_t)MSTRIDE;
	uint32_t off = param_base;
	for (size_t i = 0; i < recs.size(); ++i) {
		recs[i].param_begin = off;
		off += (uint32_t)params[i].size();
	}
	for (size_t i = 0; i < recs.size(); ++i)
		madc::dis::pod_append(f.type_payload, recs[i]);
	for (size_t i = 0; i < params.size(); ++i)
		f.type_payload.insert(f.type_payload.end(), params[i].begin(), params[i].end());
}

static bool cir_forest_record_aggregate(DataDefSTRUCT *sdd, DataDefCLASS *cdd,
					madc::dis::intern_table &pool,
					std::set<DataDef *> &recorded,
					cir_frozen_forest &f)
{
	std::vector<uint32_t> bpayload;
	if (cdd && !cir_forest_serialize_bases(cdd, recorded, bpayload))
		return false;
	std::vector<uint32_t> payload;
	if (!cir_forest_serialize_members(sdd, pool, recorded, f, payload))
		return false;

	// Anonymous aggregate groups: addAnonymousAggregate flattened each anon
	// union/struct's members into `sdd` for name lookup AND kept this grouping so
	// emission can re-nest a real `union{..}`/`struct{..}` (without it c2mir
	// re-lays-out the flat members and the overlap is lost — a silent miscompile).
	// Record each nameless sub-aggregate ON DEMAND (like a derived-type operand),
	// then a cir_forest_type_anon per group referencing it by id. Done BEFORE the
	// parent's payload slices are laid out, so recording a sub-aggregate (which
	// appends to f.type_payload/type_records) leaves the parent's *_begin offsets
	// contiguous. A sub-aggregate that is not (yet) serializable bails the parent
	// -> the outer fixpoint retries it, exactly like a not-yet-recorded member.
	std::vector<uint32_t> apayload;
	for (size_t i = 0; i < sdd->anonymous_aggregates.size(); ++i) {
		const DataDefSTRUCT::AnonymousAggregateInfo &ag =
			sdd->anonymous_aggregates[i];
		if (!ag.aggregate || ag.aggregate == sdd || ag.member_count == 0)
			continue;		// self / empty: emission skips these too
		DataDefSTRUCT *sub = const_cast<DataDefSTRUCT *>(ag.aggregate);
		if (!recorded.count(sub)
		    && !cir_forest_record_aggregate(sub, dynamic_cast<DataDefCLASS *>(sub),
						    pool, recorded, f))
			return false;		// sub-aggregate not serializable -> retry in fixpoint
		cir_forest_type_anon ta;
		memset(&ta, 0, sizeof(ta));
		ta.first_member      = (uint32_t)ag.first_member;
		ta.member_count      = (uint32_t)ag.member_count;
		ta.offset            = (uint32_t)ag.offset;
		ta.aggregate_type_id = madc_type_id_for(sub);
		madc::dis::pod_append(apayload, ta);
	}

	cir_forest_type_record r;
	memset(&r, 0, sizeof(r));
	r.type_id      = madc_type_id_for(sdd);	// this aggregate's serialization identity
	r.kind         = cdd ? CIR_TYPEK_CLASS
			     : (sdd->union_layout ? CIR_TYPEK_UNION : CIR_TYPEK_STRUCT);
	r.name_id      = pool.intern(sdd->name);
	r.spelling_id  = sdd->canonical_cpp_spelling.empty()
		       ? 0 : pool.intern(sdd->canonical_cpp_spelling);
	r.size         = (uint32_t)sdd->size;
	r.align        = (uint32_t)sdd->alignment();
	r.pack         = (uint32_t)sdd->pack;
	r.tag_align    = (uint32_t)sdd->tag_explicit_align;
	r.flags        = (sdd->union_layout ? CIR_TYPEF_UNION : 0u) | CIR_TYPEF_COMPLETE
		       | (sdd->is_anonymous ? CIR_TYPEF_ANON : 0u)
		       | (sdd->reverse_scalar_storage ? CIR_TYPEF_REVERSE : 0u)
		       | (sdd->has_anon_aggregate ? CIR_TYPEF_HAS_ANONAGG : 0u);
	if (cdd)
		r.flags |= (cdd->from_system_header ? CIR_TYPEF_SYSHDR : 0u)
			 | (cdd->has_user_ctor ? CIR_TYPEF_USER_CTOR : 0u)
			 | (cdd->has_user_dtor ? CIR_TYPEF_USER_DTOR : 0u);
	r.member_begin = (uint32_t)f.type_payload.size();
	r.member_count = (uint32_t)sdd->members.size();
	f.type_payload.insert(f.type_payload.end(), payload.begin(), payload.end());
	if (cdd) {
		r.base_begin = (uint32_t)f.type_payload.size();
		r.base_count = (uint32_t)cdd->bases.size();
		f.type_payload.insert(f.type_payload.end(), bpayload.begin(), bpayload.end());
		cir_forest_append_methods(cdd, pool, recorded, f,
					  r.method_begin, r.method_count);
	}
	r.anon_begin = (uint32_t)f.type_payload.size();
	r.anon_count = (uint32_t)(apayload.size()
				  / madc::dis::pod_words<cir_forest_type_anon>());
	f.type_payload.insert(f.type_payload.end(), apayload.begin(), apayload.end());
	f.type_records.push_back(r);
	recorded.insert(sdd);
	return true;
}

static void cir_forest_fill_type_records(Program *prog, cir_frozen_forest &f)
{
    if (!prog || !TokenBase::_active_strpool)
	return;
    madc::dis::intern_table &pool = *TokenBase::_active_strpool;

    // Aggregates that have a record (so a member ref to one resolves on load). A
    // member type is serializable iff it is a pinned primitive or is in here;
    // populated in definition order, so a by-value member aggregate is present
    // before the struct that uses it.
    std::set<DataDef *> recorded;

    for (size_t i = 0; i < prog->top_decls.size(); ++i) {
	const Program::TopDecl &td = prog->top_decls[i];
	if (td.kind != Program::DeclKind::dkStruct
	    && td.kind != Program::DeclKind::dkUnion)
	    continue;
	DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(td.dd);
	// A struct USED AS A BASE is promoted to a DataDefCLASS and struct_map is
	// repointed while this TopDecl still holds the pre-promotion object. Resolve
	// the REGISTERED type; a class is left to pass 2 (TokenCLASS::parse registers
	// classes in struct_map only, NOT in top_decls).
	datadef_map_iter smi = prog->struct_map.find(td.name);
	if (smi != prog->struct_map.end())
		if (DataDefSTRUCT *reg = dynamic_cast<DataDefSTRUCT *>(smi->second))
			sdd = reg;
	if (!sdd || !sdd->is_complete)
	    continue;
	if (dynamic_cast<DataDefCLASS *>(sdd))
	    continue;			// class: pass 2 (struct_map fixpoint)
	if (recorded.count(sdd))
	    continue;
	cir_forest_record_aggregate(sdd, NULL, pool, recorded, f);
    }

    // Pass 2 — CLASSES from struct_map (their only registry). id-stamping order is
    // NOT definition order, so a single linear pass could visit a derived class
    // before its base; a fixpoint records a class only once its bases + member
    // types are recorded, converging base-before-derived. A class carrying state
    // this slice does not serialize (a vtable / polymorphic, a union layout, or a
    // virtual base) is skipped WHOLE — bind cleanly lacks it (a loud error at the
    // use site) rather than mis-linking.
    std::vector<DataDefCLASS *> classes;
    std::set<DataDefCLASS *> seen;
    for (datadef_map_iter it = prog->struct_map.begin();
	 it != prog->struct_map.end(); ++it) {
	DataDefCLASS *cdd = dynamic_cast<DataDefCLASS *>(it->second);
	if (!cdd || !cdd->is_complete)
	    continue;
	if (cdd->union_layout || cdd->has_vtable || cdd->has_vptr_slot)
	    continue;
	if (seen.insert(cdd).second)		// dedup name aliases -> one object
	    classes.push_back(cdd);
    }
    bool progress = true;
    while (progress) {
	progress = false;
	for (size_t i = 0; i < classes.size(); ++i) {
	    if (recorded.count(classes[i]))
		continue;
	    if (cir_forest_record_aggregate(classes[i], classes[i], pool, recorded, f))
		progress = true;
	}
    }

    // File-scope typedefs -> alias records (ref0 = the underlying type's id).
    for (std::set<std::string>::const_iterator it = prog->user_typedef_names.begin();
	 it != prog->user_typedef_names.end(); ++it) {
	const std::string &name = *it;
	flat_datatype_map_iter dti = prog->datatype_map.find(name);
	if (dti == prog->datatype_map.end() || !*dti)
	    continue;
	DataDef *underlying = &(*dti)->definition;
	if (!underlying)
	    continue;
	if (!cir_forest_record_derived(underlying, NULL, pool, recorded, f))
	    continue;			// underlying not resolvable on load — skip
	uint32_t uid = madc_type_id_for(underlying);
	cir_forest_type_record r;
	memset(&r, 0, sizeof(r));
	r.kind    = CIR_TYPEK_TYPEDEF;
	r.name_id = pool.intern(name);
	r.ref0    = uid;
	f.type_records.push_back(r);
    }

    // Namespace membership: a type's defining namespace lives ONLY in
    // namespace_datatype_map's KEY (no DataDef field carries it), so stamp each
    // record's namespace_id by reverse-walking that map — the verbatim source of
    // truth. A non-template entry's key IS the record name (sdd->name), so load can
    // register namespace_datatype_map[ns][name] using the record name. Guard:
    // stamp only when the ns key interns to the SAME id as the record name (the
    // non-template guarantee) — a template instantiation product (key/name carries
    // '<', or an aliased key) is skipped, a follow-on. First defining namespace wins
    // (map iteration is deterministic).
    std::map<uint32_t, size_t> rec_by_id;
    for (size_t i = 0; i < f.type_records.size(); ++i)
	if (f.type_records[i].type_id)
	    rec_by_id[f.type_records[i].type_id] = i;
    prog->namespace_datatype_map.for_each(
	[&](const char *ns, datatype_map_t &m) -> bool {
	    if (!ns || !*ns)
		return false;
	    for (datatype_map_iter it = m.begin(); it != m.end(); ++it) {
		if (it->first.find('<') != std::string::npos || !it->second)
		    continue;			// template product / empty: follow-on
		uint32_t tid = madc_type_id_for(&it->second->definition);
		std::map<uint32_t, size_t>::iterator ri = rec_by_id.find(tid);
		if (ri == rec_by_id.end())
		    continue;			// type not serialized (e.g. skipped class)
		cir_forest_type_record &r = f.type_records[ri->second];
		if (r.namespace_id)
		    continue;			// first defining namespace wins
		if (pool.intern(it->first) != r.name_id)
		    continue;			// key != record name (aliased/mangled): follow-on
		r.namespace_id = pool.intern(ns);
	    }
	    return false;
	});
}

int madc_cir_freeze(Program *prog, const char *source_name,
		    const char *out_path, bool append)
{
    // Translate needs the c2mir contexts but never compiles: the frozen tree
    // must be PRE-check (c2mir's do_context writes attr scratch into any tree
    // it compiles; post-check trees are single-use).
    MIR_context_t ctx = MIR_init();
    MIR_set_error_func(ctx, cir_mir_error);
    c2mir_init(ctx);
    c2m_ctx_t c2m = cir_init(ctx, /*debug_p=*/false);
    CirBuilder *builder = NULL;
    int rc = -1;

    if (!c2m) {
	fprintf(stderr, "%s: cir_init failed\n", source_name);
    } else {
	node_t tree = cir_translate_guarded(c2m, prog, source_name, builder);
	int nerr = tree ? cir_report_errors(stderr, tree) : 0;
	if (tree && nerr) {
	    fprintf(stderr, "%s: %d untranslatable node(s); not freezing\n",
		    source_name, nerr);
	} else if (tree) {
	    cir_frozen_forest f;
	    if (!cir_freeze_forest(CIR_NODE(tree), source_name, f)) {
		fprintf(stderr, "%s: forest freeze failed\n", source_name);
	    } else {
		f.libs = prog->loaded_lib_paths;
		cir_forest_fill_pack_payloads(prog, f);	// grove payload v2 (B4a)
		cir_forest_fill_type_records(prog, f);	// Phase 6: complete type-table serialization
		PchCompression codec = PchCompression::Zlib;
#ifdef HAVE_ZSTD
		codec = PchCompression::Zstd;
#endif
		madc::dis::snapshot_writer w;
		if (!cir_forest_write(f, w, codec)) {
		    fprintf(stderr, "%s: forest container assembly failed\n",
			    source_name);
		} else if (!(append ? w.append_file(out_path)
				    : w.write_file(out_path))) {
		    fprintf(stderr, "%s: cannot write %s\n", source_name, out_path);
		} else {
		    size_t nrec = 0, ndecl = 0, ntok = 0;
		    for (size_t u = 0; u < f.units.size(); ++u) {
			nrec  += f.units[u].blob.records.size();
			ndecl += f.units[u].decl_index.size();
			ntok  += f.units[u].token_count;
		    }
		    // Status to stderr: program stdout stays clean for the
		    // --freeze-run child's output.
		    if (prog->pack_recording)
			fprintf(stderr, "Froze %s: %zu units, %zu records, "
				"%zu tokens, %zu decl-index entries, "
				"%zu branch macros -> %s%s\n",
				source_name, f.units.size(), nrec, ntok, ndecl,
				f.branch_macros.size(), out_path,
				append ? " (appended)" : "");
		    else
			fprintf(stderr, "Froze %s: %zu units, %zu records -> %s%s\n",
				source_name, f.units.size(), nrec, out_path,
				append ? " (appended)" : "");
		    rc = 0;
		}
	    }
	}
    }

    if (c2m) cir_finish(c2m);
    c2mir_finish(ctx);
    MIR_finish(ctx);
    delete builder;
    return rc;
}

int madc_cir_execute_frozen(const char *container_path,
			    int user_argc, char **user_argv)
{
    const void *image = NULL;
    size_t image_len = 0;
    if (!cir_forest_map_image(container_path, image, image_len)) {
	fprintf(stderr, "madc: %s: cannot map frozen container\n",
		container_path ? container_path : "/proc/self/exe");
	return -1;
    }

    // A frozen run never parses, so no Program binds the string substrate;
    // give the thaw a process-local live pool for its re-interned handles.
    if (!TokenBase::_active_strpool) {
	static madc::dis::intern_table frozen_run_pool;
	TokenBase::_active_strpool = &frozen_run_pool;
    }

    CirJitSession session;
    if (!session.build_frozen(image, image_len, "frozen"))
	return -1;

    bool ok = false;
    int result = session.run_main(user_argc, user_argv, &ok, /*out_secs=*/0);
    if (!ok) {
	fprintf(stderr, "madc: frozen module has no main()\n");
	return -1;
    }
    return result;
}

// --dump-forest: print a container's directory + grove payload v2 surfaces
// (decl index, PP exports, edges, branch macros, canonical order) in a
// stable line-oriented form — the data source for the B4a index-parity and
// -dM oracles, and the forest debugging surface.
static const char *pack_decl_kind_name(uint32_t kind)
{
    switch (kind) {
    case Program::pdkTypedef:   return "type";
    case Program::pdkStruct:    return "struct";
    case Program::pdkClass:     return "class";
    case Program::pdkEnum:      return "enum";
    case Program::pdkFunction:  return "function";
    case Program::pdkVariable:  return "variable";
    case Program::pdkTemplate:  return "template";
    case Program::pdkNamespace: return "namespace";
    default:                    return "other";
    }
}

int madc_cir_dump_forest(const char *container_path)
{
    const void *image = NULL;
    size_t image_len = 0;
    if (!cir_forest_map_image(container_path, image, image_len)) {
	fprintf(stderr, "madc: %s: cannot map frozen container\n",
		container_path ? container_path : "/proc/self/exe");
	return -1;
    }
    if (!TokenBase::_active_strpool) {
	static madc::dis::intern_table dump_pool;
	TokenBase::_active_strpool = &dump_pool;
    }
    CirFrozenForest forest;
    if (!forest.open(image, image_len, /*c2m=*/NULL))
	return -1;

    printf("forest\tunits=%u\n", forest.unit_count());
    for (size_t i = 0; i < forest.libs().size(); ++i)
	printf("lib\t%s\n", forest.libs()[i].c_str());
    const std::vector<uint32_t> &canon = forest.canon_order();
    for (size_t i = 0; i < canon.size(); ++i)
	printf("canon\t%zu\t%s\n", i, forest.unit_name(canon[i]));
    const std::vector<uint32_t> &bm = forest.branch_macros();
    for (size_t i = 0; i < bm.size(); ++i)
	if (const char *s = forest.pool_str(bm[i]))
	    printf("branchmacro\t%s\n", s);

    for (uint32_t u = 0; u < forest.unit_count(); ++u) {
	const char *uname = forest.unit_name(u);
	uint32_t anchor = forest.unit_anchor(u);
	std::vector<uint8_t> toks;
	uint32_t ntok = 0;
	forest.unit_tokens(u, toks, ntok);
	printf("unit\t%u\t%s\ttokens=%u\tanchor=%d\n", u,
	       uname ? uname : "?", ntok,
	       anchor == CIR_FOREST_ANCHOR_NONE ? -1 : (int)anchor);
	std::vector<uint32_t> edges;
	if (forest.unit_edges(u, edges))
	    for (size_t k = 0; k < edges.size(); ++k)
		printf("edge\t%s\t%s\n", uname ? uname : "?",
		       forest.unit_name(edges[k]));
	std::vector<uint32_t> ppe;
	if (forest.unit_pp_events(u, ppe))
	    for (size_t k = 0; k + 5 <= ppe.size(); ) {
		uint32_t name_id = ppe[k], tag = ppe[k + 1] & 0xff;
		uint32_t nparams = ppe[k + 4];
		const char *nm = forest.pool_str(name_id);
		printf("ppexport\t%s\t%s\t%s\n", uname ? uname : "?",
		       tag == Program::PackMacroEvent::peUndef ? "undef"
		       : tag == Program::PackMacroEvent::peDefineFn ? "define-fn"
		       : "define",
		       nm ? nm : "?");
		k += 5 + nparams;
	    }
	std::vector<cir_forest_decl_entry> di;
	if (forest.unit_decl_index(u, di))
	    for (size_t k = 0; k < di.size(); ++k) {
		const char *nm = forest.pool_str(di[k].name_id);
		printf("declindex\t%s\t%s\t%s\t[%u,%u)%s%s\n",
		       uname ? uname : "?",
		       pack_decl_kind_name(di[k].kind), nm ? nm : "?",
		       di[k].slice_begin, di[k].slice_end,
		       (di[k].aux & Program::PACK_DECL_SPANS_UNITS) ? "\tSPANS" : "",
		       (di[k].aux & Program::PACK_DECL_FUZZY_BOUNDS) ? "\tFUZZY" : "");
	    }
    }
    return 0;
}

// Multi-TU project engine: compile each TU in the manifest into its own MIR
// module within one shared MIR_context (and one shared c2m context — c2mir is
// designed to compile multiple TUs into one context and link), accumulate the
// modules, link them all with a single MIR_link, then JIT-run the entry.
//
// Programs and CirBuilders own the node arenas backing their modules, so they
// must outlive MIR_gen()+run — they are held for the whole call and torn down
// only after the run completes.

// A `.c` source is C, not C++ — gcc/clang select the language from the file
// extension, and a build driver must do the same: in C mode the C++ keywords
// (`class`, `new`, `delete`, `this`, …) are NOT reserved, so ordinary C code
// using them as identifiers parses. Matches ".c" exactly (not ".cc"/".cpp"/
// ".cxx"/".C", which are C++).
static bool is_c_source_file(const std::string &path)
{
	return path.size() >= 2 && path.compare(path.size() - 2, 2, ".c") == 0;
}

int madc_project_execute(MadcEngine &engine, const ProjectManifest &manifest,
			 int user_argc, char **user_argv)
{
	if (manifest.tus.empty()) {
		fprintf(stderr, "madc_project_execute: empty manifest\n");
		return -1;
	}

	// Phase 1: tokenize + parse EVERY TU before any MIR/c2m context exists.
	// tokenize()/parse() can throw in this codebase; doing them here keeps
	// every throwing call OUTSIDE the MIR_init()->teardown() bracket below,
	// so a parse failure can never leak the MIR + c2m contexts. (This matches
	// madc_cir_execute's ordering, where tokenize/parse run in main() before
	// the MIR bracket is ever entered.) Programs own the arenas their modules
	// will be built from, so they are held for the whole call.
	struct ParsedTU {
		std::unique_ptr<Program> prog;
		std::string name;
	};
	std::vector<ParsedTU> parsed;
	for (const ProjectTU &tu : manifest.tus) {
		std::unique_ptr<Program> prog = engine.create_program();
		prog->colors = true;
		for (const std::string &inc : tu.include_dirs)
			prog->add_include_dir(inc);
		for (const std::string &d : tu.defines)
			prog->add_cli_define(d);
		if (!tu.std_option.empty())
			prog->set_language_standard_option("--std=" + tu.std_option);
		else if (is_c_source_file(tu.file))
			// No explicit -std and a .c file → gcc's actual default C
			// dialect, gnu17 (C17 base + GNU, no __STRICT_ANSI__) — so
			// C++ keywords stay usable as C identifiers AND glibc's
			// feature gates (timercmp, strdup, …) match plain gcc.
			prog->set_language_standard_option("--std=gnu17");

		TokenProgram *tp = prog->tokenize(tu.file.c_str());
		if (!tp) {
			fprintf(stderr, "%s: tokenize failed\n", tu.file.c_str());
			return -1;	// no MIR/c2m created yet — nothing to tear down
		}
		if (!prog->parse(tp)) {
			fprintf(stderr, "%s: parse failed\n", tu.file.c_str());
			return -1;	// no MIR/c2m created yet — nothing to tear down
		}

		ParsedTU pt;
		pt.prog = std::move(prog);
		pt.name = tu.file;
		parsed.push_back(std::move(pt));
	}

	// Phase 2: now that all parsing is done, enter the MIR bracket. No
	// throwing call sits between MIR_init() and teardown().
	MIR_context_t ctx = MIR_init();
	c2mir_init(ctx);
	MIR_gen_init(ctx);
	MIR_gen_set_optimize_level(ctx, (unsigned)madc_opt_level);

	c2m_ctx_t c2m = cir_init(ctx, false);
	if (!c2m) {
		fprintf(stderr, "madc_project_execute: cir_init failed\n");
		MIR_gen_finish(ctx);
		c2mir_finish(ctx);
		MIR_finish(ctx);
		return -1;
	}

	// Builders must outlive MIR_gen()+run: their arenas back the modules.
	// Hold them (and the already-parsed Programs) for the whole call.
	std::vector<CirBuilder *> builders;
	std::vector<MIR_module_t> modules;
	auto teardown = [&]() {
		for (CirBuilder *b : builders) delete b;
		cir_finish(c2m);
		MIR_gen_finish(ctx);
		c2mir_finish(ctx);
		MIR_finish(ctx);
	};

	for (ParsedTU &pt : parsed) {
		CirBuilder *builder = NULL;
		bool stop = false;
		MIR_module_t mod = build_tu_module(ctx, c2m, pt.prog.get(),
						   pt.name.c_str(),
						   false, false, false,
						   builder, stop);
		builders.push_back(builder);	// may be NULL on failure; delete NULL is safe
		if (!mod) {
			teardown();
			return -1;
		}
		modules.push_back(mod);
	}

	for (MIR_module_t m : modules)
		MIR_load_module(ctx, m);
	MIR_link(ctx, MIR_set_gen_interface, cir_import_resolver);

	// Find the entry symbol across all modules. First match wins; a
	// duplicate entry (e.g. two main()s across TUs) is not diagnosed here.
	void *code = nullptr;
	MIR_module_t entry_mod = nullptr;
	for (MIR_module_t m : modules) {
		for (MIR_item_t item = DLIST_HEAD(MIR_item_t, m->items);
		     item != nullptr; item = DLIST_NEXT(MIR_item_t, item)) {
			if (item->item_type == MIR_func_item &&
			    strcmp(item->u.func->name, manifest.entry.c_str()) == 0) {
				code = MIR_gen(ctx, item);
				entry_mod = m;
				break;
			}
		}
		if (code) break;
	}
	if (!code) {
		fprintf(stderr, "madc_project_execute: entry '%s' not found\n",
			manifest.entry.c_str());
		teardown();
		return -1;
	}

	// Expose the entry's module to the crash handler for JIT symbolization.
	g_jit_module = entry_mod;
	int result = ((int (*)(int, char **))code)(user_argc, user_argv);
	g_jit_module = nullptr;

	teardown();
	return result;
}

// Build the cir_node tree and render it as C source (no compile/run).
// Used by `--emit=c11|mc11`.
int madc_cir_emit(Program *prog, const char *source_name, FILE *out,
		  CirEmitLang lang)
{
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

    // Same validity gate as the compile path: never render a tree containing
    // error nodes (the emitter prints N_IGNORE as nothing, so an error would
    // otherwise emit silently-broken C).
    if (int nerr = cir_report_errors(stderr, tree)) {
	fprintf(stderr, "%s: %d untranslatable node(s); not emitting\n",
		source_name ? source_name : "<source>", nerr);
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
