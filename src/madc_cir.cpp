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
#include <limits.h>	// PATH_MAX (cross-build cover analysis: realpath buffer)
#include <chrono>
#include <sys/stat.h>	// -o: chmod 0755 on the emitted executable
#include <errno.h>


#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_cir.h"
#include "madc_sys_includes.h"	// per-flavor C++ runtime link set (cir_native_link_env)
#include "madc_project.h"
#include "cir_builder.h"
#include "cir_emit_c.h"
#include "cir_freeze.h"
#include "madc_pch.h"	// v20: template token runs ride the .madh record form

extern "C" {
#include "c2mir/c2mir_api.h"
#include "mir-gen.h"
#include "mir-debug.h"
}

extern thread_local bool madc_verbose;
extern thread_local int madc_opt_level;
extern thread_local bool madc_debug_info;
extern thread_local bool madc_object_mode;

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
    // -g: stamp source locations on MIR insns and snapshot each function's
    // typed locals while compiling (consumed by cir_register_source_debug).
    opts->debug_info_p = madc_debug_info ? 1 : 0;
    // -c/-o/-shared: gen captures relocatable object code instead of
    // publishing executable code (consumed by c2mir_get_native_object).
    opts->native_object_p = madc_object_mode ? 1 : 0;
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

// -g debuggable codegen: O0 (clean stepping), no inlining (one frame per
// function), spill every local to a stable frame slot so gdb can locate it.
// Overrides -O<n> — a debug build wants describable code, not fast code.
static void cir_set_debug_codegen(MIR_context_t ctx)
{
    MIR_gen_set_optimize_level(ctx, 0);
    MIR_set_inline_permission(ctx, 0);
    MIR_set_spill_all(ctx, 1);
}

// -g, after MIR_link/gen: resolve the compile-time snapshot of each
// function's typed locals to frame offsets, emit the in-memory GDB-JIT ELF
// (symtab + .debug_line/.debug_info) and register it. The registration is
// bound to ctx — MIR_finish unregisters and frees it. Functions without
// generated code (lazy lanes) are skipped inside c2mir_get_debug_object.
static void cir_register_source_debug(MIR_context_t ctx)
{
    void *buf;
    size_t size;
    if (c2mir_get_debug_object(ctx, &buf, &size) == 0)
	MIR_debug_gdb_register(ctx, buf, size);
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
// #load'd namespace functions (task #67): the Program's __dl_<ns>_<member>
// import-name -> dlsym'd-address table, set around MIR_link exactly like the
// host-callback registrations above — dlsym(RTLD_DEFAULT) can never find
// these madc-synthesized names.
static thread_local const std::map<std::string, void *> *cir_active_dl_syms = NULL;

// The ACTIVE stdlib flavor's C++ runtime, in the process's global symbol scope.
//
// A mangled-direct import names a symbol the selected stdlib really exports
// (policy: mangled-direct only for real exports, HIDE_FROM_ABI compiles from
// parsed headers). For the AOT lanes cir_native_link_env() turns the flavor's
// link_libs into DT_NEEDED and the system loader supplies them. The JIT had no
// equivalent: cir_import_resolver's dlsym(RTLD_DEFAULT) can only see what is
// ALREADY loaded, which is whatever bin/madc itself was linked against. So a
// -stdlib=libc++ compile resolved against libstdc++ and died on the first
// symbol only libc++abi has — `std::bad_array_new_length::bad_array_new_length()`
// (libstdc++ exports the dtors and what(), not the ctor), reached from libc++'s
// inline __throw_bad_array_new_length() on <string>'s overflow path.
//
// dlopen with RTLD_GLOBAL is the JIT's DT_NEEDED. Purely ADDITIVE: the global
// scope only gains libraries, so every symbol that resolved before still
// resolves to the same address and the resolver itself is unchanged. When both
// runtimes export a name (the std:: exception vocabulary, which is NOT inside
// libc++'s __1 ABI namespace) the already-loaded one still wins — those are the
// Itanium-ABI-shared types both implement identically, so either is correct. If
// that ever stops being true the fix is to prefer the flavor's handle here, not
// to special-case a symbol.
//
// Idempotent and unconditional — the default flavor's libs are already loaded,
// so those dlopens are refcount bumps. A flavor whose runtime cannot be opened
// is a warning, not a fatal: the imports that need it fail by exact name at
// link, which is a better diagnostic than a load error naming a whole library.
static void cir_open_stdlib_runtime(const madc_stdlib_flavor *flavor)
{
    static std::set<std::string> opened;
    if (!flavor)
	flavor = &madc_stdlib_flavors[0];
    if (!flavor->link_libs)
	return;
    for (int i = 0; flavor->link_libs[i]; i++) {
	const char *lib = flavor->link_libs[i];
	if (!opened.insert(lib).second)
	    continue;
	if (!dlopen(lib, RTLD_LAZY | RTLD_GLOBAL))
	    fprintf(stderr, "madc: warning: stdlib flavor %s runtime %s: %s\n",
		    flavor->name ? flavor->name : "?", lib, dlerror());
    }
}

static void *cir_import_resolver(const char *name)
{
    if (cir_active_host_regs)
	for (const Program::HostCallbackReg &r : *cir_active_host_regs)
	    if (r.entry && r.import_sym == name)
		return (void *)r.entry;
    if (cir_active_dl_syms) {
	std::map<std::string, void *>::const_iterator it =
	    cir_active_dl_syms->find(name);
	if (it != cir_active_dl_syms->end())
	    return it->second;
    }
    void *addr = dlsym(RTLD_DEFAULT, name);
    if (!addr)
	DBG(std::cerr << "cir_import_resolver: unresolved: " << name << std::endl);
    return addr;
}

// Object mode (-c/-o/-shared): import addresses are never read — gen captures
// relocatable code and imports become undefined ELF symbols resolved by the
// system linker. Any non-NULL address satisfies MIR_link's presence check.
static void *cir_object_import_resolver(const char *name)
{
    (void)name;
    static char undef_sentinel;
    return &undef_sentinel;
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
// --run-frozen lazy-link trap stubs (rung 1)
// -----------------------------------------------------------------------
//
// A drained (pack_recording) frozen module is the TU plus the drained library
// superset: bodies whose callees reverted to on-use derivation stay frozen for
// consumer coverage, so whole-module compilation legitimately contains imports
// with no in-module definition and no dlsym home (reverted/deferred DEFBODY
// symbols; vtables of drain-only instantiation products). Only the EXECUTED
// closure of main must resolve. --run-frozen therefore pre-binds each such
// import to a trap stub that aborts loudly, naming the symbol, if it is ever
// reached — standard lazy-binding semantics, never a silent cap. Bound
// consumers are unaffected (they materialize only referenced defs and keep the
// strict resolver).

extern "C" void __madc_frozen_trap(const char *sym)
{
    fprintf(stderr, "madc: frozen module: reached unresolved drained-library "
	    "symbol %s (outside the executed closure; a live compile derives "
	    "it on use)\n", sym ? sym : "?");
    abort();
}

extern "C" void __madc_frozen_trap_vslot(const char *sym)
{
    fprintf(stderr, "madc: frozen module: virtual call through unmaterialized "
	    "vtable %s (drained-library class outside the executed closure)\n",
	    sym ? sym : "?");
    abort();
}

// TRUE for Itanium-ABI DATA symbols (vtable/typeinfo/guard) — these must
// pre-bind as data (a table of trap slots), not as a callable stub, so a
// virtual dispatch through them traps cleanly instead of executing code bytes.
static bool itanium_data_symbol(const std::string &nm)
{
    return nm.compare(0, 4, "_ZTV") == 0 || nm.compare(0, 4, "_ZTI") == 0
	|| nm.compare(0, 4, "_ZTS") == 0 || nm.compare(0, 4, "_ZGV") == 0;
}

// Named defs of a module, appended into `defined` (the trap scanners' "has a
// real definition somewhere" set).
static void cir_collect_module_defs(MIR_context_t ctx, MIR_module_t mod,
				    std::set<std::string> &defined)
{
    for (MIR_item_t it = DLIST_HEAD(MIR_item_t, mod->items); it;
	 it = DLIST_NEXT(MIR_item_t, it)) {
	switch (it->item_type) {
	case MIR_func_item: case MIR_data_item: case MIR_ref_data_item:
	case MIR_lref_data_item: case MIR_expr_data_item: case MIR_bss_item:
	    if (const char *nm = MIR_item_name(ctx, it)) defined.insert(nm);
	    break;
	default: break;
	}
    }
}

// Build + load the per-symbol trap-stub module for `undef` (shared by the
// --run-frozen whole-module lane and the bind lane's cache module).
static void cir_bind_trap_module(MIR_context_t ctx,
				 const std::vector<std::string> &undef)
{
    MIR_new_module(ctx, "__madc_frozen_traps");
    MIR_item_t trap_proto = MIR_new_proto(ctx, "__madc_frozen_trap__proto",
					  0, NULL, 1, MIR_T_P, "sym");
    MIR_item_t trap_imp = MIR_new_import(ctx, "__madc_frozen_trap");
    MIR_item_t vslot_imp = MIR_new_import(ctx, "__madc_frozen_trap_vslot");
    size_t nm_seq = 0;
    // One trap function per symbol (both callable stubs and vtable slots) so
    // a fired trap NAMES the symbol — the whole point of this diagnostic.
    auto new_trap_fn = [&](const char *fn_name, const std::string &sym,
			   MIR_item_t handler) -> MIR_item_t {
	char nm_item[32];
	snprintf(nm_item, sizeof(nm_item), "__madc_trapnm_%zu", nm_seq++);
	MIR_item_t nm_data = MIR_new_string_data(
	    ctx, nm_item, MIR_str_t{sym.size() + 1, sym.c_str()});
	MIR_item_t f = MIR_new_func(ctx, fn_name, 0, NULL, 0);
	MIR_append_insn(ctx, f,
			MIR_new_call_insn(ctx, 3,
					  MIR_new_ref_op(ctx, trap_proto),
					  MIR_new_ref_op(ctx, handler),
					  MIR_new_ref_op(ctx, nm_data)));
	MIR_append_insn(ctx, f, MIR_new_ret_insn(ctx, 0));
	MIR_finish_func(ctx);
	return f;
    };
    for (const std::string &nm : undef) {
	if (itanium_data_symbol(nm)) {
	    // Data symbol (vtable/typeinfo): a table of pointers to a
	    // per-symbol trap function, so a virtual dispatch through it
	    // traps cleanly AND names the class.
	    MIR_item_t vf = new_trap_fn((nm + ".__vtrap").c_str(), nm,
					vslot_imp);
	    MIR_new_export(ctx, nm.c_str());
	    MIR_new_ref_data(ctx, nm.c_str(), vf, 0);
	    for (int i = 1; i < 32; ++i)
		MIR_new_ref_data(ctx, NULL, vf, 0);
	    continue;
	}
	MIR_new_export(ctx, nm.c_str());
	new_trap_fn(nm.c_str(), nm, trap_imp);
    }
    MIR_finish_module(ctx);
    MIR_load_module(ctx, DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx)));
}

static void cir_prebind_frozen_traps(MIR_context_t ctx, MIR_module_t mod)
{
    std::set<std::string> defined;
    cir_collect_module_defs(ctx, mod, defined);
    std::vector<std::string> undef;
    std::set<std::string> seen;
    for (MIR_item_t it = DLIST_HEAD(MIR_item_t, mod->items); it;
	 it = DLIST_NEXT(MIR_item_t, it)) {
	if (it->item_type != MIR_import_item || !it->u.import_id) continue;
	std::string nm = it->u.import_id;
	if (defined.count(nm) || !seen.insert(nm).second) continue;
	if (cir_import_resolver(nm.c_str())) continue;
	undef.push_back(nm);
    }
    if (undef.empty()) return;
    fprintf(stderr, "madc: --run-frozen: %zu unresolved drained-library "
	    "import(s) bound to trap stubs (-v lists them)\n", undef.size());
    DBG(for (const std::string &nm : undef)
	    std::cerr << "  trap-bound: " << nm << std::endl);
    cir_bind_trap_module(ctx, undef);
}

// Bind lane, rung 3: the loaded MIR cache module carries the whole drained
// library, so it legitimately imports named data / drained-sibling symbols
// outside this consumer's emitted surface (a tag global of a header the
// consumer never references). Trap-bind ONLY the cache module's own
// otherwise-unresolvable imports; a name the CONSUMER module also imports
// stays strict — an unresolved consumer import is a live-compile correctness
// signal and must keep failing exactly as it does without the cache.
static void cir_prebind_cache_traps(MIR_context_t ctx, MIR_module_t cache_mod,
				    MIR_module_t consumer_mod)
{
    std::set<std::string> defined;
    cir_collect_module_defs(ctx, cache_mod, defined);
    cir_collect_module_defs(ctx, consumer_mod, defined);
    std::set<std::string> consumer_imports;
    for (MIR_item_t it = DLIST_HEAD(MIR_item_t, consumer_mod->items); it;
	 it = DLIST_NEXT(MIR_item_t, it))
	if (it->item_type == MIR_import_item && it->u.import_id)
	    consumer_imports.insert(it->u.import_id);
    std::vector<std::string> undef;
    std::set<std::string> seen;
    for (MIR_item_t it = DLIST_HEAD(MIR_item_t, cache_mod->items); it;
	 it = DLIST_NEXT(MIR_item_t, it)) {
	if (it->item_type != MIR_import_item || !it->u.import_id) continue;
	std::string nm = it->u.import_id;
	if (defined.count(nm) || consumer_imports.count(nm)
	    || !seen.insert(nm).second) continue;
	if (cir_import_resolver(nm.c_str())) continue;
	undef.push_back(nm);
    }
    if (undef.empty()) return;
    DBG(std::cout << "mir cache: " << undef.size() << " cache-only "
	"import(s) bound to trap stubs" << std::endl);
    DBG(for (const std::string &nm : undef)
	    std::cerr << "  cache trap-bound: " << nm << std::endl);
    cir_bind_trap_module(ctx, undef);
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

// --- MIR module cache byte IO (no FILE*: a MIR fatal's longjmp must leave
// nothing open; thread_local park spots for the with_func callbacks) ---

// Byte sink for MIR_write_module_with_func — appends into the parked vector.
static thread_local std::vector<uint8_t> *cir_mir_cache_sink = NULL;
static int cir_mir_cache_write_byte(MIR_context_t, uint8_t b)
{
    cir_mir_cache_sink->push_back(b);
    return 1;
}

// Byte source for MIR_read_with_func — fgetc semantics (EOF = -1).
static thread_local const std::vector<uint8_t> *cir_mir_cache_read_src = NULL;
static thread_local size_t cir_mir_cache_read_pos = 0;
static int cir_mir_cache_read_byte(MIR_context_t)
{
    if (!cir_mir_cache_read_src
	|| cir_mir_cache_read_pos >= cir_mir_cache_read_src->size())
	return EOF;
    return (*cir_mir_cache_read_src)[cir_mir_cache_read_pos++];
}

// --- AOT ledger pull (-static-libmadc, forest-carriers S5) ----------------
//
// Collect every import no loaded module defines. The caller's two uses are
// selection (which ledger modules to pull) and the Tier-B refusal list.
//
// `extra` joins the module-level imports as further CANDIDATES filtered by the
// same "no loaded module defines it" rule: the .o link lane's program is a set
// of native relocatables, not MIR modules, so its unresolved references arrive
// as the merged builder's UNDEF names. Selection must see them, and must stop
// seeing them once a pulled ledger module defines them — which is exactly what
// this filter does.
static void cir_ctx_unresolved_imports(MIR_context_t ctx,
				       std::vector<std::string> &out,
				       const std::vector<std::string> *extra = NULL)
{
    std::set<std::string> defined, seen;
    std::vector<std::string> imports;
    for (MIR_module_t m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx));
	 m; m = DLIST_NEXT(MIR_module_t, m))
	for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it;
	     it = DLIST_NEXT(MIR_item_t, it)) {
	    if (it->item_type == MIR_import_item) {
		if (it->u.import_id && seen.insert(it->u.import_id).second)
		    imports.push_back(it->u.import_id);
		continue;
	    }
	    if (it->item_type == MIR_export_item
		|| it->item_type == MIR_forward_item
		|| it->item_type == MIR_proto_item)
		continue;
	    const char *nm = MIR_item_name(ctx, it);
	    if (nm && nm[0])
		defined.insert(nm);
	}
    if (extra)
	for (const std::string &e : *extra)
	    if (seen.insert(e).second)
		imports.push_back(e);
    out.clear();
    for (const std::string &imp : imports)
	if (!defined.count(imp))
	    out.push_back(imp);
}

// Was a ledger actually AVAILABLE for the last pull? Distinguishes "this madc
// ships no ledger" from "this program needs Tier-B pieces" — two very
// different user actions, so they must not share a diagnostic.
static bool cir_ledger_available = false;

// Read ONE ledger module's .bmir into `ctx` and load it. The MIR error
// bracket lives in its own frame on purpose: a longjmp leaves automatic
// variables MODIFIED since the setjmp indeterminate, so arming it inside the
// caller's selection loop would corrupt that loop's state at -O2. Here the
// only automatics are the unmodified parameter and `before`, both set before
// the setjmp. False = nothing new was loaded.
static bool cir_ledger_read_module(MIR_context_t ctx,
				   const cir_ledger_module &m)
{
    MIR_module_t before = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx));
    if (setjmp(cir_mir_error_jmp)) {
	cir_mir_error_armed = false;
	cir_mir_cache_read_src = NULL;
	fprintf(stderr, "madc: ledger: %s: MIR_read failed: %s\n",
		m.name.c_str(), cir_mir_error_text);
	return false;
    }
    cir_mir_error_armed = true;
    cir_mir_cache_read_src = &m.bytes;
    cir_mir_cache_read_pos = 0;
    MIR_read_with_func(ctx, cir_mir_cache_read_byte);
    cir_mir_cache_read_src = NULL;
    cir_mir_error_armed = false;
    MIR_module_t got = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx));
    if (!got || got == before) {
	fprintf(stderr, "madc: ledger: %s: no module in the blob\n",
		m.name.c_str());
	return false;
    }
    MIR_load_module(ctx, got);
    return true;
}

// Merge the ledger modules this program needs into `ctx`, BEFORE MIR_link, so
// the eager object-mode gen puts their code in the capture and the emit-time
// cover analysis sees the symbols defined. Transitive: a pulled module's own
// unresolved imports select further ledger modules, to a fixpoint. Whole
// modules are the granularity (gcc's .a-member model) — a module is pulled
// once, in full.
//
// Silent when there is no carrier or no ledger: the emit-time verification
// owns the diagnostic, because only it knows whether this program needed the
// runtime at all (a runtime-free program must not be nagged about a missing
// ledger).
//
// `seed` names references the program has that are NOT module imports — the .o
// link lane's inputs (see cir_ledger_relocatable). Selection is otherwise
// identical: one pull implementation, two shapes of program.
static void cir_ledger_pull(MIR_context_t ctx, Program *prog,
			    const std::vector<std::string> *seed = NULL)
{
    if (!madc_static_libmadc || !prog)
	return;
    CirFrozenForest *forest = prog->ensure_ledger_forest();
    if (!forest)
	return;
    std::vector<cir_ledger_module> ledger;
    if (!forest->ledger_modules(ledger))
	return;
    cir_ledger_available = true;
    std::map<std::string, size_t> owner;	// symbol -> ledger module index
    for (size_t i = 0; i < ledger.size(); i++)
	for (const std::string &s : ledger[i].syms)
	    owner.insert(std::make_pair(s, i));

    std::vector<bool> pulled(ledger.size(), false);
    for (;;) {
	std::vector<std::string> undef;
	cir_ctx_unresolved_imports(ctx, undef, seed);
	size_t pulled_now = 0;
	for (const std::string &nm : undef) {
	    std::map<std::string, size_t>::const_iterator it = owner.find(nm);
	    if (it == owner.end() || pulled[it->second])
		continue;
	    pulled[it->second] = true;	// once, whether or not the read works
	    if (cir_ledger_read_module(ctx, ledger[it->second])) {
		pulled_now++;
		DBG(std::cout << "ledger: pulled " << ledger[it->second].name
			      << " for '" << nm << "' ("
			      << ledger[it->second].syms.size() << " symbols)"
			      << std::endl);
	    }
	}
	if (!pulled_now)
	    break;	// fixpoint: nothing new selected this round
    }
}

// --- AOT ledger as a RELOCATABLE (the .o link lane, -static-libmadc) -------
//
// The source lanes pull the ledger into the compile's own context, where the
// object-mode gen drops the runtime straight into the capture next to the
// program. The .o link lane has no compile: its program is a set of native
// relocatables merged through MIR_object_read. So the runtime is generated in
// a PRIVATE object-mode context and emitted as its own relocatable, which the
// caller merges into the same builder as the inputs.
//
// Through a serialized .o rather than generating into the input builder
// directly, because a builder's symbol table is append-only
// (MIR_object_add_symbol never dedupes by name) — the unification that turns
// the inputs' UNDEF entries into references to the runtime's definitions lives
// in the MERGE. Going in through the merge reuses that one unification
// implementation, and it rides the same read-back path the .o lane already
// gates on both containers (ELF ET_REL and MH_OBJECT), so this adds no format
// code at all.

// Serialize the private capture. The buffer is malloc'd by the emitter.
static bool cir_ledger_emit_blob(MIR_context_t ctx, std::vector<uint8_t> &blob)
{
    void *buf = NULL;
    size_t size = 0;
    if (MIR_gen_object_emit(ctx, &buf, &size) != 0 || !buf || size == 0) {
	fprintf(stderr, "madc: -static-libmadc: AOT-ledger object emission"
		" failed\n");
	free(buf);
	return false;
    }
    blob.assign((const uint8_t *)buf, (const uint8_t *)buf + size);
    free(buf);
    return true;
}

// Object mode + the selection pull. Its own MIR error frame: every module read
// arms one of its own (cir_ledger_read_module), so a bracket established before
// the pull is stale afterwards — hence the split from the link/emit frame.
static bool cir_ledger_obj_pull(MIR_context_t ctx, Program *prog,
				const std::vector<std::string> &seed)
{
    if (setjmp(cir_mir_error_jmp)) {
	cir_mir_error_armed = false;
	fprintf(stderr, "madc: -static-libmadc: cannot prepare the AOT ledger:"
		" %s\n", cir_mir_error_text);
	return false;
    }
    cir_mir_error_armed = true;
    MIR_gen_set_object_mode(ctx, 1);
    cir_ledger_pull(ctx, prog, &seed);
    cir_mir_error_armed = false;
    return true;
}

// Link + generate + serialize, under a FRESH error frame (see above).
static bool cir_ledger_obj_link_emit(MIR_context_t ctx,
				     std::vector<uint8_t> &blob)
{
    if (setjmp(cir_mir_error_jmp)) {
	cir_mir_error_armed = false;
	fprintf(stderr, "madc: -static-libmadc: cannot link the AOT ledger"
		" runtime: %s\n", cir_mir_error_text);
	return false;
    }
    cir_mir_error_armed = true;
    // Same resolver the source lanes use in object mode: an import stays an
    // UNDEF symbol in the capture instead of binding to this process.
    MIR_link(ctx, MIR_set_gen_interface, cir_object_import_resolver);
    bool ok = cir_ledger_emit_blob(ctx, blob);
    cir_mir_error_armed = false;
    return ok;
}

// `seed` = the merged input builder's UNDEF names (what the program still
// needs). False = a real failure, already reported. An EMPTY blob is a
// SUCCESS that selected nothing: whether that was enough is the emit-time
// verification's question, exactly as in the source lanes.
static bool cir_ledger_relocatable(Program *prog,
				   const std::vector<std::string> &seed,
				   std::vector<uint8_t> &blob)
{
    blob.clear();
    MIR_context_t ctx = MIR_init();
    if (!ctx) {
	fprintf(stderr, "madc: -static-libmadc: MIR_init failed\n");
	return false;
    }
    MIR_set_error_func(ctx, cir_mir_error);
    MIR_gen_init(ctx);
    MIR_gen_set_optimize_level(ctx, (unsigned)madc_opt_level);
    bool ok = cir_ledger_obj_pull(ctx, prog, seed);
    if (ok && DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx)) != NULL)
	ok = cir_ledger_obj_link_emit(ctx, blob);
    MIR_gen_finish(ctx);
    MIR_finish(ctx);
    return ok;
}

