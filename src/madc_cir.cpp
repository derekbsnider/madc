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
#include "madc_pch.h"	// v20: template token runs ride the .madh record form

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

// A btSimple scalar arithmetic type that is NOT one of the pinned primitive
// DataDef objects — a class-scope typedef alias resolved at instantiation to a
// fresh DataDef (e.g. std::string::size_type == unsigned long, given a distinct
// PROJECT id) — is byte-identical to the pinned primitive of its rawtype:
// append_type_specs emits a scalar SOLELY from rawtype() (verified: live emits
// `unsigned long _M_string_length`, no `size_type` typedef in the C at all). So
// serialize such a type as its pinned primitive id — it resolves on load via
// madc_type_from_id with NO record, exactly like a real primitive member. Returns
// that pinned slot, or 0 if dd is not a plain scalar-primitive alias. An enum
// (named constants), SIMD vector, unresolved template param, or _Complex is a
// DISTINCT concept with its own serialization — never fold it in here.
static uint32_t forest_pinned_primitive_id(DataDef *dd)
{
	if (!dd || dd->basetype() != BaseType::btSimple)
		return 0;
	// A pointer/reference (DataDefREF IS-A DataDefPTR) or a const-qualified type
	// inherits btSimple and reports is_integer()==true with the POINTEE's rawtype
	// — it is NOT a scalar. Exclude it structurally so the derived-type
	// record path (DK_PTR/DK_REF/DK_CONST) handles it. Likewise an enum (named
	// constants), SIMD vector, template param, or _Complex is its own concept.
	if (dynamic_cast<DataDefPTR *>(dd) || dynamic_cast<DataDefCONST *>(dd)
	    || dynamic_cast<DataDefENUM *>(dd) || dd->is_simd()
	    || dd->is_template_param() || dd->is_complex())
		return 0;
	if (!(dd->is_integer() || dd->is_real()))
		return 0;
	for (uint32_t slot = 1; slot < MADC_TYPEID_PRIMITIVE_END; ++slot) {
		DataDef *p = madc_primitive_for_slot(slot);
		if (p && p->basetype() == BaseType::btSimple
		      && p->rawtype() == dd->rawtype())
			return slot;			// first (== canonical) match; emission is rawtype-driven
	}
	return 0;
}

// The typeid to SERIALIZE for a type reached as a member / operand / param /
// return / typedef-underlying. A pinned primitive serializes as its own slot; a
// scalar-primitive alias normalizes to the pinned slot of its rawtype (see
// forest_pinned_primitive_id); everything else takes its real project/system id.
// Load swizzles the pinned slot back via madc_type_from_id with no record.
static uint32_t forest_serialize_type_id(DataDef *dd)
{
	if (!dd)
		return MADC_TYPEID_INVALID;
	if (dd->type_id && dd->type_id < MADC_TYPEID_PRIMITIVE_END)
		return dd->type_id;		// already a pinned primitive — serialize as-is
	if (uint32_t pinned = forest_pinned_primitive_id(dd))
		return pinned;
	return madc_type_id_for(dd);
}

// B3 write-through (SLICE 1c/1d): record a newly-created PROJECT unary derived type — pointer,
// reference, or const — into this Program's arena, keyed by its own project-id slot. Dispatches
// on the actual type for the record kind and reads the operand from base_type (DataDefREF is-a
// DataDefPTR, so check it FIRST). Reuses the ONE cross-ref policy (forest_serialize_type_id) so
// there is no parallel encoder — the operand is stored as its serialized type_id (a pinned
// primitive as its pinned slot with no record of its own; a project type as its project id,
// whose record a later slice fills). The live DataDef keeps base_type as its read-cache, so its
// read sites are UNCHANGED; only this write dual-populates the record. Called from
// getPointerType / getReferenceType / getConstType iff forest_arena_enabled.
void Program::forest_arena_record_unary(DataDef *dd)
{
	if (!dd)
		return;
	uint32_t kind;
	DataDef *operand;
	if (DataDefREF *rf = dynamic_cast<DataDefREF *>(dd))		// REF is-a PTR: check first
	{
		kind = madc::dis::DK_REF;   operand = rf->base_type;
	}
	else if (DataDefPTR *p = dynamic_cast<DataDefPTR *>(dd))
	{
		kind = madc::dis::DK_PTR;   operand = p->base_type;
	}
	else if (DataDefCONST *k = dynamic_cast<DataDefCONST *>(dd))
	{
		kind = madc::dis::DK_CONST; operand = k->base_type;
	}
	else
		return;			// not a unary derived type — nothing for this method to record

	uint32_t tid = type_id_for(dd);		// project id for the derived type (its arena slot)
	madc::dis::defrec r;
	memset(&r, 0, sizeof(r));
	r.kind     = kind;
	r.name_id  = forest_arena.strings.intern(dd->name.c_str());
	r.size     = (uint32_t)dd->size;
	r.datatype = (uint32_t)dd->rawtype();
	r.ref0     = forest_serialize_type_id(operand);	// operand, as a type-id
	forest_arena.set_def_at(tid, r);
}

