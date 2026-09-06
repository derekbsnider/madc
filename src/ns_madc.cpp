//////////////////////////////////////////////////////////////////////////
//									//
// madc:: namespace — script-level runtime eval + expression eval.	//
//									//
// The namespace madc functions below are the single real		//
// implementation and the symbols scripts bind mangled-direct		//
// (cpp-first-api.md): the embedded <ns_madc> header declares them	//
// declaration-only and the parser resolves the Itanium symbols		//
// straight to these definitions via -rdynamic. The extern "C"		//
// exports at the bottom are the C-linkage API for C hosts consuming	//
// libmadc.a/.so — never the script-side resolution path.		//
//									//
// Both layers delegate to the madc_runtime_eval_* internals in		//
// src/parser.cpp (declared in ns_common.h), which drive the active	//
// Program's runtime_eval_source / runtime_eval_expression machinery	//
// and honour the engine's madc_runtime_eval_policy.			//
//									//
//////////////////////////////////////////////////////////////////////////

#include <string>
#include <cstdint>
#include <iostream>
#include <map>
#include <vector>
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "ns_common.h"
#include "libmadc/sysinfo.h"
#include "madc_posix_io.h"	// get_host_name (host-facts seam)

// ---- madc::sys — the system object (task #91) ----------------------------
namespace madc {

static const char *sys_detect_platform()
{
#if defined(__APPLE__)
    return "darwin";
#elif defined(_WIN32)
    return "win32";
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

static const char *sys_detect_hostname()
{
    static std::string name = madc::detail::get_host_name();
    return name.c_str();
}

// The facts initialize once at load (dynamic init of this TU); argv/path
// are filled by sys_populate_args from the injected __madc_sys_init call.
SysInfo sys = { value(), value(), sys_detect_platform(), MADC_VERSION_STR,
		sys_detect_hostname() };

void sys_populate_args(int argc, char **argv)
{
    sys.argv = value::make_array();
    for ( int i = 0; i < argc && argv; i++ )
	sys.argv.array().push_back(value(argv[i] ? argv[i] : ""));
    // Initial search-path seed (future #load/eval honor mutations — the
    // Python one-way semantics): the script's directory, then cwd.
    sys.path = value::make_array();
    if ( argc > 0 && argv && argv[0] )
    {
	std::string dir = madc::detail::host_path_dirname(argv[0]);
	if ( dir.size() > 1 )	// ">1": a bare root separator stays off the path
	    sys.path.array().push_back(value(dir.substr(0, dir.size() - 1)));
    }
    sys.path.array().push_back(value("."));
}

} // namespace madc

namespace madc {

// Full-program eval: the source must define (or be wrapped into)
// `__madc_eval()`; the rendered result text lands in `out`.
std::string &eval_unit(std::string &out, std::string &source)
	{ return *(std::string *)madc_runtime_eval(&out, &source); }
bool eval_bool(std::string &source)
	{ return madc_runtime_eval_bool(&source); }
int64_t eval_int(std::string &source)
	{ return madc_runtime_eval_int(&source); }
int64_t eval_int(const char *source)
	{ std::string s = source ? source : ""; return madc_runtime_eval_int(&s); }
double eval_double(std::string &source)
	{ return madc_runtime_eval_double(&source); }
std::string &eval_string(std::string &out, std::string &source)
	{ return *(std::string *)madc_runtime_eval_string(&out, &source); }

// Value-first primaries (slice V2): const char* sources, value
// destinations. The value-destination forms carry the rendered result
// text as a string-kind value — the same convention as the channel
// value carriers (typed-kind eval results are a future runtime-core
// entry, not a header change).
value &eval_unit(value &out, const char *source)
	{ std::string s = source ? source : "", r; madc_runtime_eval(&r, &s); out = value(r); return out; }
bool eval_bool(const char *source)
	{ std::string s = source ? source : ""; return madc_runtime_eval_bool(&s); }
double eval_double(const char *source)
	{ std::string s = source ? source : ""; return madc_runtime_eval_double(&s); }
value &eval_string(value &out, const char *source)
	{ std::string s = source ? source : "", r; madc_runtime_eval_string(&r, &s); out = value(r); return out; }

// Expression eval: a single expression, no function calls unless the
// engine's expression policy allows them.
bool eval_expression_bool(const char *expr)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_bool(&e); }
int64_t eval_expression_int(const char *expr)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_int(&e); }
double eval_expression_double(const char *expr)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_double(&e); }
std::string &eval_expression_string(std::string &out, const char *expr)
	{ std::string e = expr ? expr : ""; return *(std::string *)madc_runtime_eval_expression_string(&out, &e); }