// -----------------------------------------------------------------------
// JIT symbolization for the crash handler
// -----------------------------------------------------------------------
//
// The MIR module whose functions are currently JIT-executing. Set just
// before main() is invoked so the crash handler (madc.cpp) can map a
// faulting machine-code address back to the MIR function name + offset —
// MIR generates each function lazily, recording its code range in
// machine_code / code_len, so a function on the live call stack has
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
		size_t len = item->u.func->code_len;
		if (mc && len && a >= mc && a < mc + len) {
			snprintf(out, n, "%s+0x%lx [JIT]", item->u.func->name,
				 (unsigned long)(a - mc));
			return 1;
		}
	}
	return 0;
}

// Declarator symbol of a top-level func def / declaration (both are
// (specs, declarator, ...)); NULL when the item has no named declarator.
static const char *cir_top_item_symbol(node_t n)
{
    if (!n || (n->code != N_FUNC_DEF && n->code != N_SPEC_DECL))
	return NULL;
    node_t o = c2mir_node_first_op(n);
    node_t d = o ? c2mir_node_next_op(o) : NULL;
    node_t id = (d && d->code == N_DECL) ? c2mir_node_first_op(d) : NULL;
    return (id && id->code == N_ID) ? id->u.s.s : NULL;
}

// c2mir_check_tree callback for the pack-side check gate: collect the
// 0-based module-list index of each defective top-level item. The log line
// lands directly after the item's error text in the freeze log, attributing
// each printed error to its item (no silent caps; fires only on defects).
static void pack_gate_note(node_t item, int index, unsigned n_errs, void *data)
{
    const char *sym = cir_top_item_symbol(item);
    fprintf(stderr, "pack check gate: item %d (%s): %u check error(s)\n",
	    index, sym ? sym : "?", n_errs);
    ((std::vector<int> *)data)->push_back(index);
}

// MADC_CHECK_ATTRIB diagnostic callback: name each defective item.
static void check_attrib_note(node_t item, int index, unsigned n_errs,
			      void *data)
{
    (void)data;
    const char *sym = cir_top_item_symbol(item);
    fprintf(stderr, "check-attrib: item %d (%s%s%s): %u error(s)\n", index,
	    item ? c2mir_node_code_name((c2mir_node_code_t)item->code) : "?",
	    sym ? " " : "", sym ? sym : "", n_errs);
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
    // Open the flavor's runtime BEFORE the tree build, not only at MIR link:
    // the builder's mangled-direct link tests (extern_symbol_can_link, the
    // facet-id extern recording) probe dlsym at CIR time, and under
    // -stdlib=libc++ the flavor library is not among bin/madc's own
    // DT_NEEDED — probing before the dlopen answers "unlinkable" for
    // symbols the link would in fact resolve, so the tree is SHAPED
    // differently (unrecorded facet-id externs, undeclared
    // _ZNSt3__15ctypeIcE2idE family). That bit the object lane first, then
    // the freeze lane the same way — which is why the open lives HERE, on
    // the one translate entry every lane (run, object, project, freeze)
    // flows through, not per caller. Idempotent; the MIR-link call site
    // keeps its open for lanes that skip translation entirely.
    cir_open_stdlib_runtime(prog ? prog->active_stdlib_flavor() : NULL);
    CirBuilder *builder = new CirBuilder(c2m);
    // TU identity for the object-mode per-TU init symbol (S3 init-array).
    builder->set_tu_name(source_name);
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

    // The tree build resolves token spellings / wide values / datadef ids via
    // the process-global active-owner statics. --project parses every TU
    // before building any (throwing calls stay outside the MIR bracket), so
    // by the time this TU builds, another Program's pools are active.
    prog->activate_token_pools();

    // The flavor-runtime open (CIR-time dlsym probes shape the tree — object
    // mode included) lives in cir_translate_guarded, the one translate entry
    // every lane flows through.

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

    // Env-gated (MADC_CHECK_ATTRIB=1): attribute check errors to top-level
    // items before the real compile — the whole-tree check reports some
    // errors (e.g. the incomplete-decl sweep) with no usable position. Runs
    // the checker over a throwaway deep copy in a fresh context, so the real
    // tree stays pristine for the compile below.
    if (getenv("MADC_CHECK_ATTRIB")) {
	MIR_context_t actx = MIR_init();
	MIR_set_error_func(actx, cir_mir_error);
	c2mir_init(actx);
	c2m_ctx_t ac2m = cir_init(actx, /*debug_p=*/false);
	if (ac2m) {
	    node_t copy = c2mir_copy_tree(ac2m, c2m, tree);
	    if (copy)
		c2mir_check_tree(ac2m, copy, check_attrib_note, NULL);
	    cir_finish(ac2m);
	}
	c2mir_finish(actx);
	MIR_finish(actx);
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
    : ctx(NULL), c2m(NULL), builder(NULL), forest(NULL), mod(NULL),
      cache_mod(NULL)
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
    cache_mod = NULL;	// owned by ctx (MIR_finish freed it above)
    gen_cache.clear();
}

bool CirJitSession::init_contexts(const char *source_name, bool dump_checked)
{
    ctx = MIR_init();
    MIR_set_error_func(ctx, cir_mir_error);
    c2mir_init(ctx);
    MIR_gen_init(ctx);
    MIR_gen_set_optimize_level(ctx, (unsigned)madc_opt_level);
    if (madc_object_mode)
	MIR_gen_set_object_mode(ctx, 1);
    if (madc_debug_info)
	cir_set_debug_codegen(ctx);

    c2m = cir_init(ctx, dump_checked);
    if (!c2m) {
	fprintf(stderr, "%s: cir_init failed\n", source_name);
	teardown();
	return false;
    }
    return true;
}

// Object mode (ELF-completion S3): register a TU's synthesized init into
// the capture's .init_array (one 8-byte ABS64 slot per TU). Must run after
// link — the eager gen interface has generated every function by then, so
// the init's STB_LOCAL symbol is defined in the capture. True when the TU
// has no init (nothing to register).
static bool cir_register_tu_init(MIR_context_t ctx, CirBuilder *b,
				 const char *source_name)
{
    if (!b || b->tu_init_name().empty())
	return true;
    MIR_object_t o = MIR_gen_get_object(ctx);
    if (!o || MIR_object_add_init(o, b->tu_init_name().c_str()) != 0) {
	fprintf(stderr, "%s: cannot register module init '%s' in the object"
		" capture\n", source_name, b->tu_init_name().c_str());
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
	cir_active_dl_syms = NULL;
	teardown();
	return false;
    }
    cir_mir_error_armed = true;
    // MADC_MIR_CACHE_TIME=1: per-step wall laps for the cache lane (the
    // decomposition that exposed the eager-gen link cost — keep it).
    const bool mc_time = getenv("MADC_MIR_CACHE_TIME") != NULL;
    auto mc_t0 = std::chrono::steady_clock::now();
    auto mc_lap = [&](const char *what) {
	if (!mc_time) return;
	auto now = std::chrono::steady_clock::now();
	fprintf(stderr, "[mc-time] %-18s %.3f s\n", what,
		std::chrono::duration<double>(now - mc_t0).count());
	mc_t0 = now;
    };
    // Rung 3: the privatized cache module loads FIRST; the consumer module's
    // exports then win every env-level overlap (setup_global overwrites), and
    // the cache's imports — including its privatized named data — resolve to
    // the consumer's definitions at link.
    if (cache_mod)
	MIR_load_module(ctx, cache_mod);
    mc_lap("load(cache_mod)");
    MIR_load_module(ctx, mod);
    mc_lap("load(consumer)");
    if (cache_mod)
	cir_prebind_cache_traps(ctx, cache_mod, mod);
    mc_lap("cache traps");
    // -static-libmadc: merge the AOT-ledger runtime modules this program
    // needs BEFORE the link, so the eager object-mode gen puts their code in
    // the capture. Object mode only — the JIT lane resolves the same symbols
    // out of the live libmadc it is already running inside.
    if (madc_object_mode)
	cir_ledger_pull(ctx, prog);
    mc_lap("ledger pull");
    cir_active_host_regs = prog ? &prog->host_callback_regs : NULL;
    cir_active_dl_syms = prog ? &prog->dl_symbol_map : NULL;
    // Object mode never reads import addresses (cir_object_import_resolver),
    // so it needs no runtime loaded — its DT_NEEDED comes from
    // cir_native_link_env. prog == NULL is the frozen lane, which recreates
    // its recorded link environment in build_frozen and falls back to the
    // default flavor here.
    if (!madc_object_mode)
	cir_open_stdlib_runtime(prog ? prog->active_stdlib_flavor() : NULL);
    if (cache_mod) {
	// MIR_set_gen_interface is EAGER — it MIR_gen()s every loaded func at
	// link, which for the 4290-func cache module costs ~0.8s per compile.
	// Ride the lazy interface (first-call generation) and eagerly gen only
	// the CONSUMER's funcs, so the consumer keeps exactly the no-cache
	// lane's gen-fatal surface and wall; cache bodies generate on demand.
	MIR_link(ctx, MIR_set_lazy_gen_interface, cir_import_resolver);
	for (MIR_item_t it = DLIST_HEAD(MIR_item_t, mod->items); it;
	     it = DLIST_NEXT(MIR_item_t, it))
	    if (it->item_type == MIR_func_item)
		MIR_gen(ctx, it);
    } else
	MIR_link(ctx, MIR_set_gen_interface,
		 madc_object_mode ? cir_object_import_resolver
				  : cir_import_resolver);
    cir_active_host_regs = NULL;
    cir_active_dl_syms = NULL;
    mc_lap("MIR_link");
    if (madc_debug_info) {
	// -g: JIT lane registers the GDB-JIT object; object mode attaches
	// the same builder to the capture so the native artifacts carry
	// DWARF (R5) — every function is generated by the MIR_link above.
	if (madc_object_mode)
	    c2mir_object_attach_debug(ctx);
	else
	    cir_register_source_debug(ctx);
    }
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

    // MIR module cache, rung 3 (bind lane): a container that provided the
    // bound grove may carry its compiled MIR module. Stage it BEFORE
    // translate — the export set steers the m&l fixpoint (forward proto
    // instead of loaded def for cache-exported symbols; the call resolves as
    // a MIR import against this module at link). Every fallible step runs
    // here, pre-translate, so any failure discards the cache and the
    // consumer emits everything itself, bit-for-bit.
    //
    // OPT-IN (MADC_MIR_CACHE_BIND=1): measured on testsubscript, the lane's
    // fixed floor (MIR_read 0.08s + link-time simplify of all 4290 cache
    // funcs ~0.15s) EXCEEDS what it saves a single-TU consumer (materialize
    // + emit + gen of its referenced drained bodies, ~0.1s) — see the
    // refutation note in docs/plans/2026-07-17-mir-module-cache-DESIGN.md.
    // The win case is many consumers per load (--project) or a body-heavy
    // consumer; --run-frozen's whole-module consumption (build_frozen) keeps
    // its own economics and stays on by default. MADC_NO_MIR_CACHE=1 remains
    // the master off-lever for both lanes.
    cache_mod = NULL;
    if (prog) prog->mir_cache_exports.clear();
    if (prog && prog->bind_forest && !prog->forest_chain.empty()
	&& !madc_object_mode   // object emit captures everything itself
	&& getenv("MADC_MIR_CACHE_BIND")
	&& !getenv("MADC_NO_MIR_CACHE")) {
	std::vector<uint8_t> mblob;
	auto mcs_t0 = std::chrono::steady_clock::now();
	auto mcs_lap = [&](const char *what) {
	    if (!getenv("MADC_MIR_CACHE_TIME")) return;
	    auto now = std::chrono::steady_clock::now();
	    fprintf(stderr, "[mc-time] %-18s %.3f s\n", what,
		    std::chrono::duration<double>(now - mcs_t0).count());
	    mcs_t0 = now;
	};
	if (prog->bind_forest->mir_module_bytes(mblob)) {
	    mcs_lap("blob segment read");
	    if (setjmp(cir_mir_error_jmp)) {
		// bmir version check / corrupt blob — rebuild-path fallback.
		cir_mir_error_armed = false;
		cir_mir_cache_read_src = NULL;
		cache_mod = NULL;
		fprintf(stderr, "%s: mir cache: MIR_read failed: %s — "
			"falling back to full emission\n",
			source_name, cir_mir_error_text);
	    } else {
		cir_mir_error_armed = true;
		cir_mir_cache_read_src = &mblob;
		cir_mir_cache_read_pos = 0;
		MIR_read_with_func(ctx, cir_mir_cache_read_byte);
		cir_mir_cache_read_src = NULL;
		cir_mir_error_armed = false;
		cache_mod = DLIST_TAIL(MIR_module_t,
				       *MIR_get_module_list(ctx));
	    }
	    mcs_lap("MIR_read");
	    if (cache_mod) {
		// The pack TU's entry points are never shared: the consumer
		// defines its own. A module whose exported data heads a
		// multi-item section can't be privatized — drop the cache
		// now, while the fallback is still clean.
		static const char *entry_syms[] = {"main", "__madc_global_init"};
		if (MIR_module_privatize_for_link(ctx, cache_mod,
						  entry_syms, 2)) {
		    fprintf(stderr, "%s: mir cache: module has unsplittable "
			    "exported data section(s) — falling back to full "
			    "emission\n", source_name);
		    cache_mod = NULL;	// stays in ctx unloaded; harmless
		}
	    }
	    if (cache_mod) {
		for (MIR_item_t it = DLIST_HEAD(MIR_item_t, cache_mod->items);
		     it; it = DLIST_NEXT(MIR_item_t, it))
		    if (it->item_type == MIR_func_item && it->export_p
			&& it->u.func->name)
			prog->mir_cache_exports.insert(it->u.func->name);
		DBG(std::cout << "mir cache: bind module staged ("
		    << mblob.size() << " bytes, "
		    << prog->mir_cache_exports.size()
		    << " importable bodies)" << std::endl);
	    }
	    mcs_lap("privatize+exports");
	}
    }

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

    // Rung 3: the consumer wins every overlap. Un-export from the cache
    // module any symbol this consumer module ALSO defines (an eagerly
    // emitted user func in a full-program corpus; a proto-less fallback
    // emission), so MIR_load_module can never hit a func redefinition.
    // Privatize is idempotent — the entry points and data are already done.
    if (cache_mod) {
	std::vector<const char *> unexp;
	for (MIR_item_t it = DLIST_HEAD(MIR_item_t, mod->items); it;
	     it = DLIST_NEXT(MIR_item_t, it))
	    if (it->item_type == MIR_func_item && it->u.func->name
		&& prog->mir_cache_exports.count(it->u.func->name))
		unexp.push_back(it->u.func->name);
	if (!unexp.empty()) {
	    DBG(std::cout << "mir cache: " << unexp.size()
		<< " consumer-defined overlap(s) un-exported from the cache "
		"module" << std::endl);
	    MIR_module_privatize_for_link(ctx, cache_mod,
					  unexp.data(), unexp.size());
	}
    }

    if (!load_and_link(source_name, prog))
	return false;
    if (madc_object_mode && !cir_register_tu_init(ctx, builder, source_name)) {
	teardown();
	return false;
    }
    return true;
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

    // MIR module cache: a container carrying its compiled module skips the
    // node materialization + c2mir compile entirely — MIR_read the blob into
    // this context. Any failure (no segment, stamp mismatch, bmir read error)
    // falls through to the rebuild path bit-for-bit. MADC_NO_MIR_CACHE=1
    // forces the rebuild (the equivalence gate's A/B lever).
    mod = NULL;
    if (!getenv("MADC_NO_MIR_CACHE")) {
	std::vector<uint8_t> mblob;
	if (forest->mir_module_bytes(mblob)) {
	    if (setjmp(cir_mir_error_jmp)) {
		// The bmir stream's internal version check (or a corrupt
		// blob) fataled — fall back to the rebuild below.
		cir_mir_error_armed = false;
		cir_mir_cache_read_src = NULL;
		mod = NULL;
		fprintf(stderr, "%s: mir cache: MIR_read failed: %s — "
			"falling back to node materialization\n",
			module_name, cir_mir_error_text);
	    } else {
		cir_mir_error_armed = true;
		cir_mir_cache_read_src = &mblob;
		cir_mir_cache_read_pos = 0;
		MIR_read_with_func(ctx, cir_mir_cache_read_byte);
		cir_mir_cache_read_src = NULL;
		cir_mir_error_armed = false;
		mod = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx));
		DBG(std::cout << "mir cache: module loaded from container ("
		    << mblob.size() << " bytes; node rebuild skipped)"
		    << std::endl);
	    }
	}
    }

    if (!mod) {
	cir_node *root = forest->root();
	if (!root) {
	    fprintf(stderr, "%s: forest root failed to materialize\n", module_name);
	    teardown();
	    return false;
	}

	// MADC_THAW_DUMP_TREE=<path>: dump the materialized tree AS THAWED —
	// side B of the freeze/thaw round-trip diff (side A is
	// MADC_FREEZE_DUMP_TREE in madc_cir_freeze).
	if (const char *dp = getenv("MADC_THAW_DUMP_TREE")) {
	    FILE *df = fopen(dp, "w");
	    if (df) { c2mir_dump_tree(c2m, df, root->as_node()); fclose(df); }
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
    }
    if (getenv("MADC_DUMP_MIR"))
	MIR_output(ctx, stderr);

    // MADC_MIR_CACHE_PROBE=<path>: serialize the compiled module (probe A of
    // docs/plans/2026-07-17-mir-module-cache-DESIGN.md — GO/NO-GO size and
    // read-time numbers only; no container integration).
    if (const char *probe_path = getenv("MADC_MIR_CACHE_PROBE")) {
	FILE *pf = fopen(probe_path, "wb");
	if (pf) {
	    auto _w0 = std::chrono::steady_clock::now();
	    MIR_write_module(ctx, pf, mod);
	    double wsecs = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - _w0).count();
	    long sz = ftell(pf);
	    fclose(pf);
	    fprintf(stderr, "[mir-cache-probe] wrote %s: %ld bytes in %.3fs\n",
		    probe_path, sz, wsecs);
	} else
	    fprintf(stderr, "[mir-cache-probe] cannot open %s for write\n",
		    probe_path);
    }

    // Rung 1: a drained module carries library bodies outside the TU's
    // executed closure — bind their unresolved imports to loud trap stubs
    // (see cir_prebind_frozen_traps) so only genuinely-executed gaps fail.
    cir_prebind_frozen_traps(ctx, mod);

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

// Write an emitted image buffer (malloc'd; consumed either way) to disk;
// executables get mode 0755. The one file writer for every native artifact.
static bool cir_write_image_file(const char *out_path, void *buf, size_t size,
				 bool executable_mode)
{
    FILE *f = fopen(out_path, "wb");
    if (!f) {
	fprintf(stderr, "madc: cannot open %s: %s\n", out_path,
		strerror(errno));
	free(buf);
	return false;
    }
    bool ok = fwrite(buf, 1, size, f) == size;
    if (fclose(f) != 0) ok = false;
    free(buf);
    if (ok && executable_mode && chmod(out_path, 0755) != 0) {
	fprintf(stderr, "madc: chmod %s: %s\n", out_path, strerror(errno));
	ok = false;
    }
    if (!ok)
	fprintf(stderr, "madc: short write to %s\n", out_path);
    return ok;
}

// --pack-forest=<container> (forest-carriers S2): the emitted native
// executable carries the container in its self-image carrier. One format,
// one loader, N carriers: the ELF arm appends the blob after the write
// (snapshot_append_blob — footer at EOF, byte-equivalent to the
// --freeze-append placement); the Mach-O arm rides the fork writer's
// extra-section seam so the blob sits INSIDE the signed image as
// __MADC,__forest (a signed Mach-O cannot take appended bytes; signed once
// at emit, no post-link surgery).
const char *madc_pack_forest_path = NULL;

// -static-libmadc (S5): merge the AOT-ledger runtime into the emitted image
// instead of depending on libmadc at run time. See madc_cir.h.
bool madc_static_libmadc = false;

// -fno-eval-shims: this artifact will never be host-called through the value
// ABI, so the __madc_shim_* adapters (and their Tier-B madc_value_* imports)
// are left out. See madc_cir.h.
bool madc_no_eval_shims = false;

// Load + validate the container with the production reader — an emitted
// binary carrying an unopenable carrier would ship dead weight silently,
// so a non-container file fails the emit loudly instead.
static bool cir_pack_forest_load(std::vector<uint8_t> &blob)
{
    FILE *f = fopen(madc_pack_forest_path, "rb");
    if (!f) {
	fprintf(stderr, "madc: --pack-forest: cannot open %s: %s\n",
		madc_pack_forest_path, strerror(errno));
	return false;
    }
    bool ok = fseek(f, 0, SEEK_END) == 0;
    long sz = ok ? ftell(f) : -1;
    ok = ok && sz > 0 && fseek(f, 0, SEEK_SET) == 0;
    if (ok) {
	blob.resize((size_t)sz);
	ok = fread(blob.data(), 1, blob.size(), f) == blob.size();
    }
    fclose(f);
    if (!ok) {
	fprintf(stderr, "madc: --pack-forest: cannot read %s\n",
		madc_pack_forest_path);
	return false;
    }
    madc::dis::snapshot_reader r;
    if (!r.open(blob.data(), blob.size())) {
	fprintf(stderr, "madc: --pack-forest: %s is not a frozen forest"
		" container\n", madc_pack_forest_path);
	return false;
    }
    return true;
}

// The pre-emit half of the carrier: Mach-O images take the blob as the
// writer's extra section (it must be inside the code signature). The blob
// must stay live until the emit call returns.
static void cir_pack_forest_attach(MIR_object_exec_params &xp,
				   const std::vector<uint8_t> &blob)
{
#if MADC_TARGET_APPLE_P
    if (!blob.empty()) {
	xp.extra_segname = "__MADC";
	xp.extra_sectname = "__forest";
	xp.extra_data = blob.data();
	xp.extra_size = blob.size();
    }
#else
    (void)xp;
    (void)blob;		// ELF: the carrier is the post-write trailer
#endif
}

// The post-write half: ELF appends the container (pad-to-16 + blob, the
// placement-2 shape). Mach-O images already carry it under the signature.
static bool cir_pack_forest_finish(const char *out_path,
				   const std::vector<uint8_t> &blob)
{
    if (blob.empty())
	return true;
#if MADC_TARGET_APPLE_P
    (void)out_path;
    return true;
#else
    if (madc::dis::snapshot_append_blob(out_path, blob.data(), blob.size()))
	return true;
    fprintf(stderr, "madc: --pack-forest: cannot append container to %s\n",
	    out_path);
    return false;
#endif
}

// Pull the finished object capture out of ctx and write it as a
// relocatable .o. Shared by the single-TU session (-c), the per-TU
// project loop, and the whole-program -r lane.
static bool cir_write_native_object(MIR_context_t ctx, const char *out_path)
{
    void *buf = NULL;
    size_t size = 0;
    if (c2mir_get_native_object(ctx, &buf, &size) != 0 || !buf) {
	fprintf(stderr, "madc: native object emission failed\n");
	return false;
    }
    return cir_write_image_file(out_path, buf, size, false);
}

bool CirJitSession::emit_native_object(const char *out_path)
{
    if (!ctx || !mod) return false;
    return cir_write_native_object(ctx, out_path);
}