// B3 write-through (SLICE 1e): record a COMPLETED project aggregate — struct, union, or class
// — into this Program's arena at its own project-id slot. Called ONCE at the aggregate's
// completion point (TokenSTRUCT::parse after registration; TokenCLASS::parse after the layout
// trio) iff forest_arena_enabled. Per-aggregate + NON-recursive: unlike test_cir_arena's
// arena_ensure spike (which recursively encodes children to prove the schema), production
// stores every cross-ref (member / base / method-FuncDef / vbase / vgroup-owner type) as a
// SERIALIZED type-id via the ONE forest_serialize_type_id policy — the referent records ITSELF
// when ITS parse completes, and the id-addressed arena tolerates a cross-ref to a not-yet-written
// slot (a method's DK_FUNC record, e.g., is filled by slice 1f). So there is no recursion and no
// `seen` set here. The resolve-first payload discipline still holds trivially: forest_serialize_type_id
// appends NOTHING to forest_arena.payload (it only stamps/returns an id), so no cross-ref can
// interleave a run — but each run's child ids are still resolved into a local vector before the
// run is appended, mirroring the spike. The live DataDef keeps its fields as the read-cache, so
// reads are UNCHANGED; only this write dual-populates the record. Field coverage mirrors the
// freeze's cir_forest_record_aggregate; the name-keyed maps + anonymous sub-aggregates are
// follow-on coverage (the structural graph lands first).
void Program::forest_arena_record_aggregate(DataDefSTRUCT *sdd)
{
	if (!sdd)
		return;
	DataDefCLASS *cdd = dynamic_cast<DataDefCLASS *>(sdd);
	uint32_t tid = type_id_for(sdd);	// project id (binds the active table) == arena slot

	madc::dis::defrec r;
	memset(&r, 0, sizeof(r));
	r.kind     = cdd ? madc::dis::DK_CLASS
			 : (sdd->union_layout ? madc::dis::DK_UNION : madc::dis::DK_STRUCT);
	r.name_id  = forest_arena.strings.intern(sdd->name.c_str());
	r.canon_id = sdd->canonical_cpp_spelling.empty()
		   ? 0u : forest_arena.strings.intern(sdd->canonical_cpp_spelling.c_str());
	r.size     = (uint32_t)sdd->size;
	r.datatype = (uint32_t)sdd->rawtype();
	r.pack               = (uint32_t)sdd->pack;
	r.max_align          = (uint32_t)sdd->max_align;
	r.tag_explicit_align = (uint32_t)sdd->tag_explicit_align;
	if (sdd->union_layout)           r.flags |= madc::dis::DF_UNION_LAYOUT;
	if (sdd->is_complete)            r.flags |= madc::dis::DF_IS_COMPLETE;
	if (sdd->is_anonymous)           r.flags |= madc::dis::DF_IS_ANONYMOUS;
	if (sdd->reverse_scalar_storage) r.flags |= madc::dis::DF_REVERSE_SCALAR;
	if (sdd->has_anon_aggregate)     r.flags |= madc::dis::DF_HAS_ANON_AGG;

	// --- anonymous sub-aggregate groups (ensure each nameless sub-aggregate's OWN
	//     record first: addAnonymousAggregate flattens it into the parent DURING the
	//     body parse, so it never reaches a registration/layout hook of its own — the
	//     parent's completion IS its completion point. The recursive call appends the
	//     SUB's runs to payload BEFORE any of this record's runs are captured, so every
	//     run stays contiguous. The anonrec run itself is appended with the other runs
	//     below; without it a bound anon union's overlap is lost (silent miscompile —
	//     the v11 lesson). ---
	std::vector<madc::dis::anonrec> arun;
	for (size_t i = 0; i < sdd->anonymous_aggregates.size(); ++i) {
		const DataDefSTRUCT::AnonymousAggregateInfo &ag = sdd->anonymous_aggregates[i];
		if (!ag.aggregate || ag.aggregate == sdd || ag.member_count == 0)
			continue;		// self / empty: emission skips these too
		DataDefSTRUCT *sub = const_cast<DataDefSTRUCT *>(ag.aggregate);
		uint32_t sub_id = forest_serialize_type_id(sub);
		if (madc::dis::arena_id_is_project(sub_id) && !forest_arena.has_def(sub_id))
			forest_arena_record_aggregate(sub);
		madc::dis::anonrec ar;
		memset(&ar, 0, sizeof(ar));
		ar.first_member = (uint32_t)ag.first_member;
		ar.member_count = (uint32_t)ag.member_count;
		ar.offset       = (uint32_t)ag.offset;
		ar.sub_type_id  = sub_id;
		arun.push_back(ar);
	}

	// --- members (resolve every member type-id first; then the contiguous run) ---
	std::vector<uint32_t> mt(sdd->members.size());
	for (size_t i = 0; i < sdd->members.size(); ++i)
		mt[i] = forest_serialize_type_id(sdd->members[i].second);
	r.members_begin = (uint32_t)forest_arena.payload.size();
	r.members_count = (uint32_t)sdd->members.size();
	for (size_t i = 0; i < sdd->members.size(); ++i) {
		madc::dis::memberrec m;
		memset(&m, 0, sizeof(m));
		m.name_id    = forest_arena.strings.intern(sdd->members[i].first.c_str());
		m.type_id    = mt[i];
		m.typedef_id = sdd->members[i].typedef_name.empty()
			     ? 0u : forest_arena.strings.intern(sdd->members[i].typedef_name.c_str());
		m.offset = (uint32_t)(i < sdd->member_offsets.size() ? sdd->member_offsets[i] : 0);
		m.count  = (uint32_t)(i < sdd->member_counts.size() ? sdd->member_counts[i] : 1);
		m.flags  = (i < sdd->member_array_flags.size() && sdd->member_array_flags[i]) ? 1u : 0u;
		m.access = i < sdd->member_access.size() ? sdd->member_access[i] : 0u;
		m.origin = i < sdd->member_origin.size() ? (int32_t)sdd->member_origin[i] : -1;
		if (i < sdd->member_bitfields.size() && sdd->member_bitfields[i].is_bitfield) {
			const DataDefSTRUCT::BitFieldInfo &bf = sdd->member_bitfields[i];
			m.bf_flags = 1u | (bf.is_unsigned ? 2u : 0u) | (bf.reverse_storage ? 4u : 0u);
			m.bf_bit_offset     = (uint32_t)bf.bit_offset;
			m.bf_bit_width      = (uint32_t)bf.bit_width;
			m.bf_storage_offset = (uint32_t)bf.storage_offset;
			m.bf_storage_size   = (uint32_t)bf.storage_size;
		}
		forest_arena.add_payload(m);
	}

	if (cdd) {
		r.nvsize        = (uint32_t)cdd->nvsize;
		r.class_align   = (uint32_t)cdd->class_align;
		r.own_block_off = (uint32_t)cdd->own_block_off;
		if (cdd->has_vtable)         r.flags |= madc::dis::DF_HAS_VTABLE;
		if (cdd->has_vptr_slot)      r.flags |= madc::dis::DF_HAS_VPTR_SLOT;
		if (cdd->from_system_header) r.flags |= madc::dis::DF_FROM_SYSTEM_HDR;
		if (cdd->has_user_ctor)      r.flags |= madc::dis::DF_HAS_USER_CTOR;
		if (cdd->has_user_dtor)      r.flags |= madc::dis::DF_HAS_USER_DTOR;

		// --- bases (resolve base type-ids first) ---
		std::vector<uint32_t> bid(cdd->bases.size());
		for (size_t i = 0; i < cdd->bases.size(); ++i)
			bid[i] = forest_serialize_type_id(cdd->bases[i].base);
		r.bases_begin = (uint32_t)forest_arena.payload.size();
		r.bases_count = (uint32_t)cdd->bases.size();
		for (size_t i = 0; i < cdd->bases.size(); ++i) {
			madc::dis::baserec b;
			memset(&b, 0, sizeof(b));
			b.base_id = bid[i];
			b.offset  = (uint32_t)cdd->bases[i].offset;
			b.flags   = (cdd->bases[i].is_virtual ? madc::dis::BSF_VIRTUAL : 0u)
				  | (cdd->bases[i].is_primary ? madc::dis::BSF_PRIMARY : 0u)
				  | ((uint32_t)cdd->bases[i].access << madc::dis::BSF_ACCESS_SHIFT);
			forest_arena.add_payload(b);
		}

		// --- methods (resolve each method's FuncDef type-id AND record its DK_FUNC
		//     record now — slice 1f-a. Both happen in this RESOLVE loop, BEFORE the
		//     methodrec run's begin is captured, so recording a method's param run
		//     (which appends to payload) cannot interleave the aggregate's own
		//     contiguous methodrec run. This closes the func_id forward-ref the
		//     methodrec would otherwise leave for a later slice. ---
		std::vector<uint32_t> fid(cdd->methods.size());
		for (size_t i = 0; i < cdd->methods.size(); ++i) {
			DataDef *mt = cdd->methods[i] ? cdd->methods[i]->type : NULL;
			fid[i] = mt ? forest_serialize_type_id(mt) : (uint32_t)MADC_TYPEID_INVALID;
			if (FuncDef *mfd = dynamic_cast<FuncDef *>(mt))
				forest_arena_record_func(mfd);
		}
		// Class-membership classification (structural, name-independent — the
		// v6 freeze's rule): a ctor is in cdd->ctors; the dtor is the "~"
		// method_map key whose Variable is in methods; static via vfSTATIC.
		std::set<Variable *> ctor_set(cdd->ctors.begin(), cdd->ctors.end());
		Variable *dtor_var = NULL;
		std::string dtor_key;
		for (std::map<std::string, Variable *>::const_iterator kv = cdd->method_map.begin();
		     kv != cdd->method_map.end(); ++kv) {
			if (kv->first.empty() || kv->first[0] != '~' || !kv->second)
				continue;
			if (std::find(cdd->methods.begin(), cdd->methods.end(), kv->second)
			    != cdd->methods.end()) {
				dtor_var = kv->second; dtor_key = kv->first; break;
			}
		}
		r.methods_begin = (uint32_t)forest_arena.payload.size();
		r.methods_count = (uint32_t)cdd->methods.size();
		for (size_t i = 0; i < cdd->methods.size(); ++i) {
			Variable *mv = cdd->methods[i];
			madc::dis::methodrec md;
			memset(&md, 0, sizeof(md));
			md.name_id = mv ? forest_arena.strings.intern(mv->name.c_str()) : 0u;
			md.func_id = fid[i];
			bool is_ctor   = mv && ctor_set.count(mv) != 0;
			bool is_dtor   = mv && mv == dtor_var;
			bool is_static = mv && (mv->flags & vfSTATIC) != 0;
			md.flags = (is_ctor ? madc::dis::MF_CTOR : 0u)
				 | (is_dtor ? madc::dis::MF_DTOR : 0u)
				 | (is_static ? madc::dis::MF_STATIC : 0u);
			// method_map KEY: the "~" tag for the dtor; method_display_name for a plain
			// method/operator; empty for a concrete ctor (resolves via cdd->ctors, not by name).
			FuncDef *mfd = mv ? dynamic_cast<FuncDef *>(mv->type) : NULL;
			std::string key = is_dtor ? dtor_key
					: (is_ctor ? std::string()
					   : (mfd ? mfd->method_display_name : std::string()));
			md.disp_key_id = key.empty() ? 0u : forest_arena.strings.intern(key.c_str());
			forest_arena.add_payload(md);
		}

		// --- vbase_offset: flatten the pointer-KEYED map -> a sorted (class_id,offset) run ---
		std::vector<std::pair<uint32_t, uint32_t> > vb;
		for (std::map<DataDefCLASS *, size_t>::const_iterator vi = cdd->vbase_offset.begin();
		     vi != cdd->vbase_offset.end(); ++vi)
			vb.push_back(std::make_pair(forest_serialize_type_id(vi->first),
						    (uint32_t)vi->second));
		std::sort(vb.begin(), vb.end());
		r.vbase_begin = (uint32_t)forest_arena.payload.size();
		r.vbase_count = (uint32_t)vb.size();
		for (size_t i = 0; i < vb.size(); ++i) {
			madc::dis::vbaserec vr;
			memset(&vr, 0, sizeof(vr));
			vr.class_id = vb[i].first;
			vr.offset   = vb[i].second;
			forest_arena.add_payload(vr);
		}

		// --- vtable_groups: nested vector -> two-level slice. Resolve owners first; append
		//     each group's slot-id run (recording slots_begin); THEN the vgrouprec run. ---
		std::vector<uint32_t> owners(cdd->vtable_groups.size());
		for (size_t g = 0; g < cdd->vtable_groups.size(); ++g)
			owners[g] = forest_serialize_type_id(cdd->vtable_groups[g].owner);
		std::vector<madc::dis::vgrouprec> vgs(cdd->vtable_groups.size());
		for (size_t g = 0; g < cdd->vtable_groups.size(); ++g) {
			const DataDefCLASS::VtableGroup &vg = cdd->vtable_groups[g];
			uint32_t sbegin = (uint32_t)forest_arena.payload.size();
			for (size_t k = 0; k < vg.slots.size(); ++k)
				forest_arena.add_word(forest_arena.strings.intern(vg.slots[k].c_str()));
			memset(&vgs[g], 0, sizeof(vgs[g]));
			vgs[g].owner_id    = owners[g];
			vgs[g].this_offset = (uint32_t)vg.this_offset;
			vgs[g].slots_begin = sbegin;
			vgs[g].slots_count = (uint32_t)vg.slots.size();
			vgs[g].addr_point  = (uint32_t)vg.addr_point;
		}
		r.vgroup_begin = (uint32_t)forest_arena.payload.size();
		r.vgroup_count = (uint32_t)vgs.size();
		for (size_t g = 0; g < vgs.size(); ++g)
			forest_arena.add_payload(vgs[g]);

		// --- class-scope name maps (v20): type aliases, static member types,
		//     static-const values. Resolve every type-id FIRST (lazy stamping
		//     appends nothing to payload, but keep the discipline uniform),
		//     then append each run contiguously. ---
		std::vector<std::pair<uint32_t, uint32_t> > als;
		for (std::map<std::string, DataDef *>::const_iterator ai = cdd->type_aliases.begin();
		     ai != cdd->type_aliases.end(); ++ai)
			als.push_back(std::make_pair(
				forest_arena.strings.intern(ai->first.c_str()),
				ai->second ? forest_serialize_type_id(ai->second) : 0u));
		r.alias_begin = (uint32_t)forest_arena.payload.size();
		r.alias_count = (uint32_t)als.size();
		for (size_t i = 0; i < als.size(); ++i) {
			madc::dis::aliasrec ar;
			memset(&ar, 0, sizeof(ar));
			ar.name_id = als[i].first;
			ar.type_id = als[i].second;
			forest_arena.add_payload(ar);
		}
		std::vector<std::pair<uint32_t, uint32_t> > sts;
		for (std::map<std::string, DataDef *>::const_iterator si = cdd->static_member_types.begin();
		     si != cdd->static_member_types.end(); ++si)
			sts.push_back(std::make_pair(
				forest_arena.strings.intern(si->first.c_str()),
				si->second ? forest_serialize_type_id(si->second) : 0u));
		r.statty_begin = (uint32_t)forest_arena.payload.size();
		r.statty_count = (uint32_t)sts.size();
		for (size_t i = 0; i < sts.size(); ++i) {
			madc::dis::aliasrec ar;
			memset(&ar, 0, sizeof(ar));
			ar.name_id = sts[i].first;
			ar.type_id = sts[i].second;
			forest_arena.add_payload(ar);
		}
		r.constval_begin = (uint32_t)forest_arena.payload.size();
		r.constval_count = (uint32_t)cdd->static_member_const_values.size();
		for (std::map<std::string, int64_t>::const_iterator ci =
			 cdd->static_member_const_values.begin();
		     ci != cdd->static_member_const_values.end(); ++ci) {
			madc::dis::constvalrec cr;
			memset(&cr, 0, sizeof(cr));
			cr.name_id = forest_arena.strings.intern(ci->first.c_str());
			cr.val_lo  = (uint32_t)((uint64_t)ci->second & 0xffffffffu);
			cr.val_hi  = (uint32_t)((uint64_t)ci->second >> 32);
			forest_arena.add_payload(cr);
		}
	}

	// --- anonymous sub-aggregate run (entries collected above, before any run of
	//     this record was captured — the recursive sub-record calls appended payload) ---
	r.anon_begin = (uint32_t)forest_arena.payload.size();
	r.anon_count = (uint32_t)arun.size();
	for (size_t i = 0; i < arun.size(); ++i)
		forest_arena.add_payload(arun[i]);

	forest_arena.set_def_at(tid, r);
}