value &eval_expression_string(value &out, const char *expr)
	{ std::string e = expr ? expr : "", r; madc_runtime_eval_expression_string(&r, &e); out = value(r); return out; }

// Typed out-parameter forms (overloaded on the destination). The
// string-destination form RENDERS any result type ("42", "4.000000",
// "echo") via the untyped runtime; eval_expression_string above is the
// strict string-typed coercion.
void eval_expression(int64_t &out, const char *expr)
	{ std::string e = expr ? expr : ""; out = madc_runtime_eval_expression_int(&e); }
void eval_expression(double &out, const char *expr)
	{ std::string e = expr ? expr : ""; out = madc_runtime_eval_expression_double(&e); }
void eval_expression(std::string &out, const char *expr)
	{ std::string e = expr ? expr : ""; madc_runtime_eval_expression(&out, &e); }
void eval_expression(value &out, const char *expr)
	{ std::string e = expr ? expr : "", r; madc_runtime_eval_expression(&r, &e); out = value(r); }

// Context-carrying expression forms: `ctx` IS a madc::value (the unified
// script array, package A0) of nested key/value entries built with the
// context_set_* helpers below.
std::string &eval_expression_ctx(std::string &out, const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; return *(std::string *)madc_runtime_eval_expression_ctx(&out, &e, &ctx); }
bool eval_expression_bool_ctx(const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_bool_ctx(&e, &ctx); }
int64_t eval_expression_int_ctx(const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_int_ctx(&e, &ctx); }
double eval_expression_double_ctx(const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_double_ctx(&e, &ctx); }
std::string &eval_expression_string_ctx(std::string &out, const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; return *(std::string *)madc_runtime_eval_expression_string_ctx(&out, &e, &ctx); }
value &eval_expression_string_ctx(value &out, const char *expr, value &ctx)
	{ std::string e = expr ? expr : "", r; madc_runtime_eval_expression_string_ctx(&r, &e, &ctx); out = value(r); return out; }

// Typed out-parameter ctx forms — every eval_expression overload needs a
// _ctx sibling or the call-site scope-capture rebind has no target (the
// std::string& destination is covered by the render form above, which
// the rebind's overload re-rank already selects).
void eval_expression_ctx(int64_t &out, const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; out = madc_runtime_eval_expression_int_ctx(&e, &ctx); }
void eval_expression_ctx(double &out, const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; out = madc_runtime_eval_expression_double_ctx(&e, &ctx); }
void eval_expression_ctx(value &out, const char *expr, value &ctx)
	{ std::string e = expr ? expr : "", r; madc_runtime_eval_expression_ctx(&r, &e, &ctx); out = value(r); }

// Context-carrying full-eval forms — the rebind targets for call-site
// scope capture (the parser rebinds a madc::eval_* call to its _ctx
// sibling and appends the captured scope when scope access is enabled).
std::string &eval_unit_ctx(std::string &out, std::string &source, value &ctx)
	{ return *(std::string *)madc_runtime_eval_ctx(&out, &source, &ctx); }
bool eval_bool_ctx(std::string &source, value &ctx)
	{ return madc_runtime_eval_bool_ctx(&source, &ctx); }
int64_t eval_int_ctx(std::string &source, value &ctx)
	{ return madc_runtime_eval_int_ctx(&source, &ctx); }
int64_t eval_int_ctx(const char *source, value &ctx)
	{ std::string s = source ? source : ""; return madc_runtime_eval_int_ctx(&s, &ctx); }
double eval_double_ctx(std::string &source, value &ctx)
	{ return madc_runtime_eval_double_ctx(&source, &ctx); }