// The one writer for -o/-shared images: pull the finished whole-context
// True when the import named `name` is provided by one of the DT_NEEDED
// entries in `covers` (spellings like "libc.so.6" / "libcrypt.so"): resolve
// the symbol in this process (bin/madc has the whole JIT resolution surface
// loaded), then map the resolved address back to its defining object and
// prefix-match that object's basename against the cover spellings. A symbol
// that does not resolve, or resolves outside every cover (including into the
// madc runtime itself — statically linked into this executable, so dladdr
// reports the self exe), is NOT covered.
#ifdef MADC_CROSS_TARGET
// Is `name` a symbol the MADC RUNTIME owns — i.e. one libmadc itself defines?
// dladdr the process's definition and compare the defining image against this
// madc's own (the shared libmadc, or the executable in the monolithic shape,
// where a static libmadc IS the exe's text).
static bool cir_symbol_from_madc_image(const char *name)
{
    void *addr = dlsym(RTLD_DEFAULT, name);
    if (!addr)
	return false;
    Dl_info info;
    if (!dladdr(addr, &info) || !info.dli_fname || !info.dli_fname[0])
	return false;
    char real[PATH_MAX];
    std::string img = realpath(info.dli_fname, real) ? real : info.dli_fname;
    return img == madc_self_lib_path() || img == madc_self_exe_path();
}
#endif

static bool cir_import_covered(const char *name,
			       const std::vector<std::string> &covers)
{
#ifdef MADC_CROSS_TARGET
    // A CROSS build's target system libraries are not this host's: dlsym here
    // sees the host's libc, so "does the target's libc provide __stderrp"
    // (darwin's backing symbol for stderr) simply cannot be probed — it is
    // absent from glibc, and the old answer, "uncovered", made every darwin
    // program touching stderr look like it needed the madc runtime.
    // The question that IS answerable, and the one the runtime-need analysis
    // actually asks, is "does LIBMADC define this?" — libmadc is loaded in
    // this very process. Anything it does not define belongs to the target
    // system, exactly as with any cross compiler: a genuinely missing symbol
    // surfaces at the target's loader, not as a bogus madc-runtime refusal.
    (void)covers;
    return !cir_symbol_from_madc_image(name);
#else
    // Ask each cover library DIRECTLY first: does IT define this symbol?
    // dladdr answers "where does the process's winning definition live",
    // which is the wrong question for a COPY-relocated libc data symbol —
    // `stderr`/`stdout`/`environ` are copied into bin/madc's own .bss by the
    // static linker, so dladdr reports the executable and the symbol looked
    // uncovered (every AOT program touching stderr kept a needless
    // libmadc.so.0 DT_NEEDED; under -static-libmadc it would refuse
    // outright). RTLD_NOLOAD: these libraries are already mapped — we are
    // interrogating them, never loading anything new.
    for (const std::string &c : covers) {
	if (c.find(".so") == std::string::npos
	    && c.find(".dylib") == std::string::npos)
	    continue;	// a bare stem (darwin's libsystem_/libc++) is a
			// prefix cover, handled by the dladdr pass below
	void *h = dlopen(c.c_str(), RTLD_LAZY | RTLD_NOLOAD);
	if (!h)
	    continue;
	void *sym = dlsym(h, name);
	dlclose(h);
	if (sym)
	    return true;
    }
    void *addr = dlsym(RTLD_DEFAULT, name);
    if (!addr)
	return false;
    Dl_info info;
    if (!dladdr(addr, &info) || !info.dli_fname || !info.dli_fname[0])
	return false;
    const char *bn = strrchr(info.dli_fname, '/');
    bn = bn ? bn + 1 : info.dli_fname;
    for (const std::string &c : covers) {
	size_t slash = c.rfind('/');
	const char *cb = slash == std::string::npos ? c.c_str()
						    : c.c_str() + slash + 1;
	if (strncmp(bn, cb, strlen(cb)) == 0)
	    return true;
    }
    return false;
#endif
}

// Does the image actually need the madc runtime? Classify the program's
// external references: if every one is covered by the non-madc DT_NEEDED
// entries, the produced binary is runtime-free and libmadc.so.0 can be
// dropped. Any uncovered one — a madc runtime symbol, or a symbol only
// reachable through libmadc's dependency closure — keeps the dependency
// (conservative: false negatives cost one extra DT_NEEDED, false positives
// break the binary at load).
//
// The reference LIST is the parameter, not its source, because the two lanes
// carry the program differently: from source it is the context's unresolved
// module imports (in --project the TUs satisfy each other's in-context), and in
// the .o link lane it is the merged builder's UNDEF symbols (unification
// already netted out cross-object references). One classifier, both lanes.
static void cir_filter_uncovered(const std::vector<std::string> &imports,
				 const std::vector<std::string> &covers,
				 std::vector<std::string> &out)
{
    out.clear();
    for (const std::string &imp : imports)
	if (!cir_import_covered(imp.c_str(), covers)) {
	    DBG(std::cout << "native image needs madc runtime: import '"
			  << imp << "' is not covered by base/user libs"
			  << std::endl);
	    out.push_back(imp);
	}
}

static bool cir_imports_need_madc_runtime(const std::vector<std::string> &imports,
					  const std::vector<std::string> &covers)
{
    std::vector<std::string> uncovered;
    cir_filter_uncovered(imports, covers, uncovered);
    return !uncovered.empty();
}

// The merged builder's unresolved references — the .o link lane's answer to
// "what does this program still import?".
static void cir_object_undef_names(MIR_object_t obj,
				   std::vector<std::string> &out)
{
    out.clear();
    const char *nm;
    for (size_t i = 0; (nm = MIR_object_undef_name(obj, i)) != NULL; i++)
	out.push_back(nm);
}

// -static-libmadc verification (S5). The ledger pull already merged every
// Tier-A piece this program selected, so anything still uncovered is Tier B —
// the C++ script-lane runtime, which exists only as host-toolchain objects.
// Refuse LOUDLY and name the symbols: the same contract the Apple cross lane
// ships, and the only honest answer (silently keeping the dependency would
// break the flag's whole promise). True = clear to emit.
// `hint` is an extra, lane-specific line of "what to do about it" printed
// after the symbol list — the .o link lane can name a concrete fix its inputs
// were built without, which the source lanes have no equivalent of.
static bool cir_static_libmadc_verify(const std::vector<std::string> &imports,
				      const char *out_path,
				      const std::vector<std::string> &covers,
				      const char *hint = NULL)
{
    std::vector<std::string> uncovered;
    cir_filter_uncovered(imports, covers, uncovered);
    if (uncovered.empty())
	return true;
    if (!cir_ledger_available) {
	// No ledger reached the pull at all: this madc's carrier chain found
	// no container, or the container carries no ledger segment (packed
	// without --freeze-ledger=, or --with-forest=none). A build problem, not a
	// program one — say so instead of blaming the program's symbols.
	fprintf(stderr, "madc: %s: -static-libmadc needs this madc's AOT"
		" ledger, and no carrier provided one (%zu runtime symbol(s)"
		" would be unresolved). A packed madc carries the ledger in"
		" its forest container; point at one with --forest-bind=<file>"
		" (or a madc.ini forest key) or use an installed/release"
		" build.\n",
		out_path, uncovered.size());
	return false;
    }
    fprintf(stderr, "madc: %s: -static-libmadc cannot cover %zu runtime"
	    " symbol(s) — they are not on the AOT ledger (Tier B: the C++"
	    " script-lane runtime, which exists only as host-toolchain"
	    " objects):\n", out_path, uncovered.size());
    for (const std::string &s : uncovered)
	fprintf(stderr, "    %s\n", s.c_str());
    if (hint)
	fprintf(stderr, "%s", hint);
    fprintf(stderr, "madc: link against libmadc instead (drop"
	    " -static-libmadc), or keep the program on the C lane\n");
    return false;
}

// Split the DT_NEEDED list around libmadc.so.0 (the covers for the
// runtime-need predicates); returns whether it was present at all.
static bool cir_split_needed(const std::vector<std::string> &needed,
			     std::vector<std::string> &other)
{
    bool have_madc = false;
    for (const std::string &l : needed) {
	if (l == "libmadc.so.0")
	    have_madc = true;
	else
	    other.push_back(l);
    }
    return have_madc;
}

// The flavor → exec-params mapping, in one place. File-scope ctors ride the
// capture's .init_array in every flavor (DT_INIT_ARRAY when non-empty; the
// emitter owns the tag) — ld.so runs a shared object's entries at load, and
// glibc >= 2.34 runs an executable's own array from __libc_start_main.
// Executables get entry=main and, per gcc parity, the PIE layout unless
// -no-pie chose fixed-base ET_EXEC.
static void cir_fill_exec_params(MIR_object_exec_params &xp,
				 MadcNativeKind kind,
				 const std::vector<const char *> &libs,
				 const std::string &runpath)
{
    memset(&xp, 0, sizeof xp);
    xp.needed = libs.data();
    xp.n_needed = libs.size();
    if (kind == mnkShared) {
	xp.shared_p = 1;
    } else {
	xp.entry = "main";
	xp.pie_p = kind == mnkPieExecutable;
    }
    xp.runpath = runpath.empty() ? NULL : runpath.c_str();
}

// capture out of ctx and write it to disk. Shared by the single-TU session
// (emit_native_executable) and the --project whole-program lane.
static bool cir_write_native_image(MIR_context_t ctx, const char *out_path,
				   const std::vector<std::string> &needed,
				   const std::string &runpath,
				   MadcNativeKind kind)
{
    bool shared = kind == mnkShared;
    // Conditional runtime dependency: a program whose every dynamic import
    // resolves within the base C/C++ and user -l libraries gets no
    // libmadc.so.0 DT_NEEDED — it runs on hosts without madc installed.
    std::vector<std::string> other;
    bool have_madc = cir_split_needed(needed, other);
    std::vector<std::string> imports;
    cir_ctx_unresolved_imports(ctx, imports);
    // -static-libmadc: the ledger pull ran before the link, so the runtime is
    // IN the capture. Verify nothing madc-side is left over, then drop the
    // dependency unconditionally — that is the flag's whole promise.
    if (madc_static_libmadc
	&& !cir_static_libmadc_verify(imports, out_path, other))
	return false;
    bool drop_madc = have_madc
		     && (madc_static_libmadc
			 || !cir_imports_need_madc_runtime(imports, other));
    DBG(std::cout << "native image DT_NEEDED: libmadc.so.0 "
		  << (drop_madc ? "dropped (runtime-free)"
				: (have_madc ? "kept" : "absent"))
		  << std::endl);
    std::vector<const char *> libs;
#if MADC_TARGET_APPLE_P
    // Mach-O: the base C/C++ sonames are cover analysis only — the emitter
    // links the implicit libSystem, nothing else. A program whose imports
    // are NOT covered would need a target madc runtime that does not exist:
    // fail at emit, not at dyld.
    if (have_madc && !drop_madc) {
	fprintf(stderr, "madc: %s: program needs the madc runtime, which does"
		" not exist as a library for Mach-O targets; build it into the"
		" image with -static-libmadc (C-lane machinery only)\n",
		out_path);
	return false;
    }
#else
    for (const std::string &l : (drop_madc ? other : needed))
	libs.push_back(l.c_str());
#endif
    MIR_object_exec_params xp;
    cir_fill_exec_params(xp, kind, libs, runpath);
    // Apple targets: the ad-hoc code-signature identifier is conventionally
    // the output basename (ignored by the ELF writer).
    const char *out_base = strrchr(out_path, '/');
    xp.identifier = out_base ? out_base + 1 : out_path;
    std::vector<uint8_t> pack_blob;
    if (madc_pack_forest_path && !cir_pack_forest_load(pack_blob))
	return false;
    cir_pack_forest_attach(xp, pack_blob);
    void *buf = NULL;
    size_t size = 0;
    if (c2mir_get_native_executable(ctx, &xp, &buf, &size) != 0 || !buf) {
	fprintf(stderr, shared
		? "madc: native shared-object emission failed\n"
		: "madc: native executable emission failed"
		  " (is main() defined?)\n");
	return false;
    }
    return cir_write_image_file(out_path, buf, size, !shared)
	   && cir_pack_forest_finish(out_path, pack_blob);
}

bool CirJitSession::emit_native_executable(const char *out_path,
					   const std::vector<std::string> &needed,
					   const std::string &runpath,
					   MadcNativeKind kind)
{
    if (!ctx || !mod) return false;
    return cir_write_native_image(ctx, out_path, needed, runpath, kind);
}

// bin/madc lives in <root>/bin; the runtime lives in <root>/lib. An
// installed madc pairs with /usr/local/lib — both go on the produced
// binary's library search path so it works from either layout.
static std::string cir_selfexe_libdir(void)
{
    std::string d = madc_self_exe_path();
    size_t slash = d.rfind('/');
    if (slash == std::string::npos)
	return std::string();
    return d.substr(0, slash) + "/../lib";
}

// DT_NEEDED / DT_RUNPATH for every produced binary — shared by the
// single-TU and --project native-emit entries.
// DT_NEEDED: the madc runtime (its dependency closure brings libmir's
// builtin exports, libz/libzstd, libpthread), the C++/C runtimes, and
// the user's -l libraries ("-lfoo" → the same lib<foo>.so spelling the
// JIT dlopen lane uses; a path form passes through verbatim).
// The C++ runtime set is the ACTIVE stdlib flavor's own (probed from its
// toolchain at build time — madc_stdlib_flavor::link_libs), never a host
// #ifdef: a -stdlib=libc++ emit from a libstdc++-built madc names
// libc++.so.1. NULL flavor = the build default (table entry 0).
static void cir_native_link_env(const madc_stdlib_flavor *flavor,
				const std::vector<std::string> &user_libs,
				std::vector<std::string> &needed,
				std::string &runpath)
{
    needed.push_back("libmadc.so.0");
#ifdef __APPLE__
    // Cover analysis runs in THIS process (dlsym + dladdr below), so the
    // base C/C++ cover spellings must name the HOST's runtime images. On
    // darwin the libc surface dladdr-reports as libsystem_*.dylib pieces
    // under the libSystem umbrella; libc++/libc++abi carry the C++ runtime.
    // Prefix-matched, so the bare stems cover every versioned dylib name.
    // (The Apple emit gate never turns these into load commands — the
    // emitter links implicit libSystem only.)
    (void)flavor;
    needed.push_back("libc++");
    needed.push_back("libsystem_");
    needed.push_back("libSystem");
#else
    if (!flavor)
	flavor = &madc_stdlib_flavors[0];
    if (flavor->link_libs && flavor->link_libs[0])
	for (int i = 0; flavor->link_libs[i]; i++)
	    needed.push_back(flavor->link_libs[i]);
    else
	// No probed link set (no compiler at build time): the historical
	// GNU default this binary was linked against itself.
	needed.push_back("libstdc++.so.6");
    needed.push_back("libm.so.6");
    needed.push_back("libc.so.6");
#endif
    for (const std::string &l : user_libs) {
	if (l.compare(0, 2, "-l") == 0)
	    needed.push_back("lib" + l.substr(2) + MADC_DSO_SUFFIX);
	else
	    needed.push_back(l);
    }
    runpath = cir_selfexe_libdir();
    if (!runpath.empty())
	runpath += ":/usr/local/lib";
    else
	runpath = "/usr/local/lib";
}

int madc_cir_emit_native(Program *prog, const char *source_name,
			 MadcNativeKind kind, const char *out_path,
			 const std::vector<std::string> &user_libs)
{
    madc_object_mode = true;	// one-shot CLI path; process exits after this

    // Standalone executables skip the __madc_shim_* eval adapters (Pass
    // 0.74): nothing can host-call them, and dropping their madc_value_*
    // imports keeps pure programs runtime-free. Objects and shared objects
    // keep the shims — they are the embedding / run-objects eval contract —
    // unless the build says otherwise with -fno-eval-shims (a .o headed for a
    // standalone -static-libmadc link).
    prog->aot_skip_eval_shims = madc_no_eval_shims
			     || kind == mnkExecutable || kind == mnkPieExecutable;
#ifdef MADC_CROSS_TARGET
    // Emit-only cross build: .o's can never feed the (refused) in-process
    // eval lane, so only -shared keeps the embedding shims — they carry
    // meaning on the TARGET, where a host with a target libmadc runtime
    // may dlopen the artifact.
    if (kind != mnkShared)
	prog->aot_skip_eval_shims = true;
#endif

    CirJitSession session;
    bool stop = false;
    if (!session.build(prog, source_name, false, false, false, &stop))
	return -1;

    // -r on a single TU is the same capture as -c: one relocatable .o.
    if (kind == mnkObject || kind == mnkRelocatable)
	return session.emit_native_object(out_path) ? 0 : -1;

    // MIR assembles the executable (-o) or shared object (-shared)
    // directly — no external toolchain.
    std::vector<std::string> needed;
    std::string runpath;
    cir_native_link_env(prog->active_stdlib_flavor(), user_libs, needed,
			runpath);
    return session.emit_native_executable(out_path, needed, runpath,
					  kind) ? 0 : -1;
}

// R4b resolver: the JIT lane's chain (host-callback regs → dlsym; bin/madc
// links -rdynamic, so the madc runtime, mir.* builtin exports, and
// __mir_*oti helpers are all dlsym-visible). The loader consults it for
// EVERY undefined symbol before failing, so each miss is named here —
// never just the first.
static void *cir_run_object_resolve(const char *name, void *env)
{
    void *addr = cir_import_resolver(name);
    if (!addr)
	fprintf(stderr, "madc: %s: unresolved symbol: %s\n",
		(const char *)env, name);
    return addr;
}

extern char **environ;

// Read a whole file into bytes; false (with the error printed) on failure.
static bool cir_read_file(const char *path, std::vector<unsigned char> &bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
	fprintf(stderr, "madc: cannot open %s: %s\n", path, strerror(errno));
	return false;
    }
    unsigned char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
	bytes.insert(bytes.end(), chunk, chunk + n);
    bool read_err = ferror(f) != 0;
    fclose(f);
    if (read_err) {
	fprintf(stderr, "madc: read error on %s\n", path);
	return false;
    }
    return true;
}

// Enter a loaded image's main, running the image's init array first — the
// loader lane's twin of ld.so/glibc's pre-main init walk (each per-TU init
// is called with the platform (argc, argv, envp) signature). Old caches
// have an empty array and keep their main-wrapped ctors. The image is
// deliberately never unloaded: the program may have registered atexit
// handlers pointing into it.
static int cir_enter_loaded_main(MIR_object_loaded_t lo, const char *display,
				 int argc, char **argv)
{
    typedef int (*cir_main_fn)(int, char **, char **);
    cir_main_fn entry = (cir_main_fn)MIR_object_loaded_sym(lo, "main");
    if (!entry) {
	fprintf(stderr, "madc: %s: no main() defined in object\n", display);
	return 1;
    }
    typedef void (*cir_init_fn)(int, char **, char **);
    size_t n_init = 0;
    void *const *inits = MIR_object_loaded_init_array(lo, &n_init);
    for (size_t i = 0; i < n_init; i++)
	((cir_init_fn)inits[i])(argc, argv, environ);
    fflush(stdout);
    return entry(argc, argv, environ);
}

int madc_cir_run_object(const char *path, int argc, char **argv)
{
    std::vector<unsigned char> bytes;
    if (!cir_read_file(path, bytes))
	return 1;

    char err[256];
    MIR_object_loaded_t lo = MIR_object_load(bytes.data(), bytes.size(),
					     cir_run_object_resolve,
					     (void *)path, err, sizeof err);
    if (!lo) {
	fprintf(stderr, "madc: %s: cannot load object: %s\n", path, err);
	return 1;
    }
    return cir_enter_loaded_main(lo, path, argc, argv);
}

// Read + merge madc-emitted .o files into one object builder — the
// multi-object link (fork MIR_object_read: sections concatenate, symbols
// unify, relocations rebase; cross-object references become internal and
// resolve at the final emit). NULL on error, with the failing path named.
static MIR_object_t cir_read_objects(const std::vector<std::string> &paths)
{
    MIR_object_t obj = MIR_object_create();
    if (!obj) {
	fprintf(stderr, "madc: object builder unavailable on this host\n");
	return NULL;
    }
    for (const std::string &p : paths) {
	std::vector<unsigned char> bytes;
	if (!cir_read_file(p.c_str(), bytes)) {
	    MIR_object_destroy(obj);
	    return NULL;
	}
	char err[256];
	// -g inputs' DWARF merges too (multi-CU output); a cache emitted
	// before the cross-section debug relocations existed is refused by
	// the reader with a re-emit message.
	if (MIR_object_read(obj, bytes.data(), bytes.size(),
			    err, sizeof err) != 0) {
	    fprintf(stderr, "madc: %s: cannot merge object: %s\n",
		    p.c_str(), err);
	    MIR_object_destroy(obj);
	    return NULL;
	}
    }
    // A cache from the pre-init-array model defines __madc_global_init and
    // relies on a main wrap this madc no longer emits — its ctors would be
    // silently skipped in a link (the very fence S3 lifts). Refuse loudly.
    if (MIR_object_find_symbol(obj, "__madc_global_init", NULL, NULL, NULL)) {
	fprintf(stderr, "madc: input predates the init-array model"
		" (defines __madc_global_init); re-emit the .o with this"
		" madc version\n");
	MIR_object_destroy(obj);
	return NULL;
    }
    return obj;
}

int madc_cir_link_objects(const std::vector<std::string> &paths,
			  MadcNativeKind kind, const char *out_path,
			  const std::vector<std::string> &user_libs,
			  Program *prog)
{
    // No compile happens here — the inputs are already captures; the fork
    // does the whole link (read/merge + emit).
    MIR_object_t obj = cir_read_objects(paths);
    if (!obj)
	return -1;
    // -static-libmadc: the runtime pieces this program needs come in as one
    // more relocatable, generated from the AOT ledger against the merged
    // builder's own UNDEF names. -r never reaches here (the CLI requires a
    // linked output), so the runtime is merged exactly once, at the link that
    // produces the image — gcc's -static-libgcc placement.
    if (madc_static_libmadc) {
	std::vector<std::string> seed;
	cir_object_undef_names(obj, seed);
	std::vector<uint8_t> rt;
	if (!cir_ledger_relocatable(prog, seed, rt)) {
	    MIR_object_destroy(obj);
	    return -1;
	}
	char err[256];
	if (!rt.empty()
	    && MIR_object_read(obj, rt.data(), rt.size(), err, sizeof err) != 0) {
	    fprintf(stderr, "madc: -static-libmadc: cannot merge the AOT-ledger"
		    " runtime: %s\n", err);
	    MIR_object_destroy(obj);
	    return -1;
	}
    }
    bool ok = false;
    void *buf = NULL;
    size_t size = 0;
    if (kind == mnkRelocatable) {
	// ld -r shape: the merged builder re-emitted as one .o.
	if (MIR_object_emit(obj, &buf, &size) != 0 || !buf)
	    fprintf(stderr, "madc: relocatable link emission failed\n");
	else
	    ok = cir_write_image_file(out_path, buf, size, false);
    } else {
	std::vector<std::string> needed;
	std::string runpath;
	cir_native_link_env(prog ? prog->active_stdlib_flavor() : NULL,
			    user_libs, needed, runpath);
	// Conditional libmadc.so.0, same policy as the source lanes; here
	// the external imports are the merged builder's UNDEF names
	// (unification already netted out cross-object references, and the
	// ledger merge above already satisfied every piece it carries).
	std::vector<std::string> other;
	bool have_madc = cir_split_needed(needed, other);
	std::vector<std::string> imports;
	cir_object_undef_names(obj, imports);
	// -static-libmadc: same contract as the source lanes — anything still
	// uncovered is Tier B, so refuse loudly rather than keep a dependency
	// the flag promised to remove. The usual cause here is the inputs'
	// host-call adapters: a `.o` carries them by default (it may feed an
	// embedding host), and their value-ABI imports are Tier B — so name the
	// compile-time flag that leaves them out.
	if (madc_static_libmadc
	    && !cir_static_libmadc_verify(imports, out_path, other,
		    "madc: if these are madc_value_* accessors, the inputs carry"
		    " the __madc_shim_* host-call adapters; recompile the"
		    " objects with -fno-eval-shims for a standalone static"
		    " link\n")) {
	    MIR_object_destroy(obj);
	    return -1;
	}
	bool drop_madc = have_madc
			 && (madc_static_libmadc
			     || !cir_imports_need_madc_runtime(imports, other));
	DBG(std::cout << "linked image DT_NEEDED: libmadc.so.0 "
		      << (drop_madc ? "dropped (runtime-free)"
				    : (have_madc ? "kept" : "absent"))
		      << std::endl);
	std::vector<const char *> libs;
#if MADC_TARGET_APPLE_P
	// Same Mach-O rule as cir_write_native_image: covers are analysis
	// only; a runtime-needing link fails at emit, not at dyld.
	if (have_madc && !drop_madc) {
	    fprintf(stderr, "madc: %s: program needs the madc runtime, which"
		    " does not exist for Mach-O targets (runtime-free"
		    " programs only)\n", out_path);
	    MIR_object_destroy(obj);
	    return -1;
	}
#else
	for (const std::string &l : (drop_madc ? other : needed))
	    libs.push_back(l.c_str());
#endif
	MIR_object_exec_params xp;
	cir_fill_exec_params(xp, kind, libs, runpath);
	const char *out_base = strrchr(out_path, '/');
	xp.identifier = out_base ? out_base + 1 : out_path;
	std::vector<uint8_t> pack_blob;
	if (madc_pack_forest_path && !cir_pack_forest_load(pack_blob)) {
	    MIR_object_destroy(obj);
	    return -1;
	}
	cir_pack_forest_attach(xp, pack_blob);
	if (MIR_object_emit_executable(obj, &xp, &buf, &size) != 0 || !buf)
	    fprintf(stderr, kind == mnkShared
		    ? "madc: native shared-object emission failed\n"
		    : "madc: native executable emission failed"
		      " (is main() defined?)\n");
	else
	    ok = cir_write_image_file(out_path, buf, size,
				      kind != mnkShared)
		 && cir_pack_forest_finish(out_path, pack_blob);
    }
    MIR_object_destroy(obj);
    return ok ? 0 : -1;
}

