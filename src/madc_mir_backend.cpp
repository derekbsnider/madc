// madc_mir_backend.cpp — Glue between the C emitter and MIR.
//
// Feeds generated C11 text to c2mir → MIR → machine code, then
// executes the result.
//
// Import resolution uses dlsym — all runtime helpers are either
// C-linkage builtins or extern "C" namespace wrappers.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <new>
#include <map>
#include <list>
#include <vector>
#include <deque>
#include <queue>
#include <stack>
#include <stdint.h>
#include <dlfcn.h>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "ns_common.h"
#include "libmadc/sysinfo.h"

extern "C" {
#include "mir.h"
#include "mir-gen.h"
#include "c2mir.h"
}

// -----------------------------------------------------------------------
// madc runtime builtins — exported with C linkage so dlsym can find them.
// These wrap the C++ implementations in parser.cpp.
// -----------------------------------------------------------------------

extern "C" {

void *__madc_get_stdout(void) { return (void *)stdout; }
void *__madc_get_stdin(void)  { return (void *)stdin; }
void *__madc_get_stderr(void) { return (void *)stderr; }

void madc_puti(int64_t i)    { std::cout << i << std::endl; }
void madc_putu(uint64_t i)   { std::cout << i << std::endl; }
void madc_putd(double d)     { std::cout << d << std::endl; }
void madc_putf(float f)      { std::cout << f << std::endl; }
void madc_puts(const char *s) { if (s) puts(s); }
void madc_printstr(const char *s) { if (s) std::cout << s << std::endl; }

// madc array runtime — construct/destruct/size for transpiled array
// variables. The script `array` IS the public madc::value
// (include/libmadc/value.h, pulled in via datadef.h). A default-constructed
// value is kind::null (reads as empty; mutators vivify it to kind::array).
// The CIR builder lowers `array a;` to an _Alignas(alignof(madc::value))
// long[] buffer (CirBuilder::array_storage_decl passes the alignment, the
// 16 of the embedded madc_value) that placement-new constructs into.
void *madarray_construct(void *ptr)
    { return new(ptr) madc::value; }
void madarray_destruct(void *ptr)
    { ((madc::value *)ptr)->~value(); }
// Range-for length over a script array. Intentionally NOT
// ns_common::value_count: foreach iterates indexed elements only, so an
// object-kind ctx must read as length 0 here.
long madarray_size(void *ptr)
    {
	madc::value *v = (madc::value *)ptr;
	return v->is_array() ? (long)v->as_array().size() : 0;
    }

// Scalar (re)assignment surface for the intrinsic value/array carrier —
// the native operator= family add_array_methods registers on ddARRAY.
// Each retags the carrier in place through madc::value's own operator=
// (so freeze() rejection and payload-cell release ride the one owner).
// Returns the receiver: operator= yields *this.
void *madarray_assign_cstr(void *ptr, const char *s)
    { *(madc::value *)ptr = madc::value(s ? s : ""); return ptr; }
void *madarray_assign_int(void *ptr, long i)
    { *(madc::value *)ptr = madc::value((int64_t)i); return ptr; }
void *madarray_assign_real(void *ptr, double d)
    { *(madc::value *)ptr = madc::value(d); return ptr; }
void *madarray_assign_bool(void *ptr, long b)
    { *(madc::value *)ptr = madc::value(b != 0); return ptr; }
void *madarray_assign_value(void *ptr, void *src)
    { *(madc::value *)ptr = *(const madc::value *)src; return ptr; }

// Text view of a value for C varargs (printf "%s") — the coercion the CIR
// builder applies to a value argument in a variadic call. String kind
// returns the value's own payload (stable, value-owned). Other kinds
// render through the ONE value->text owner (ns_common::value_to_string)
// into a thread-local ring, so several value args in one call keep
// distinct buffers (the inet_ntoa model; a pointer stays valid until its
// slot recycles). Container kinds render a diagnostic tag, never crash.
const char *madarray_cstr(void *ptr)
    {
	const madc::value *v = (const madc::value *)ptr;
	if (v->is_string())
	    return (const char *)v->data();
	thread_local std::string ring[8];
	thread_local unsigned ring_i = 0;
	std::string &slot = ring[ring_i++ & 7u];
	if (v->is_null())
	    slot = "";
	else if (v->is_boolean())
	    slot = v->as_boolean() ? "true" : "false";
	else if (!ns_common::value_to_string(*v, slot))
	    slot = std::string("[") + madc::value::kind_name(v->type())
		 + ":" + std::to_string((long long)v->size()) + "]";
	return slot.c_str();
    }

// madc::sys population (task #91) — injected by the CIR builder in TUs
// that included <ns_madc>. Two entries over one population:
// __madc_sys_init is the explicit RUN entry (the JIT lane's main wrap) —
// unconditional, so a multi-session embedding host repopulates per run.
// __madc_sys_init_once is the LOAD-time entry (the top of a TU's
// .init_array init, ELF-completion S3) — it yields to any prior
// population: several TU inits may call it (same argc/argv, harmless),
// but a madc-built module dlopen'd MID-program is handed the main
// program's arguments by ld.so, and repopulating then would stomp the
// sys.path/sys.argv mutations the running script already made.
static bool madc_sys_populated = false;
void __madc_sys_init(long argc, void *argv)
    {
	madc_sys_populated = true;
	madc::sys_populate_args((int)argc, (char **)argv);
    }
void __madc_sys_init_once(long argc, void *argv)
    {
	if (!madc_sys_populated)
	    __madc_sys_init(argc, argv);
    }


} // extern "C"