// B3 write-through (SLICE 1f): record a FuncDef as a DK_FUNC record at its project-id slot —
// ref0 = the return type-id, a params run of paramrec (type-id + const flag + cpp-spelling), and
// the signature flags. Reuses the ONE forest_serialize_type_id policy for the return + every
// parameter type (no parallel encoder). Stores the parameters VERBATIM, INCLUDING a method's hidden
// __this (param 0): the record mirrors the live FuncDef's complete signature, and the eventual
// read-flip reconstructs the same list — B3's store-complete-state model, distinct from the freeze's
// rebuild-__this-on-load. Called from forest_arena_record_aggregate for each class method (1f-a,
// which closes the methodrec.func_id forward-ref); free functions route through it at their own
// parse-completion in a follow-on. Resolve-first: param type-ids resolve into a local vector before
// the paramrec run is appended (forest_serialize_type_id touches no payload, so this is not strictly
// required here, but it keeps the one payload discipline uniform).
void Program::forest_arena_record_func(FuncDef *fd)
{
	if (!fd)
		return;
	uint32_t tid = type_id_for(fd);		// project id for the FuncDef == its arena slot

	std::vector<uint32_t> pt(fd->parameters.size());
	for (size_t p = 0; p < fd->parameters.size(); ++p)
		pt[p] = forest_serialize_type_id(fd->parameters[p]);

	madc::dis::defrec r;
	memset(&r, 0, sizeof(r));
	r.kind    = madc::dis::DK_FUNC;
	r.name_id = fd->name.empty() ? 0u : forest_arena.strings.intern(fd->name.c_str());
	r.ref0    = forest_serialize_type_id(&fd->returns);	// return type, as a type-id
	if (fd->is_varargs)       r.flags |= madc::dis::DF_IS_VARARGS;
	if (fd->is_void_params)   r.flags |= madc::dis::DF_IS_VOID_PARAMS;
	if (fd->declaration_only) r.flags |= madc::dis::DF_DECLARATION_ONLY;
	if (fd->is_const_method)  r.flags |= madc::dis::DF_IS_CONST_METHOD;
	if (fd->is_member_template || !fd->template_param_names.empty())
		r.flags |= madc::dis::DF_IS_MEMBER_TEMPLATE;	// load skips it (the v6 rule)
	// FuncDef-intrinsic method metadata available at parse time (emit_symbol / display name).
	// The INLINE-body location (body_unit/body_idx + DF_HAS_FOREST_BODY) is FREEZE-time info
	// (it indexes the partitioned grove), stamped by a freeze-time fixup — not here.
	r.emit_symbol_id = fd->emit_symbol.empty()
			 ? 0u : forest_arena.strings.intern(fd->emit_symbol.c_str());
	r.disp_id        = fd->method_display_name.empty()
			 ? 0u : forest_arena.strings.intern(fd->method_display_name.c_str());
	r.params_begin = (uint32_t)forest_arena.payload.size();
	r.params_count = (uint32_t)fd->parameters.size();
	for (size_t p = 0; p < fd->parameters.size(); ++p) {
		madc::dis::paramrec pr;
		memset(&pr, 0, sizeof(pr));
		pr.type_id         = pt[p];
		pr.flags           = (p < fd->const_params.size() && fd->const_params[p]) ? 1u : 0u;
		pr.cpp_spelling_id = (p < fd->param_cpp_spellings.size()
				      && !fd->param_cpp_spellings[p].empty())
				   ? forest_arena.strings.intern(fd->param_cpp_spellings[p].c_str()) : 0u;
		forest_arena.add_payload(pr);
	}
	forest_arena.set_def_at(tid, r);
}

