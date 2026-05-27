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

void *__madc_get_stdout(void) { return (void *)stdout; }
void *__madc_get_stdin(void)  { return (void *)stdin; }
void *__madc_get_stderr(void) { return (void *)stderr; }

void madc_puti(int64_t i)    { std::cout << i << std::endl; }
void madc_putu(uint64_t i)   { std::cout << i << std::endl; }
void madc_putd(double d)     { std::cout << d << std::endl; }
void madc_putf(float f)      { std::cout << f << std::endl; }
void madc_puts(const char *s) { if (s) puts(s); }
void madc_printstr(const char *s) { if (s) std::cout << s << std::endl; }

// String runtime — mirrors the legacy compiler's string_construct/string_destruct
// pattern from compiler.cpp.  std::string is treated as a C++ object with
// extern ctor/dtor, not special-cased.
void *string_construct(void *ptr)
    { return new(ptr) std::string; }
void string_destruct(void *ptr)
    { ((std::string *)ptr)->~basic_string(); }
void *string_construct_cstr(void *ptr, const char *s)
    { return new(ptr) std::string(s ? s : ""); }
void string_assign(void *dst, void *src)
    { *(std::string *)dst = *(std::string *)src; }
void string_assign_cstr(void *dst, const char *s)
    { *(std::string *)dst = s ? s : ""; }
const char *string_cstr(void *ptr)
    { return ((std::string *)ptr)->c_str(); }
long string_length(void *ptr)
    { return (long)((std::string *)ptr)->length(); }
void string_append(void *dst, void *src)
    { *(std::string *)dst += *(std::string *)src; }
void string_append_cstr(void *dst, const char *s)
    { if (s) *(std::string *)dst += s; }

// Generic ostream wrappers — mirrors streamout_string/streamout_cstr/
// streamout_numeric from compiler_operators.cpp.  Take ostream* as first
// arg so they work with cout, cerr, ofstream, stringstream, etc.
void streamout_string(void *os, void *ptr)
    { *(std::ostream *)os << *(std::string *)ptr; }
void streamout_cstr(void *os, const char *s)
    { if (s) *(std::ostream *)os << s; }
void streamout_int64(void *os, long i)
    { *(std::ostream *)os << i; }
void streamout_uint64(void *os, unsigned long u)
    { *(std::ostream *)os << u; }
void streamout_char(void *os, int c)
    { *(std::ostream *)os << (char)c; }
void streamout_double(void *os, double d)
    { *(std::ostream *)os << d; }
void streamout_endl(void *os)
    { *(std::ostream *)os << std::endl; }
void streamout_flush(void *os)
    { *(std::ostream *)os << std::flush; }
// Stream manipulators
void streamout_hex(void *os)
    { *(std::ostream *)os << std::hex; }
void streamout_oct(void *os)
    { *(std::ostream *)os << std::oct; }
void streamout_dec(void *os)
    { *(std::ostream *)os << std::dec; }
void streamout_fixed(void *os)
    { *(std::ostream *)os << std::fixed; }
void streamout_scientific(void *os)
    { *(std::ostream *)os << std::scientific; }
void streamout_left(void *os)
    { *(std::ostream *)os << std::left; }
void streamout_right(void *os)
    { *(std::ostream *)os << std::right; }
void streamout_boolalpha(void *os)
    { *(std::ostream *)os << std::boolalpha; }
void streamout_noboolalpha(void *os)
    { *(std::ostream *)os << std::noboolalpha; }
void streamout_showbase(void *os)
    { *(std::ostream *)os << std::showbase; }
void streamout_noshowbase(void *os)
    { *(std::ostream *)os << std::noshowbase; }
void streamout_setw(void *os, int w)
    { *(std::ostream *)os << std::setw(w); }
void streamout_setprecision(void *os, int p)
    { *(std::ostream *)os << std::setprecision(p); }
void streamout_setfill(void *os, int c)
    { *(std::ostream *)os << std::setfill((char)c); }

// istream wrappers — take istream* as first arg
void streamin_int(void *is, long *out)
    { *(std::istream *)is >> *out; }
void streamin_uint(void *is, unsigned long *out)
    { *(std::istream *)is >> *out; }
void streamin_double(void *is, double *out)
    { *(std::istream *)is >> *out; }
void streamin_char(void *is, char *out)
    { *(std::istream *)is >> *out; }
void streamin_string(void *is, void *str)
    { *(std::istream *)is >> *(std::string *)str; }
void streamin_getline(void *is, void *str)
    { std::getline(*(std::istream *)is, *(std::string *)str); }

// Global stream pointers accessible from generated C
void *__madc_cout_ptr(void) { return &std::cout; }
void *__madc_cerr_ptr(void) { return &std::cerr; }
void *__madc_cin_ptr(void)  { return &std::cin; }

// File stream wrappers — typed per stream class because std::ios is a
// virtual base class; casting void* to ios* gives wrong pointer offset.
// Pattern from legacy compiler.cpp:2577.

// ofstream
void *ofstream_construct(void *ptr)
	{ return new(ptr) std::ofstream; }
void ofstream_destruct(void *ptr)
	{ ((std::ofstream *)ptr)->~basic_ofstream(); }