int madc_cir_run_objects(const std::vector<std::string> &paths,
			 int argc, char **argv,
			 const madc_stdlib_flavor *flavor)
{
    // The loader resolves through cir_import_resolver, the same dlsym(RTLD_DEFAULT)
    // chain the JIT uses, so it needs the same link environment: a .o compiled
    // -stdlib=libc++ carries mangled-direct imports only libc++/libc++abi export.
    // Without this, `madc -stdlib=libc++ prog.o` failed with "unresolved symbol:
    // _ZNSt20bad_array_new_lengthC1Ev" while the JIT lane on the same source ran.
    cir_open_stdlib_runtime(flavor);
    if (paths.size() == 1)	// the R4b single-cache lane, load-direct
	return madc_cir_run_object(paths[0].c_str(), argc, argv);
    MIR_object_t obj = cir_read_objects(paths);
    if (!obj)
	return 1;
    void *buf = NULL;
    size_t size = 0;
    int erc = MIR_object_emit(obj, &buf, &size);
    MIR_object_destroy(obj);
    if (erc != 0 || !buf) {
	fprintf(stderr, "madc: multi-object merge emission failed\n");
	return 1;
    }
    char err[256];
    MIR_object_loaded_t lo = MIR_object_load(buf, size,
					     cir_run_object_resolve,
					     (void *)paths[0].c_str(),
					     err, sizeof err);
    free(buf);	// the loader copies into its own mapping
    if (!lo) {
	fprintf(stderr, "madc: %s (+%zu more): cannot load merged object:"
		" %s\n", paths[0].c_str(), paths.size() - 1, err);
	return 1;
    }
    return cir_enter_loaded_main(lo, paths[0].c_str(), argc, argv);
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
	// Emission is rawtype-driven, but the restored dd's NAME feeds identity
	// formers (template-binding inst keys, substituted spellings). Flavor
	// TWINS share a rawtype (dtINT == dtINT32: ddINT "int" at slot 5 shadows
	// the canonical ddINT32 "int32_t" at slot 9), so "first match" restored
	// the NON-canonical name and a bound consumer's fresh mints split from
	// the pack's (the v0.68 release-lane freeze failure). Prefer the twin
	// THE one builtin table maps to itself; a table-unknown primitive
	// (int24) keeps the first-match pinning.
	uint32_t first_match = 0;
	for (uint32_t slot = 1; slot < MADC_TYPEID_PRIMITIVE_END; ++slot) {
		DataDef *p = madc_primitive_for_slot(slot);
		if (!(p && p->basetype() == BaseType::btSimple
		      && p->rawtype() == dd->rawtype()))
			continue;
		if (Program::resolve_builtin_type_spelling(p->name) == p)
			return slot;			// the canonical flavor
		if (!first_match)
			first_match = slot;
	}
	return first_match;
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

// An OPAQUE dependent placeholder never freezes (see the kill arm in
// forest_arena_record_aggregate); an alias / static-member-type entry that
// TARGETS one must not freeze either — its serialized tid has a killed
// (DK_NONE) record, and a restored alias to that husk re-launders the
// placeholder into the consumer's name resolution.
static bool forest_type_is_opaque_placeholder(DataDef *dd)
{
	DataDefCLASS *cdd = dynamic_cast<DataDefCLASS *>(dd);
	// A CONCRETE forward tag (opaque_concrete_tag — the empty-pack
	// recursion tail) is legitimate live state and freezes in the v21
	// empty shape; only pattern-context placeholders are pack artifacts.
	return cdd && cdd->is_dependent_placeholder && !cdd->opaque_concrete_tag;
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
	uint64_t carray_count = 0;
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
	else if (DataDefCArray *ca = dynamic_cast<DataDefCArray *>(dd))
	{
		// v25: a fixed-size C array type (va_list's `struct tag[1]`
		// typedef underlying) is one more unary derived type over its
		// element. A runtime-sized array (count_expr — a function-local
		// VLA shape) is not header state and is never recorded.
		if (ca->has_runtime_size())
			return;
		kind = madc::dis::DK_CARRAY; operand = ca->element_type;
		carray_count = (uint64_t)ca->count;
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
	r.carray_count_lo = (uint32_t)(carray_count & 0xffffffffu);
	r.carray_count_hi = (uint32_t)(carray_count >> 32);
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
	// Env-gated probe (MADC_MTI_PROBE_CLASS=<substr>): every aggregate
	// record write for a matching name — the duplicate-record diagnostic.
	{
		static const char *mtp = ::getenv("MADC_MTI_PROBE_CLASS");
		if (mtp && *mtp && strstr(sdd->name.c_str(), mtp))
			fprintf(stderr, "MTIPROBE recagg name=%s sdd=%p tid=%u\n",
				sdd->name.c_str(), (void *)sdd, tid);
	}
	// An OPAQUE dependent placeholder (materialize_dependent_member_type /
	// materialize_opaque_class_type: `char_traits_wchar_t__reference`,
	// `X__deref`, dependent bases) is a pack-context artifact of a
	// resolution that could not complete — not a real type. No DefFlags
	// bit carries placeholder-ness, so a frozen record restores as an
	// ORDINARY class the consumer's member-typedef/alias resolution can
	// pick up (packed testforeach2: for_each's element typed as the
	// laundered opaque -> "no matching constructor"). Skip + kill exactly
	// like the function-local arm; a consumer that genuinely needs the
	// opaque re-synthesizes it on demand, as a live parse does.
	if (cdd && cdd->is_dependent_placeholder
	    && !cdd->opaque_concrete_tag) {
		madc::dis::defrec dead;
		madc::dis::defrec old;
		memset(&dead, 0, sizeof(dead));
		if (forest_arena.get_def_at(tid, old)
		    && old.kind != madc::dis::DK_NONE)
			forest_arena.set_def_at(tid, dead);
		return;
	}

	madc::dis::defrec r;
	memset(&r, 0, sizeof(r));
	r.kind     = cdd ? madc::dis::DK_CLASS
			 : (sdd->union_layout ? madc::dis::DK_UNION : madc::dis::DK_STRUCT);
	r.name_id  = forest_arena.strings.intern(sdd->name.c_str());
	r.canon_id = sdd->canonical_cpp_spelling().empty()
		   ? 0u : forest_arena.strings.intern(sdd->canonical_cpp_spelling().c_str());
	r.size     = (uint32_t)sdd->size;
	r.datatype = (uint32_t)sdd->rawtype();
	r.pack               = (uint32_t)sdd->pack;
	r.max_align          = (uint32_t)sdd->max_align;
	r.tag_explicit_align = (uint32_t)sdd->tag_explicit_align;
	if (sdd->union_layout)           r.flags |= madc::dis::DF_UNION_LAYOUT;
	if (sdd->is_complete)            r.flags |= madc::dis::DF_IS_COMPLETE;
	// Spared CONCRETE forward tag: carry the placeholder-ness so the
	// restore re-stamps it (consumer dependence classification == live).
	if (cdd && cdd->is_dependent_placeholder)
		r.flags |= madc::dis::DF_OPAQUE_TAG;
	if (sdd->is_anonymous)           r.flags |= madc::dis::DF_IS_ANONYMOUS;
	if (sdd->reverse_scalar_storage) r.flags |= madc::dis::DF_REVERSE_SCALAR;
	if (sdd->has_anon_aggregate)     r.flags |= madc::dis::DF_HAS_ANON_AGG;
	// Function-local class (hoisted local class of a fn/method body):
	// unnameable outside its function, so a bound consumer can never
	// demand it — the bind-side admitted-set seeding skips it (R1).
	{
		HoistedDeclIdentity hid;
		if (cdd && function_local_class_identity(cdd, hid))
			r.flags |= madc::dis::DF_CLASS_FN_LOCAL;
	}
	// Known definition provenance is intrinsic to the aggregate. Unknown is
	// retained for legacy/opaque paths and preserves the previous record before
	// falling back to the ambient parser position.
	if (sdd->definition_origin ==
	    AggregateDefinitionOrigin::TranslationUnitRoot)
		r.flags |= madc::dis::DF_TU_ROOT_ORIGIN;
	else if (sdd->definition_origin == AggregateDefinitionOrigin::Unknown) {
		madc::dis::defrec old;
		if (forest_arena.get_def_at(tid, old) && old.kind != madc::dis::DK_NONE)
			r.flags |= old.flags & madc::dis::DF_TU_ROOT_ORIGIN;
		else if (forest_is_tu_root_file(TokenBase::_parse_file))
			r.flags |= madc::dis::DF_TU_ROOT_ORIGIN;
	}

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
	for (size_t i = 0; i < sdd->members.size(); ++i) {
		mt[i] = forest_serialize_type_id(sdd->members[i].second);
		// v22: a fn-ptr member type (ios_base's _Callback_list::_M_fn) has
		// no completion funnel of its own — record it (+ its target
		// signature) here, BEFORE this record's runs begin, so the nested
		// DK_FUNC param run stays contiguous.
		forest_arena_record_fptr(sdd->members[i].second);
	}
	// v32: per-member virtual-base provenance (member_vbase), resolved before
	// the run begins like every other cross-ref.
	std::map<size_t, uint32_t> mvb;
	for (std::map<size_t, DataDefCLASS *>::const_iterator vi = sdd->member_vbase.begin();
	     vi != sdd->member_vbase.end(); ++vi)
		mvb[vi->first] = forest_serialize_type_id(vi->second);
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
		std::map<size_t, uint32_t>::const_iterator vbi = mvb.find(i);
		if (vbi != mvb.end())
			m.vbase_id = vbi->second;
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
				forest_arena_record_func(mfd,
					cdd->methods[i]->data
					    ? (Method *)cdd->methods[i]->data : NULL);
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
		     ai != cdd->type_aliases.end(); ++ai) {
			if (forest_type_is_opaque_placeholder(ai->second))
				continue;
			als.push_back(std::make_pair(
				forest_arena.strings.intern(ai->first.c_str()),
				ai->second ? forest_serialize_type_id(ai->second) : 0u));
		}
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
		     si != cdd->static_member_types.end(); ++si) {
			if (forest_type_is_opaque_placeholder(si->second))
				continue;
			sts.push_back(std::make_pair(
				forest_arena.strings.intern(si->first.c_str()),
				si->second ? forest_serialize_type_id(si->second) : 0u));
		}
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
void Program::forest_arena_record_func(FuncDef *fd, Method *mth)
{
	if (!fd)
		return;
	uint32_t tid = type_id_for(fd);		// project id for the FuncDef == its arena slot

	std::vector<uint32_t> pt(fd->parameters.size());
	for (size_t p = 0; p < fd->parameters.size(); ++p) {
		pt[p] = forest_serialize_type_id(fd->parameters[p]);
		// v22: a fn-ptr param records itself + its target signature BEFORE
		// this record's param run begins (contiguity preserved).
		forest_arena_record_fptr(fd->parameters[p]);
	}
	forest_arena_record_fptr(&fd->returns);		// v22: fn-ptr return type

	madc::dis::defrec r;
	memset(&r, 0, sizeof(r));
	r.kind    = madc::dis::DK_FUNC;
	r.name_id = fd->name.empty() ? 0u : forest_arena.strings.intern(fd->name.c_str());
	r.ref0    = forest_serialize_type_id(&fd->returns);	// return type, as a type-id
	if (fd->is_varargs)       r.flags |= madc::dis::DF_IS_VARARGS;
	if (fd->is_void_params)   r.flags |= madc::dis::DF_IS_VOID_PARAMS;
	if (fd->declaration_only) r.flags |= madc::dis::DF_DECLARATION_ONLY;
	if (fd->is_const_method)  r.flags |= madc::dis::DF_IS_CONST_METHOD;
	if (fd->pure_virtual)     r.flags |= madc::dis::DF_PURE_VIRTUAL;
	if (fd->noexcept_spec == FuncDef::NxTrue)
		r.flags |= madc::dis::DF_NOEXCEPT_TRUE;
	else if (fd->noexcept_spec == FuncDef::NxUnknown)
		r.flags |= madc::dis::DF_NOEXCEPT_UNKNOWN;
	if (fd->is_member_template || !fd->template_param_names.empty())
		r.flags |= madc::dis::DF_IS_MEMBER_TEMPLATE;	// load skips it (the v6 rule)
	// FuncDef-intrinsic method metadata available at parse time (emit_symbol / display name).
	// The INLINE-body location (body_unit/body_idx + DF_HAS_FOREST_BODY) is FREEZE-time info
	// (it indexes the partitioned grove), stamped by a freeze-time fixup — not here.
	r.emit_symbol_id = fd->emit_symbol.empty()
			 ? 0u : forest_arena.strings.intern(fd->emit_symbol.c_str());
	r.disp_id        = fd->method_display_name.empty()
			 ? 0u : forest_arena.strings.intern(fd->method_display_name.c_str());
	// v21: FuncDef-intrinsic free-function state — inline_builtin_kind
	// ("addressof"/"destroy"/"forward") and the identity-return deduce
	// pattern record_skipped_template_return_pattern derives (std::move/
	// forward/declval — the call's return is the deduced arg type). All
	// empty/0 on a plain method; loaded verbatim by the free-fn restore.
	r.builtin_kind_id = fd->inline_builtin_kind.empty()
			  ? 0u : forest_arena.strings.intern(fd->inline_builtin_kind.c_str());
	r.tret_name_id    = fd->template_return_param_name.empty()
			  ? 0u : forest_arena.strings.intern(fd->template_return_param_name.c_str());
	r.tret_arg_index  = (uint32_t)fd->template_return_deduce_arg_index;
	if (fd->template_return_deduce_from_pointer)
		r.flags |= madc::dis::DF_TRET_FROM_POINTER;
	if (fd->template_return_ref)
		r.flags |= madc::dis::DF_TRET_REF;
	// v23: serialize each param's DEFAULT-argument token run (raw source
	// tokens, .madh record form) into the arena tokbytes block BEFORE the
	// paramrec run is appended (tokbytes is a separate block, but resolve-
	// first keeps the one payload discipline uniform). The parsed tree
	// (param_defaults[i]) is load-side state — the flush re-derives it by
	// re-running parseExpression over these tokens.
	std::vector<madc::dis::paramrec> prs(fd->parameters.size());
	for (size_t p = 0; p < fd->parameters.size(); ++p) {
		madc::dis::paramrec &pr = prs[p];
		memset(&pr, 0, sizeof(pr));
		pr.type_id         = pt[p];
		pr.flags           = (p < fd->const_params.size() && fd->const_params[p]) ? 1u : 0u;
		pr.cpp_spelling_id = (p < fd->param_cpp_spellings.size()
				      && !fd->param_cpp_spellings[p].empty())
				   ? forest_arena.strings.intern(fd->param_cpp_spellings[p].c_str()) : 0u;
		if (p < fd->param_default_tokens.size()
		    && !fd->param_default_tokens[p].empty()) {
			const std::vector<TokenBase *> &toks = fd->param_default_tokens[p];
			// Env-gated probe (MADC_DEFARG_PROBE=<substr>): print every
			// serialized default-arg run containing a matching identifier
			// token — the cross-flavor capture diagnostic (which FuncDef
			// froze whose substitution). Diagnostic only.
			{
				static const char *dap = ::getenv("MADC_DEFARG_PROBE");
				if (dap && *dap)
					for (TokenBase *t : toks)
						if (t && t->type() == TokenType::ttIdentifier
						    && strstr(((TokenIdent *)t)->spelling(), dap))
							fprintf(stderr, "DEFARGPROBE fd=%s param=%zu tok=%s\n",
								fd->name.c_str(), p,
								((TokenIdent *)t)->spelling());
			}
			std::vector<uint8_t> bytes;
			if (madc_pch::serialize_token_seq(toks, bytes) && !bytes.empty()) {
				uint32_t cnt = 0;
				for (TokenBase *t : toks)
					if (t)
						++cnt;
				pr.def_tok_off   = forest_arena.add_tokbytes(bytes);
				pr.def_tok_bytes = (uint32_t)bytes.size();
				pr.def_tok_count = cnt;
				for (TokenBase *t : toks)
					if (t && t->file) {
						pr.def_file_id = forest_arena.strings.intern(t->file);
						break;
					}
			}
		}
	}
	r.params_begin = (uint32_t)forest_arena.payload.size();
	r.params_count = (uint32_t)fd->parameters.size();
	for (size_t p = 0; p < prs.size(); ++p)
		forest_arena.add_payload(prs[p]);
	// v26: the Method's NAMED parameter Variables (the scope a deferred
	// body's re-parse resolves `__n` against — parseFunction's
	// temp_param_method shape). Rides the CLASS-only alias slice fields,
	// unused on a DK_FUNC record: aliasrec { name_id, type_id } per param.
	if (mth && !mth->parameters.empty()) {
		r.alias_begin = (uint32_t)forest_arena.payload.size();
		r.alias_count = (uint32_t)mth->parameters.size();
		for (size_t p = 0; p < mth->parameters.size(); ++p) {
			Variable *pv = mth->parameters[p];
			madc::dis::aliasrec ar;
			memset(&ar, 0, sizeof(ar));
			if (pv) {
				ar.name_id = forest_arena.strings.intern(
					pv->name.c_str());
				ar.type_id = pv->type
					   ? forest_serialize_type_id(pv->type) : 0;
			}
			forest_arena.add_payload(ar);
		}
	}
	forest_arena.set_def_at(tid, r);
}

// v22 (iostream): record a FUNCTION-POINTER type reached through a member /
// param / return cross-ref. DataDefFPTR has no completion funnel (born at ~10
// declarator sites), so the recording rides the cross-ref resolve loops: walk
// the unary chain (ptr/ref/const read-caches) to the FPTR, write its DK_FPTR
// record (ref0 = the target FuncDef's DK_FUNC record, encoded by the ONE
// forest_arena_record_func) at its own project slot. The defrec is written
// BEFORE the target recurses, so a self-referential signature terminates; the
// has_def guards make the whole walk idempotent. Without this, every fn-ptr
// member's chain fails at load and the aggregate is dropped — ios_base (and
// with it the whole iostream hierarchy) fell on _Callback_list::_M_fn.
void Program::forest_arena_record_fptr(DataDef *dd)
{
	if (!forest_arena_enabled)
		return;
	for (int depth = 0; dd && depth < 16; ++depth) {
		if (DataDefFPTR *fp = dynamic_cast<DataDefFPTR *>(dd)) {
			uint32_t tid = type_id_for(fp);
			if (!madc::dis::arena_id_is_project(tid)
			    || forest_arena.has_def(tid))
				return;
			madc::dis::defrec r;
			memset(&r, 0, sizeof(r));
			r.kind     = madc::dis::DK_FPTR;
			r.name_id  = forest_arena.strings.intern(fp->name.c_str());
			r.size     = (uint32_t)fp->size;
			r.datatype = (uint32_t)fp->rawtype();
			if (fp->ptr_syntax)
				r.flags |= madc::dis::DF_FPTR_PTR_SYNTAX;
			forest_arena.set_def_at(tid, r);	// self-ref guard: write first
			if (fp->target) {
				r.ref0 = forest_serialize_type_id(fp->target);
				forest_arena.set_def_at(tid, r);
				if (madc::dis::arena_id_is_project(r.ref0)
				    && !forest_arena.has_def(r.ref0))
					forest_arena_record_func(fp->target);
			}
			return;
		}
		// REF is-a PTR; both (and CONST) expose the operand as base_type.
		if (DataDefPTR *p = dynamic_cast<DataDefPTR *>(dd)) {
			dd = p->base_type;
			continue;
		}
		if (DataDefCONST *k = dynamic_cast<DataDefCONST *>(dd)) {
			dd = k->base_type;
			continue;
		}
		return;			// chain ended without an FPTR
	}
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
    // v22: the namespace a header declared the global in. Live's var-decl inside
    // `namespace std {}` registers namespace_map["std"][name] = the SAME Variable
    // that sits in tkProgram->variables; a consumer's fresh instantiation resolves
    // the tag by QUALIFIED name through that binding, so record it (reverse walk —
    // the v10 namespace discipline; first match wins, live has one owner per tag).
    auto global_ns_id = [&](Variable *gv) -> uint32_t {
	for (auto &nskv : prog->namespace_map) {
	    if (nskv.first.empty())
		continue;
	    variable_map_t::iterator ni = nskv.second.find(gv->name);
	    if (ni != nskv.second.end() && ni->second == gv)
		return pool.intern(nskv.first);
	}
	return 0;
    };
    // v24: a global DECLARED in the TU's root file (the program's own globals)
    // is fenced from the bind restore. Provenance = its dkGlobalVar TopDecl's
    // decl token; a global without one classifies include-origin (conservative:
    // it keeps restoring, like the header tag globals).
    auto global_tu_root = [&](Variable *gv) -> bool {
	for (auto &t : prog->top_decls)
	    if (t.kind == Program::DeclKind::dkGlobalVar && t.var == gv)
		return t.decl && prog->forest_is_tu_root_file(t.decl->file);
	return false;
    };
    for (Variable *v : prog->tkProgram->variables) {
	if (!v || !v->type)
	    continue;
	if ((v->flags & vfLOCAL) && !(v->flags & vfSTATIC))
	    continue;		// a non-static local is not file-scope
	// v26: a parse-time CONSTANT scalar — a plain / anonymous enum's
	// ENUMERATOR (TokenENUM's global branch: addVariable + set +
	// makeconstant; references FOLD at parse, no TopDecl, no storage,
	// live emits none). Checked BEFORE the vfEXTERN branch: glibc wraps
	// headers in extern "C" (__BEGIN_DECLS), so parsing_extern_decl
	// stamps vfEXTERN on these constants — an EXTERN_REF record restored
	// them as unresolved data imports (SOCK_STREAM / DT_REG "undefined
	// MIR import"). The one live registration stamps the origin into
	// forest_enum_const_origin; a name not in it is not enum-born.
	if (v->is_constant()) {
	    std::map<std::string, const char *>::const_iterator eo =
		prog->forest_enum_const_origin.find(v->name);
	    if (eo != prog->forest_enum_const_origin.end()) {
		uint32_t esid = forest_serialize_type_id(v->type);
		if (!esid || !seen_globals.insert(v->name).second)
			continue;
		cir_forest_global_record g;
		memset(&g, 0, sizeof(g));
		g.name_id    = pool.intern(v->name);
		g.type_id    = esid;
		g.flags      = v->flags;	// vfEXTERN/vfCONSTANT verbatim
		g.gflags     = CIR_GLOBALF_CONST_SCALAR;
		if (eo->second && prog->forest_is_tu_root_file(eo->second))
			g.gflags |= CIR_GLOBALF_TU_ROOT;
		g.ns_id      = global_ns_id(v);
		g.init_value = v->get<int64_t>();
		DBG(std::cout << "cir_forest_fill_globals: enum const "
			  << v->name << " = " << g.init_value
			  << ((g.gflags & CIR_GLOBALF_TU_ROOT)
			      ? " (TU-root)" : "")
			  << std::endl);
		f.globals.push_back(g);
		continue;
	    }
	}
	if (v->flags & vfEXTERN) {
	    // v22: an `extern T name;` REFERENCE to a library-defined object
	    // (std::cout — the iostream reducer's last gap). Record it so the
	    // flush rebuilds live's registration (vfEXTERN Variable + Itanium
	    // storage alias + namespace binding); no init, no ctor, no storage.
	    // A type not in the arena cleanly lacks, like every other global.
	    // v25: ANY resolvable extern type — a real header's `extern FILE
	    // *stdout;` is a POINTER extern the old class-only guard dropped
	    // (bound real <stdio.h>: "use of undeclared identifier 'stdout'").
	    uint32_t xtid = forest_serialize_type_id(v->type);
	    if (!(madc::dis::arena_id_is_pinned(xtid)
		  || (madc::dis::arena_id_is_project(xtid)
		      && prog->forest_arena.has_def(xtid))))
		continue;
	    if (!seen_globals.insert(v->name).second)
		continue;
	    cir_forest_global_record g;
	    memset(&g, 0, sizeof(g));
	    g.name_id = pool.intern(v->name);
	    g.type_id = xtid;
	    g.flags   = v->flags;	// vfEXTERN rides verbatim
	    g.gflags  = CIR_GLOBALF_EXTERN_REF;
	    if (global_tu_root(v))
		g.gflags |= CIR_GLOBALF_TU_ROOT;
	    g.ns_id   = global_ns_id(v);
	    f.globals.push_back(g);
	    continue;
	}
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
	    if (global_tu_root(v))
		g.gflags |= CIR_GLOBALF_TU_ROOT;
	    g.ns_id   = global_ns_id(v);
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
		// v25: ctor-syntax initializer `T name(args);` (the <compare>
		// ordering constants) — serialize the args list's raw-token run
		// (captured at parse under forest_arena_enabled) into the arena
		// tokbytes; the flush re-runs the live args-list parse over it.
		// Uncaptured args (empty run) leave gflags 0 -> the v13
		// default-ctor synthesis, today's behavior.
		if (t.decl && !t.decl->ctor_args.empty()
		    && !t.decl->ctor_arg_src.empty()) {
			std::vector<uint8_t> bytes;
			if (madc_pch::serialize_token_seq(t.decl->ctor_arg_src, bytes)
			    && !bytes.empty()) {
				uint32_t cnt = 0;
				for (TokenBase *ct : t.decl->ctor_arg_src)
					if (ct)
						++cnt;
				g.ctor_tok_off   = f.arena.add_tokbytes(bytes);
				g.ctor_tok_bytes = (uint32_t)bytes.size();
				g.ctor_tok_count = cnt;
				for (TokenBase *ct : t.decl->ctor_arg_src)
					if (ct && ct->file) {
						g.ctor_file_id =
						    f.arena.strings.intern(ct->file);
						break;
					}
				g.gflags |= CIR_GLOBALF_CTOR_ARG_TOKENS;
			}
			break;
		}
		TokenBase *rhs = t.decl ? t.decl->initialize : NULL;
		if (TokenAssign *as = dynamic_cast<TokenAssign *>(rhs))
			rhs = as->right;
		if (TokenObjTemp *ot = dynamic_cast<TokenObjTemp *>(rhs)) {
			if (ot->ctor_args.empty())
				g.gflags |= CIR_GLOBALF_CLASS_COPY_TEMP;
		} else if (TokenVar *tv = dynamic_cast<TokenVar *>(rhs)) {
			if (&tv->var == v)	// value-init RHS is the variable itself
				g.gflags |= CIR_GLOBALF_CLASS_VALUE_INIT;
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
	if (!td)
	    continue;
	// An UNINITIALIZED file-scope definition (`int REQ;` — a plain-C
	// tentative definition; also plain struct/array globals): live emits
	// a bss item (var_decl with no initializer), no ctor, no value.
	// Record the form; the flush leaves the rebuilt decl's initialize
	// NULL so the same passes emit the same bss item.
	if (!td->initialize) {
	    uint32_t usid = forest_serialize_type_id(v->type);
	    if (!usid)
		continue;
	    if (!seen_globals.insert(v->name).second)
		continue;
	    cir_forest_global_record g;
	    memset(&g, 0, sizeof(g));
	    g.name_id = pool.intern(v->name);
	    g.type_id = usid;
	    g.flags   = v->flags;
	    g.gflags  = CIR_GLOBALF_SCALAR_UNINIT;
	    if (prog->forest_is_tu_root_file(td->file))
		g.gflags |= CIR_GLOBALF_TU_ROOT;
	    g.ns_id   = global_ns_id(v);
	    f.globals.push_back(g);
	    continue;
	}
	TokenBase *rhs = td->initialize;
	if (TokenAssign *as = dynamic_cast<TokenAssign *>(rhs))
	    rhs = as->right;
	// A constant CAST of an integer literal unwraps to the literal:
	// `char *last_log = NULL;` is `((void *)0)` after the NULL macro —
	// TokenCast(void*, TokenInt(0)) — and live emits the same scalar
	// data item a bare 0 does (`last_log: u64 0`). Any non-literal cast
	// operand still fails the tkInt check below and cleanly lacks.
	if (TokenCast *tc = dynamic_cast<TokenCast *>(rhs))
	    rhs = tc->expr;
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
	if (td && prog->forest_is_tu_root_file(td->file))
	    g.gflags |= CIR_GLOBALF_TU_ROOT;
	g.ns_id      = global_ns_id(v);
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

    auto pattern_words_of = [&](const Program::ClassPattern &pattern) {
	std::vector<uint32_t> words;
	auto word64 = [&](uint64_t value) {
	    words.push_back((uint32_t)(value & 0xffffffffu));
	    words.push_back((uint32_t)(value >> 32));
	};
	auto intern_spelling = [&](const std::string &value) -> uint32_t {
	    return value.empty() ? 0 : pool.intern(value);
	};
	auto token_run = [&](const std::vector<TokenBase *> &tokens) {
	    cir_forest_token_run run = run_of(tokens);
	    words.push_back(run.tok_off);
	    words.push_back(run.tok_bytes);
	    words.push_back(run.tok_count);
	    words.push_back(run.file_id);
	};
	words.push_back(CIR_CLASS_PATTERN_MAGIC);
	words.push_back(CIR_CLASS_PATTERN_PAYLOAD_VERSION);
	words.push_back((uint32_t)pattern.capture_reason);
	word64(pattern.semantic_fingerprint);
	words.push_back(intern_spelling(pattern.identity));
	words.push_back(intern_spelling(pattern.class_name));
	words.push_back(intern_spelling(pattern.defining_namespace));
	words.push_back(pattern.is_partial_specialization ? 1u : 0u);
	words.push_back((uint32_t)pattern.types.size());
	for ( size_t i = 0; i < pattern.types.size(); ++i )
	{
	    const Program::ClassTypePattern &type = pattern.types[i];
	    words.push_back((uint32_t)type.kind);
	    words.push_back(type.flags);
	    words.push_back(type.concrete_type_id);
	    words.push_back(type.operand);
	    words.push_back(type.secondary);
	    words.push_back(type.template_param_index);
	    words.push_back(type.nested_node_id);
	    words.push_back(type.pack_param_index);
	    words.push_back(intern_spelling(type.name));
	    words.push_back((uint32_t)type.arguments.size());
	    words.push_back((uint32_t)type.dimensions.size());
	    for ( size_t a = 0; a < type.arguments.size(); ++a )
		words.push_back(type.arguments[a]);
	    for ( size_t d = 0; d < type.dimensions.size(); ++d )
		word64(type.dimensions[d]);
	}
	words.push_back((uint32_t)pattern.nodes.size());
	for ( size_t i = 0; i < pattern.nodes.size(); ++i )
	{
	    const Program::ClassAggregatePatternNode &node = pattern.nodes[i];
	    words.push_back(node.local_id);
	    words.push_back(node.parent_id);
	    words.push_back((uint32_t)node.kind);
	    words.push_back(intern_spelling(node.source_name));
	    words.push_back(intern_spelling(node.canonical_spelling));
	    words.push_back(node.complete ? 1u : 0u);
	    words.push_back(node.from_system_header ? 1u : 0u);
	    words.push_back((uint32_t)node.bases.size());
	    for ( size_t b = 0; b < node.bases.size(); ++b )
	    {
		words.push_back(node.bases[b].type);
		words.push_back(node.bases[b].access);
		words.push_back(node.bases[b].is_virtual ? 1u : 0u);
	    }
	    words.push_back((uint32_t)node.declarations.size());
	    for ( size_t d = 0; d < node.declarations.size(); ++d )
		words.push_back((uint32_t)node.declarations[d]);
	    words.push_back((uint32_t)node.aliases.size());
	    for ( size_t a = 0; a < node.aliases.size(); ++a )
	    {
		words.push_back(intern_spelling(node.aliases[a].name));
		words.push_back(node.aliases[a].type);
	    }
	    words.push_back((uint32_t)node.members.size());
	    for ( size_t m = 0; m < node.members.size(); ++m )
	    {
		const Program::ClassMemberPattern &member = node.members[m];
		words.push_back(intern_spelling(member.name));
		words.push_back(member.type);
		word64(member.count);
		words.push_back(member.access);
		words.push_back(member.is_array ? 1u : 0u);
		words.push_back(member.is_bitfield ? 1u : 0u);
		words.push_back(member.is_anonymous ? 1u : 0u);
		word64(member.bit_width);
		words.push_back((uint32_t)member.dimensions.size());
		for ( size_t d = 0; d < member.dimensions.size(); ++d )
		    word64(member.dimensions[d]);
	    }
	    words.push_back((uint32_t)node.methods.size());
	    for ( size_t m = 0; m < node.methods.size(); ++m )
	    {
		const Program::ClassMethodPattern &method = node.methods[m];
		words.push_back((uint32_t)method.kind);
		words.push_back(intern_spelling(method.variable_name));
		words.push_back(intern_spelling(method.display_name));
		words.push_back(intern_spelling(method.storage_alias_name));
		words.push_back(intern_spelling(method.local_emit_name)); // allowed-exception: pattern persistence field, not symbol build
		words.push_back(intern_spelling(method.emit_symbol));
		words.push_back(intern_spelling(method.return_typedef_name));
		words.push_back(method.return_type);
		words.push_back(method.flags);
		words.push_back(method.is_varargs ? 1u : 0u);
		words.push_back(method.is_void_params ? 1u : 0u);
		words.push_back(method.declaration_only ? 1u : 0u);
		words.push_back(method.defaulted_or_deleted ? 1u : 0u);
		words.push_back(method.is_deleted ? 1u : 0u);
		words.push_back((uint32_t)method.noexcept_spec);
		words.push_back(method.pure_virtual ? 1u : 0u);
		words.push_back(method.is_const_method ? 1u : 0u);
		words.push_back(method.is_member_template ? 1u : 0u);
		words.push_back(method.has_eager_body ? 1u : 0u);
		words.push_back((uint32_t)method.parameters.size());
		for ( size_t p = 0; p < method.parameters.size(); ++p )
		{
		    const Program::ClassMethodParamPattern &param =
			method.parameters[p];
		    words.push_back(intern_spelling(param.name));
		    words.push_back(param.type);
		    words.push_back(param.flags);
		    words.push_back(param.is_const ? 1u : 0u);
		    words.push_back(param.template_param_spelled_directly ? 1u : 0u);
		    words.push_back(intern_spelling(param.cpp_spelling));
		    words.push_back(intern_spelling(param.typedef_name));
		    token_run(param.default_tokens);
		}
		words.push_back((uint32_t)method.template_param_names.size());
		for ( size_t p = 0; p < method.template_param_names.size(); ++p )
		    words.push_back(intern_spelling(method.template_param_names[p]));
		words.push_back((uint32_t)method.template_param_is_type.size());
		for ( size_t p = 0; p < method.template_param_is_type.size(); ++p )
		    words.push_back(method.template_param_is_type[p] ? 1u : 0u);
		words.push_back((uint32_t)method.template_param_is_pack.size());
		for ( size_t p = 0; p < method.template_param_is_pack.size(); ++p )
		    words.push_back(method.template_param_is_pack[p] ? 1u : 0u);
		words.push_back(intern_spelling(method.template_return_spelling));
		words.push_back((uint32_t)method.template_param_spellings.size());
		for ( size_t p = 0; p < method.template_param_spellings.size(); ++p )
		    words.push_back(intern_spelling(method.template_param_spellings[p]));
		token_run(method.body_tokens);
		token_run(method.definition_tokens);
		token_run(method.trailing_ret_tokens);
		token_run(method.ctor_init_tokens);
		token_run(method.member_template_decl);
		token_run(method.member_template_return_tokens);
		// v36: per-param default token runs ([temp.deduct]/8 SFINAE).
		words.push_back((uint32_t)method.template_param_defaults.size());
		for ( size_t p = 0; p < method.template_param_defaults.size(); ++p )
		    token_run(method.template_param_defaults[p]);
		// v38: per-param CONSTRAINT-type runs (gate the non-type
		// default fill) — between defaults (v36) and the function-
		// param type runs (v37), matching the reader's order.
		words.push_back((uint32_t)method.template_param_constraints.size());
		for ( size_t p = 0; p < method.template_param_constraints.size(); ++p )
		    token_run(method.template_param_constraints[p]);
		// v37: per FUNCTION-parameter TYPE token runs (the OTHER
		// [temp.deduct]/8 half — SFINAE in a param type,
		// `typename _Up::iterator_category* = nullptr`).
		words.push_back((uint32_t)method.param_type_token_runs.size());
		for ( size_t p = 0; p < method.param_type_token_runs.size(); ++p )
		    token_run(method.param_type_token_runs[p]);
	    }
	    words.push_back((uint32_t)node.using_members.size());
	    for ( size_t u = 0; u < node.using_members.size(); ++u )
	    {
		words.push_back(node.using_members[u].owner_type);
		words.push_back(intern_spelling(node.using_members[u].name));
	    }
	    words.push_back((uint32_t)node.nested_templates.size());
	    for ( size_t t = 0; t < node.nested_templates.size(); ++t )
	    {
		const Program::ClassNestedTemplatePattern &nested =
		    node.nested_templates[t];
		words.push_back((uint32_t)nested.kind);
		words.push_back((uint32_t)nested.typeparams.size());
		for ( size_t p = 0; p < nested.typeparams.size(); ++p )
		    words.push_back(intern_spelling(nested.typeparams[p]));
		words.push_back((uint32_t)nested.typeparam_defaults.size());
		for ( size_t p = 0; p < nested.typeparam_defaults.size(); ++p )
		    token_run(nested.typeparam_defaults[p]);
		words.push_back((uint32_t)nested.typeparam_is_type.size());
		for ( size_t p = 0; p < nested.typeparam_is_type.size(); ++p )
		    words.push_back(nested.typeparam_is_type[p] ? 1u : 0u);
		words.push_back((uint32_t)nested.typeparam_is_pack.size());
		for ( size_t p = 0; p < nested.typeparam_is_pack.size(); ++p )
		    words.push_back(nested.typeparam_is_pack[p] ? 1u : 0u);
		words.push_back(nested.has_non_type_params ? 1u : 0u);
		words.push_back(intern_spelling(nested.class_name));
		token_run(nested.body);
		words.push_back(intern_spelling(nested.defining_namespace));
		words.push_back(nested.is_partial_specialization ? 1u : 0u);
		words.push_back((uint32_t)nested.spec_pattern.size());
		for ( size_t p = 0; p < nested.spec_pattern.size(); ++p )
		    token_run(nested.spec_pattern[p]);
		token_run(nested.constraint);
		token_run(nested.target);
	    }
	    words.push_back((uint32_t)node.static_members.size());
	    for ( size_t s = 0; s < node.static_members.size(); ++s )
	    {
		words.push_back(intern_spelling(node.static_members[s].first));
		words.push_back(node.static_members[s].second);
	    }
	    words.push_back((uint32_t)node.static_values.size());
	    for ( size_t s = 0; s < node.static_values.size(); ++s )
	    {
		words.push_back(intern_spelling(node.static_values[s].first));
		word64((uint64_t)node.static_values[s].second);
	    }
	    words.push_back((uint32_t)node.friend_classes.size());
	    for ( size_t fidx = 0; fidx < node.friend_classes.size(); ++fidx )
		words.push_back(intern_spelling(node.friend_classes[fidx]));
	    words.push_back((uint32_t)node.friend_functions.size());
	    for ( size_t fidx = 0; fidx < node.friend_functions.size(); ++fidx )
		words.push_back(intern_spelling(node.friend_functions[fidx]));
	}
	return words;
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
	    const std::vector<std::vector<TokenBase *> > &spec,
	    const Program::ClassPattern *pattern,
	    Program::ClassParseReason pattern_reason) {
	cir_forest_template_record r;
	memset(&r, 0, sizeof(r));
	r.kind    = kind;
	r.key_id  = pool.intern(key);
	r.name_id = name.empty() ? 0 : pool.intern(name);
	r.ns_id   = ns.empty() ? 0 : pool.intern(ns);
	r.extra_id = extra.empty() ? 0 : pool.intern(extra);
	r.owner_type_id = owner ? forest_serialize_type_id(owner) : 0;
	r.flags   = flags;
	r.pattern_reason = (uint32_t)pattern_reason;
	// v24: a pattern CAPTURED in the TU's root file (the program's own
	// templates) is fenced from the bind restore. Provenance = the first
	// body/decl token carrying a file; a record with no body run (a
	// decl-only member template's return range, a concept's constraint)
	// derives it from the constraint run instead.
	bool fenced_provenance = false;
	for (TokenBase *t : body)
		if (t && t->file) {
			if (prog->forest_is_tu_root_file(t->file))
				r.flags |= CIR_TMPLF_TU_ROOT;
			fenced_provenance = true;
			break;
		}
	if (!fenced_provenance)
		for (TokenBase *t : constraint)
			if (t && t->file) {
				if (prog->forest_is_tu_root_file(t->file))
					r.flags |= CIR_TMPLF_TU_ROOT;
				break;
			}
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
	if (pattern) {
	    std::vector<uint32_t> words = pattern_words_of(*pattern);
	    r.pattern_begin = (uint32_t)f.template_payload.size();
	    r.pattern_words = (uint32_t)words.size();
	    f.template_payload.insert(f.template_payload.end(),
		words.begin(), words.end());
	}
	{
	    static const char *cpp_probe = ::getenv("MADC_CLASS_PATTERN_PROBE");
	    if (cpp_probe && (name.find(cpp_probe) != std::string::npos
			       || !strcmp(cpp_probe, "*")))
		std::cerr << "[class-pattern-probe] freeze-emit: " << ns << "::"
		    << name << " kind=" << kind
		    << " reason=" << r.pattern_reason
		    << " pattern=" << (pattern ? 1 : 0)
		    << " words=" << r.pattern_words << std::endl;
	}
	f.templates.push_back(r);
    };

    static const std::vector<std::vector<TokenBase *> > no_multi;
    static const std::vector<TokenBase *> no_toks;
    static const std::vector<bool> no_bools;

    // v31 serialized member-template keys used owner-name + US + bare-name.
    // Keep that cold wire spelling for reader compatibility and closure-filter
    // semantics; the live registry itself is keyed only by the bare-name id.
    auto frozen_template_key = [](const char *bare_name,
	    const std::string &template_name, DataDefCLASS *owner) {
	if (!owner)
	    return std::string(bare_name);
	std::string key;
	key.reserve(owner->name.size() + 1 + template_name.size());
	key.append(owner->name);
	key.push_back('\x1f');
	key.append(template_name);
	return key;
    };

    auto emit_class_registry = [&](uint32_t kind, const char *key,
	    Program::template_registry_entry_t &registry) {
	auto emit_variants = [&](std::vector<Program::TemplateDef> &variants) {
	    for (Program::TemplateDef &td : variants) {
		const std::string record_key = frozen_template_key(
		    key, td.class_name, td.owner_class);
		emit(kind, record_key.c_str(), td.class_name,
		     td.defining_namespace,
		     std::string(), td.owner_class,
		     (td.has_non_type_params ? CIR_TMPLF_HAS_NON_TYPE_PARAMS : 0)
		     | (td.is_partial_specialization ? CIR_TMPLF_IS_PARTIAL_SPEC : 0),
		     td.typeparams, td.typeparam_is_type, td.typeparam_is_pack,
		     td.typeparam_defaults, td.body, td.constraint, td.spec_pattern,
		     prog->materialize_class_pattern(td), td.class_pattern_reason);
	    }
	};
	emit_variants(registry.namespace_variants);
	std::vector<std::pair<std::string,
	    std::vector<Program::TemplateDef> *> > owners;
	for (std::unordered_map<DataDefCLASS *,
		std::vector<Program::TemplateDef> >::iterator it =
		registry.member_variants.begin();
	     it != registry.member_variants.end(); ++it) {
	    const std::string &canonical =
		it->first->canonical_cpp_spelling();
	    owners.push_back(std::make_pair(
		canonical.empty() ? it->first->name : canonical, &it->second));
	}
	std::sort(owners.begin(), owners.end(),
	    [](const std::pair<std::string,
		       std::vector<Program::TemplateDef> *> &a,
	       const std::pair<std::string,
		       std::vector<Program::TemplateDef> *> &b) {
		return a.first < b.first;
	    });
	for (size_t i = 0; i < owners.size(); ++i)
	    emit_variants(*owners[i].second);
    };
    prog->template_map.for_each([&](const char *key,
	    Program::template_registry_entry_t &registry) {
	emit_class_registry(CIR_TMPLK_CLASS, key, registry);
	return false;
    });
    prog->partial_spec_map.for_each([&](const char *key,
	    Program::template_registry_entry_t &registry) {
	emit_class_registry(CIR_TMPLK_PARTIAL, key, registry);
	return false;
    });
    prog->template_alias_map.for_each([&](const char *key,
	    Program::template_alias_registry_entry_t &registry) {
	auto emit_variants = [&](std::vector<Program::TemplateAliasDef> &variants) {
	    for (Program::TemplateAliasDef &ad : variants) {
		const std::string record_key = frozen_template_key(
		    key, ad.alias_name, ad.owner_class);
		emit(CIR_TMPLK_ALIAS, record_key.c_str(), ad.alias_name,
		     ad.defining_namespace, std::string(), ad.owner_class,
		     ad.has_non_type_params ? CIR_TMPLF_HAS_NON_TYPE_PARAMS : 0,
		     ad.typeparams, ad.typeparam_is_type, ad.typeparam_is_pack,
		     ad.typeparam_defaults, ad.target, no_toks, no_multi, NULL,
		     Program::ClassParseReason::None);
	    }
	};
	emit_variants(registry.namespace_variants);
	std::vector<std::pair<std::string,
	    std::vector<Program::TemplateAliasDef> *> > owners;
	for (std::unordered_map<DataDefCLASS *,
		std::vector<Program::TemplateAliasDef> >::iterator it =
		registry.member_variants.begin();
	     it != registry.member_variants.end(); ++it) {
	    const std::string &canonical =
		it->first->canonical_cpp_spelling();
	    owners.push_back(std::make_pair(
		canonical.empty() ? it->first->name : canonical, &it->second));
	}
	std::sort(owners.begin(), owners.end(),
	    [](const std::pair<std::string,
		       std::vector<Program::TemplateAliasDef> *> &a,
	       const std::pair<std::string,
		       std::vector<Program::TemplateAliasDef> *> &b) {
		return a.first < b.first;
	    });
	for (size_t i = 0; i < owners.size(); ++i)
	    emit_variants(*owners[i].second);
	return false;
    });
    prog->fn_template_map.for_each([&](const char *key, std::vector<Program::FnTemplateDef> &v) {
	for (Program::FnTemplateDef &fd : v)
	    emit(CIR_TMPLK_FN, key, std::string(), fd.ns, fd.inline_builtin_kind,
		 fd.owner_class,
		 fd.instance_method ? CIR_TMPLF_INSTANCE_METHOD : 0,
		 fd.typeparams, fd.typeparam_is_type, fd.typeparam_is_pack,
		 fd.typeparam_defaults, fd.decl, no_toks,
		 fd.typeparam_constraints, NULL,
		 Program::ClassParseReason::None);
	return false;
    });
    prog->fn_template_decl_map.for_each([&](const char *key, std::vector<Program::FnTemplateDef> &v) {
	for (Program::FnTemplateDef &fd : v)
	    emit(CIR_TMPLK_FN_DECL, key, std::string(), fd.ns, fd.inline_builtin_kind,
		 fd.owner_class,
		 fd.instance_method ? CIR_TMPLF_INSTANCE_METHOD : 0,
		 fd.typeparams, fd.typeparam_is_type, fd.typeparam_is_pack,
		 fd.typeparam_defaults, fd.decl, no_toks,
		 fd.typeparam_constraints, NULL,
		 Program::ClassParseReason::None);
	return false;
    });
    prog->var_template_map.for_each([&](const char *key, Program::VarTemplateDef &vd) {
	emit(CIR_TMPLK_VAR, key, std::string(), vd.defining_namespace,
	     std::string(), NULL, 0,
	     vd.typeparams, no_bools, vd.typeparam_is_pack,
	     no_multi, vd.init, no_toks, no_multi, NULL,
	     Program::ClassParseReason::None);
	return false;
    });
    for (std::map<std::string, Program::ConceptDef>::iterator ci =
	     prog->concept_map.begin(); ci != prog->concept_map.end(); ++ci)
	emit(CIR_TMPLK_CONCEPT, ci->first.c_str(), std::string(),
	     ci->second.defining_namespace, std::string(), NULL, 0,
	     ci->second.typeparams, no_bools, no_bools,
	     no_multi, no_toks, ci->second.constraint, no_multi, NULL,
	     Program::ClassParseReason::None);

    // v21: body-bearing MEMBER function templates — the pattern state
    // register_skipped_class_template_function leaves on a class's FuncDef
    // (_Destroy_aux::__destroy, allocator_traits::construct, ...). The record
    // carries the owner + params + the retained decl tokens (declarator +
    // params + body, sans template<> header); load re-runs the SAME live
    // registration over the restored tokens, so every derived field
    // (return-token range, spellings, static/instance, ctor-hood) reproduces
    // by the one production path. An owner not in the arena cleanly lacks.
    // v34: DECL-ONLY member templates (the __do_common_type_impl::_S_test
    // SFINAE shape) retain no decl tokens — only the dependent return-type
    // range, which rides the record's otherwise-empty constraint-run slot;
    // the flush stamps the restored placeholder's fields directly. Without
    // the record a thawed decltype(_S_test<...>(0)) fell to the placeholder's
    // implicit 64-bit return (LOADED != parsed, silent wrong answer).
    for (funcdef_map_iter fi = prog->funcdef_map.begin();
	 fi != prog->funcdef_map.end(); ++fi) {
	FuncDef *fd = fi->second;
	if (!fd || !fd->is_member_template)
	    continue;
	bool body_bearing = !fd->member_template_decl.empty();
	if (!body_bearing && fd->member_template_return_tokens.empty())
	    continue;	// nothing restorable: no body, no return range
	DataDefCLASS *owner = dynamic_cast<DataDefCLASS *>(fd->member_template_owner);
	if (!owner)
	    continue;
	DataDefPTR *p0 = fd->parameters.empty()
		       ? NULL : dynamic_cast<DataDefPTR *>(fd->parameters[0]);
	bool instance = p0 && p0->base_type == owner;
	// v36 semantics on the UNCHANGED record layout: the per-param default
	// runs (always in the layout, previously written empty here) now carry
	// the member template's [temp.deduct]/8 SFINAE defaults
	// (`typename = decltype(declval<_Tp1&>().~_Tp1())`).
	if (::getenv("MADC_XTEST_VRI_DEBUG")) {
		size_t nd = 0;
		for (size_t di = 0; di < fd->member_template_param_defaults.size(); ++di)
			nd += fd->member_template_param_defaults[di].size();
		fprintf(stderr, "[vriprobe] FREEZE member %s defaults=%zu toks=%zu\n",
			fd->method_display_name.c_str(),
			fd->member_template_param_defaults.size(), nd);
	}
	// v38: the per-param CONSTRAINT runs ride the record's spec slot —
	// the same convention the FN lane adopted at v33 for its
	// typeparam_constraints. A reader of an older record sees spec
	// empty and degrades to clearing every non-type default.
	emit(CIR_TMPLK_MEMBER, fi->first.c_str(), fd->method_display_name,
	     std::string(), std::string(), owner,
	     instance ? CIR_TMPLF_INSTANCE_METHOD : 0,
	     fd->template_param_names, fd->template_param_is_type,
	     fd->template_param_is_pack, fd->member_template_param_defaults,
	     fd->member_template_decl,
	     body_bearing ? no_toks : fd->member_template_return_tokens,
	     fd->member_template_param_constraints, NULL,
	     Program::ClassParseReason::None);
    }

    // v21: out-of-line member DEFINITIONS of class templates (vector.tcc's
    // `template<..> RET vector<..>::_M_realloc_insert(..){..}`) — the eighth
    // pattern map. OUTER (class) params first, INNER (member-template) params
    // flagged CIR_TMPLP_IS_INNER — a dedicated writer because the shared emit
    // has one param list; the record/run layout is identical (body run +
    // constraint run + one default run per param, so the generic reader
    // applies).
    for (std::map<std::string, std::vector<Program::OutOfLineMemberDef> >::iterator
	     oi = prog->out_of_line_member_defs.begin();
	 oi != prog->out_of_line_member_defs.end(); ++oi) {
	for (size_t di = 0; di < oi->second.size(); ++di) {
	    Program::OutOfLineMemberDef &d = oi->second[di];
	    cir_forest_template_record r;
	    memset(&r, 0, sizeof(r));
	    r.kind    = CIR_TMPLK_OUTOFLINE;
	    r.key_id  = pool.intern(oi->first);
	    r.name_id = d.member_name.empty() ? 0 : pool.intern(d.member_name);
	    r.flags   = d.is_member_template ? CIR_TMPLF_OOL_MEMBER_TMPL : 0;
	    size_t total = d.typeparams.size() + d.inner_typeparams.size();
	    std::vector<cir_forest_token_run> runs;
	    runs.push_back(run_of(d.decl));
	    runs.push_back(run_of(std::vector<TokenBase *>()));
	    for (size_t i = 0; i < total; ++i)
		runs.push_back(run_of(std::vector<TokenBase *>()));
	    r.param_count = (uint32_t)total;
	    r.spec_count  = 0;
	    bool first = true;
	    for (size_t i = 0; i < total; ++i) {
		cir_forest_template_param p;
		if (i < d.typeparams.size()) {
		    p.name_id = pool.intern(d.typeparams[i]);
		    p.pflags  = CIR_TMPLP_IS_TYPE;
		} else {
		    size_t j = i - d.typeparams.size();
		    p.name_id = pool.intern(d.inner_typeparams[j]);
		    p.pflags  = CIR_TMPLP_IS_INNER;
		    if (j >= d.inner_is_type.size() || d.inner_is_type[j])
			p.pflags |= CIR_TMPLP_IS_TYPE;
		    if (j < d.inner_is_pack.size() && d.inner_is_pack[j])
			p.pflags |= CIR_TMPLP_IS_PACK;
		}
		uint32_t off = madc::dis::pod_append(f.template_payload, p);
		if (first) { r.param_begin = off; first = false; }
	    }
	    first = true;
	    for (size_t i = 0; i < runs.size(); ++i) {
		uint32_t off = madc::dis::pod_append(f.template_payload, runs[i]);
		if (first) { r.run_begin = off; first = false; }
	    }
	    f.templates.push_back(r);
	}
    }
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
static void cir_forest_arena_complete(Program *prog, cir_frozen_forest &f,
				      const std::set<std::string> *pack_uncarriable)
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

	// 1b (v20, discriminator revised v25): BODIED free-function body
	// locations — header-defined function definitions (embedded <ns_*>
	// namespace wrappers, user-header helpers, instantiated __mti /
	// __ns_*__oN definitions). The stamp fences by the v24 discriminator —
	// ROOT-vs-INCLUDE — on the body's OWN origin (funcdef_files: the def's
	// origin-token file; an instantiated definition lands in the main-file
	// unit but its tokens carry the header origin), falling back to the
	// unit name. A producer ROOT definition (main, program fns) stays
	// body-less — a consumer never inherits the producer's roots — and an
	// unknown origin conservatively stays body-less too. (The old
	// is_system_header_path predicate dropped EMBEDDED headers' bodied
	// wrappers — `perl::chop`'s "ns_perl" pseudo-path — and every
	// user-header helper.)
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
			std::string fsym = a.strings.str(r.name_id);
			std::map<std::string, std::pair<uint32_t, uint32_t> >::const_iterator
				bl = f.funcdef_locs.find(fsym);
			if (bl == f.funcdef_locs.end()) {
				DBG(std::cout << "arena_complete 1b: bodied " << fsym
					      << " has NO func-def in the partition"
					      << std::endl);
				continue;
			}
			uint32_t unit = bl->second.first;
			if (unit >= f.units.size())
				continue;
			// A record whose DECLARATION provenance is already the TU
			// root (decl_file — precise when set) is fenced at restore
			// anyway; never stamp it.
			if (r.flags & madc::dis::DF_TU_ROOT_ORIGIN) {
				DBG(std::cout << "arena_complete 1b: bodied " << fsym
					      << " is TU-root origin (no body stamp)"
					      << std::endl);
				continue;
			}
			// BODY origin: the def's own file, else the unit name.
			const char *body_file = NULL;
			std::map<std::string, const char *>::const_iterator
				ff = f.funcdef_files.find(fsym);
			if (ff != f.funcdef_files.end() && ff->second)
				body_file = ff->second;
			if (!body_file)
				body_file = pool.c_str(f.units[unit].unit_name_id);
			if (!body_file || prog->forest_is_tu_root_file(body_file)) {
				DBG(std::cout << "arena_complete 1b: bodied " << fsym
					      << " body origin "
					      << (body_file ? body_file : "(unknown)")
					      << " is TU-root/unknown (no body stamp)"
					      << std::endl);
				continue;
			}
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
	struct arena_alias { uint32_t name_id, ns_id, ref0, flags; };
	std::vector<arena_alias> aliases;

	for (std::set<std::string>::const_iterator it = prog->user_typedef_names.begin();
	     it != prog->user_typedef_names.end(); ++it) {
		flat_datatype_map_iter dti = prog->datatype_map.find(*it);
		if (dti == prog->datatype_map.end() || !*dti) {
			DBG(std::cout << "arena_complete: flat typedef " << *it
				      << " has no datatype_map entry — cleanly lacks"
				      << std::endl);
			continue;
		}
		DataDef *underlying = &(*dti)->definition;
		if (!underlying)
			continue;
		uint32_t uid = forest_serialize_type_id(underlying);
		if (!(madc::dis::arena_id_is_pinned(uid)
		      || (madc::dis::arena_id_is_project(uid) && a.has_def(uid)))) {
			DBG(std::cout << "arena_complete: flat typedef " << *it
				      << " underlying tid=" << uid
				      << " not resolvable — cleanly lacks" << std::endl);
			continue;	// underlying not resolvable from the arena — cleanly lack
		}
		arena_alias p;
		p.name_id = a.strings.intern(*it);
		p.ns_id   = 0;
		p.ref0    = uid;
		// v24: a typedef declared in the TU's root file is fenced from the
		// bind restore (provenance = the registered token's file).
		p.flags   = prog->forest_is_tu_root_file((*dti)->file)
			  ? (uint32_t)madc::dis::DF_TU_ROOT_ORIGIN : 0u;
		// A struct/class-KEYWORD typedef (`typedef struct [tag] {...} X;` /
		// `typedef struct tag X;`) ALSO registers the alias as a tag —
		// struct_map[X] = the aggregate — so `struct X` resolves (the
		// tagless glibc fd_set/div_t shape). The plain TokenTYPEDEF path
		// never writes struct_map, so the producer's own map state is the
		// discriminator (same key -> same definition, the
		// DF_TYPEDEF_FLAT_ALIAS precedent); the restore reproduces the write.
		// The no-body array form `typedef struct tag X[N];` maps the key to
		// the tag's STRUCT while the alias definition is the CARRAY (parser
		// ~24055 gates on !is_pointer only) — same-definition check walks
		// the array element to match it.
		{
			DataDef *tag_dd = underlying;
			if (DataDefCArray *ca = dynamic_cast<DataDefCArray *>(underlying))
				tag_dd = ca->element_type;
			datadef_map_citer smi = prog->struct_map.find(*it);
			if (smi != prog->struct_map.end() && tag_dd
			    && smi->second == tag_dd)
				p.flags |= (uint32_t)madc::dis::DF_TYPEDEF_TAG_ALIAS;
		}
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
				p.flags   = prog->forest_is_tu_root_file(it->second->file)
					  ? (uint32_t)madc::dis::DF_TU_ROOT_ORIGIN : 0u;
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
					p.flags   = prog->forest_is_tu_root_file(
							it->second->file)
						  ? (uint32_t)madc::dis::DF_TU_ROOT_ORIGIN : 0u;
					// v26: an explicit-specialization alias (the
					// alias_key surface, TokenTEMPLATE::parse) ALSO
					// lives in the producer's FLAT datatype_map under
					// the same key -> same definition — the use-site
					// instantiation cache reads the flat map, so the
					// restore must reproduce that write (a pmr-style
					// namespaced typedef has no flat twin and must
					// NOT — the [iobind] clobber class).
					flat_datatype_map_iter fdi =
						prog->datatype_map.find(it->first);
					if (fdi != prog->datatype_map.end() && *fdi
					    && &(*fdi)->definition
						== &it->second->definition)
						p.flags |= (uint32_t)
						    madc::dis::DF_TYPEDEF_FLAT_ALIAS;
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
		r.flags   = aliases[i].flags;	// v24: TU-root fence bit
		a.set_def_at(next, r);
	}

	// 4 (v25): INLINE-namespace links — `namespace std { inline namespace
	// __cxx11 { ... } }` makes the child's members visible in the parent via
	// mirror_inline_namespace_into_parent at each block close. The link map
	// (Program::inline_namespace_children) is parse-time state with no
	// DataDef; serialize it so the flush can RE-RUN the one live mirror over
	// restored state (stod / to_string are std::__cxx11 members a consumer
	// resolves as members of std).
	for (std::map<std::string, std::vector<std::string> >::const_iterator
	     li = prog->inline_namespace_children.begin();
	     li != prog->inline_namespace_children.end(); ++li) {
		for (size_t c = 0; c < li->second.size(); ++c, ++next) {
			madc::dis::defrec r;
			memset(&r, 0, sizeof(r));
			r.kind    = madc::dis::DK_NSLINK;
			r.ns_id   = li->first.empty()
				  ? 0u : a.strings.intern(li->first.c_str());
			r.name_id = a.strings.intern(li->second[c].c_str());
			a.set_def_at(next, r);
		}
	}

	// 5 (v25): USING-DECLARATION function imports — `namespace std {
	// using ::abort; }` registers TWO surfaces ([namespace.udecl]): the
	// namespace_map[ns][name] binding (first-wins) AND membership in
	// ns::name's overload set. Both must round-trip: a bound consumer
	// ranking a SMALLER set than the freezing parse resolves a different
	// winner and mints a DIFFERENT instantiation identity (LOADED==parsed
	// violation — the release pack's stl_vector.h:428 verify failure).
	// 5a walks the overload sets for cross-home members (the using-arm is
	// the only writer of those) and records them FLAGGED; 5b reverse-walks
	// namespace_map for any remaining fn binding (an import that WON the
	// map slot is already covered by 5a; a mirrored inline-ns entry
	// records bind-only and rebinds the same Variable first-wins at flush
	// — the live mirror grows no sets, so its record must not either). A
	// TU-root target's record is fenced at restore, so its bind cleanly
	// lacks.
	{
		std::map<FuncDef *, std::string> fd_keys;
		for (funcdef_map_iter fit = prog->funcdef_map.begin();
		     fit != prog->funcdef_map.end(); ++fit)
			if (fit->second)
				fd_keys[fit->second] = fit->first;
		std::set<std::string> recorded;	// ns \x01 name \x01 key
		// 5a: overload-set MEMBERSHIP (flagged records).
		for (const auto &osi : prog->namespace_fn_overload_sets) {
			size_t sep = osi.first.rfind("::");
			if (sep == std::string::npos || sep == 0)
				continue;	// global-scope key ("::name"): no ns to rebind
			std::string ns   = osi.first.substr(0, sep);
			std::string name = osi.first.substr(sep + 2);
			for (size_t ei = 0; ei < osi.second.size(); ++ei) {
				const Program::NamespaceFnOverload &e = osi.second[ei];
				if (!e.var || !e.var->type
				    || (!e.param_spelling.empty()
					&& e.param_spelling[0] == '\x01'))
					continue;	// the fn-template placeholder seed
				FuncDef *fd = dynamic_cast<FuncDef *>(e.var->type);
				if (!fd)
					continue;
				if (fd->namespace_name == ns
				    && fd->function_display_name == name)
					continue;	// home registration: funcs-flush reproduces it
				std::map<FuncDef *, std::string>::const_iterator
					ki = fd_keys.find(fd);
				if (ki == fd_keys.end())
					continue;
				recorded.insert(ns + "\x01" + name + "\x01" + ki->second);
				madc::dis::defrec r;
				memset(&r, 0, sizeof(r));
				r.kind    = madc::dis::DK_NSBIND;
				r.flags   = madc::dis::DF_NSBIND_OVERLOAD_MEMBER;
				r.ns_id   = a.strings.intern(ns.c_str());
				r.name_id = a.strings.intern(name.c_str());
				r.disp_id = a.strings.intern(ki->second.c_str());
				a.set_def_at(next++, r);
			}
		}
		// 5b: remaining map bindings (bind-only records).
		for (namespace_map_t::iterator ni = prog->namespace_map.begin();
		     ni != prog->namespace_map.end(); ++ni) {
			if (ni->first.empty())
				continue;
			for (variable_map_iter vi = ni->second.begin();
			     vi != ni->second.end(); ++vi) {
				Variable *v = vi->second;
				if (!v || !v->type || !v->type->is_function())
					continue;
				FuncDef *fd = dynamic_cast<FuncDef *>(v->type);
				if (!fd)
					continue;
				if (fd->namespace_name == ni->first
				    && fd->function_display_name == vi->first)
					continue;	// the defining registration
				std::map<FuncDef *, std::string>::const_iterator
					ki = fd_keys.find(fd);
				if (ki == fd_keys.end())
					continue;
				if (recorded.count(ni->first + "\x01" + vi->first
						   + "\x01" + ki->second))
					continue;	// 5a already carries bind + join
				madc::dis::defrec r;
				memset(&r, 0, sizeof(r));
				r.kind    = madc::dis::DK_NSBIND;
				r.ns_id   = a.strings.intern(ni->first.c_str());
				r.name_id = a.strings.intern(vi->first.c_str());
				r.disp_id = a.strings.intern(ki->second.c_str());
				a.set_def_at(next++, r);
			}
		}
	}

	// 6 (v26): DEFERRED METHOD BODIES — a system-header method body the
	// producer never ODR-used (live materializes it from TOKENS via
	// parse_deferred_lazy_body on first use; the frozen AST has no func-def
	// for it, so without this record the consumer's first use dies as an
	// undefined MIR import — basic_string's copy ctor, __gnu_cxx
	// char_traits compare). Serialize each map entry's four token vectors
	// as tokbytes runs + the owner class + full_definition + position; the
	// flush rebuilds the entry (var = the restored method Variable) so the
	// EXISTING m&l fixpoint re-runs the one live derivation on use.
	for (std::map<std::string, Program::DeferredFunctionBody>::const_iterator
	     di = prog->deferred_lazy_bodies.begin();
	     di != prog->deferred_lazy_bodies.end(); ++di) {
		const Program::DeferredFunctionBody &b = di->second;
		if (di->first.empty())
			continue;
		// Env-gated probe (MADC_MTI_PROBE=<substr>): DEFBODY freeze walk.
		{
			static const char *mtp = ::getenv("MADC_MTI_PROBE");
			if (mtp && *mtp
			    && di->first.find(mtp) != std::string::npos)
				fprintf(stderr, "MTIPROBE defbody sym=%s file=%s"
					" root=%d full=%d body=%zu def=%zu\n",
					di->first.c_str(), b.file ? b.file : "(none)",
					b.file ? (int)prog->forest_is_tu_root_file(b.file)
					       : -1,
					(int)b.full_definition,
					b.body_tokens.size(),
					b.definition_tokens.size());
		}
		// The forest holds #include state only (v24): a root-file class's
		// deferred body never restores into a consumer.
		if (b.file && prog->forest_is_tu_root_file(b.file))
			continue;
		// An instantiation-born FREE-function body (a namespace
		// fn-template product the pack's own evaluation enqueued —
		// __ns_std_uninitialized_copy__o2): its token-run derive needs
		// the instantiation's template-param bindings, which the record
		// does not carry ("Expecting a type argument to
		// iterator_traits<>"). The consumer's home for these is
		// RE-INSTANTIATION through the template machinery. Class-member
		// products (owner_class set) keep their DEFBODY — the owner
		// scope restores their derive context.
		if (b.var && (!b.method || !b.method->owner_class)) {
			FuncDef *vfd = dynamic_cast<FuncDef *>(b.var->type);
			if (vfd && vfd->tsubst_source)
				continue;
		}
		const std::vector<TokenBase *> *seqs[4] = {
			&b.body_tokens, &b.definition_tokens,
			&b.trailing_ret_tokens, &b.ctor_init_tokens
		};
		uint32_t runs[4][4];	// off / bytes / count / file_id
		memset(runs, 0, sizeof(runs));
		bool any = false;
		for (int sx = 0; sx < 4; ++sx) {
			if (seqs[sx]->empty())
				continue;
			std::vector<uint8_t> bytes;
			if (!madc_pch::serialize_token_seq(*seqs[sx], bytes)
			    || bytes.empty())
				continue;
			uint32_t cnt = 0;
			for (TokenBase *t : *seqs[sx])
				if (t)
					++cnt;
			runs[sx][0] = a.add_tokbytes(bytes);
			runs[sx][1] = (uint32_t)bytes.size();
			runs[sx][2] = cnt;
			for (TokenBase *t : *seqs[sx])
				if (t && t->file) {
					runs[sx][3] = a.strings.intern(t->file);
					break;
				}
			any = true;
		}
		if (!any)
			continue;
		DataDefCLASS *owner = b.method ? b.method->owner_class : NULL;
		madc::dis::defrec r;
		memset(&r, 0, sizeof(r));
		r.kind      = madc::dis::DK_DEFBODY;
		r.name_id   = a.strings.intern(di->first.c_str());
		r.ref0      = owner ? forest_serialize_type_id(owner) : 0;
		r.disp_id   = b.file ? a.strings.intern(b.file) : 0;
		r.body_unit = (uint32_t)b.line;
		r.body_idx  = (uint32_t)b.column;
		if (b.full_definition)
			r.flags |= madc::dis::DF_DEFBODY_FULL_DEFINITION;
		r.params_begin = (uint32_t)a.payload.size();
		r.params_count = 4;
		for (int sx = 0; sx < 4; ++sx)
			for (int w = 0; w < 4; ++w)
				a.payload.push_back(runs[sx][w]);
		a.set_def_at(next++, r);
	}

	// 6b (v26 piece a): UNREFERENCED bodied FREE functions — an
	// include-origin fn the producer parsed but never called (std::abs's
	// overloads) has NO TRANSLATED def in the frozen AST, so its RC2
	// record was WAS_BODIED without a body stamp and the restore dropped
	// even the DECLARATION ("'abs' is not a member of namespace 'std'").
	// parseFunction captured the raw body span (forest_body_tokens, the
	// parse_deferred_function_body::body_tokens shape); serialize it as an
	// OWNERLESS DK_DEFBODY (ref0=0, non-full) and stamp the DK_FUNC record
	// DF_FUNC_DEF_TOKENS so the declaration restores — the flush plants
	// the deferred entry and the m&l fixpoint materializes the body on
	// first ODR-use through the ONE live derivation.
	for (funcdef_map_t::const_iterator fi = prog->funcdef_map.begin();
	     fi != prog->funcdef_map.end(); ++fi) {
		FuncDef *fd = fi->second;
		if (!fd || fd->forest_body_tokens.empty() || fi->first.empty())
			continue;
		// Instantiation-born product (tsubst_source): its token-run
		// derive needs the instantiation's template-param bindings,
		// which no record carries — the consumer re-instantiates
		// instead (same rule as the deferred-map walk above).
		if (fd->tsubst_source)
			continue;
		// Same rule via the pack's own verdict: an UN-CARRIABLE symbol
		// (pack_dropped — a product whose def the cascade dropped
		// without a DEFBODY revert, e.g. __ns_std__Destroy__i* whose
		// tsubst_source is NULL) must not plant a body span either —
		// its captured tokens spell PATTERN params (substitution lives
		// in parser state, not tokens) and the consumer's derive dies
		// ("Expecting a type argument to iterator_traits<>"). With no
		// span AND no DF_FUNC_DEF_TOKENS stamp, the was_bodied
		// declaration drop applies at load and the consumer's fresh
		// use re-instantiates the pattern — live semantics.
		if (pack_uncarriable && pack_uncarriable->count(fi->first))
			continue;
		uint32_t ftid = madc_type_id_for(fd);
		madc::dis::defrec fr;
		if (!madc::dis::arena_id_is_project(ftid)
		    || !a.get_def_at(ftid, fr)
		    || fr.kind != madc::dis::DK_FUNC)
			continue;	// no restored declaration -> no body to plant
		if (fr.flags & (madc::dis::DF_HAS_FOREST_BODY
				| madc::dis::DF_TU_ROOT_ORIGIN
				| madc::dis::DF_FUNC_DEF_TOKENS))
			continue;	// translated / fenced / already stamped
		// The BODY's own origin fences too (the 1b body-stamp rule):
		// a header-DECLARED fn DEFINED in the TU root (testmacroargsspace's
		// `char *get_hint args((int level));` proto + root body) has an
		// include-origin DK_FUNC (decl_file = the header) but a ROOT-origin
		// body — the forest holds #include state only, and the consumer
		// parses the root definition itself (a restored body would define
		// it twice: "Repeated item declaration"). Unknown origin = fenced.
		const char *bfile = NULL;
		int bline = 0, bcol = 0;
		for (TokenBase *t : fd->forest_body_tokens)
			if (t && t->file) {
				bfile = t->file;
				bline = t->line;
				bcol  = t->column;
				break;
			}
		if (!bfile || prog->forest_is_tu_root_file(bfile)) {
			DBG(std::cout << "arena_complete 6b: bodied " << fi->first
				      << " body origin "
				      << (bfile ? bfile : "(unknown)")
				      << " is TU-root/unknown (no DEFBODY)"
				      << std::endl);
			continue;
		}
		std::vector<uint8_t> bytes;
		if (!madc_pch::serialize_token_seq(fd->forest_body_tokens, bytes)
		    || bytes.empty())
			continue;
		uint32_t cnt = 0;
		for (TokenBase *t : fd->forest_body_tokens)
			if (t)
				++cnt;
		madc::dis::defrec r;
		memset(&r, 0, sizeof(r));
		r.kind      = madc::dis::DK_DEFBODY;
		r.name_id   = a.strings.intern(fi->first.c_str());
		r.ref0      = 0;	// ownerless: a FREE function's body
		r.disp_id   = bfile ? a.strings.intern(bfile) : 0;
		r.body_unit = (uint32_t)bline;
		r.body_idx  = (uint32_t)bcol;
		r.params_begin = (uint32_t)a.payload.size();
		r.params_count = 4;
		uint32_t brun[4] = { a.add_tokbytes(bytes), (uint32_t)bytes.size(),
				     cnt, r.disp_id };
		for (int w = 0; w < 4; ++w)
			a.payload.push_back(brun[w]);
		// Slot 1 (definition) + slot 2 (trailing-ret) stay empty; a
		// ctor's MEM-INITIALIZER-LIST rides slot 3 (the flush's
		// ctor_init_tokens slot) when the class-close capture saw one.
		std::vector<uint8_t> ibytes;
		uint32_t icnt = 0;
		if (!fd->forest_ctor_init_tokens.empty()
		    && madc_pch::serialize_token_seq(fd->forest_ctor_init_tokens,
						     ibytes) && !ibytes.empty())
			for (TokenBase *t : fd->forest_ctor_init_tokens)
				if (t)
					++icnt;
		for (int sx = 1; sx < 4; ++sx) {
			if (sx == 3 && icnt) {
				uint32_t irun[4] = { a.add_tokbytes(ibytes),
						     (uint32_t)ibytes.size(),
						     icnt, r.disp_id };
				for (int w = 0; w < 4; ++w)
					a.payload.push_back(irun[w]);
				continue;
			}
			for (int w = 0; w < 4; ++w)
				a.payload.push_back(0u);
		}
		a.set_def_at(next++, r);
		fr.flags |= madc::dis::DF_FUNC_DEF_TOKENS;
		// v27: the body's PARSE CONTEXT rides the DK_FUNC — an
		// instantiated __oN definition was parsed at
		// fn_template_instantiation_depth > 0 and its re-run must be.
		if (fd->forest_body_in_instantiation)
			fr.flags |= madc::dis::DF_BODY_IN_INSTANTIATION;
		a.set_def_at(ftid, fr);
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
static void cir_forest_arena_refresh(Program *prog,
				     const std::set<std::string> *pack_uncarriable)
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
			if (!sdd)
				continue;
			// v21: an INCOMPLETE aggregate is live state too when it has
			// no member/base payload to mistrust — the synthesized empty
			// tail of a recursion (tuple's _Tuple_impl<N>) is registered,
			// inherited from, and emitted by live without ever completing.
			// Dropping it dropped every class derived from it.
			if (!sdd->is_complete) {
				DataDefCLASS *icdd = dynamic_cast<DataDefCLASS *>(sdd);
				if (!sdd->members.empty() || (icdd && !icdd->bases.empty()))
					continue;
			}
			prog->forest_arena_record_aggregate(sdd);
		}
		done = n;
	}

	// v21: ENUM types — a live parse leaves each C++ enum tag as a
	// DataDefENUM in datatype_map (the namespace membership is stamped by
	// the completion walk like any named record) and its SCOPED enumerators
	// as constant Variables in the tag's pseudo-namespace
	// (namespace_map["ns::Tag"] — TokenENUM::parse). Record the type as
	// DK_ENUM at its project slot; the enumerators ride a constvalrec run;
	// the pseudo-namespace key derives from canonical_cpp_spelling() exactly
	// as the live registration built it.
	prog->datatype_map.for_each([&](const char *key, TokenDataType *&tdt) -> bool {
		if (!tdt)
			return false;
		DataDefENUM *edd = dynamic_cast<DataDefENUM *>(&tdt->definition);
		if (!edd || edd->name != key)
			return false;	// an alias entry records at its tag only
		uint32_t tid = madc_type_id_for(edd);
		if (!madc::dis::arena_id_is_project(tid)
		    || prog->forest_arena.has_def(tid))
			return false;
		madc::dis::defrec r;
		memset(&r, 0, sizeof(r));
		r.kind    = madc::dis::DK_ENUM;
		// v24: an enum tag defined in the TU's root file is fenced from
		// the bind restore (provenance = its registered token's file).
		if (prog->forest_is_tu_root_file(tdt->file))
			r.flags |= madc::dis::DF_TU_ROOT_ORIGIN;
		r.name_id = prog->forest_arena.strings.intern(edd->name.c_str());
		r.canon_id = edd->canonical_cpp_spelling().empty() ? 0u
			   : prog->forest_arena.strings.intern(
				edd->canonical_cpp_spelling().c_str());
		r.size    = (uint32_t)edd->size;
		// A FIXED underlying base drives the enum's layout AND its
		// lowered C type ([dcl.enum]p8, DataDefENUM::set_underlying);
		// the restore must re-adopt both, so carry the base's type-id
		// in ref0 (free for DK_ENUM; primitives are pinned ids).
		r.ref0    = edd->underlying
			  ? forest_serialize_type_id(edd->underlying) : 0u;
		// Scoped enumerators: the pseudo-namespace key is the canonical
		// spelling when namespaced (std::__cmp_cat::_Ord), else the tag.
		const std::string &pk = edd->canonical_cpp_spelling().empty()
				      ? edd->name : edd->canonical_cpp_spelling();
		std::map<std::string, variable_map_t>::iterator ni =
			prog->namespace_map.find(pk);
		std::vector<madc::dis::constvalrec> evs;
		if (ni != prog->namespace_map.end())
			for (variable_map_iter vi = ni->second.begin();
			     vi != ni->second.end(); ++vi) {
				Variable *ev = vi->second;
				if (!ev || !ev->is_constant())
					continue;
				madc::dis::constvalrec cv;
				memset(&cv, 0, sizeof(cv));
				cv.name_id = prog->forest_arena.strings.intern(
					vi->first.c_str());
				uint64_t uv = (uint64_t)ev->get<int64_t>();
				cv.val_lo = (uint32_t)(uv & 0xffffffffu);
				cv.val_hi = (uint32_t)(uv >> 32);
				evs.push_back(cv);
			}
		r.constval_begin = (uint32_t)prog->forest_arena.payload.size();
		r.constval_count = (uint32_t)evs.size();
		for (size_t e = 0; e < evs.size(); ++e)
			prog->forest_arena.add_payload(evs[e]);
		prog->forest_arena.set_def_at(tid, r);
		return false;
	});

	// RC2: FREE FUNCTIONS — record each file-scope free function (the
	// funcdef_map surface a live header parse leaves behind) as its own
	// DK_FUNC, flagged DF_IS_FREE_FUNC with name_id = the funcdef_map key (the
	// call name). Same interim freeze-time capture as the aggregates above
	// (a parseFunction write-through is part of the ~411-site rollout).
	// Runs AFTER the aggregate fixpoint so every METHOD FuncDef already has
	// its DK_FUNC record (recorded via its class) and is skipped structurally
	// by has_def. Selection: every funcdef_map entry — prototypes (RC2),
	// BODIED functions (v20, flagged DF_WAS_BODIED: an instantiated __mti /
	// __ns_*__oN definition whose loaded callers reference its symbol, or a
	// producer root like main — cir_forest_arena_complete stamps a forest
	// body location ONLY for the system-header-origin ones, and load restores
	// ONLY those; a bodied record without a body location cleanly lacks),
	// PLUS (v21) declaration-only entries WITH a function_display_name — the
	// skipped-ns-fn-template PLACEHOLDERS (__ns_std__Destroy) that are the
	// resolution chokepoint for a qualified `std::_Destroy(...)` call in a
	// NEW-specialization instantiation. Templates / member-template PATTERNS
	// still skip (a pattern is not a concrete symbol).
	for (funcdef_map_iter it = prog->funcdef_map.begin();
	     it != prog->funcdef_map.end(); ++it) {
		FuncDef *fd = it->second;
#ifdef MADC_DBG_PACK
		bool rc2probe = it->first.find("stoi") != std::string::npos
			     || it->first.find("_to_string") != std::string::npos;
		if (rc2probe)
			fprintf(stderr, "[RC2] key=%s fd=%p mt=%d tparams=%zu unc=%d\n",
				it->first.c_str(), (void *)fd,
				fd ? (int)fd->is_member_template : -1,
				fd ? fd->template_param_names.size() : (size_t)0,
				pack_uncarriable ? (int)pack_uncarriable->count(it->first) : -1);
#endif
		if (!fd || it->first.empty())
			continue;
		if (fd->is_member_template || !fd->template_param_names.empty())
			continue;
		// UN-CARRIABLE pack product (pack_dropped): no record at all —
		// a restored decl-only rank would SHADOW the pattern
		// placeholder in the consumer's overload set (ranking picks
		// the concrete-shaped rank, cannot instantiate it — mt=0,
		// decl=0, tparams=0 — and falls back to the un-emittable BASE
		// symbol). With no record, a fresh call routes to the pattern
		// placeholder and instantiates — live semantics.
		if (pack_uncarriable && pack_uncarriable->count(it->first))
			continue;
		uint32_t tid = madc_type_id_for(fd);
#ifdef MADC_DBG_PACK
		if (rc2probe)
			fprintf(stderr, "[RC2] key=%s tid=%u proj=%d has_def=%d\n",
				it->first.c_str(), tid,
				(int)madc::dis::arena_id_is_project(tid),
				(int)prog->forest_arena.has_def(tid));
#endif
		if (!madc::dis::arena_id_is_project(tid)
		    || prog->forest_arena.has_def(tid))
			continue;	// already recorded = a class's method
		{
			// v26: pass the fn's Method (its NAMED param scope) when the
			// program Variable carries one — a future deferred re-parse
			// resolves param names against it.
			Variable *fnv = prog->tkProgram
				? prog->tkProgram->findVariable(prog->strpool, it->first)
				: NULL;
			prog->forest_arena_record_func(fd,
				fnv && fnv->data ? (Method *)fnv->data : NULL);
		}
		madc::dis::defrec r;
		if (!prog->forest_arena.get_def_at(tid, r))
			continue;
		r.name_id = prog->forest_arena.strings.intern(it->first.c_str());
		r.flags  |= madc::dis::DF_IS_FREE_FUNC;
		if (!fd->declaration_only)
			r.flags |= madc::dis::DF_WAS_BODIED;
		// v24: a free function DECLARED in the TU's root file (the
		// program's own prototypes) is fenced from the bind restore.
		if (prog->forest_is_tu_root_file(fd->decl_file))
			r.flags |= madc::dis::DF_TU_ROOT_ORIGIN;
		// v21: the free-function source identity the live registration
		// sets — a skipped-ns-fn-template PLACEHOLDER (__ns_std__Destroy,
		// declaration-only + display + ns) and an instantiated __oN
		// definition both carry it; load rebuilds the placeholder's
		// namespace_map binding + overload-set seed from these.
		// (method_display_name is empty on a free FuncDef, so disp_id is
		// free to carry function_display_name on a DF_IS_FREE_FUNC record.)
		if (!fd->function_display_name.empty())
			r.disp_id = prog->forest_arena.strings.intern(
				fd->function_display_name.c_str());
		if (!fd->namespace_name.empty())
			r.ns_id = prog->forest_arena.strings.intern(
				fd->namespace_name.c_str());
		prog->forest_arena.set_def_at(tid, r);
	}
}

// MIR module cache (2026-07-17 design): compile a just-assembled container
// image to its MIR module in FRESH contexts — exactly the compile a consumer's
// --run-frozen would run — and serialize it with MIR_write_module. Error-
// contained: check-clean does NOT imply gen-clean (c2mir can still fatal on a
// defective drained body — "undeclared func reg fp"), so a MIR fatal here
// longjmps back, the pack carries no blob, and every consumer falls back to
// node materialization bit-for-bit.
static bool cir_forest_mir_cache_blob(const void *image, size_t image_len,
				      const char *source_name,
				      std::vector<uint8_t> &out)
{
    MIR_context_t ctx = MIR_init();
    MIR_set_error_func(ctx, cir_mir_error);
    c2mir_init(ctx);
    c2m_ctx_t c2m = cir_init(ctx, /*debug_p=*/false);
    bool ok = false;
    {
	CirFrozenForest forest;
	do {
	    if (!c2m)
		break;
	    if (!forest.open(image, image_len, c2m))
		break;
	    cir_node *root = forest.root();
	    if (!root)
		break;
	    if (cir_report_errors(stderr, root->as_node()))
		break;
	    if (setjmp(cir_mir_error_jmp)) {
		cir_mir_error_armed = false;
		cir_mir_cache_sink = NULL;
		fprintf(stderr, "%s: mir cache: MIR error during module "
			"compile: %s — blob skipped (consumers fall back)\n",
			source_name, cir_mir_error_text);
		break;
	    }
	    cir_mir_error_armed = true;
	    bool compiled = cir_compile(ctx, c2m, root->as_node(), "frozen");
	    MIR_module_t mod = compiled
		? DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx)) : NULL;
	    if (!mod) {
		cir_mir_error_armed = false;
		fprintf(stderr, "%s: mir cache: module compile failed — "
			"blob skipped (consumers fall back)\n", source_name);
		break;
	    }
	    cir_forest_mir_header mh;
	    mh.forest_version = CIR_FOREST_FORMAT_VERSION;
	    mh.mir_api_x100 = (uint32_t)(_MIR_get_api_version() * 100.0 + 0.5);
	    out.clear();
	    out.insert(out.end(), (const uint8_t *)&mh,
		       (const uint8_t *)&mh + sizeof(mh));
	    cir_mir_cache_sink = &out;
	    MIR_write_module_with_func(ctx, cir_mir_cache_write_byte, mod);
	    cir_mir_cache_sink = NULL;
	    cir_mir_error_armed = false;
	    ok = out.size() > sizeof(mh);
	} while (0);
	if (c2m)
	    cir_finish(c2m);
    }
    c2mir_finish(ctx);
    MIR_finish(ctx);
    if (!ok)
	out.clear();
    return ok;
}