// File-scope global VARIABLE definitions (v13/v14/v16) — the CIR_GLOBALS
// segment, which survives the v18 flip (the arena replaced only the type-graph
// records). A header's file-scope globals are a separate category from types,
// so binding <string> used to omit its inline globals (in_place,
// piecewise_construct, ...) and the __madc_global_init that runs their ctors.
// Serialize each file-scope CLASS-typed global (same predicate as
// collect_global_ctors: not a non-static local, not extern) whose class has an
// arena record — load swizzles the type-id through the arena reconstruct, and
// a class the load-side selection drops makes its global cleanly LACK, exactly
// as the retired v6 save-side filter did. No initializer is stored for a class
// global: collect_global_ctors synthesizes the default ctor from the class's
// restored ctor set (v12); the initializer FORM rides gflags (v16) and a
// scalar-const's VALUE rides init_value (v14). Dedup by name (a global appears
// once). Names intern into the ACTIVE pool that cir_forest_write serializes.
static void cir_forest_fill_globals(Program *prog, cir_frozen_forest &f)
{
    if (!prog || !TokenBase::_active_strpool || !prog->tkProgram)
	return;
    madc::dis::intern_table &pool = *TokenBase::_active_strpool;
    std::set<std::string> seen_globals;
    for (Variable *v : prog->tkProgram->variables) {
	if (!v || !v->type)
	    continue;
	if ((v->flags & vfLOCAL) && !(v->flags & vfSTATIC))
	    continue;		// a non-static local is not file-scope
	if (v->flags & vfEXTERN)
	    continue;		// a reference to a definition elsewhere
	DataDefCLASS *cdd = dynamic_cast<DataDefCLASS *>(v->type);
	if (cdd) {
	    uint32_t ctid = forest_serialize_type_id(cdd);
	    if (!madc::dis::arena_id_is_project(ctid)
		|| !prog->forest_arena.has_def(ctid))
		continue;	// class not in the arena -> its global cleanly lacks
	    if (!seen_globals.insert(v->name).second)
		continue;
	    cir_forest_global_record g;
	    memset(&g, 0, sizeof(g));
	    g.name_id = pool.intern(v->name);
	    g.type_id = ctid;
	    g.flags   = v->flags;
	    // v16: classify the header's initializer FORM so the load rebuilds a
	    // TokenDecl whose emission is byte-identical to a live parse (v13 stored
	    // NO form -> flush set decl=NULL -> collect_global_ctors' built-in path
	    // default-constructed DIRECTLY on the global; a live parse builds a stack
	    // temp for `T x = T()` or a trivially-copyable self-copy for `T x{}`).
	    // Inspect the dkGlobalVar TopDecl's assign RHS structurally (no
	    // name-keying): a functional-construction temporary `T()` with NO args ->
	    // COPY_TEMP; the variable itself (value-init `T x{}`) -> VALUE_INIT. Any
	    // other/absent form leaves gflags 0 -> v13's default-ctor synthesis.
	    for (auto &t : prog->top_decls) {
		if (t.kind != Program::DeclKind::dkGlobalVar || t.var != v)
			continue;
		TokenBase *rhs = t.decl ? t.decl->initialize : NULL;
		if (TokenAssign *as = dynamic_cast<TokenAssign *>(rhs))
			rhs = as->right;
		if (TokenObjTemp *ot = dynamic_cast<TokenObjTemp *>(rhs)) {
			if (ot->ctor_args.empty())
				g.gflags = CIR_GLOBALF_CLASS_COPY_TEMP;
		} else if (TokenVar *tv = dynamic_cast<TokenVar *>(rhs)) {
			if (&tv->var == v)	// value-init RHS is the variable itself
				g.gflags = CIR_GLOBALF_CLASS_VALUE_INIT;
		}
		break;
	    }
	    f.globals.push_back(g);
	    continue;
	}
	// v14: a SCALAR-const file-scope global (e.g. hardware_*_interference_size
	// = 64). No ctor runs — its init is a compile-time constant baked into the
	// data segment (live emits `u64 64`), so serialize the VALUE. Only a global
	// that the live dkGlobalVar pass emits qualifies: it has a dkGlobalVar
	// TopDecl whose initializer folds to an integer literal. A function
	// (FuncDef-typed Variable), a non-foldable / non-integer initializer, or an
	// unresolvable type cleanly LACKS (same discipline as the class path).
	if (v->type->is_function())
	    continue;
	TokenDecl *td = NULL;
	for (auto &t : prog->top_decls)
	    if (t.kind == Program::DeclKind::dkGlobalVar && t.var == v) {
		td = t.decl;
		break;
	    }
	if (!td || !td->initialize)
	    continue;
	TokenBase *rhs = td->initialize;
	if (TokenAssign *as = dynamic_cast<TokenAssign *>(rhs))
	    rhs = as->right;
	if (!rhs || !rhs->is_constant() || rhs->id() != TokenID::tkInt)
	    continue;
	uint32_t sid = forest_serialize_type_id(v->type);
	if (!sid)
	    continue;
	if (!seen_globals.insert(v->name).second)
	    continue;
	cir_forest_global_record g;
	memset(&g, 0, sizeof(g));
	g.name_id    = pool.intern(v->name);
	g.type_id    = sid;
	g.flags      = v->flags;
	g.gflags     = CIR_GLOBALF_SCALAR_INIT;
	g.init_value = rhs->ival();
	f.globals.push_back(g);
    }
}

