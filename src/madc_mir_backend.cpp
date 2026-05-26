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
#include <iomanip>
#include <sstream>
#include <fstream>
#include <string>
#include <map>
#include <list>
#include <vector>
#include <deque>
#include <queue>
#include <stack>
#include <stdint.h>
#include <dlfcn.h>
#include <asmjit/x86.h>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

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

void madc_puti(int64_t i)    { std::cout << i << std::endl; }
void madc_putu(uint64_t i)   { std::cout << i << std::endl; }
void madc_putd(double d)     { std::cout << d << std::endl; }
void madc_putf(float f)      { std::cout << f << std::endl; }
void madc_puts(const char *s) { if (s) puts(s); }
void madc_printstr(const char *s) { if (s) std::cout << s << std::endl; }

// C++ iostream wrappers — thin C-linkage functions that call real
// std::cout << operators.  The transpiler emits these instead of printf,
// preserving iostream formatting behavior.
// String runtime — manages std::string objects from generated C code.
// String variables are stack-allocated char[32] buffers with placement new.
void __madc_string_construct(void *ptr)
    { new(ptr) std::string; }
void __madc_string_destruct(void *ptr)
    { ((std::string *)ptr)->~basic_string(); }
void __madc_string_assign_cstr(void *ptr, const char *s)
    { *(std::string *)ptr = s ? s : ""; }
void __madc_string_assign(void *dst, void *src)
    { *(std::string *)dst = *(std::string *)src; }
const char *__madc_string_cstr(void *ptr)
    { return ((std::string *)ptr)->c_str(); }
long __madc_string_length(void *ptr)
    { return (long)((std::string *)ptr)->length(); }
void __madc_string_append_cstr(void *ptr, const char *s)
    { if (s) *(std::string *)ptr += s; }
void __madc_string_append(void *dst, void *src)
    { *(std::string *)dst += *(std::string *)src; }
// Generic ostream wrappers — take ostream* as first arg so they work
// with cout, cerr, ofstream, stringstream, etc.
void __madc_ostream_str(void *os, const char *s)
    { if (s) *(std::ostream *)os << s; }
void __madc_ostream_stdstr(void *os, void *ptr)
    { *(std::ostream *)os << *(std::string *)ptr; }
void __madc_ostream_int(void *os, long i)
    { *(std::ostream *)os << i; }
void __madc_ostream_uint(void *os, unsigned long u)
    { *(std::ostream *)os << u; }
void __madc_ostream_char(void *os, int c)
    { *(std::ostream *)os << (char)c; }
void __madc_ostream_double(void *os, double d)
    { *(std::ostream *)os << d; }
void __madc_ostream_endl(void *os)
    { *(std::ostream *)os << std::endl; }
void __madc_ostream_flush(void *os)
    { *(std::ostream *)os << std::flush; }
// Stream manipulators
void __madc_ostream_hex(void *os)
    { *(std::ostream *)os << std::hex; }
void __madc_ostream_oct(void *os)
    { *(std::ostream *)os << std::oct; }
void __madc_ostream_dec(void *os)
    { *(std::ostream *)os << std::dec; }
void __madc_ostream_fixed(void *os)
    { *(std::ostream *)os << std::fixed; }
void __madc_ostream_scientific(void *os)
    { *(std::ostream *)os << std::scientific; }
void __madc_ostream_left(void *os)
    { *(std::ostream *)os << std::left; }
void __madc_ostream_right(void *os)
    { *(std::ostream *)os << std::right; }
void __madc_ostream_boolalpha(void *os)
    { *(std::ostream *)os << std::boolalpha; }
void __madc_ostream_noboolalpha(void *os)
    { *(std::ostream *)os << std::noboolalpha; }
void __madc_ostream_showbase(void *os)
    { *(std::ostream *)os << std::showbase; }
void __madc_ostream_noshowbase(void *os)
    { *(std::ostream *)os << std::noshowbase; }
void __madc_ostream_setw(void *os, int w)
    { *(std::ostream *)os << std::setw(w); }
void __madc_ostream_setprecision(void *os, int p)
    { *(std::ostream *)os << std::setprecision(p); }
void __madc_ostream_setfill(void *os, int c)
    { *(std::ostream *)os << std::setfill((char)c); }

// Global stream pointers accessible from generated C
void *__madc_cout_ptr(void) { return &std::cout; }
void *__madc_cerr_ptr(void) { return &std::cerr; }

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

int madc_mir_execute(const std::string &c_source, const std::string &source_name)
{
    MIR_context_t ctx = MIR_init();
    c2mir_init(ctx);
    MIR_gen_init(ctx);

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

    int result = ((int (*)(void))code)();

    MIR_gen_finish(ctx);
    c2mir_finish(ctx);
    MIR_finish(ctx);

    return result;
}