// Serialize one built module to .bmir bytes. Its own frame, for the same
// reason as cir_ledger_read_module: a longjmp out of the MIR error bracket
// must not leave the caller's loop variables indeterminate.
static bool cir_ledger_write_module(MIR_context_t ctx, MIR_module_t mod,
				    const char *name,
				    std::vector<uint8_t> &out)
{
    if (setjmp(cir_mir_error_jmp)) {
	cir_mir_error_armed = false;
	cir_mir_cache_sink = NULL;
	fprintf(stderr, "madc: ledger: %s: MIR error while serializing: %s\n",
		name, cir_mir_error_text);
	return false;
    }
    cir_mir_error_armed = true;
    cir_mir_cache_sink = &out;
    MIR_write_module_with_func(ctx, cir_mir_cache_write_byte, mod);
    cir_mir_cache_sink = NULL;
    cir_mir_error_armed = false;
    return true;
}

// --- AOT ledger (forest-carriers S5) -------------------------------------
//
// Compile each C-lane runtime source through the SAME front end a user
// program takes (engine -> Program -> tokenize -> parse -> build_tu_module)
// and serialize the resulting MIR module. The module is target-correct by
// construction: it is this madc, with this madc's target and headers, so a
// cross madc packs a cross-target ledger from the identical sources.
//
// MADC_RT_TLS is defined EMPTY here: MIR has no thread-local storage, so the
// ledger's copy of the exception state is process-global (see src/rt/rt_except.c).
bool madc_cir_ledger_compile(MadcEngine &engine,
			     const std::vector<std::string> &sources,
			     std::vector<cir_ledger_module> &out,
			     Program *restore)
{
    out.clear();
    if (sources.empty())
	return true;

    // Parse every source before any MIR context exists — tokenize()/parse()
    // can throw, and a throw inside the MIR bracket would leak the contexts
    // (the project lane's ordering rule, project_parse_all).
    std::vector<std::unique_ptr<Program> > parsed;
    for (const std::string &src : sources) {
	std::unique_ptr<Program> prog = engine.create_program();
	prog->colors = true;
	// A ledger source is madc's OWN runtime: never bind grove state into
	// it (the container being packed is the one we would bind), and read
	// it as the C dialect gcc uses for a .c file.
	prog->registration_policy.enable_forest_bind = false;
	// A ledger module is madc's own runtime, merged into a program's image
	// — never host-called through the MadValue ABI. Without this every
	// exported runtime function would carry a __madc_shim_<sym> adapter
	// importing madc_value_*, which is exactly the libmadc dependency
	// -static-libmadc exists to remove.
	prog->aot_skip_eval_shims = true;
	prog->add_cli_define("MADC_RT_TLS=");
	prog->set_language_standard_option("--std=gnu17");
	TokenProgram *tp = prog->tokenize(src.c_str());
	if (!tp) {
	    fprintf(stderr, "madc: ledger: %s: tokenize failed\n", src.c_str());
	    return false;
	}
	if (!prog->parse(tp)) {
	    fprintf(stderr, "madc: ledger: %s: parse failed\n", src.c_str());
	    return false;
	}
	parsed.push_back(std::move(prog));
    }

    MIR_context_t ctx = MIR_init();
    MIR_set_error_func(ctx, cir_mir_error);
    c2mir_init(ctx);
    c2m_ctx_t c2m = cir_init(ctx, /*debug_p=*/false);
    std::vector<CirBuilder *> builders;
    bool ok = true;
    if (!c2m) {
	fprintf(stderr, "madc: ledger: cir_init failed\n");
	ok = false;
    }
    for (size_t i = 0; ok && i < parsed.size(); i++) {
	CirBuilder *builder = NULL;
	bool stop = false;
	MIR_module_t mod = build_tu_module(ctx, c2m, parsed[i].get(),
					   sources[i].c_str(),
					   false, false, false, builder, stop);
	builders.push_back(builder);
	if (!mod) {
	    fprintf(stderr, "madc: ledger: %s: module build failed\n",
		    sources[i].c_str());
	    ok = false;
	    break;
	}
	cir_ledger_module lm;
	lm.name = sources[i];
	// The DEFINED symbol set is the selection index: every func / data
	// item the module owns. Imports, protos and forwards define nothing.
	for (MIR_item_t it = DLIST_HEAD(MIR_item_t, mod->items); it;
	     it = DLIST_NEXT(MIR_item_t, it)) {
	    if (it->item_type == MIR_import_item
		|| it->item_type == MIR_export_item
		|| it->item_type == MIR_forward_item
		|| it->item_type == MIR_proto_item)
		continue;
	    const char *nm = MIR_item_name(ctx, it);
	    if (nm && nm[0])
		lm.syms.push_back(nm);
	}
	if (!cir_ledger_write_module(ctx, mod, sources[i].c_str(), lm.bytes)) {
	    ok = false;
	    break;
	}
	if (lm.bytes.empty() || lm.syms.empty()) {
	    fprintf(stderr, "madc: ledger: %s: empty module (%zu symbols,"
		    " %zu bytes)\n", sources[i].c_str(), lm.syms.size(),
		    lm.bytes.size());
	    ok = false;
	    break;
	}
	DBG(std::cout << "ledger: " << lm.name << ": " << lm.syms.size()
		      << " symbols, " << lm.bytes.size() << " bytes"
		      << std::endl);
	out.push_back(std::move(lm));
    }
    for (CirBuilder *b : builders) delete b;
    if (c2m)
	cir_finish(c2m);
    c2mir_finish(ctx);
    MIR_finish(ctx);
    // build_tu_module made each ledger Program the ACTIVE owner of the
    // process-global token/value/type pools; those Programs die when `parsed`
    // unwinds, so leaving them active would hand the caller dangling pools
    // (a freeze interning into freed memory — SIGSEGV, seen 2026-07-26).
    // Restore whoever was active on entry: this is a side excursion, and it
    // owns putting the statics back.
    if (restore)
	restore->activate_token_pools();
    if (!ok)
	out.clear();
    return ok;
}

