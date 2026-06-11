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

namespace madc {

// Full-program eval: the source must define (or be wrapped into)
// `__madc_eval()`; the rendered result text lands in `out`.
std::string &eval_unit(std::string &out, std::string &source)
	{ return *(std::string *)madc_runtime_eval(&out, &source); }
bool eval_bool(std::string &source)
	{ return madc_runtime_eval_bool(&source); }
long eval_int(std::string &source)
	{ return madc_runtime_eval_int(&source); }
long eval_int(const char *source)
	{ std::string s = source ? source : ""; return madc_runtime_eval_int(&s); }
double eval_double(std::string &source)
	{ return madc_runtime_eval_double(&source); }
std::string &eval_string(std::string &out, std::string &source)
	{ return *(std::string *)madc_runtime_eval_string(&out, &source); }

// Expression eval: a single expression, no function calls unless the
// engine's expression policy allows them.
bool eval_expression_bool(const char *expr)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_bool(&e); }
long eval_expression_int(const char *expr)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_int(&e); }
double eval_expression_double(const char *expr)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_double(&e); }
std::string &eval_expression_string(std::string &out, const char *expr)
	{ std::string e = expr ? expr : ""; return *(std::string *)madc_runtime_eval_expression_string(&out, &e); }

// Typed out-parameter forms (overloaded on the destination). The
// string-destination form RENDERS any result type ("42", "4.000000",
// "echo") via the untyped runtime; eval_expression_string above is the
// strict string-typed coercion.
void eval_expression(long &out, const char *expr)
	{ std::string e = expr ? expr : ""; out = madc_runtime_eval_expression_int(&e); }
void eval_expression(double &out, const char *expr)
	{ std::string e = expr ? expr : ""; out = madc_runtime_eval_expression_double(&e); }
void eval_expression(std::string &out, const char *expr)
	{ std::string e = expr ? expr : ""; madc_runtime_eval_expression(&out, &e); }

// Context-carrying expression forms: `ctx` IS a madc::value (the unified
// script array, package A0) of nested key/value entries built with the
// context_set_* helpers below.
std::string &eval_expression_ctx(std::string &out, const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; return *(std::string *)madc_runtime_eval_expression_ctx(&out, &e, &ctx); }
bool eval_expression_bool_ctx(const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_bool_ctx(&e, &ctx); }
long eval_expression_int_ctx(const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_int_ctx(&e, &ctx); }
double eval_expression_double_ctx(const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; return madc_runtime_eval_expression_double_ctx(&e, &ctx); }
std::string &eval_expression_string_ctx(std::string &out, const char *expr, value &ctx)
	{ std::string e = expr ? expr : ""; return *(std::string *)madc_runtime_eval_expression_string_ctx(&out, &e, &ctx); }

// Context-carrying full-eval forms — the rebind targets for call-site
// scope capture (the parser rebinds a madc::eval_* call to its _ctx
// sibling and appends the captured scope when scope access is enabled).
std::string &eval_unit_ctx(std::string &out, std::string &source, value &ctx)
	{ return *(std::string *)madc_runtime_eval_ctx(&out, &source, &ctx); }
bool eval_bool_ctx(std::string &source, value &ctx)
	{ return madc_runtime_eval_bool_ctx(&source, &ctx); }
long eval_int_ctx(std::string &source, value &ctx)
	{ return madc_runtime_eval_int_ctx(&source, &ctx); }
long eval_int_ctx(const char *source, value &ctx)
	{ std::string s = source ? source : ""; return madc_runtime_eval_int_ctx(&s, &ctx); }
double eval_double_ctx(std::string &source, value &ctx)
	{ return madc_runtime_eval_double_ctx(&source, &ctx); }
std::string &eval_string_ctx(std::string &out, std::string &source, value &ctx)
	{ return *(std::string *)madc_runtime_eval_string_ctx(&out, &source, &ctx); }

// Context builders. Kind-safe via value_object_for_write: a null ctx
// vivifies to kind::object; any other non-object kind degrades to a
// diagnosed no-op instead of throwing across the JIT boundary.
void context_set_int(value &ctx, std::string &key, long v)
	{ ns_common::value_object_for_write(ctx, "madc::context_set_int")[key] = value(int64_t(v)); }
void context_set_real(value &ctx, std::string &key, double v)
	{ ns_common::value_object_for_write(ctx, "madc::context_set_real")[key] = value(v); }
void context_set_string(value &ctx, std::string &key, const char *v)
	{ ns_common::value_object_for_write(ctx, "madc::context_set_string")[key] = value(std::string(v ? v : "")); }
void context_set_array(value &ctx, std::string &key, value &v)
	{ ns_common::value_object_for_write(ctx, "madc::context_set_array")[key] = v; }

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