std::string &eval_string_ctx(std::string &out, std::string &source, value &ctx)
	{ return *(std::string *)madc_runtime_eval_string_ctx(&out, &source, &ctx); }

// Value-first _ctx siblings — every value-first eval overload above needs
// one, or the call-site scope-capture rebind has no target.
value &eval_unit_ctx(value &out, const char *source, value &ctx)
	{ std::string s = source ? source : "", r; madc_runtime_eval_ctx(&r, &s, &ctx); out = value(r); return out; }
bool eval_bool_ctx(const char *source, value &ctx)
	{ std::string s = source ? source : ""; return madc_runtime_eval_bool_ctx(&s, &ctx); }
double eval_double_ctx(const char *source, value &ctx)
	{ std::string s = source ? source : ""; return madc_runtime_eval_double_ctx(&s, &ctx); }
value &eval_string_ctx(value &out, const char *source, value &ctx)
	{ std::string s = source ? source : "", r; madc_runtime_eval_string_ctx(&r, &s, &ctx); out = value(r); return out; }

// Compiler data as data (madcide IDE-3): compile-NEVER-execute a source
// buffer in a child Program and return the compiler's own structured
// data as a value array — the meta-level surface an IDE's diagnostics
// pane and outline project. The <ns_madc> declarations carry the row
// shapes and the thread contract.
value &diagnostics(value &out, const char *source)
	{ std::string s = source ? source : "", f; madc_source_diagnostics(&out, &s, &f); return out; }
value &diagnostics(value &out, const char *source, const char *filename)
	{ std::string s = source ? source : "", f = filename ? filename : ""; madc_source_diagnostics(&out, &s, &f); return out; }
value &outline(value &out, const char *source)
	{ std::string s = source ? source : "", f; madc_source_outline(&out, &s, &f); return out; }
value &outline(value &out, const char *source, const char *filename)
	{ std::string s = source ? source : "", f = filename ? filename : ""; madc_source_outline(&out, &s, &f); return out; }
// The render query (the view seam's code views): parse the buffer in the
// same child and render its cir_node tree as `target` (the --emit=
// vocabulary). The <ns_madc> declaration carries the contract.
bool emit(value &out, const char *source, const char *filename,
	  const char *target)
	{ std::string s = source ? source : "", f = filename ? filename : "", t = target ? target : ""; return madc_source_emit(&out, &s, &f, &t); }

// Persistent parse handles (madcide AST-1): the compiler-data surfaces
// above given a LIFETIME — open a live parse per TU, refresh it whole
// on save/check, query outline / check / enclosing from the RETAINED
// state without re-running the front end. The <ns_madc> declarations
// carry the row shapes and the thread contract.
int64_t parse_open(const char *source, const char *filename)
	{ std::string s = source ? source : "", f = filename ? filename : ""; return madc_parse_open(&s, &f); }
int64_t parse_open_file(const char *path)
	{ std::string p = path ? path : ""; return madc_parse_open_file(&p); }
bool parse_refresh(int64_t handle, const char *source)
	{ std::string s = source ? source : ""; return madc_parse_refresh(handle, &s); }
bool parse_close(int64_t handle)
	{ return madc_parse_close(handle); }
value &parse_outline(value &out, int64_t handle)
	{ madc_parse_outline(&out, handle); return out; }
value &parse_check(value &out, int64_t handle)
	{ madc_parse_diagnostics(&out, handle); return out; }
value &parse_enclosing(value &out, int64_t handle, int64_t line,
		       int64_t column)
	{ madc_parse_enclosing(&out, handle, line, column); return out; }
value &parse_spans(value &out, int64_t handle)
	{ madc_parse_spans(&out, handle); return out; }

// The live-tree build/run pair (OWNER RULING 2026-08-27 — the running
// madc IS the compiler; ^B never re-parses, never execs a madc):
// parse_build emits a native artifact from the handle's EXISTING parsed
// tree; parse_run runs that tree in a fork() child. The <ns_madc>
// declarations carry the contracts.
bool parse_build(value &out_diags, int64_t handle, const char *kind,
		 const char *outpath)
	{
	    std::string k = kind ? kind : "";
	    std::string o = outpath ? outpath : "";
	    return madc_parse_build(&out_diags, handle, &k, &o);
	}