// Serialize the compiled ledger into its container segment payload:
// header, directory, then the name / symbol / MIR blocks in the same order
// (the read side is CirFrozenForest::ledger_modules).
static void cir_ledger_serialize(const std::vector<cir_ledger_module> &ledger,
				 std::vector<uint8_t> &out)
{
    cir_forest_ledger_header lh;
    memset(&lh, 0, sizeof lh);
    lh.forest_version = CIR_FOREST_FORMAT_VERSION;
    lh.mir_api_x100 = (uint32_t)(_MIR_get_api_version() * 100.0 + 0.5);
    lh.module_count = (uint32_t)ledger.size();
    out.assign((const uint8_t *)&lh, (const uint8_t *)&lh + sizeof lh);

    std::vector<std::string> symblocks;
    for (const cir_ledger_module &m : ledger) {
	std::string syms;
	for (const std::string &s : m.syms) {
	    syms += s;
	    syms += '\0';
	}
	symblocks.push_back(std::move(syms));
    }
    for (size_t i = 0; i < ledger.size(); i++) {
	cir_forest_ledger_entry e;
	memset(&e, 0, sizeof e);
	e.name_bytes = (uint32_t)ledger[i].name.size();
	e.sym_bytes = (uint32_t)symblocks[i].size();
	e.mir_bytes = (uint32_t)ledger[i].bytes.size();
	out.insert(out.end(), (const uint8_t *)&e,
		   (const uint8_t *)&e + sizeof e);
    }
    for (size_t i = 0; i < ledger.size(); i++) {
	out.insert(out.end(), ledger[i].name.begin(), ledger[i].name.end());
	out.insert(out.end(), symblocks[i].begin(), symblocks[i].end());
	out.insert(out.end(), ledger[i].bytes.begin(), ledger[i].bytes.end());
    }
}