// -----------------------------------------------------------------------
// Import resolver — dlsym finds C-linkage symbols directly.
// Namespace functions have extern "C" wrappers in ns_*.cpp.
// -----------------------------------------------------------------------

static void *madc_import_resolver(const char *name)
{
    void *addr = dlsym(RTLD_DEFAULT, name);
    if (!addr)
	DBG(std::cerr << "madc_import_resolver: unresolved: "
		      << name << std::endl);
    return addr;
}

// -----------------------------------------------------------------------
// String reader for c2mir
// -----------------------------------------------------------------------

struct CStringReader {
    const char *data;
    size_t pos;
    size_t len;
};

static int c_string_getc(void *data)
{
    CStringReader *r = (CStringReader *)data;
    if (r->pos >= r->len) return EOF;
    return (unsigned char)r->data[r->pos++];
}

// -----------------------------------------------------------------------
// madc_mir_execute — compile C text via c2mir and execute main()
//
// Returns the exit code from main(), or -1 on compilation failure.
// -----------------------------------------------------------------------

int madc_mir_execute(const std::string &c_source, const std::string &source_name,
		     int user_argc, char **user_argv)
{
    MIR_context_t ctx = MIR_init();
    c2mir_init(ctx);
    MIR_gen_init(ctx);
    // Optimization level from the `-O<n>` flag (default 1). Level 1
    // (RA+combiner only) is the safe default: level >= 2 enables an addr-
    // elimination pass with an SSA-version bug when a pointer variable is
    // address-taken and then reassigned later. -O2/-O3 may hit it; -O0 is fine.
    MIR_gen_set_optimize_level(ctx, (unsigned)madc_opt_level);

    struct c2mir_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.message_file = stderr;

    CStringReader reader = {c_source.c_str(), 0, c_source.size()};
    int ok = c2mir_compile(ctx, &opts, c_string_getc, &reader,
			   source_name.c_str(), nullptr);
    if (!ok) {
	fprintf(stderr, "madc_mir_execute: c2mir compilation failed\n");
	MIR_gen_finish(ctx);
	c2mir_finish(ctx);
	MIR_finish(ctx);
	return -1;
    }

    MIR_module_t mod = nullptr;
    for (MIR_module_t m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx));
	 m != nullptr; m = DLIST_NEXT(MIR_module_t, m))
	mod = m;

    if (!mod) {
	fprintf(stderr, "madc_mir_execute: no module produced\n");
	MIR_gen_finish(ctx);
	c2mir_finish(ctx);
	MIR_finish(ctx);
	return -1;
    }

    MIR_load_module(ctx, mod);
    MIR_link(ctx, MIR_set_gen_interface, madc_import_resolver);

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
	fprintf(stderr, "madc_mir_execute: main() not found\n");
	MIR_gen_finish(ctx);
	c2mir_finish(ctx);
	MIR_finish(ctx);
	return -1;
    }

    int result = ((int (*)(int, char **))code)(user_argc, user_argv);

    MIR_gen_finish(ctx);
    c2mir_finish(ctx);
    MIR_finish(ctx);

    return result;
}