// v20 (widening slice 2): serialize the parser's TEMPLATE-NAME state — the
// template pattern maps — VERBATIM. Live keeps each captured pattern as cloned
// TOKENS (the Borland model: instantiation clones + substitutes the saved
// tokens), so the state serializes as .madh-form token runs
// (madc_pch::serialize_token_seq — the B4a record form) + POD metadata (params /
// flags / namespace / owner type-id). Load restores the maps before the
// consumer parses; the UNCHANGED live instantiation machinery then runs — an
// exact-match use memo-hits the restored product, a new specialization
// instantiates through the same path. A pattern whose owner class is not in the
// arena cleanly lacks at load (same discipline as a dropped class's global).
static void cir_forest_fill_templates(Program *prog, cir_frozen_forest &f)
{
    if (!prog || !TokenBase::_active_strpool)
	return;
    madc::dis::intern_table &pool = *TokenBase::_active_strpool;

    // One captured token sequence -> a token-run descriptor + its bytes in
    // f.template_tokens. file_id = the sequence's first token's origin file
    // (instantiate_template_use points _parse_file at exactly that token, so
    // one file per run reproduces the live provenance).
    auto run_of = [&](const std::vector<TokenBase *> &toks) -> cir_forest_token_run {
	cir_forest_token_run r;
	r.tok_off = (uint32_t)f.template_tokens.size();
	r.tok_bytes = 0;
	r.tok_count = 0;
	r.file_id = 0;
	if (toks.empty())
	    return r;
	std::vector<uint8_t> bytes;
	if (!madc_pch::serialize_token_seq(toks, bytes))
	    return r;
	uint32_t cnt = 0;
	for (TokenBase *t : toks)
	    if (t)
		++cnt;
	for (TokenBase *t : toks)
	    if (t && t->file) {
		r.file_id = pool.intern(t->file);
		break;
	    }
	r.tok_bytes = (uint32_t)bytes.size();
	r.tok_count = cnt;
	f.template_tokens.insert(f.template_tokens.end(), bytes.begin(), bytes.end());
	return r;
    };

    // Emit one record: params first, then the positional run table
    // (body, constraint, per-param defaults, per-slot spec patterns) — both as
    // contiguous pod_append slices into f.template_payload.
    auto emit = [&](uint32_t kind, const char *key, const std::string &name,
		    const std::string &ns, const std::string &extra,
		    DataDefCLASS *owner, uint32_t flags,
		    const std::vector<std::string> &typeparams,
		    const std::vector<bool> &is_type,
		    const std::vector<bool> &is_pack,
		    const std::vector<std::vector<TokenBase *> > &defaults,
		    const std::vector<TokenBase *> &body,
		    const std::vector<TokenBase *> &constraint,
		    const std::vector<std::vector<TokenBase *> > &spec) {
	cir_forest_template_record r;
	memset(&r, 0, sizeof(r));
	r.kind    = kind;
	r.key_id  = pool.intern(key);
	r.name_id = name.empty() ? 0 : pool.intern(name);
	r.ns_id   = ns.empty() ? 0 : pool.intern(ns);
	r.extra_id = extra.empty() ? 0 : pool.intern(extra);
	r.owner_type_id = owner ? forest_serialize_type_id(owner) : 0;
	r.flags   = flags;
	// Serialize every run's TOKEN BYTES before appending any payload words,
	// then lay the params + run table down contiguously (resolve-first).
	std::vector<cir_forest_token_run> runs;
	runs.push_back(run_of(body));
	runs.push_back(run_of(constraint));
	for (size_t i = 0; i < typeparams.size(); ++i)
	    runs.push_back(i < defaults.size() ? run_of(defaults[i])
					       : run_of(std::vector<TokenBase *>()));
	for (size_t i = 0; i < spec.size(); ++i)
	    runs.push_back(run_of(spec[i]));
	r.param_count = (uint32_t)typeparams.size();
	r.spec_count  = (uint32_t)spec.size();
	bool first = true;
	for (size_t i = 0; i < typeparams.size(); ++i) {
	    cir_forest_template_param p;
	    p.name_id = pool.intern(typeparams[i]);
	    p.pflags  = 0;
	    if (i < is_type.size() && is_type[i])
		p.pflags |= CIR_TMPLP_IS_TYPE;
	    if (i < is_pack.size() && is_pack[i])
		p.pflags |= CIR_TMPLP_IS_PACK;
	    uint32_t off = madc::dis::pod_append(f.template_payload, p);
	    if (first) {
		r.param_begin = off;
		first = false;
	    }
	}
	first = true;
	for (size_t i = 0; i < runs.size(); ++i) {
	    uint32_t off = madc::dis::pod_append(f.template_payload, runs[i]);
	    if (first) {
		r.run_begin = off;
		first = false;
	    }
	}
	f.templates.push_back(r);
    };

    static const std::vector<std::vector<TokenBase *> > no_multi;
    static const std::vector<TokenBase *> no_toks;
    static const std::vector<bool> no_bools;

    prog->template_map.for_each([&](const char *key, std::vector<Program::TemplateDef> &v) {
	for (Program::TemplateDef &td : v)
	    emit(CIR_TMPLK_CLASS, key, td.class_name, td.defining_namespace,
		 std::string(), td.owner_class,
		 (td.has_non_type_params ? CIR_TMPLF_HAS_NON_TYPE_PARAMS : 0)
		 | (td.is_partial_specialization ? CIR_TMPLF_IS_PARTIAL_SPEC : 0),
		 td.typeparams, td.typeparam_is_type, td.typeparam_is_pack,
		 td.typeparam_defaults, td.body, td.constraint, td.spec_pattern);
	return false;
    });
    prog->partial_spec_map.for_each([&](const char *key, std::vector<Program::TemplateDef> &v) {
	for (Program::TemplateDef &td : v)
	    emit(CIR_TMPLK_PARTIAL, key, td.class_name, td.defining_namespace,
		 std::string(), td.owner_class,
		 (td.has_non_type_params ? CIR_TMPLF_HAS_NON_TYPE_PARAMS : 0)
		 | (td.is_partial_specialization ? CIR_TMPLF_IS_PARTIAL_SPEC : 0),
		 td.typeparams, td.typeparam_is_type, td.typeparam_is_pack,
		 td.typeparam_defaults, td.body, td.constraint, td.spec_pattern);
	return false;
    });
    prog->template_alias_map.for_each([&](const char *key, std::vector<Program::TemplateAliasDef> &v) {
	for (Program::TemplateAliasDef &ad : v)
	    emit(CIR_TMPLK_ALIAS, key, ad.alias_name, ad.defining_namespace,
		 std::string(), ad.owner_class,
		 ad.has_non_type_params ? CIR_TMPLF_HAS_NON_TYPE_PARAMS : 0,
		 ad.typeparams, ad.typeparam_is_type, ad.typeparam_is_pack,
		 ad.typeparam_defaults, ad.target, no_toks, no_multi);
	return false;
    });
    prog->fn_template_map.for_each([&](const char *key, std::vector<Program::FnTemplateDef> &v) {
	for (Program::FnTemplateDef &fd : v)
	    emit(CIR_TMPLK_FN, key, std::string(), fd.ns, fd.inline_builtin_kind,
		 fd.owner_class,
		 fd.instance_method ? CIR_TMPLF_INSTANCE_METHOD : 0,
		 fd.typeparams, fd.typeparam_is_type, fd.typeparam_is_pack,
		 fd.typeparam_defaults, fd.decl, no_toks, no_multi);
	return false;
    });
    prog->fn_template_decl_map.for_each([&](const char *key, std::vector<Program::FnTemplateDef> &v) {
	for (Program::FnTemplateDef &fd : v)
	    emit(CIR_TMPLK_FN_DECL, key, std::string(), fd.ns, fd.inline_builtin_kind,
		 fd.owner_class,
		 fd.instance_method ? CIR_TMPLF_INSTANCE_METHOD : 0,
		 fd.typeparams, fd.typeparam_is_type, fd.typeparam_is_pack,
		 fd.typeparam_defaults, fd.decl, no_toks, no_multi);
	return false;
    });
    prog->var_template_map.for_each([&](const char *key, Program::VarTemplateDef &vd) {
	emit(CIR_TMPLK_VAR, key, std::string(), vd.defining_namespace,
	     std::string(), NULL, 0,
	     vd.typeparams, no_bools, vd.typeparam_is_pack,
	     no_multi, vd.init, no_toks, no_multi);
	return false;
    });
    for (std::map<std::string, Program::ConceptDef>::iterator ci =
	     prog->concept_map.begin(); ci != prog->concept_map.end(); ++ci)
	emit(CIR_TMPLK_CONCEPT, ci->first.c_str(), std::string(),
	     ci->second.defining_namespace, std::string(), NULL, 0,
	     ci->second.typeparams, no_bools, no_bools,
	     no_multi, no_toks, ci->second.constraint, no_multi);
}