int madc_cir_freeze(Program *prog, const char *source_name,
		    const char *out_path, bool append, bool mir_cache,
		    const std::vector<cir_ledger_module> *ledger)
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
	// Rung 1 (2026-07-09 plan): under a grove-carrying freeze the builder's
	// m&l fixpoint DRAINS deferred_lazy_bodies (referenced or not) so the
	// container carries translated func-defs, not DK_DEFBODY token runs.
	// Report the drain so the left-deferred fallback count is visible.
	size_t db_before = prog->deferred_lazy_bodies.size();
	node_t tree = cir_translate_guarded(c2m, prog, source_name, builder);
	size_t db_after = prog->deferred_lazy_bodies.size();
	if (prog->pack_recording && db_before)
	    fprintf(stderr, "%s: pack drain: %zu deferred bodies evaluated, "
		    "%zu left deferred\n", source_name,
		    db_before > db_after ? db_before - db_after : 0, db_after);
	// MADC_FREEZE_DUMP_TREE=<path>: dump the translated tree AS FROZEN —
	// side A of the freeze/thaw round-trip diff (side B is
	// MADC_THAW_DUMP_TREE in build_frozen).
	if (tree)
	    if (const char *dp = getenv("MADC_FREEZE_DUMP_TREE")) {
		FILE *df = fopen(dp, "w");
		if (df) { c2mir_dump_tree(c2m, df, tree); fclose(df); }
	    }
	int nerr = tree ? cir_report_errors(stderr, tree) : 0;
	// Rung 1, layer 4 — pack-side c2mir check gate. The drain evaluates
	// bodies live never compiles, so a defective drained lowering would
	// freeze silently and only surface as a --run-frozen / consumer check
	// error. Gate: run c2mir's checker (do_context) over a deep COPY of
	// the pristine tree in a FRESH compile context (check mutates attrs
	// and its symbol tables are not reentrant), map each defective
	// top-level item back to its stashed pack def, revert it to DEFBODY
	// (consumers derive on use — today's pre-drain semantics), cascade its
	// callers, splice the dropped defs out of the pristine tree, and
	// re-check until clean. A defective item that is NOT a drained def is
	// a TU defect: abort the freeze loudly.
	bool gate_ok = true;
	if (tree && !nerr && prog->pack_recording && builder) {
	    for (int round = 1; ; ++round) {
		std::vector<int> bad;
		int cerr = -1;
		MIR_context_t cctx = MIR_init();
		MIR_set_error_func(cctx, cir_mir_error);
		c2mir_init(cctx);
		c2m_ctx_t cc2m = cir_init(cctx, /*debug_p=*/false);
		if (cc2m) {
		    node_t copy = c2mir_copy_tree(cc2m, c2m, tree);
		    // Diagnostic (env-gated): dump original + copy for a
		    // structural diff when the gate's verdict is suspected of
		    // being a COPY artifact rather than a tree defect.
		    if (copy && getenv("MADC_GATE_DUMP")) {
			FILE *fo = fopen("tmp/gate_orig.dump", "w");
			FILE *fc = fopen("tmp/gate_copy.dump", "w");
			if (fo) { c2mir_dump_tree(c2m, fo, tree); fclose(fo); }
			if (fc) { c2mir_dump_tree(cc2m, fc, copy); fclose(fc); }
		    }
		    if (copy)
			cerr = c2mir_check_tree(cc2m, copy, pack_gate_note, &bad);
		    cir_finish(cc2m);
		}
		c2mir_finish(cctx);
		MIR_finish(cctx);
		if (cerr < 0) {
		    fprintf(stderr, "%s: pack check gate: cannot check the "
			    "translated tree\n", source_name);
		    gate_ok = false;
		    break;
		}
		if (cerr == 0) {
		    if (round > 1)
			fprintf(stderr, "%s: pack check gate: clean after "
				"%d drop round(s)\n", source_name, round - 1);
		    break;
		}
		int nd = builder->pack_gate_drop(tree, bad);
		if (nd <= 0) {
		    fprintf(stderr, "%s: pack check gate: %d check error(s) "
			    "not attributable to drained defs; not freezing\n",
			    source_name, cerr);
		    gate_ok = false;
		    break;
		}
		fprintf(stderr, "%s: pack check gate round %d: %d check "
			"error(s) across %zu def(s), dropped %d def(s) "
			"(cascade included)\n",
			source_name, round, cerr, bad.size(), nd);
	    }
	}
	if (tree && nerr) {
	    fprintf(stderr, "%s: %d untranslatable node(s); not freezing\n",
		    source_name, nerr);
	} else if (tree && gate_ok) {
	    cir_frozen_forest f;
	    if (!cir_freeze_forest(CIR_NODE(tree), source_name, f)) {
		fprintf(stderr, "%s: forest freeze failed\n", source_name);
	    } else {
		f.libs = prog->loaded_lib_paths;
		// The flavor runtime the freezing process opened
		// (cir_open_stdlib_runtime, before the tree build) is part of
		// the link environment the thaw must recreate: under a
		// non-default flavor its exports (std::__1::cout, the facet
		// ids) are not among bin/madc's DT_NEEDED, so a thaw that
		// reopens only the #load/-l libs leaves every flavor-library
		// import unresolved (trap-bound). Sonames, so the container
		// stays machine-portable; dlopen of an already-loaded soname
		// is a no-op for the default flavor.
		{
		    const madc_stdlib_flavor *flavor = prog->active_stdlib_flavor();
		    if (!flavor)
			flavor = &madc_stdlib_flavors[0];
		    if (flavor->link_libs)
			for (int li = 0; flavor->link_libs[li]; li++)
			    if (std::find(f.libs.begin(), f.libs.end(),
					  flavor->link_libs[li]) == f.libs.end())
				f.libs.push_back(flavor->link_libs[li]);
		}
		f.language_std = madc_forest_config_word(prog);	// v27 producer-config gate
		f.defines_hash = madc_forest_defines_hash(prog);
		cir_forest_fill_pack_payloads(prog, f);	// grove payload v2 (B4a)
		cir_forest_arena_refresh(prog,
					 builder ? &builder->pack_uncarriable_syms()
						 : NULL);	// re-record live aggregates (post-completion mutations)
		f.arena = prog->forest_arena;		// B3 (v18): the arena dump IS the type-graph serialization
		// Emission split (rung 1): consumer-excluded defs (local-class
		// methods, DEFBODY-reverted bodies, their cascade closure) are
		// tree-resident for --run-frozen but must never stamp
		// DF_HAS_FOREST_BODY — a consumer derives them on use.
		if (builder)
		    for (const std::string &sym : builder->pack_stamp_exclusions())
			f.funcdef_locs.erase(sym);
		cir_forest_arena_complete(prog, f,
					  builder ? &builder->pack_uncarriable_syms()
						  : NULL);	// freeze-time fidelity (inline bodies / typedefs / ns)
		cir_forest_fill_globals(prog, f);	// file-scope globals (CIR_GLOBALS; ids swizzle via the arena)
		cir_forest_fill_templates(prog, f);	// v20: template-NAME state (pattern maps, token runs)
		// Per-segment zstd, per the container design (2026-07-04 plan:
		// zstd frames + per-segment codec directory; zlib is the
		// explicit --without-zstd fallback only). The INTERN pool
		// blocks stay codec None inside add_seg — they are the
		// zero-copy bind-in-place spine. Everything else reaches
		// consumers through a copying read_segment, and the per-unit
		// payloads load on demand (unit_segment), so a consumer
		// decodes only the units it actually binds. History: a
		// whole-corpus zlib inflate per compile cost 569M Ir (10.7%
		// of bound testsubscript, callgrind 2026-07-11); the RAW
		// interim that replaced it grew the packed release binary
		// 9MB -> 100MB — never an agreed trade (task #37).
#ifdef HAVE_ZSTD
		PchCompression codec = PchCompression::Zstd;
#else
		PchCompression codec = PchCompression::Zlib;
		fprintf(stderr, "%s: WARNING: built --without-zstd — forest "
			"pack falls back to zlib (larger container, slower "
			"binds)\n", source_name);
#endif
		madc::dis::snapshot_writer w;
		// Level by placement: the appended release pack pays a high
		// level once per release build; a standalone dev freeze stays
		// at the codec default so the drain-ladder loop is never
		// taxed. 15, not 19: this box hard-kills processes at ~120s
		// CPU and the pack freeze itself uses ~70s — level 19 costs
		// ~53s on this corpus (SIGXCPU), level 15 ~5s for ~97% of
		// the plain-level ratio (measured 2026-07-14, task #37).
		// The intern spine compresses only in the appended pack
		// (owner-approved ~7ms-per-process trade; dev freezes keep
		// zero-copy binds).
		bool staged = cir_forest_write(f, w, codec, append ? 15 : 0,
					       append);
		// MIR module cache (--freeze-mir-cache): assemble the container
		// in memory, thaw + compile that EXACT image (what a consumer's
		// --run-frozen builds), and stage the serialized module as one
		// more segment before the file is written. Blob failure never
		// fails the freeze — the segment is simply absent and every
		// consumer falls back to node materialization.
		if (staged && mir_cache) {
		    std::vector<uint8_t> image, mblob;
		    if (w.build(image)
			&& cir_forest_mir_cache_blob(image.data(), image.size(),
						     source_name, mblob)) {
			if (!w.add_segment(CIR_FOREST_SEG_MIR_MODULE,
					   SNAP_KIND_CIR_MIR_MODULE,
					   mblob.data(), mblob.size(), codec,
					   append ? 15 : 0))
			    fprintf(stderr, "%s: mir cache: segment add "
				    "failed — blob skipped\n", source_name);
			else
			    fprintf(stderr, "%s: mir cache: %zu-byte module "
				    "blob packed\n", source_name, mblob.size());
		    }
		}
		// AOT ledger (--freeze-ledger=): the C-lane runtime as MIR
		// modules, so -static-libmadc programs carry it inside their
		// own image. Unlike the mir cache this is NOT best-effort — the
		// caller asked for a ledger, and a container that silently
		// lacks one turns every later -static-libmadc into a
		// Tier-B-looking refusal.
		if (staged && ledger && !ledger->empty()) {
		    std::vector<uint8_t> lblob;
		    cir_ledger_serialize(*ledger, lblob);
		    if (!w.add_segment(CIR_FOREST_SEG_LEDGER,
				       SNAP_KIND_CIR_LEDGER,
				       lblob.data(), lblob.size(), codec,
				       append ? 15 : 0)) {
			fprintf(stderr, "%s: ledger: segment add failed\n",
				source_name);
			staged = false;
		    } else {
			size_t nsym = 0;
			for (const cir_ledger_module &m : *ledger)
			    nsym += m.syms.size();
			fprintf(stderr, "%s: ledger: %zu module(s), %zu symbols,"
				" %zu bytes packed\n", source_name, ledger->size(),
				nsym, lblob.size());
		    }
		}
		if (!staged) {
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
		container_path ? container_path : "<self-executable>");
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
		container_path ? container_path : "<self-executable>");
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
    {
	std::vector<uint8_t> mblob;
	if (forest.mir_module_bytes(mblob))
	    printf("mircache\tbytes=%zu\n", mblob.size());
    }
    {
	// AOT ledger (S5): the C-lane runtime this container carries. One
	// summary line plus one per module — the surface the ledger gate
	// asserts against, and the answer to "can this madc -static-libmadc?".
	std::vector<cir_ledger_module> ledger;
	if (forest.ledger_modules(ledger)) {
	    size_t nsym = 0, nbytes = 0;
	    for (const cir_ledger_module &m : ledger) {
		nsym += m.syms.size();
		nbytes += m.bytes.size();
	    }
	    printf("ledger\tmodules=%zu\tsymbols=%zu\tbytes=%zu\n",
		   ledger.size(), nsym, nbytes);
	    for (const cir_ledger_module &m : ledger)
		printf("ledgermod\t%s\tsymbols=%zu\tbytes=%zu\n",
		       m.name.c_str(), m.syms.size(), m.bytes.size());
	}
    }
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

struct CirParsedTU {
	std::unique_ptr<Program> prog;
	std::string name;
};

// Phase 1 of every project lane (run + native-emit): tokenize + parse EVERY
// TU before any MIR/c2m context exists. tokenize()/parse() can throw in this
// codebase; doing them first keeps every throwing call OUTSIDE the callers'
// MIR_init()->teardown() brackets, so a parse failure can never leak the MIR
// + c2m contexts. (This matches madc_cir_execute's ordering, where
// tokenize/parse run in main() before the MIR bracket is ever entered.)
// Programs own the arenas their modules will be built from, so the caller
// holds `parsed` for its whole compile.
static bool project_parse_all(MadcEngine &engine,
			      const ProjectManifest &manifest,
			      bool forest_bind,
			      const std::string &forest_bind_path,
			      bool class_pattern_live_capture,
			      std::vector<CirParsedTU> &parsed)
{
	for (const ProjectTU &tu : manifest.tus) {
		std::unique_ptr<Program> prog = engine.create_program();
		prog->colors = true;
		// Each TU binds the one embedded/standalone forest (compiles
		// BIND; only the build-time pack freezes). ensure_bind_forest()
		// falls through to live parse when no container is present.
		prog->registration_policy.enable_forest_bind = forest_bind;
		prog->forest_bind_path = forest_bind_path;
		prog->class_pattern_live_capture = class_pattern_live_capture;
		for (const std::string &inc : tu.include_dirs)
			prog->add_include_dir(inc);
		for (const std::string &d : tu.defines)
			prog->add_cli_define(d);
		if (!tu.stdlib_option.empty()
		 && !prog->set_stdlib_flavor_option("-stdlib=" + tu.stdlib_option)) {
			fprintf(stderr, "%s: unknown -stdlib flavor '%s' (this madc was built with: %s)\n",
				tu.file.c_str(), tu.stdlib_option.c_str(),
				prog->stdlib_flavor_names().c_str());
			return false;
		}
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
			return false;
		}
		if (!prog->parse(tp)) {
			fprintf(stderr, "%s: parse failed\n", tu.file.c_str());
			return false;
		}

		CirParsedTU pt;
		pt.prog = std::move(prog);
		pt.name = tu.file;
		parsed.push_back(std::move(pt));
	}
	return true;
}

