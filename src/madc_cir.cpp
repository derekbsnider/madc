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

// Phase 6 (design 2026-07-05): collect the parser's decl graph into serializable
// records so a fresh process can RECONSTRUCT the symbol tables on load — never
// re-parse. Slice 1: file-scope typedefs, whose aliased type is a primitive
// (its type-id is pinned and resolves in any process). Widens to struct/class/
// func/template (aggregate types get system-segment ids) in later slices.
// The container stays Program-blind; this is the Program->payload bridge (the
// same seam as cir_forest_fill_pack_payloads). Names intern into the ACTIVE pool
// that cir_forest_write serializes.
static void cir_forest_collect_decls(Program *prog, cir_frozen_forest &f)
{
    if (!prog || !TokenBase::_active_strpool)
	return;
    madc::dis::intern_table &pool = *TokenBase::_active_strpool;

    // --- typedefs (slice 1b): file-scope typedefs of a primitive type. The
    // underlying type-id is pinned and resolves in any process. ---
    for (std::set<std::string>::const_iterator it = prog->user_typedef_names.begin();
	 it != prog->user_typedef_names.end(); ++it) {
	const std::string &name = *it;
	flat_datatype_map_iter dti = prog->datatype_map.find(name);
	if (dti == prog->datatype_map.end() || !*dti)
	    continue;
	DataDef *dd = &(*dti)->definition;
	if (!dd)
	    continue;
	cir_forest_decl_record r;
	r.name_id = pool.intern(name);
	r.kind    = Program::pdkTypedef;
	r.type_id = prog->type_id_for(dd);
	r.ns_id   = 0;		// slice 1: global scope only
	r.aux     = 0;
	r.member_begin = 0;
	r.member_count = 0;
	f.decl_records.push_back(r);
    }

    // --- structs (slice 3a): file-scope PLAIN structs/unions with primitive
    // (or already-serialized forest-aggregate) scalar members. top_decls is in
    // definition order, so a member aggregate is assigned its system id before
    // the struct that uses it. Each forest aggregate gets a stable system-
    // segment id (freeze-local — NOT the process's project id, which the frozen
    // node tree already uses) so member references link inside the container.
    // Anything richer (a class = methods/bases/vtable, anon aggregates, bit-
    // fields, or a member of an unserializable type) is simply not emitted;
    // bind falls back for it. Widens in 3b/3c. ---
    std::map<DataDef *, uint32_t> sys_id;
    uint32_t next_sys = MADC_TYPEID_SYSTEM_BASE;
    for (size_t i = 0; i < prog->top_decls.size(); ++i) {
	const Program::TopDecl &td = prog->top_decls[i];
	if (td.kind != Program::DeclKind::dkStruct
	    && td.kind != Program::DeclKind::dkUnion)
	    continue;
	DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(td.dd);
	if (!sdd || !sdd->is_complete)
	    continue;
	if (dynamic_cast<DataDefCLASS *>(sdd) || sdd->has_anon_aggregate)
	    continue;			// methods/bases/vtable/anon: 3b/3c
	// Serialize members into a scratch triple list; bail the WHOLE struct if
	// any member is not (yet) representable (a struct half-serialized would
	// mis-lay-out on load).
	std::vector<uint32_t> triples;
	bool ok = true;
	for (size_t m = 0; m < sdd->members.size(); ++m) {
	    DataDef *mdd = sdd->members[m].second;
	    bool is_bf = m < sdd->member_bitfields.size()
		      && sdd->member_bitfields[m].is_bitfield;
	    if (!mdd || is_bf) { ok = false; break; }
	    uint32_t mtid;
	    if (mdd->type_id && mdd->type_id < MADC_TYPEID_PRIMITIVE_END) {
		mtid = mdd->type_id;			// pinned primitive
	    } else {
		std::map<DataDef *, uint32_t>::iterator si = sys_id.find(mdd);
		if (si == sys_id.end()) { ok = false; break; }
		mtid = si->second;			// an earlier forest aggregate
	    }
	    uint32_t mcount = m < sdd->member_counts.size()
			    ? (uint32_t)sdd->member_counts[m] : 1;
	    triples.push_back(pool.intern(sdd->members[m].first));
	    triples.push_back(mtid);
	    triples.push_back(mcount ? mcount : 1);
	}
	if (!ok)
	    continue;
	uint32_t my_sys = next_sys++;
	sys_id[sdd] = my_sys;
	cir_forest_decl_record r;
	r.name_id      = pool.intern(sdd->name);
	r.kind         = Program::pdkStruct;
	r.type_id      = my_sys;			// this struct's serialization identity
	r.ns_id        = 0;				// slice 3a: global scope
	r.aux          = sdd->union_layout ? 1u : 0u;
	r.member_begin = (uint32_t)(f.struct_members.size() / 3);
	r.member_count = (uint32_t)(triples.size() / 3);
	f.decl_records.push_back(r);
	f.struct_members.insert(f.struct_members.end(), triples.begin(), triples.end());
    }
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
		cir_forest_collect_decls(prog, f);	// Phase 6: serialized decl graph
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