int64_t parse_run(int64_t handle)
	{ return madc_parse_run(handle); }

// The build surface (madcide IDE-10c): the CLI's AOT lane run IN-PROCESS —
// the IDE lives inside the compiler and never shells out to a PATH madc.
// The <ns_madc> declaration carries the contract (kinds, row shape).
bool build_native(value &out_diags, const char *path, const char *kind,
		  const char *outpath)
	{
	    std::string p = path ? path : "", k = kind ? kind : "";
	    std::string o = outpath ? outpath : "";
	    return madc_build_native(&out_diags, &p, &k, &o);
	}

// The running compiler's own resolved executable path (madcide IDE-10c:
// "run this program" spawns a child OF SELF — never a PATH madc). Stable
// for the process lifetime (C++11 static init; immutable after — safe
// for concurrent readers); empty string = the platform probe failed.
const char *compiler_path()
	{
	    static const std::string p = madc_self_exe_path();
	    return p.c_str();
	}
value &lex_spans(value &out, const char *text, const char *filename)
	{
	    std::string src = text ? text : "";
	    std::string disp = filename ? filename : "";
	    madc_lex_spans(&out, &src, &disp);
	    return out;
	}
int64_t project_open(const char *manifest)
	{ std::string m = manifest ? manifest : ""; return madc_project_open(&m); }
value &project_tus(value &out, int64_t handle)
	{ madc_project_tus(&out, handle); return out; }
bool project_close(int64_t handle)
	{ return madc_project_close(handle); }
// The project build/run pair (madcide ^B correlation, owner design doc
// 2026-08-31: with a manifest open, Build = the --project build). The
// <ns_madc> declarations carry the contracts.
bool project_build(value &out_diags, const char *manifest, const char *kind,
		   const char *outpath)
	{
	    std::string m = manifest ? manifest : "", k = kind ? kind : "";
	    std::string o = outpath ? outpath : "";
	    return madc_project_build(&out_diags, &m, &k, &o);
	}
int64_t project_run(const char *manifest)
	{ std::string m = manifest ? manifest : ""; return madc_project_run(&m); }

// Context builders. Kind-safe via value_object_for_write: a null ctx
// vivifies to kind::object; any other non-object kind degrades to a
// diagnosed no-op instead of throwing across the JIT boundary.
void context_set_int(value &ctx, std::string &key, int64_t v)
	{ ns_common::value_object_for_write(ctx, "madc::context_set_int")[key] = value(int64_t(v)); }
void context_set_real(value &ctx, std::string &key, double v)
	{ ns_common::value_object_for_write(ctx, "madc::context_set_real")[key] = value(v); }
void context_set_string(value &ctx, std::string &key, const char *v)
	{ ns_common::value_object_for_write(ctx, "madc::context_set_string")[key] = value(std::string(v ? v : "")); }
void context_set_array(value &ctx, std::string &key, value &v)
	{ ns_common::value_object_for_write(ctx, "madc::context_set_array")[key] = v; }

// Value-first context builders: const char* keys, so building a context
// never requires <string> in the script.
void context_set_int(value &ctx, const char *key, int64_t v)
	{ std::string k = key ? key : ""; ns_common::value_object_for_write(ctx, "madc::context_set_int")[k] = value(int64_t(v)); }
void context_set_real(value &ctx, const char *key, double v)
	{ std::string k = key ? key : ""; ns_common::value_object_for_write(ctx, "madc::context_set_real")[k] = value(v); }
void context_set_string(value &ctx, const char *key, const char *v)
	{ std::string k = key ? key : ""; ns_common::value_object_for_write(ctx, "madc::context_set_string")[k] = value(std::string(v ? v : "")); }
void context_set_array(value &ctx, const char *key, value &v)
	{ std::string k = key ? key : ""; ns_common::value_object_for_write(ctx, "madc::context_set_array")[k] = v; }