// B3: freeze-time COMPLETION of the arena copy staged in f.arena. Three
// fidelity categories live in freeze-time or name-keyed state the parse-time
// write-throughs cannot see, so they are stamped here (the retired v6 walks
// used to gather the same three at freeze time):
//   1. INLINE method body locations: f.funcdef_locs (mangled symbol -> unit/idx)
//      exists only AFTER grove partitioning. A class methodrec whose symbol has a
//      func-def in the AST is INLINE -> its DK_FUNC record gets DF_HAS_FOREST_BODY
//      + body_unit/body_idx; absent = LIBRARY (declaration-only + emit_symbol).
//   2. FLAT file-scope typedefs: user_typedef_names maps a NAME to an underlying
//      type — no DataDef of its own, hence no project slot — so each becomes a
//      DK_TYPEDEF record at a SYNTHETIC slot past the live table (nothing
//      cross-references a typedef BY id; the record is name/ns-keyed content).
//      An underlying the arena cannot resolve (e.g. an enum, its own follow-on
//      kind) cleanly lacks, exactly as the v6 walk skips it.
//   3. Namespace membership: a type's defining namespace lives ONLY in
//      namespace_datatype_map's KEY. Stamp ns_id on the type's own record when the
//      key IS the record name (the non-template guarantee); a namespaced ALIAS to a
//      recorded aggregate (std::string -> the basic_string<char,...> product) emits
//      a namespaced DK_TYPEDEF record instead. A '<'-bearing template-product key
//      is skipped (the v10 follow-on, unchanged).
static void cir_forest_arena_complete(Program *prog, cir_frozen_forest &f)
{
	if (!prog || !prog->forest_arena_enabled)
		return;
	madc::dis::DefArena &a = f.arena;

	// 1. Inline-body locations (from the freeze-built funcdef_locs index).
	uint32_t nslots = a.def_slots();
	for (uint32_t s = 0; s < nslots; ++s) {
		madc::dis::defrec r;
		if (!a.get_def_at(madc::dis::arena_id_of(s), r)
		    || r.kind != madc::dis::DK_CLASS)
			continue;
		for (uint32_t i = 0; i < r.methods_count; ++i) {
			madc::dis::methodrec md;
			if (!a.get_payload(r.methods_begin, i, md) || !md.name_id)
				continue;
			std::map<std::string, std::pair<uint32_t, uint32_t> >::const_iterator
				bl = f.funcdef_locs.find(a.strings.str(md.name_id));
			if (bl == f.funcdef_locs.end())
				continue;
			madc::dis::defrec fr;
			if (!madc::dis::arena_id_is_project(md.func_id)
			    || !a.get_def_at(md.func_id, fr)
			    || fr.kind != madc::dis::DK_FUNC)
				continue;
			fr.flags    |= madc::dis::DF_HAS_FOREST_BODY;
			fr.body_unit = bl->second.first;
			fr.body_idx  = bl->second.second;
			a.set_def_at(md.func_id, fr);
		}
	}

	// 1b (v20): BODIED free-function body locations — the producer's
	// instantiated definitions (static member-template __mti, namespace
	// fn-template __ns_*__oN). Stamp a body ONLY when the func-def landed in
	// a SYSTEM-header unit (its tokens carry the template's real origin): a
	// producer ROOT (main, user-file functions) stays body-less and its
	// DF_WAS_BODIED record cleanly lacks at load — a consumer must never
	// inherit the producer's roots.
	if (TokenBase::_active_strpool) {
		madc::dis::intern_table &pool = *TokenBase::_active_strpool;
		for (uint32_t s = 0; s < nslots; ++s) {
			uint32_t tid = madc::dis::arena_id_of(s);
			madc::dis::defrec r;
			if (!a.get_def_at(tid, r)
			    || r.kind != madc::dis::DK_FUNC
			    || !(r.flags & madc::dis::DF_IS_FREE_FUNC)
			    || !(r.flags & madc::dis::DF_WAS_BODIED)
			    || (r.flags & madc::dis::DF_HAS_FOREST_BODY))
				continue;
			std::map<std::string, std::pair<uint32_t, uint32_t> >::const_iterator
				bl = f.funcdef_locs.find(a.strings.str(r.name_id));
			if (bl == f.funcdef_locs.end())
				continue;
			uint32_t unit = bl->second.first;
			if (unit >= f.units.size())
				continue;
			const char *un = pool.c_str(f.units[unit].unit_name_id);
			if (!un || !prog->is_system_header_path(un))
				continue;
			r.flags    |= madc::dis::DF_HAS_FOREST_BODY;
			r.body_unit = unit;
			r.body_idx  = bl->second.second;
			a.set_def_at(tid, r);
		}
	}

	// 2+3 phase A: RESOLVE ids for every pending typedef/alias record first.
	// Resolution can stamp a fresh live project id (madc_type_id_for is lazy), and
	// the synthetic slots must start PAST the last live id — a collision would make
	// a typedef record masquerade as that live type's record — so nothing is
	// appended until every resolution is done.
	struct arena_alias { uint32_t name_id, ns_id, ref0; };
	std::vector<arena_alias> aliases;

	for (std::set<std::string>::const_iterator it = prog->user_typedef_names.begin();
	     it != prog->user_typedef_names.end(); ++it) {
		flat_datatype_map_iter dti = prog->datatype_map.find(*it);
		if (dti == prog->datatype_map.end() || !*dti)
			continue;
		DataDef *underlying = &(*dti)->definition;
		if (!underlying)
			continue;
		uint32_t uid = forest_serialize_type_id(underlying);
		if (!(madc::dis::arena_id_is_pinned(uid)
		      || (madc::dis::arena_id_is_project(uid) && a.has_def(uid))))
			continue;	// underlying not resolvable from the arena — cleanly lack
		arena_alias p;
		p.name_id = a.strings.intern(*it);
		p.ns_id   = 0;
		p.ref0    = uid;
		aliases.push_back(p);
	}

	prog->namespace_datatype_map.for_each(
	    [&](const char *ns, datatype_map_t &m) -> bool {
		if (!ns || !*ns)
			return false;
		for (datatype_map_iter it = m.begin(); it != m.end(); ++it) {
			if (it->first.find('<') != std::string::npos || !it->second)
				continue;		// template product / empty: follow-on
			// The ONE cross-ref policy (forest_serialize_type_id): a
			// btSimple scalar alias (std::size_t — a project-side copy
			// of unsigned long, the A2 class) resolves to its PINNED
			// slot; aggregates/derived keep their project id.
			uint32_t tid = forest_serialize_type_id(&it->second->definition);
			DBG(std::cout << "cir_forest_arena_complete: ns entry " << ns
				      << "::" << it->first << " tid=" << tid
				      << (madc::dis::arena_id_is_pinned(tid) ? " pinned"
					  : (madc::dis::arena_id_is_project(tid)
					     ? " project" : " other")) << std::endl);
			// v20: a namespaced alias to a PINNED primitive (std::size_t
			// -> unsigned long, std::ptrdiff_t -> long, …) has no arena
			// record to stamp — emit the namespaced DK_TYPEDEF alias
			// directly (ref0 = the pinned id; load resolves it via
			// madc_type_from_id). Left out, a restored-template body
			// instantiation fails at `typedef std::size_t size_type;`.
			if (madc::dis::arena_id_is_pinned(tid)) {
				arena_alias p;
				p.name_id = a.strings.intern(it->first);
				p.ns_id   = a.strings.intern(ns);
				p.ref0    = tid;
				aliases.push_back(p);
				continue;
			}
			madc::dis::defrec r;
			if (!madc::dis::arena_id_is_project(tid) || !a.get_def_at(tid, r)
			    || r.kind == madc::dis::DK_NONE)
				continue;		// type not in the arena (follow-on kind)
			uint32_t key_id = a.strings.intern(it->first);
			if (key_id != r.name_id) {
				// A namespaced ALIAS whose key differs from the aggregate's
				// record name: emit a namespaced DK_TYPEDEF (name=alias, ns,
				// ref0=product) so a bound `std::string` resolves to the product.
				if (r.kind == madc::dis::DK_STRUCT
				    || r.kind == madc::dis::DK_UNION
				    || r.kind == madc::dis::DK_CLASS) {
					arena_alias p;
					p.name_id = key_id;
					p.ns_id   = a.strings.intern(ns);
					p.ref0    = tid;
					aliases.push_back(p);
				}
				continue;
			}
			if (r.ns_id)
				continue;		// first defining namespace wins
			r.ns_id = a.strings.intern(ns);
			a.set_def_at(tid, r);
		}
		return false;
	    });

	// 2+3 phase B: append the collected alias records at synthetic slots past both
	// the live project table and the arena's own high-water slot.
	uint32_t next = MADC_TYPEID_PROJECT_BASE
		      + (uint32_t)(madc_active_project_types
				   ? madc_active_project_types->size() : 0);
	if (madc::dis::arena_id_of(a.def_slots()) > next)
		next = madc::dis::arena_id_of(a.def_slots());
	for (size_t i = 0; i < aliases.size(); ++i, ++next) {
		madc::dis::defrec r;
		memset(&r, 0, sizeof(r));
		r.kind    = madc::dis::DK_TYPEDEF;
		r.name_id = aliases[i].name_id;
		r.ns_id   = aliases[i].ns_id;
		r.ref0    = aliases[i].ref0;
		a.set_def_at(next, r);
	}
}