int madc_project_execute(MadcEngine &engine, const ProjectManifest &manifest,
			 int user_argc, char **user_argv,
			 bool forest_bind, const std::string &forest_bind_path,
			 bool class_pattern_live_capture)
{
	if (manifest.tus.empty()) {
		fprintf(stderr, "madc_project_execute: empty manifest\n");
		return -1;
	}

	// Phase 1: tokenize + parse EVERY TU before any MIR/c2m context exists.
	std::vector<CirParsedTU> parsed;
	if (!project_parse_all(engine, manifest, forest_bind, forest_bind_path,
			       class_pattern_live_capture, parsed))
		return -1;	// no MIR/c2m created yet — nothing to tear down

	// Phase 2: now that all parsing is done, enter the MIR bracket. No
	// throwing call sits between MIR_init() and teardown().
	MIR_context_t ctx = MIR_init();
	// C++ TUs each emit their own copy of every template instantiation they
	// use (ODR: the copies are identical). MIR treats a same-named exported
	// func in a later module as a fatal redefinition; permit it so the last
	// copy wins — the linkonce/COMDAT analogue for the multi-TU JIT.
	// (Named DATA duplicates still get per-module addresses — split-state
	// hazard for template statics; revisit when a corpus actually hits it.)
	MIR_set_func_redef_permission(ctx, TRUE);
	c2mir_init(ctx);
	MIR_gen_init(ctx);
	MIR_gen_set_optimize_level(ctx, (unsigned)madc_opt_level);
	if (madc_debug_info)
		cir_set_debug_codegen(ctx);

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

	for (CirParsedTU &pt : parsed) {
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
	// Same link environment the single-TU JIT builds: the active flavor's
	// C++ runtime must be loaded before imports are resolved. A project's
	// TUs share one flavor (the manifest's stdlib_option), so any TU's
	// Program answers for all of them.
	if (!parsed.empty() && parsed[0].prog)
		cir_open_stdlib_runtime(parsed[0].prog->active_stdlib_flavor());
	MIR_link(ctx, MIR_set_gen_interface, cir_import_resolver);
	if (madc_debug_info)
		cir_register_source_debug(ctx);

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

// Native AOT over a whole project (task #85): the project twin of
// madc_cir_emit_native. mnkObject emits one .o per TU — object capture is
// context-wide, so each TU gets its own session/context (gcc semantics:
// <TU-base>.o in the current directory; out_path overrides only for a
// single-TU manifest, enforced by the caller). The linked kinds (PIE /
// ET_EXEC executables, -shared) emit ONE native image of every TU: here
// the shared context is the RIGHT
// granularity — all TU modules land in one capture, cross-TU references
// resolve internally at emit, and only genuine runtime imports stay UNDEF
// (the sentinel resolver, exactly as in the single-TU lane).
int madc_project_emit_native(MadcEngine &engine,
			     const ProjectManifest &manifest,
			     MadcNativeKind kind, const char *out_path,
			     const std::vector<std::string> &user_libs,
			     bool forest_bind,
			     const std::string &forest_bind_path)
{
	if (manifest.tus.empty()) {
		fprintf(stderr, "madc_project_emit_native: empty manifest\n");
		return -1;
	}
	madc_object_mode = true;   // one-shot CLI path; process exits after

	std::vector<CirParsedTU> parsed;
	if (!project_parse_all(engine, manifest, forest_bind, forest_bind_path,
			       false, parsed))
		return -1;	// no MIR/c2m created yet — nothing to tear down

	// Standalone executables — and every non--shared artifact of an
	// emit-only cross build — skip the __madc_shim_* eval adapters (see
	// madc_cir_emit_native); stamped per TU before the CIR builds run.
	{
		bool skip_shims = madc_no_eval_shims
			       || kind == mnkExecutable || kind == mnkPieExecutable;
#ifdef MADC_CROSS_TARGET
		skip_shims = skip_shims || kind != mnkShared;
#endif
		if (skip_shims)
			for (CirParsedTU &pt : parsed)
				pt.prog->aot_skip_eval_shims = true;
	}

	if (kind == mnkObject) {
		for (CirParsedTU &pt : parsed) {
			CirJitSession session;
			bool stop = false;
			if (!session.build(pt.prog.get(), pt.name.c_str(),
					   false, false, false, &stop))
				return -1;
			std::string out;
			if (out_path && *out_path && parsed.size() == 1)
				out = out_path;
			else {
				out = pt.name;
				size_t slash = out.rfind('/');
				if (slash != std::string::npos)
					out = out.substr(slash + 1);
				size_t dot = out.rfind('.');
				if (dot != std::string::npos && dot > 0)
					out = out.substr(0, dot);
				out += ".o";
			}
			if (!session.emit_native_object(out.c_str()))
				return -1;
		}
		return 0;
	}

	// One output (-o / -shared / -r): the same MIR bracket shape as
	// madc_project_execute, in object-capture mode, emitting instead of
	// running. cir_init reads madc_object_mode → native_object_p.
	MIR_context_t ctx = MIR_init();
	MIR_set_func_redef_permission(ctx, TRUE);   // C++ ODR linkonce analogue
	c2mir_init(ctx);
	MIR_gen_init(ctx);
	MIR_gen_set_optimize_level(ctx, (unsigned)madc_opt_level);
	MIR_gen_set_object_mode(ctx, 1);
	if (madc_debug_info)
		cir_set_debug_codegen(ctx);   // -g codegen shape; DWARF-in-.o = R5

	c2m_ctx_t c2m = cir_init(ctx, false);
	if (!c2m) {
		fprintf(stderr, "madc_project_emit_native: cir_init failed\n");
		MIR_gen_finish(ctx);
		c2mir_finish(ctx);
		MIR_finish(ctx);
		return -1;
	}

	std::vector<CirBuilder *> builders;
	std::vector<MIR_module_t> modules;
	auto teardown = [&]() {
		for (CirBuilder *b : builders) delete b;
		cir_finish(c2m);
		MIR_gen_finish(ctx);
		c2mir_finish(ctx);
		MIR_finish(ctx);
	};

	for (CirParsedTU &pt : parsed) {
		CirBuilder *builder = NULL;
		bool stop = false;
		MIR_module_t mod = build_tu_module(ctx, c2m, pt.prog.get(),
						   pt.name.c_str(),
						   false, false, false,
						   builder, stop);
		builders.push_back(builder);
		if (!mod) {
			teardown();
			return -1;
		}
		modules.push_back(mod);
	}

	for (MIR_module_t m : modules)
		MIR_load_module(ctx, m);
	// -static-libmadc: the whole-program capture pulls its AOT-ledger
	// runtime here, once for the program (not once per TU) — the imports
	// the TUs satisfy for each other are already netted out by the loads
	// above. Any TU's Program answers for the carrier; they share one
	// installation.
	if (!parsed.empty())
		cir_ledger_pull(ctx, parsed[0].prog.get());
	MIR_link(ctx, MIR_set_gen_interface, cir_object_import_resolver);
	if (madc_debug_info)
		c2mir_object_attach_debug(ctx);   // R5: whole-program DWARF
	// Every TU's init joins the capture's .init_array (link order).
	for (size_t bi = 0; bi < builders.size(); bi++)
		if (!cir_register_tu_init(ctx, builders[bi],
					  parsed[bi].name.c_str())) {
			teardown();
			return -1;
		}

	bool ok;
	if (kind == mnkRelocatable)
		// -r: the same whole-program capture, written as ONE
		// relocatable .o (gcc/ld -r shape) instead of an image.
		ok = cir_write_native_object(ctx, out_path);
	else {
		std::vector<std::string> needed;
		std::string runpath;
		// The link's stdlib flavor comes from the manifest: the
		// first TU that names one (a mixed-flavor project is
		// already ill-formed). None named = the build default.
		const madc_stdlib_flavor *flavor = NULL;
		for (const ProjectTU &tu : manifest.tus)
			if (!tu.stdlib_option.empty()) {
				flavor = madc_stdlib_flavor_lookup(
						tu.stdlib_option);
				break;
			}
		cir_native_link_env(flavor, user_libs, needed, runpath);
		ok = cir_write_native_image(ctx, out_path, needed, runpath,
					    kind);
	}
	teardown();
	return ok ? 0 : -1;
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