// madc::getline — line input, value-first (the <ns_madc> declaration
// carries the contract). std::getline(cin, string&) is the oracle:
// failbit means NOTHING was extracted (clean EOF -> false); a final
// unterminated line and an empty line both deliver (true). The value
// takes the line through its own assignment, so freeze rejection stays
// with the one owner.
bool getline(value &out)
{
	std::string line;
	bool got = (bool)std::getline(std::cin, line);
	out = value(line);
	return got;
}

} // namespace madc

// ---- C-linkage API for C hosts (libmadc.a/.so) --------------------------

extern "C" {

void *__madc_eval_runtime(void *result, void *source)
{
    return madc_runtime_eval(result, source);
}

bool __madc_eval_bool_runtime(void *source)
{
    return madc_runtime_eval_bool(source);
}

int64_t __madc_eval_int_runtime(void *source)
{
    return madc_runtime_eval_int(source);
}

double __madc_eval_double_runtime(void *source)
{
    return madc_runtime_eval_double(source);
}

void *__madc_eval_string_runtime(void *result, void *source)
{
    return madc_runtime_eval_string(result, source);
}

void *__madc_eval_ctx_runtime(void *result, void *source, void *ctx)
{
    return madc_runtime_eval_ctx(result, source, ctx);
}

bool __madc_eval_bool_ctx_runtime(void *source, void *ctx)
{
    return madc_runtime_eval_bool_ctx(source, ctx);
}

int64_t __madc_eval_int_ctx_runtime(void *source, void *ctx)
{
    return madc_runtime_eval_int_ctx(source, ctx);
}

double __madc_eval_double_ctx_runtime(void *source, void *ctx)
{
    return madc_runtime_eval_double_ctx(source, ctx);
}

void *__madc_eval_string_ctx_runtime(void *result, void *source, void *ctx)
{
    return madc_runtime_eval_string_ctx(result, source, ctx);
}

void *__madc_eval_expression_runtime(void *result, void *expr)
{
    return madc_runtime_eval_expression(result, expr);
}

bool __madc_eval_expression_bool_runtime(void *expr)
{
    return madc_runtime_eval_expression_bool(expr);
}

int64_t __madc_eval_expression_int_runtime(void *expr)
{
    return madc_runtime_eval_expression_int(expr);
}

double __madc_eval_expression_double_runtime(void *expr)
{
    return madc_runtime_eval_expression_double(expr);
}

void *__madc_eval_expression_string_runtime(void *result, void *expr)
{
    return madc_runtime_eval_expression_string(result, expr);
}

void *__madc_eval_expression_ctx_runtime(void *result, void *expr, void *ctx)
{
    return madc_runtime_eval_expression_ctx(result, expr, ctx);
}

bool __madc_eval_expression_bool_ctx_runtime(void *expr, void *ctx)
{
    return madc_runtime_eval_expression_bool_ctx(expr, ctx);
}

int64_t __madc_eval_expression_int_ctx_runtime(void *expr, void *ctx)
{
    return madc_runtime_eval_expression_int_ctx(expr, ctx);
}

double __madc_eval_expression_double_ctx_runtime(void *expr, void *ctx)
{
    return madc_runtime_eval_expression_double_ctx(expr, ctx);
}

void *__madc_eval_expression_string_ctx_runtime(void *result, void *expr, void *ctx)
{
    return madc_runtime_eval_expression_string_ctx(result, expr, ctx);
}

void *__madc_context_set_int_runtime(void *ctx, void *key, int64_t value)
{
    madc::context_set_int(*(madc::value *)ctx, *(std::string *)key, value);
    return ctx;
}

void *__madc_context_set_real_runtime(void *ctx, void *key, double value)
{
    madc::context_set_real(*(madc::value *)ctx, *(std::string *)key, value);
    return ctx;
}

void *__madc_context_set_string_runtime(void *ctx, void *key, const char *value)
{
    madc::context_set_string(*(madc::value *)ctx, *(std::string *)key, value);
    return ctx;
}

void *__madc_context_set_array_runtime(void *ctx, void *key, void *value)
{
    madc::context_set_array(*(madc::value *)ctx, *(std::string *)key,
			    *(madc::value *)value);
    return ctx;
}

}
