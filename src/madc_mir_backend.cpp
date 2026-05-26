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
// cout for std::string* — outputs the string content
void __std_cout_stdstr(void *ptr)
    { std::cout << *(std::string *)ptr; }

void __std_cout_str(const char *s)  { if (s) std::cout << s; }
void __std_cout_int(long i)         { std::cout << i; }
void __std_cout_uint(unsigned long u) { std::cout << u; }
void __std_cout_char(int c)         { std::cout << (char)c; }
void __std_cout_double(double d)    { std::cout << d; }
void __std_cout_endl(void)          { std::cout << std::endl; }
void __std_cout_flush(void)         { std::cout << std::flush; }
// Stream manipulators
void __std_cout_hex(void)           { std::cout << std::hex; }
void __std_cout_oct(void)           { std::cout << std::oct; }
void __std_cout_dec(void)           { std::cout << std::dec; }
void __std_cout_fixed(void)         { std::cout << std::fixed; }
void __std_cout_scientific(void)    { std::cout << std::scientific; }
void __std_cout_left(void)          { std::cout << std::left; }
void __std_cout_right(void)         { std::cout << std::right; }
void __std_cout_boolalpha(void)     { std::cout << std::boolalpha; }
void __std_cout_noboolalpha(void)   { std::cout << std::noboolalpha; }
void __std_cout_showbase(void)      { std::cout << std::showbase; }
void __std_cout_noshowbase(void)    { std::cout << std::noshowbase; }
void __std_cout_setw(int w)         { std::cout << std::setw(w); }
void __std_cout_setprecision(int p) { std::cout << std::setprecision(p); }
void __std_cout_setfill(int c)      { std::cout << std::setfill((char)c); }

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