// B3 flip (Chunk 2): RE-RECORD every live project aggregate into the arena at
// freeze time. The parse-time write-throughs capture an aggregate at its
// COMPLETION hook, but state keeps mutating afterwards — a method's emit_symbol
// materializes at instantiation/use, a method Variable can rebind its FuncDef,
// and a struct later used as a base is PROMOTED to a fresh DataDefCLASS that
// never passes a hook (the arena oracle surfaced all three on real <string>).
// Until the remaining B3 mutation sites write through (the ~411-site rollout),
// the freeze-time state is captured by re-recording from the LIVE objects —
// the SAME encoders, no parallel format. A superseded record is overwritten in
// place; its old payload runs become dead words in the dump (size, not
// correctness). The walk runs to a fixpoint because recording can stamp fresh
// project ids (an aggregate first reached as a cross-ref).
static void cir_forest_arena_refresh(Program *prog)
{
	if (!prog || !prog->forest_arena_enabled || !madc_active_project_types)
		return;
	uint32_t base = madc_active_project_types->base();
	uint32_t done = 0;
	for (;;) {
		uint32_t n = (uint32_t)madc_active_project_types->size();
		if (done >= n)
			break;
		for (uint32_t i = done; i < n; ++i) {
			DataDef *dd = madc_active_project_types->get(base + i);
			DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(dd);
			if (!sdd || !sdd->is_complete)
				continue;
			prog->forest_arena_record_aggregate(sdd);
		}
		done = n;
	}

	// RC2: FREE FUNCTIONS — record each file-scope free function (the
	// funcdef_map surface a live header parse leaves behind) as its own
	// DK_FUNC, flagged DF_IS_FREE_FUNC with name_id = the funcdef_map key (the
	// call name). Same interim freeze-time capture as the aggregates above
	// (a parseFunction write-through is part of the ~411-site rollout).
	// Runs AFTER the aggregate fixpoint so every METHOD FuncDef already has
	// its DK_FUNC record (recorded via its class) and is skipped structurally
	// by has_def. Selection: prototypes without a tracked C++ overload name
	// (the RC2 slice-1 set, unchanged), PLUS (v20) every BODIED function —
	// flagged DF_WAS_BODIED. A bodied entry is an instantiated definition the
	// producer's lowering created (a static member-template __mti, a namespace
	// fn-template __ns_*__oN) whose loaded callers reference its symbol, OR a
	// producer root like main(): cir_forest_arena_complete stamps a forest
	// body location ONLY for the system-header-origin ones, and load restores
	// ONLY those (a bodied record without a body location cleanly lacks — a
	// producer root never restores into a consumer). Templates / member-
	// template PATTERNS still skip (a pattern is not a concrete symbol).
	for (funcdef_map_iter it = prog->funcdef_map.begin();
	     it != prog->funcdef_map.end(); ++it) {
		FuncDef *fd = it->second;
		if (!fd || it->first.empty())
			continue;
		if (fd->declaration_only && !fd->function_display_name.empty())
			continue;
		if (fd->is_member_template || !fd->template_param_names.empty())
			continue;
		uint32_t tid = madc_type_id_for(fd);
		if (!madc::dis::arena_id_is_project(tid)
		    || prog->forest_arena.has_def(tid))
			continue;	// already recorded = a class's method
		prog->forest_arena_record_func(fd);
		madc::dis::defrec r;
		if (!prog->forest_arena.get_def_at(tid, r))
			continue;
		r.name_id = prog->forest_arena.strings.intern(it->first.c_str());
		r.flags  |= madc::dis::DF_IS_FREE_FUNC;
		if (!fd->declaration_only)
			r.flags |= madc::dis::DF_WAS_BODIED;
		prog->forest_arena.set_def_at(tid, r);
	}
}

int madc_cir_freeze(Program *prog, const char *source_name,
		    const char *out_path, bool append)
{
    // The type graph rides the parse-populated DefArena (v18): a Program that
    // parsed with forest_arena_enabled OFF would freeze a type-less container
    // that binds nothing — fail loudly instead of dumping silent data loss.
    // (--freeze / --freeze-run set the flag before tokenize; a unit test must
    // do the same.)
    if (!prog || !prog->forest_arena_enabled) {
	fprintf(stderr, "%s: freeze requires forest_arena_enabled before parse "
		"(the DefArena is the type-graph serialization)\n", source_name);
	return -1;
    }

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
		cir_forest_arena_refresh(prog);		// re-record live aggregates (post-completion mutations)
		f.arena = prog->forest_arena;		// B3 (v18): the arena dump IS the type-graph serialization
		cir_forest_arena_complete(prog, f);	// freeze-time fidelity (inline bodies / typedefs / ns)
		cir_forest_fill_globals(prog, f);	// file-scope globals (CIR_GLOBALS; ids swizzle via the arena)
		cir_forest_fill_templates(prog, f);	// v20: template-NAME state (pattern maps, token runs)
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