void ofstream_open(void *ptr, const char *path)
	{ ((std::ofstream *)ptr)->open(path); }
void ofstream_close(void *ptr)
	{ ((std::ofstream *)ptr)->close(); }
long ofstream_good(void *ptr)
	{ return ((std::ofstream *)ptr)->good() ? 1 : 0; }
long ofstream_is_open(void *ptr)
	{ return ((std::ofstream *)ptr)->is_open() ? 1 : 0; }

// ifstream
void *ifstream_construct(void *ptr)
	{ return new(ptr) std::ifstream; }
void ifstream_destruct(void *ptr)
	{ ((std::ifstream *)ptr)->~basic_ifstream(); }
void ifstream_open(void *ptr, const char *path)
	{ ((std::ifstream *)ptr)->open(path); }
void ifstream_close(void *ptr)
	{ ((std::ifstream *)ptr)->close(); }
long ifstream_good(void *ptr)
	{ return ((std::ifstream *)ptr)->good() ? 1 : 0; }
long ifstream_eof(void *ptr)
	{ return ((std::ifstream *)ptr)->eof() ? 1 : 0; }
long ifstream_is_open(void *ptr)
	{ return ((std::ifstream *)ptr)->is_open() ? 1 : 0; }

// fstream
void *fstream_construct(void *ptr)
	{ return new(ptr) std::fstream; }
void fstream_destruct(void *ptr)
	{ ((std::fstream *)ptr)->~basic_fstream(); }
void fstream_open(void *ptr, const char *path)
	{ ((std::fstream *)ptr)->open(path); }
void fstream_close(void *ptr)
	{ ((std::fstream *)ptr)->close(); }

// stringstream
void *sstream_construct(void *ptr)
	{ return new(ptr) std::stringstream; }
void sstream_destruct(void *ptr)
	{ ((std::stringstream *)ptr)->~basic_stringstream(); }
const char *sstream_str(void *ptr)
	{ static thread_local std::string s; s = ((std::stringstream *)ptr)->str(); return s.c_str(); }
void sstream_str_set(void *ptr, const char *s)
	{ ((std::stringstream *)ptr)->str(s ? s : ""); }
void printstream(void *ptr)
	{ std::cout << ((std::stringstream *)ptr)->str() << std::endl; }

// std:: namespace functions — madc programs call these as std::stoi etc.
// The emitter mangles them to __std_stoi / __std_stod / __std_to_string.
// Take void* for string args (pointing to a std::string placement-new'd
// at the stack slot) and cast to std::string*.
long __std_stoi(void *str)
{
    try { return (long)std::stoi(((std::string *)str)->c_str()); }
    catch (...) { return 0; }
}
long __std_stol(void *str)
{
    try { return std::stol(((std::string *)str)->c_str()); }
    catch (...) { return 0; }
}
unsigned long __std_stoul(void *str)
{
    try { return std::stoul(((std::string *)str)->c_str()); }
    catch (...) { return 0; }
}
double __std_stof(void *str)
{
    try { return (double)std::stof(((std::string *)str)->c_str()); }
    catch (...) { return 0.0; }
}
double __std_stod(void *str)
{
    try { return std::stod(((std::string *)str)->c_str()); }
    catch (...) { return 0.0; }
}
double __std_stold(void *str)
{
    try { return (double)std::stold(((std::string *)str)->c_str()); }
    catch (...) { return 0.0; }
}
void __std_to_string(void *dst, long val)
{
    std::string *s = (std::string *)dst;
    *s = std::to_string(val);
}

// std::for_each — calls a function pointer for each element in a MadArray.
// Provides the extern "C" symbol the transpiled code expects.
void __std_for_each(void *arr, void *fn_ptr)
{
    MadArray &a = *(MadArray *)arr;
    typedef void (*fn_cstr_t)(const char *);
    fn_cstr_t fn = (fn_cstr_t)fn_ptr;
    for (size_t i = 0; i < a.data.size(); ++i) {
	MadValue &v = a.data[i];
	if (v.is_string()) {
	    std::string s = v.as_string();
	    fn(s.c_str());
	} else if (v.is_int()) {
	    std::string s = std::to_string(v.as_int());
	    fn(s.c_str());
	} else if (v.is_double()) {
	    std::string s = std::to_string(v.as_double());
	    fn(s.c_str());
	}
    }
}

// MadArray runtime — construct/destruct/size for transpiled array variables.
// MadArray is defined in include/datadef.h (already included).
void *madarray_construct(void *ptr)
    { return new(ptr) MadArray; }
void madarray_destruct(void *ptr)
    { ((MadArray *)ptr)->~MadArray(); }
long madarray_size(void *ptr)
    { return (long)((MadArray *)ptr)->data.size(); }

// STL container wrappers removed — ns_stl.cpp was a proof-of-concept.
// Template instantiation will be generated on demand by the transpiler
// (Cfront-style: per-translation-unit, driven by user declarations).


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
    // Use optimization level 1 (RA+combiner only) to avoid the addr-
    // elimination pass at level >= 2 which has an SSA-version bug when
    // a pointer variable is address-taken and then reassigned later.
    MIR_gen_set_optimize_level(ctx, 1);

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
