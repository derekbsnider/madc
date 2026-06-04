//////////////////////////////////////////////////////////////////////////
//									//
// madc parser methods to parse the tokens into an AST			//
//									//
//////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <dlfcn.h>
#include <unistd.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <list>
#include <set>
#include <vector>
#include <functional>
#include <queue>
#include <stack>
#include "libmadc/program.h"
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_mangle.h"

using namespace std;

extern "C" {
int64_t __madc_regex_match(void *, void *);
int64_t __madc_regex_search(void *, void *);
void *__madc_regex_replace(void *, void *, void *, void *);
}

// Static parse position — inherited by all new tokens automatically
const char *TokenBase::_parse_file = NULL;
int TokenBase::_parse_line = 0;
int TokenBase::_parse_column = 0;

static size_t find_struct_member_index(DataDefSTRUCT *sdd, const std::string &field_name);

namespace madc {
bool internal_program_runtime_eval_source(::Program &self,
					  const std::string &source_text,
					  value &result,
					  const std::string &display_name,
					  const value *context = NULL,
					  const char *wrapper_return_type = NULL);
bool internal_program_runtime_eval_expression(::Program &self,
					      const std::string &expression,
					      value &result,
					      const std::string &display_name,
					      const value *context);
}

namespace {

thread_local Program *g_runtime_program = NULL;

DataDef *complex_builtin_type(DataDef *base_type)
{
    static std::map<DataDef *, DataDefCOMPLEX *> cache;
    if ( !base_type )
	return &ddVOID;
    std::map<DataDef *, DataDefCOMPLEX *>::iterator it = cache.find(base_type);
    if ( it != cache.end() )
	return it->second;
    DataDefCOMPLEX *complex_type = new DataDefCOMPLEX(*base_type);
    cache[base_type] = complex_type;
    return complex_type;
}

FuncDef *make_implicit_complex_builtin_func(const std::string &fname)
{
    DataDef *arg_type = &ddINT64;
    DataDef *ret_type = &ddINT32;

    if ( fname == "__builtin_conjf" || fname == "conjf" )
	arg_type = ret_type = complex_builtin_type(&ddFLOAT);
    else if ( fname == "__builtin_conj" || fname == "conj"
	   || fname == "__builtin_conjl" || fname == "conjl" )
	arg_type = ret_type = complex_builtin_type(&ddDOUBLE);
    else if ( fname == "__builtin_crealf" || fname == "crealf"
	   || fname == "__builtin_cimagf" || fname == "cimagf" )
    {
	arg_type = complex_builtin_type(&ddFLOAT);
	ret_type = &ddFLOAT;
    }
    else if ( fname == "__builtin_creal" || fname == "creal"
	   || fname == "__builtin_creall" || fname == "creall"
	   || fname == "__builtin_cimag" || fname == "cimag"
	   || fname == "__builtin_cimagl" || fname == "cimagl" )
    {
	arg_type = complex_builtin_type(&ddDOUBLE);
	ret_type = &ddDOUBLE;
    }

    FuncDef *func = new FuncDef(*ret_type);
    // Fixed single-argument signature — NOT varargs. The real libm symbols
    // (conjf/crealf/…) take exactly one _Complex (or, for creal/cimag, return a
    // scalar). Marking them varargs routed a _Complex argument through the
    // variadic ABI, which mismatches the SysV register classification for a
    // complex value and corrupted the call (wrong conjugate / return value).
    func->parameters.push_back(arg_type);
    func->param_typedef_names.push_back("");
    return func;
}

bool is_implicit_complex_builtin_name(const std::string &fname)
{
    return fname == "__builtin_conj"
	|| fname == "__builtin_conjf"
	|| fname == "__builtin_conjl"
	|| fname == "__builtin_creal"
	|| fname == "__builtin_crealf"
	|| fname == "__builtin_creall"
	|| fname == "__builtin_cimag"
	|| fname == "__builtin_cimagf"
	|| fname == "__builtin_cimagl"
	|| fname == "conj"
	|| fname == "conjf"
	|| fname == "conjl"
	|| fname == "creal"
	|| fname == "crealf"
	|| fname == "creall"
	|| fname == "cimag"
	|| fname == "cimagf"
	|| fname == "cimagl";
}

std::string stringify_runtime_eval_value(const madc::value &resolved)
{
    switch ( resolved.type() )
    {
	case madc::value::kind::null:
	    return std::string();
	case madc::value::kind::boolean:
	    return resolved.as_boolean() ? "true" : "false";
	case madc::value::kind::integer:
	    return std::to_string(resolved.as_integer());
	case madc::value::kind::real:
	    return std::to_string(resolved.as_real());
	case madc::value::kind::string:
	    return resolved.as_string();
	default:
	    break;
    }
    return std::string();
}

bool value_from_madarray_context(const MadArray &arr,
				 madc::value &out,
				 std::string &reason);

bool value_from_madvalue_context(const MadValue &in,
				 madc::value &out,
				 std::string &reason)
{
    switch ( in.kind )
    {
	case MadValueKind::mvINT:
	    out = madc::value(static_cast<int64_t>(in.as_int()));
	    return true;
	case MadValueKind::mvDOUBLE:
	    out = madc::value(in.as_double());
	    return true;
	case MadValueKind::mvSTRING:
	    out = madc::value(in.as_string());
	    return true;
	case MadValueKind::mvARRAY:
	    return value_from_madarray_context(in.as_array(), out, reason);
	case MadValueKind::mvNONE:
	    break;
    }

    reason = std::string("unsupported context value kind '")
	+ std::to_string((int)in.kind) + "'";
    return false;
}

bool value_from_madarray_context(const MadArray &arr,
				 madc::value &out,
				 std::string &reason)
{
    if ( !arr.data.empty() )
    {
	reason = "context arrays cannot contain positional elements";
	return false;
    }

    std::map<std::string, madc::value> fields;
    for ( std::size_t i = 0; i < arr.assoc.size(); ++i )
    {
	madc::value field_value;
	if ( !value_from_madvalue_context(arr.assoc[i].second, field_value, reason) )
	{
	    reason = std::string("context field '") + arr.assoc[i].first + "': " + reason;
	    return false;
	}
	fields[arr.assoc[i].first] = field_value;
    }

    out = madc::value::make_object(fields);
    return true;
}

bool build_runtime_expression_context(const MadArray *ctx_array,
				      Program &active,
				      const char *helper_name,
				      madc::value &context)
{
    context = madc::value();
    if ( !ctx_array )
	return true;

    std::string reason;
    if ( !value_from_madarray_context(*ctx_array, context, reason) )
    {
	active.set_error(Program::DiagnosticPhase::runtime,
			 std::string(helper_name)
			 + " rejected context: "
			 + reason);
	active.print_last_diagnostic(active.error());
	return false;
    }
    return true;
}

void report_runtime_eval_expression_type_error(Program &active,
					       const char *helper_name,
					       const char *expected_kind,
					       const madc::value &resolved)
{
    active.set_error(Program::DiagnosticPhase::runtime,
		     std::string(helper_name)
		     + " requires a "
		     + expected_kind
		     + " expression result, got "
		     + madc::value::kind_name(resolved.type()));
    active.print_last_diagnostic(active.error());
}

bool coerce_runtime_expression_bool(Program &active,
				    const madc::value &resolved,
				    const char *helper_name,
				    bool &out)
{
    switch ( resolved.type() )
    {
	case madc::value::kind::boolean:
	    out = resolved.as_boolean();
	    return true;
	case madc::value::kind::integer:
	    out = resolved.as_integer() != 0;
	    return true;
	default:
	    break;
    }

    report_runtime_eval_expression_type_error(active, helper_name, "boolean", resolved);
    return false;
}

bool coerce_runtime_expression_int(Program &active,
				   const madc::value &resolved,
				   const char *helper_name,
				   int64_t &out)
{
    if ( resolved.is_integer() )
    {
	out = resolved.as_integer();
	return true;
    }

    report_runtime_eval_expression_type_error(active, helper_name, "integer", resolved);
    return false;
}

bool coerce_runtime_expression_double(Program &active,
				      const madc::value &resolved,
				      const char *helper_name,
				      double &out)
{
    if ( resolved.is_real() )
    {
	out = resolved.as_real();
	return true;
    }
    if ( resolved.is_integer() )
    {
	out = (double)resolved.as_integer();
	return true;
    }

    report_runtime_eval_expression_type_error(active, helper_name, "real", resolved);
    return false;
}

bool coerce_runtime_expression_string(Program &active,
				      const madc::value &resolved,
				      const char *helper_name,
				      std::string &out)
{
    if ( resolved.is_string() )
    {
	out = resolved.as_string();
	return true;
    }

    report_runtime_eval_expression_type_error(active, helper_name, "string", resolved);
    return false;
}

Program *require_active_runtime_program()
{
    return Program::active_runtime_program();
}

Program *require_runtime_eval_program(std::unique_ptr<Program> &owned)
{
    Program *active = require_active_runtime_program();
    if ( active )
	return active;

    static MadcEngine engine;
    owned = engine.create_program();
    return owned.get();
}

bool is_runtime_eval_scope_helper_name(const std::string &name)
{
    return name == "__madc_eval_runtime"
	|| name == "__madc_eval_bool_runtime"
	|| name == "__madc_eval_int_runtime"
	|| name == "__madc_eval_double_runtime"
	|| name == "__madc_eval_string_runtime"
	|| name == "__madc_eval_expression_runtime"
	|| name == "__madc_eval_expression_bool_runtime"
	|| name == "__madc_eval_expression_int_runtime"
	|| name == "__madc_eval_expression_double_runtime"
	|| name == "__madc_eval_expression_string_runtime";
}

bool is_runtime_eval_scope_ctx_helper_name(const std::string &name)
{
    return name == "__madc_eval_ctx_runtime"
	|| name == "__madc_eval_bool_ctx_runtime"
	|| name == "__madc_eval_int_ctx_runtime"
	|| name == "__madc_eval_double_ctx_runtime"
	|| name == "__madc_eval_string_ctx_runtime"
	|| name == "__madc_eval_expression_ctx_runtime"
	|| name == "__madc_eval_expression_bool_ctx_runtime"
	|| name == "__madc_eval_expression_int_ctx_runtime"
	|| name == "__madc_eval_expression_double_ctx_runtime"
	|| name == "__madc_eval_expression_string_ctx_runtime";
}

bool is_runtime_eval_scope_public_name(const std::string &name)
{
    return name == "eval"
	|| name == "eval_bool"
	|| name == "eval_int"
	|| name == "eval_double"
	|| name == "eval_string"
	|| name == "eval_expression"
	|| name == "eval_expression_bool"
	|| name == "eval_expression_int"
	|| name == "eval_expression_double"
	|| name == "eval_expression_string";
}

bool is_runtime_eval_source_helper_name(const std::string &name)
{
    return name == "__madc_eval_runtime"
	|| name == "__madc_eval_bool_runtime"
	|| name == "__madc_eval_int_runtime"
	|| name == "__madc_eval_double_runtime"
	|| name == "__madc_eval_string_runtime";
}

bool is_runtime_eval_expression_helper_name(const std::string &name)
{
    return name == "__madc_eval_expression_runtime"
	|| name == "__madc_eval_expression_bool_runtime"
	|| name == "__madc_eval_expression_int_runtime"
	|| name == "__madc_eval_expression_double_runtime"
	|| name == "__madc_eval_expression_string_runtime";
}

void *madc_runtime_eval_expression(void *result, void *expr)
{
    std::string &out = *(std::string *)result;
    std::string &expression = *(std::string *)expr;
    out.clear();

    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return result;

    madc::value resolved;
    if ( !active->runtime_eval_expression(expression,
					  resolved,
					  "__madc_runtime_eval_expression") )
    {
	active->print_last_diagnostic(active->error());
	return result;
    }

    out = stringify_runtime_eval_value(resolved);
    return result;
}

void *madc_runtime_eval_expression_ctx(void *result, void *expr, void *ctx)
{
    std::string &out = *(std::string *)result;
    std::string &expression = *(std::string *)expr;
    MadArray &context_array = *(MadArray *)ctx;
    out.clear();

    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return result;

    madc::value context;
    if ( !build_runtime_expression_context(&context_array, *active, "madc::eval_expression_ctx", context) )
	return result;

    madc::value resolved;
    if ( !active->runtime_eval_expression(expression,
					  resolved,
					  "__madc_runtime_eval_expression",
					  &context) )
    {
	active->print_last_diagnostic(active->error());
	return result;
    }

    out = stringify_runtime_eval_value(resolved);
    return result;
}

void *madc_runtime_eval(void *result, void *source)
{
    std::string &out = *(std::string *)result;
    std::string &program_source = *(std::string *)source;
    out.clear();

    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return result;

    madc::value resolved;
    if ( !active->runtime_eval_source(program_source, resolved, "__madc_runtime_eval") )
    {
	active->print_last_diagnostic(active->error());
	return result;
    }

    out = stringify_runtime_eval_value(resolved);
    return result;
}

bool madc_runtime_eval_bool(void *source)
{
    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return false;

    std::string &program_source = *(std::string *)source;
    madc::value resolved;
    bool out = false;

    if ( !active->runtime_eval_source(program_source, resolved, "__madc_runtime_eval", NULL, "bool") )
	return false;
    if ( !coerce_runtime_expression_bool(*active, resolved, "madc::eval_bool", out) )
	return false;
    return out;
}

int64_t madc_runtime_eval_int(void *source)
{
    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return 0;

    std::string &program_source = *(std::string *)source;
    madc::value resolved;
    int64_t out = 0;

    if ( !active->runtime_eval_source(program_source, resolved, "__madc_runtime_eval", NULL, "int") )
	return 0;
    if ( !coerce_runtime_expression_int(*active, resolved, "madc::eval_int", out) )
	return 0;
    return out;
}

double madc_runtime_eval_double(void *source)
{
    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return 0.0;

    std::string &program_source = *(std::string *)source;
    madc::value resolved;
    double out = 0.0;

    if ( !active->runtime_eval_source(program_source, resolved, "__madc_runtime_eval", NULL, "double") )
	return 0.0;
    if ( !coerce_runtime_expression_double(*active, resolved, "madc::eval_double", out) )
	return 0.0;
    return out;
}

void *madc_runtime_eval_string(void *result, void *source)
{
    std::string &out = *(std::string *)result;
    std::string &program_source = *(std::string *)source;
    out.clear();

    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return result;

    madc::value resolved;
    if ( !active->runtime_eval_source(program_source, resolved, "__madc_runtime_eval", NULL, "char *") )
	return result;
    if ( !coerce_runtime_expression_string(*active, resolved, "madc::eval_string", out) )
	return result;
    return result;
}

void *madc_runtime_eval_ctx(void *result, void *source, void *ctx)
{
    std::string &out = *(std::string *)result;
    std::string &program_source = *(std::string *)source;
    MadArray &context_array = *(MadArray *)ctx;
    out.clear();

    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return result;

    madc::value context;
    if ( !build_runtime_expression_context(&context_array, *active, "madc::eval", context) )
	return result;

    madc::value resolved;
    if ( !active->runtime_eval_source(program_source, resolved, "__madc_runtime_eval", &context) )
    {
	active->print_last_diagnostic(active->error());
	return result;
    }

    out = stringify_runtime_eval_value(resolved);
    return result;
}

bool madc_runtime_eval_bool_ctx(void *source, void *ctx)
{
    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return false;

    std::string &program_source = *(std::string *)source;
    MadArray &context_array = *(MadArray *)ctx;
    madc::value context;
    madc::value resolved;
    bool out = false;

    if ( !build_runtime_expression_context(&context_array, *active, "madc::eval_bool", context) )
	return false;
    if ( !active->runtime_eval_source(program_source, resolved, "__madc_runtime_eval", &context, "bool") )
    {
	active->print_last_diagnostic(active->error());
	return false;
    }
    if ( !coerce_runtime_expression_bool(*active, resolved, "madc::eval_bool", out) )
	return false;
    return out;
}

int64_t madc_runtime_eval_int_ctx(void *source, void *ctx)
{
    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return 0;

    std::string &program_source = *(std::string *)source;
    MadArray &context_array = *(MadArray *)ctx;
    madc::value context;
    madc::value resolved;
    int64_t out = 0;

    if ( !build_runtime_expression_context(&context_array, *active, "madc::eval_int", context) )
	return 0;
    if ( !active->runtime_eval_source(program_source, resolved, "__madc_runtime_eval", &context, "int") )
    {
	active->print_last_diagnostic(active->error());
	return 0;
    }
    if ( !coerce_runtime_expression_int(*active, resolved, "madc::eval_int", out) )
	return 0;
    return out;
}

double madc_runtime_eval_double_ctx(void *source, void *ctx)
{
    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return 0.0;

    std::string &program_source = *(std::string *)source;
    MadArray &context_array = *(MadArray *)ctx;
    madc::value context;
    madc::value resolved;
    double out = 0.0;

    if ( !build_runtime_expression_context(&context_array, *active, "madc::eval_double", context) )
	return 0.0;
    if ( !active->runtime_eval_source(program_source, resolved, "__madc_runtime_eval", &context, "double") )
    {
	active->print_last_diagnostic(active->error());
	return 0.0;
    }
    if ( !coerce_runtime_expression_double(*active, resolved, "madc::eval_double", out) )
	return 0.0;
    return out;
}

void *madc_runtime_eval_string_ctx(void *result, void *source, void *ctx)
{
    std::string &out = *(std::string *)result;
    std::string &program_source = *(std::string *)source;
    MadArray &context_array = *(MadArray *)ctx;
    out.clear();

    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return result;

    madc::value context;
    if ( !build_runtime_expression_context(&context_array, *active, "madc::eval_string", context) )
	return result;

    madc::value resolved;
    if ( !active->runtime_eval_source(program_source, resolved, "__madc_runtime_eval", &context, "char *") )
    {
	active->print_last_diagnostic(active->error());
	return result;
    }
    if ( !coerce_runtime_expression_string(*active, resolved, "madc::eval_string", out) )
	return result;
    return result;
}

bool madc_runtime_eval_expression_bool(void *expr)
{
    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return false;

    std::string &expression = *(std::string *)expr;
    madc::value resolved;
    bool out = false;

    if ( !active->runtime_eval_expression(expression,
					  resolved,
					  "__madc_runtime_eval_expression") )
	return false;
    if ( !coerce_runtime_expression_bool(*active, resolved, "madc::eval_expression_bool", out) )
	return false;
    return out;
}

bool madc_runtime_eval_expression_bool_ctx(void *expr, void *ctx)
{
    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return false;

    std::string &expression = *(std::string *)expr;
    MadArray &context_array = *(MadArray *)ctx;
    madc::value resolved;
    bool out = false;

    madc::value context;
    if ( !build_runtime_expression_context(&context_array, *active, "madc::eval_expression_bool_ctx", context) )
	return false;
    if ( !active->runtime_eval_expression(expression,
					  resolved,
					  "__madc_runtime_eval_expression",
					  &context) )
	return false;
    if ( !coerce_runtime_expression_bool(*active, resolved, "madc::eval_expression_bool_ctx", out) )
	return false;
    return out;
}

int64_t madc_runtime_eval_expression_int(void *expr)
{
    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return 0;

    std::string &expression = *(std::string *)expr;
    madc::value resolved;
    int64_t out = 0;

    if ( !active->runtime_eval_expression(expression,
					  resolved,
					  "__madc_runtime_eval_expression") )
	return 0;
    if ( !coerce_runtime_expression_int(*active, resolved, "madc::eval_expression_int", out) )
	return 0;
    return out;
}

int64_t madc_runtime_eval_expression_int_ctx(void *expr, void *ctx)
{
    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return 0;

    std::string &expression = *(std::string *)expr;
    MadArray &context_array = *(MadArray *)ctx;
    madc::value resolved;
    int64_t out = 0;

    madc::value context;
    if ( !build_runtime_expression_context(&context_array, *active, "madc::eval_expression_int_ctx", context) )
	return 0;
    if ( !active->runtime_eval_expression(expression,
					  resolved,
					  "__madc_runtime_eval_expression",
					  &context) )
	return 0;
    if ( !coerce_runtime_expression_int(*active, resolved, "madc::eval_expression_int_ctx", out) )
	return 0;
    return out;
}

double madc_runtime_eval_expression_double(void *expr)
{
    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return 0.0;

    std::string &expression = *(std::string *)expr;
    madc::value resolved;
    double out = 0.0;

    if ( !active->runtime_eval_expression(expression,
					  resolved,
					  "__madc_runtime_eval_expression") )
	return 0.0;
    if ( !coerce_runtime_expression_double(*active, resolved, "madc::eval_expression_double", out) )
	return 0.0;
    return out;
}

double madc_runtime_eval_expression_double_ctx(void *expr, void *ctx)
{
    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return 0.0;

    std::string &expression = *(std::string *)expr;
    MadArray &context_array = *(MadArray *)ctx;
    madc::value resolved;
    double out = 0.0;

    madc::value context;
    if ( !build_runtime_expression_context(&context_array, *active, "madc::eval_expression_double_ctx", context) )
	return 0.0;
    if ( !active->runtime_eval_expression(expression,
					  resolved,
					  "__madc_runtime_eval_expression",
					  &context) )
	return 0.0;
    if ( !coerce_runtime_expression_double(*active, resolved, "madc::eval_expression_double_ctx", out) )
	return 0.0;
    return out;
}

void *madc_runtime_eval_expression_string(void *result, void *expr)
{
    std::string &out = *(std::string *)result;
    std::string &expression = *(std::string *)expr;
    out.clear();

    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return result;

    madc::value resolved;
    if ( !active->runtime_eval_expression(expression,
					  resolved,
					  "__madc_runtime_eval_expression") )
	return result;
    if ( !coerce_runtime_expression_string(*active, resolved, "madc::eval_expression_string", out) )
	return result;
    return result;
}

void *madc_runtime_eval_expression_string_ctx(void *result, void *expr, void *ctx)
{
    std::string &out = *(std::string *)result;
    std::string &expression = *(std::string *)expr;
    MadArray &context_array = *(MadArray *)ctx;
    out.clear();

    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return result;

    madc::value context;
    if ( !build_runtime_expression_context(&context_array, *active, "madc::eval_expression_string_ctx", context) )
	return result;
    madc::value resolved;
    if ( !active->runtime_eval_expression(expression,
					  resolved,
					  "__madc_runtime_eval_expression",
					  &context) )
	return result;
    if ( !coerce_runtime_expression_string(*active, resolved, "madc::eval_expression_string_ctx", out) )
	return result;
    return result;
}

void *madc_context_set_int(void *ctx, void *key, int64_t value)
{
    ((MadArray *)ctx)->set(*(std::string *)key, MadValue(value));
    return ctx;
}

void *madc_context_set_real(void *ctx, void *key, double value)
{
    ((MadArray *)ctx)->set(*(std::string *)key, MadValue(value));
    return ctx;
}

void *madc_context_set_string(void *ctx, void *key, const char *value)
{
    ((MadArray *)ctx)->set(*(std::string *)key, MadValue(std::string(value ? value : "")));
    return ctx;
}

void *madc_context_set_array(void *ctx, void *key, void *value)
{
    ((MadArray *)ctx)->set(*(std::string *)key, MadValue(*(MadArray *)value));
    return ctx;
}

} // namespace

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
    return madc_context_set_int(ctx, key, value);
}

void *__madc_context_set_real_runtime(void *ctx, void *key, double value)
{
    return madc_context_set_real(ctx, key, value);
}

void *__madc_context_set_string_runtime(void *ctx, void *key, const char *value)
{
    return madc_context_set_string(ctx, key, value);
}

void *__madc_context_set_array_runtime(void *ctx, void *key, void *value)
{
    return madc_context_set_array(ctx, key, value);
}

}

static bool is_restrict_token(TokenBase *tb)
{
    return tb && tb->type() == TokenType::ttKeyword && tb->id() == TokenID::tkRESTRICT;
}

static bool is_post_pointer_qualifier_token(TokenBase *tb)
{
    return tb && tb->type() == TokenType::ttKeyword
        && (tb->id() == TokenID::tkRESTRICT
	 || tb->id() == TokenID::tkCONST
	 || tb->id() == TokenID::tkVOLATILE);
}

static bool is_type_qualifier_token(TokenBase *tb)
{
    return tb && tb->type() == TokenType::ttKeyword
	&& (tb->id() == TokenID::tkCONST
	 || tb->id() == TokenID::tkRESTRICT
	 || tb->id() == TokenID::tkVOLATILE
	 || tb->id() == TokenID::tkSTATIC
	 || tb->id() == TokenID::tkEXTERN);
}

static bool is_attribute_identifier_token(TokenBase *tb)
{
    if ( !tb || tb->type() != TokenType::ttIdentifier )
	return false;
    const std::string &name = ((TokenIdent *)tb)->str;
    return name == "__attribute__" || name == "__attribute";
}

static int64_t parse_constant_integer_expression(Program &pgm);

static TokenBase *consume_gnu_attributes(Program &pgm, TokenBase *nt,
					 std::set<std::string> *attrs = NULL,
					 std::string *alias_target = NULL,
					 size_t *explicit_align = NULL,
					 size_t *vector_bytes = NULL)
{
    while ( nt && is_attribute_identifier_token(nt) )
    {
	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrk )
	{
	    int adepth = 0;
	    bool saw_alias = false;
	    bool saw_aligned = false;
	    bool saw_vector_size = false;
	    do {
		TokenBase *at = pgm.nextToken();
		if ( !at ) break;
		if ( attrs && adepth >= 2 && at->type() == TokenType::ttIdentifier )
		    attrs->insert(((TokenIdent *)at)->str);
		if ( at->type() == TokenType::ttIdentifier
		  && ((TokenIdent *)at)->str == "alias" )
		    saw_alias = true;
		else if ( alias_target && saw_alias && at->type() == TokenType::ttString )
		{
		    *alias_target = ((TokenStr *)at)->str;
		    saw_alias = false;
		}
		else if ( at->type() == TokenType::ttIdentifier
		  && (((TokenIdent *)at)->str == "aligned"
		   || ((TokenIdent *)at)->str == "__aligned__") )
		    saw_aligned = true;
		else if ( explicit_align && saw_aligned && at->type() == TokenType::ttInteger )
		{
		    int64_t aval = static_cast<TokenInt *>(at)->ival();
		    if ( aval > 0 )
			*explicit_align = static_cast<size_t>(aval);
		    saw_aligned = false;
		}
		else if ( at->type() == TokenType::ttIdentifier
		  && (((TokenIdent *)at)->str == "vector_size"
		   || ((TokenIdent *)at)->str == "__vector_size__") )
		    saw_vector_size = true;
		else if ( saw_vector_size && at->id() == TokenID::tkOpBrk )
		{
		    int64_t n = parse_constant_integer_expression(pgm);
		    if ( vector_bytes && n > 0 )
			*vector_bytes = (size_t)n;
		    TokenBase *cl = pgm.nextToken();
		    if ( cl && cl->id() == TokenID::tkClBrk )
			; // consumed vector_size(...) argument paren
		    else if ( cl )
			pgm.pushToken(cl);
		    saw_vector_size = false;
		    continue; // don't do depth tracking for this paren pair
		}
		if ( at->id() == TokenID::tkOpBrk ) ++adepth;
		else if ( at->id() == TokenID::tkClBrk ) --adepth;
	    } while ( adepth > 0 );
	}
	nt = pgm.nextToken();
    }
    return nt;
}

static bool is_gnu_asm_identifier_token(TokenBase *tb)
{
    TokenIdent *ident = dynamic_cast<TokenIdent *>(tb);
    if ( !ident )
	return false;
    std::string name = ident->str;
    return name == "asm" || name == "__asm" || name == "__asm__";
}

static TokenBase *consume_gnu_asm_label(Program &pgm, TokenBase *nt,
					std::string *alias_target)
{
    while ( nt && is_gnu_asm_identifier_token(nt) )
    {
	TokenBase *open = pgm.nextToken();
	if ( !open || open->id() != TokenID::tkOpBrk )
	    pgm.Throw(open ? open : nt) << "Expecting '(' after asm label" << flush;
	TokenBase *label = pgm.nextToken();
	if ( alias_target && label && label->type() == TokenType::ttString )
	    *alias_target = ((TokenStr *)label)->str;
	TokenBase *close = pgm.nextToken();
	if ( !close || close->id() != TokenID::tkClBrk )
	    pgm.Throw(close ? close : label) << "Expecting ')' after asm label" << flush;
	nt = pgm.nextToken();
    }
    return nt;
}

// Skip C23 [[...]] attributes: [[gnu::noipa]], [[nodiscard]], etc.
// Returns the next meaningful token after any [[...]] sequences.
static void skip_c23_attributes(Program &pgm)
{
    while ( pgm.peekToken()
	 && pgm.peekToken()->id() == TokenID::tkOpSqr )
    {
	// peek ahead: is the NEXT token also [ ?
	TokenBase *first_sq = pgm.nextToken(); // consume first [
	if ( !pgm.peekToken()
	  || pgm.peekToken()->id() != TokenID::tkOpSqr )
	{
	    pgm.pushToken(first_sq); // not [[, push back
	    return;
	}
	pgm.nextToken(); // consume second [
	// Skip until ]]
	int depth = 1;
	while ( depth > 0 )
	{
	    TokenBase *t = pgm.nextToken();
	    if ( !t )
		break;
	    if ( t->id() == TokenID::tkOpSqr )
	    {
		if ( pgm.peekToken()
		  && pgm.peekToken()->id() == TokenID::tkOpSqr )
		{
		    pgm.nextToken();
		    ++depth;
		}
	    }
	    else if ( t->id() == TokenID::tkClSqr )
	    {
		if ( pgm.peekToken()
		  && pgm.peekToken()->id() == TokenID::tkClSqr )
		{
		    pgm.nextToken();
		    --depth;
		}
	    }
	}
    }
}

static size_t parse_gnu_vector_size_attribute(Program &pgm)
{
    if ( !is_attribute_identifier_token(pgm.peekToken()) )
	return 0;
    pgm.nextToken(); // consume __attribute__
    if ( !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkOpBrk )
	return 0;
    int depth = 0;
    bool saw_vector_size = false;
    size_t vector_bytes = 0;
    do {
	TokenBase *at = pgm.nextToken();
	if ( !at ) break;
	if ( saw_vector_size && at->id() == TokenID::tkOpBrk )
	{
	    int64_t n = parse_constant_integer_expression(pgm);
	    if ( n > 0 )
		vector_bytes = (size_t)n;
	    TokenBase *cl = pgm.nextToken();
	    if ( !cl || cl->id() != TokenID::tkClBrk )
		pgm.Throw(cl ? cl : at) << "Expected ')' after vector_size argument" << flush;
	    saw_vector_size = false;
	}
	else if ( at->id() == TokenID::tkOpBrk ) ++depth;
	else if ( at->id() == TokenID::tkClBrk ) --depth;
	else if ( at->type() == TokenType::ttIdentifier
	       && (((TokenIdent *)at)->str == "vector_size"
		|| ((TokenIdent *)at)->str == "__vector_size__") )
	    saw_vector_size = true;
	else if ( saw_vector_size && at->type() == TokenType::ttInteger )
	{
	    int64_t n = at->ival();
	    if ( n > 0 )
		vector_bytes = (size_t)n;
	}
    } while ( depth > 0 );
    return vector_bytes;
}

static void consume_typedef_gnu_attributes(Program &pgm,
					   std::string *mode_name = NULL,
					   size_t *vector_bytes = NULL)
{
    while ( is_attribute_identifier_token(pgm.peekToken()) )
    {
	pgm.nextToken(); // consume __attribute__
	if ( !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkOpBrk )
	    continue;
	int depth = 0;
	bool saw_mode = false;
	bool saw_vector_size = false;
	do {
	    TokenBase *at = pgm.nextToken();
	    if ( !at ) break;
	    if ( saw_vector_size && at->id() == TokenID::tkOpBrk )
	    {
		int64_t n = parse_constant_integer_expression(pgm);
		if ( vector_bytes && n > 0 )
		    *vector_bytes = (size_t)n;
		TokenBase *cl = pgm.nextToken();
		if ( !cl || cl->id() != TokenID::tkClBrk )
		    pgm.Throw(cl ? cl : at) << "Expected ')' after vector_size argument" << flush;
		saw_vector_size = false;
	    }
	    else if ( at->id() == TokenID::tkOpBrk )
		++depth;
	    else if ( at->id() == TokenID::tkClBrk )
		--depth;
	    else if ( at->type() == TokenType::ttIdentifier
		   && ((TokenIdent *)at)->str == "mode" )
		saw_mode = true;
	    else if ( at->type() == TokenType::ttIdentifier
		   && (((TokenIdent *)at)->str == "vector_size"
		    || ((TokenIdent *)at)->str == "__vector_size__") )
		saw_vector_size = true;
	    else if ( mode_name && saw_mode && at->type() == TokenType::ttIdentifier )
	    {
		*mode_name = ((TokenIdent *)at)->str;
		saw_mode = false;
	    }
	    else if ( vector_bytes && saw_vector_size && at->type() == TokenType::ttInteger )
	    {
		int64_t n = at->ival();
		if ( n > 0 )
		    *vector_bytes = (size_t)n;
		saw_vector_size = false;
	    }
	} while ( depth > 0 );
    }
}

static DataDef *apply_gnu_mode_alias(DataDef *base_dd, const std::string &mode_name)
{
    if ( !base_dd || mode_name.empty() )
	return base_dd;

    bool is_unsigned = false;
    switch ( base_dd->rawtype() )
    {
	case DataType::dtUINT8:
	case DataType::dtUINT16:
	case DataType::dtUINT24:
	case DataType::dtUINT32:
	case DataType::dtUINT64:
	    is_unsigned = true;
	    break;
	default:
	    break;
    }

    if ( mode_name == "QI" )
	return is_unsigned ? static_cast<DataDef *>(&ddUINT8) : static_cast<DataDef *>(&ddINT8);
    if ( mode_name == "HI" )
	return is_unsigned ? static_cast<DataDef *>(&ddUINT16) : static_cast<DataDef *>(&ddINT16);
    if ( mode_name == "SI" )
	return is_unsigned ? static_cast<DataDef *>(&ddUINT32) : static_cast<DataDef *>(&ddINT32);
    if ( mode_name == "DI" )
	return is_unsigned ? static_cast<DataDef *>(&ddUINT64) : static_cast<DataDef *>(&ddINT64);

    return base_dd;
}

static bool host_is_little_endian()
{
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
    return __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
#else
    const uint16_t probe = 0x0100;
    return *((const uint8_t *)&probe) == 0;
#endif
}

static bool reverse_scalar_storage_requested(const std::string &order_name)
{
    if ( order_name == "big-endian" )
	return host_is_little_endian();
    if ( order_name == "little-endian" )
	return !host_is_little_endian();
    return false;
}

static bool is_contextual_identifier_token(TokenBase *tb);
static std::string contextual_identifier_name(TokenBase *tb);

static bool token_origin_allows_c89_implicit_function(TokenBase *tb)
{
    if ( !tb || !tb->file )
	return false;
    std::string path(tb->file);
    return path.size() >= 2 && path.substr(path.size() - 2) == ".c";
}

static bool is_typeof_identifier(const std::string &name)
{
    return name == "typeof" || name == "typeof_unqual";
}

static bool is_alignof_identifier(const std::string &name)
{
    return name == "alignof" || name == "_Alignof" || name == "__alignof__";
}

static bool is_static_assert_identifier(const std::string &name)
{
    return name == "_Static_assert" || name == "static_assert";
}

static bool is_nullptr_identifier(const std::string &name)
{
    return name == "nullptr";
}

static DataDef *effective_pointer_type_for_member_access(Program &pgm, TokenBase *tb)
{
    if ( !tb )
	return NULL;

    DataDef *dd = tb->datadef();
    if ( dd && dd->is_pointer() )
	return dd;
    if ( DataDefCArray *add = dynamic_cast<DataDefCArray *>(dd) )
    {
	DataDef *elem = add->element_type ? add->element_type : &ddINT64;
	return pgm.getPointerType(elem);
    }

    if ( TokenVar *tv = dynamic_cast<TokenVar *>(tb) )
    {
	if ( tv->var.is_fixed_array() && tv->var.type )
	    return pgm.getPointerType(tv->var.type);
    }

    if ( TokenMember *tm = dynamic_cast<TokenMember *>(tb) )
    {
	if ( tm->is_fixed_array_member() && tm->var.type )
	    return pgm.getPointerType(tm->var.type);
    }

    if ( TokenOperator *op = dynamic_cast<TokenOperator *>(tb) )
    {
	if ( op->id() == TokenID::tkAdd || op->id() == TokenID::tkSub )
	{
	    DataDef *lptr = effective_pointer_type_for_member_access(pgm, op->left);
	    if ( lptr )
		return lptr;
	    return effective_pointer_type_for_member_access(pgm, op->right);
	}
    }

    return NULL;
}

static uint32_t member_proxy_flags(uint32_t owner_flags)
{
    return owner_flags & ~vfFIXEDARRAY;
}

static DataDef *deref_type_for_variable(Variable *var)
{
    if ( !var || !var->type )
	return NULL;

    if ( var->is_fixed_array() )
	return var->type;

    if ( var->type->is_pointer() )
    {
	DataDefPTR *dptr = dynamic_cast<DataDefPTR *>(var->type);
	return (dptr && dptr->base_type) ? dptr->base_type : &ddINT64;
    }

    return NULL;
}

static TokenBase *skip_expression_whitespace(Program &pgm)
{
    while ( pgm.peekToken()
	&& (pgm.peekToken()->type() == TokenType::ttSpace
	  || pgm.peekToken()->type() == TokenType::ttTab
	  || pgm.peekToken()->type() == TokenType::ttEOL) )
	pgm.nextToken();
    return pgm.peekToken();
}

static bool is_realpart_identifier(const std::string &name)
{
    return name == "__real" || name == "__real__";
}

static bool is_imagpart_identifier(const std::string &name)
{
    return name == "__imag" || name == "__imag__";
}

static TokenBase *make_complex_component_token(TokenBase *expr, bool imag_part)
{
    return new TokenComplexPart(expr, imag_part);
}

static std::string make_nested_function_name(TokenCpnd *scope,
					     const std::string &local_name)
{
    static size_t nested_counter = 0;
    std::string owner = "nested";
    if ( scope && scope->method )
	owner = scope->method->returns.name;
    return owner + "__" + local_name + "__" + std::to_string(++nested_counter);
}

static std::string namespace_function_symbol(const std::string &ns_name,
					     const std::string &member_name)
{
    std::string sym = "__ns_";
    for ( size_t i = 0; i < ns_name.size(); ++i )
    {
	char c = ns_name[i];
	sym += (isalnum((unsigned char)c) || c == '_') ? c : '_';
    }
    sym += "_";
    for ( size_t i = 0; i < member_name.size(); ++i )
    {
	char c = member_name[i];
	sym += (isalnum((unsigned char)c) || c == '_') ? c : '_';
    }
    return sym;
}

static DataDef *unwrap_subscript_element_type(DataDef *base_type)
{
    if ( !base_type )
	return &ddINT64;
    if ( DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(base_type) )
	return pdd->base_type ? pdd->base_type : &ddINT64;
    if ( DataDefCArray *add = dynamic_cast<DataDefCArray *>(base_type) )
	return add->element_type ? add->element_type : &ddINT64;
    return base_type;
}

static DataDef *build_fixed_array_query_type(DataDef *base_type,
					     const std::vector<uint32_t> &dims,
					     size_t consumed_dims)
{
    if ( !base_type )
	return &ddINT64;
    if ( consumed_dims >= dims.size() )
	return base_type;

    DataDef *result = base_type;
    for ( size_t i = dims.size(); i-- > consumed_dims; )
	result = new DataDefCArray(*result, result->name, dims[i], NULL);
    return result;
}

static DataDef *type_query_chain_datadef(TokenBase *chain)
{
    if ( !chain )
	return NULL;
    if ( TokenSubscript *ts = dynamic_cast<TokenSubscript *>(chain) )
    {
	if ( ts->object.is_fixed_array() )
	    return build_fixed_array_query_type(ts->object.type, ts->object.dims,
						1 + ts->extra_indices.size());
    }
    if ( TokenSubscriptExpr *tse = dynamic_cast<TokenSubscriptExpr *>(chain) )
    {
	if ( TokenVar *tv = dynamic_cast<TokenVar *>(tse->base_expr) )
	    if ( tv->var.is_fixed_array() )
		return build_fixed_array_query_type(tv->var.type, tv->var.dims, 1);
    }
    return chain->datadef();
}

static DataDef *resolve_named_datadef(Program &pgm, const std::string &name)
{
    datadef_map_iter dmi = pgm.struct_map.find(name);
    if ( dmi != pgm.struct_map.end() )
	return dmi->second;

    datatype_map_iter bmi = pgm.datatype_map.find(name);
    if ( bmi != pgm.datatype_map.end() )
	return &bmi->second->definition;

    return pgm.lazy_resolve_type(name);
}

static TokenDataType *resolve_namespaced_type_token(Program &pgm, TokenBase *tb, bool consume_tokens)
{
    if ( !tb || tb->type() != TokenType::ttIdentifier || !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkNS )
	return NULL;

    std::string ns_name = ((TokenIdent *)tb)->str;
    namespace_datatype_map_t::iterator nti = pgm.namespace_datatype_map.find(ns_name);
    if ( nti == pgm.namespace_datatype_map.end() )
	return NULL;

    if ( pgm.tokens.size() < 2 )
	return NULL;
    TokenBase *member_tb = pgm.tokens[1];
    if ( !member_tb || member_tb->type() != TokenType::ttIdentifier )
	return NULL;

    std::string member_name = ((TokenIdent *)member_tb)->str;
    datatype_map_iter dti = nti->second.find(member_name);
    if ( dti == nti->second.end() )
	return NULL;

    if ( consume_tokens )
    {
	pgm.nextToken(); // consume ::
	pgm.nextToken(); // consume member
    }
    return dti->second;
}

// Instantiate a captured template on use: `Name<ConcreteType>`. Clones the
// template's body tokens, substitutes the type parameter(s) with the concrete
// type token(s) and renames the class to a per-instantiation mangled name, then
// injects the synthesized `class Mangled { ... };` to the front of the parse
// deque and re-parses it via the existing class parser (which registers a normal
// DataDefCLASS). Returns a use-site TokenDataType for the instantiated class, or
// NULL if `tname` is not a template / the syntax doesn't match (caller falls
// through). Assumes `tb` (the template name) is already current; the next token
// must be '<'. See docs/plans/2026-05-30-template-instantiation.md.
static TokenDataType *resolve_declared_type_token(Program &pgm, TokenBase *tb,
						  bool consume_ns_tokens,
						  bool allow_lazy_types);
static TokenDataType *use_site_type_token(TokenDataType *proto, TokenBase *at);

// Match `<`...`>` as a balanced delimiter pair, like `(`...`)` and `{`...`}`:
// each `<` opens one template level and is closed by exactly ONE `>`. The lexer,
// having no template context, emits `>>` as a single TokenBSR (right-shift), so a
// `>>` at a nested template-id close supplies the `>` for THIS level and leaves a
// `>` for the enclosing one (the C++11 rule). `tok` is the already-consumed
// candidate close token; on a `>`-class token this consumes the close (pushing
// back the leftover `>` for `>>`) and returns true, else returns false and
// consumes nothing extra. Applied at every template-angle close site.
// (The open side never needs splitting — `<<` cannot appear as two template
// opens in a well-formed template-id, since each level is introduced by a name.)
static bool consume_template_close(Program &pgm, TokenBase *tok)
{
    if ( !tok )
	return false;
    if ( tok->id() == TokenID::tkGT )
	return true;                          // single '>'
    if ( tok->id() == TokenID::tkBSR )    // '>>' = this level's '>' + the outer one
    {
	pgm.pushToken(new TokenGT());
	return true;
    }
    return false;
}

static TokenDataType *instantiate_template_use(Program &pgm, const std::string &tname,
					       TokenBase *tb)
{
    auto tmi = pgm.template_map.find(tname);
    if ( tmi == pgm.template_map.end() )
	return NULL;
    if ( !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkLT )
	return NULL;
    Program::TemplateDef &td = tmi->second;

    // Parse `< Arg [, Arg ...] >` — resolve each argument to a concrete type.
    pgm.nextToken(); // consume '<'
    std::vector<TokenDataType *> args;
    std::string mangled = tname;
    for (;;)
    {
	TokenBase *at = pgm.nextToken();
	TokenDataType *adt = resolve_declared_type_token(pgm, at, true, true);
	if ( !adt )
	    pgm.Throw(at) << "Expecting a type argument to " << tname << "<>" << flush;
	// Fold trailing pointer stars into the argument's type, mirroring a
	// pointer typedef (`Box<char*>`, `Vec<T**>`). A single pointer-typed
	// TokenDataType substitutes cleanly into the body — the same shape the
	// parser produces for `typedef char *charptr; charptr m;`.
	while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkMul )
	{
	    pgm.nextToken();
	    DataDefPTR *ptr = pgm.getPointerType(&adt->definition);
	    TokenDataType *padt = new TokenDataType(ptr->name.c_str(), *ptr);
	    padt->file = at->file; padt->line = at->line; padt->column = at->column;
	    adt = padt;
	}
	args.push_back(adt);
	// Sanitize the type-name fragment into a valid C identifier piece — a
	// pointer type's name carries '*' (e.g. "char*"), which is illegal in
	// the generated struct tag / method symbols.
	mangled += "_";
	for ( char c : adt->definition.name )
	    mangled += (c == '*') ? 'p' : c;
	TokenBase *sep = pgm.nextToken();
	if ( sep->id() == TokenID::tkComma ) continue;
	if ( consume_template_close(pgm, sep) ) break;
	pgm.Throw(sep) << "Expecting ',' or '>' in " << tname << "<...>" << flush;
    }
    if ( args.size() != td.typeparams.size() )
	pgm.Throw(tb) << tname << "<> expects " << td.typeparams.size()
		      << " type argument(s), got " << args.size() << flush;

    // Already instantiated? Return a use-site clone of the cached type.
    datatype_map_iter have = pgm.datatype_map.find(mangled);
    if ( have != pgm.datatype_map.end() )
	return use_site_type_token((TokenDataType *)have->second, tb);

    // Map each type-parameter name -> its concrete type token.
    std::map<std::string, TokenDataType *> subst;
    for ( size_t i = 0; i < td.typeparams.size(); ++i )
	subst[td.typeparams[i]] = args[i];

    // Build the substituted, renamed class-definition token sequence:
    // clone each body token; rename the class tag (TokenIdent == class_name) to
    // `mangled`; replace each type-parameter ident with its concrete type token.
    std::vector<TokenBase *> inj;
    for ( TokenBase *bt : td.body )
    {
	if ( bt->type() == TokenType::ttIdentifier )
	{
	    const std::string &s = ((TokenIdent *)bt)->str;
	    std::map<std::string, TokenDataType *>::iterator si = subst.find(s);
	    if ( si != subst.end() ) { inj.push_back(si->second->clone()); continue; }
	    if ( s == td.class_name )
	    {
		TokenIdent *ni = (TokenIdent *)bt->clone();
		ni->str = mangled;
		inj.push_back(ni);
		continue;
	    }
	}
	inj.push_back(bt->clone());
    }
    // Terminate the class definition so the class parser stops cleanly without
    // consuming the caller's following tokens (e.g. the declared variable name).
    TokenSemi *semi = new TokenSemi();
    inj.push_back(semi);

    DBG(std::cout << "instantiate_template_use(): injecting " << inj.size()
	<< " tokens for " << mangled << std::endl);

    // Inject to the FRONT of the parse deque (push_front in reverse so they
    // dequeue in order), then re-parse the class definition via its keyword token.
    for ( std::vector<TokenBase *>::reverse_iterator it = inj.rbegin(); it != inj.rend(); ++it )
	pgm.pushToken(*it);
    // Re-parse the injected class definition at TOP-LEVEL scope. Instantiation
    // is triggered mid-statement (e.g. while parsing `Box<int> b;` inside a
    // function), so the enclosing function's compound scope is active — without
    // this the class's methods would be registered as nested/local functions
    // (id `caller__Class__method__N`), breaking call-site resolution. Save and
    // clear the function context, instantiate, then restore.
    std::stack<TokenCpnd *> saved_compounds;
    std::swap(pgm.compounds, saved_compounds);
    std::string saved_func = pgm.cur_func_name;
    pgm.cur_func_name.clear();

    // Canonical C++ spelling for Itanium mangling, built from the defining
    // namespace + template name + each arg's canonical spelling (the args were
    // instantiated first during the loop above, so they already carry theirs).
    // Stashed around the re-parse; TokenCLASS::parse records it on the DataDefCLASS
    // so a bodyless std:: method binds to the real libstdc++ symbol — no literal.
    std::string canon = td.defining_namespace.empty() ? std::string()
		      : (td.defining_namespace + "::");
    canon += tname + "<";
    for ( size_t i = 0; i < args.size(); ++i )
    {
	if ( i ) canon += ",";
	const std::string &cs = args[i]->definition.canonical_cpp_spelling;
	canon += cs.empty() ? args[i]->definition.name : cs;
    }
    canon += ">";
    std::string saved_canon = pgm.instantiating_canonical_spelling;
    pgm.instantiating_canonical_spelling = canon;

    TokenBase *class_kw = pgm.nextToken();   // the injected `class` keyword
    pgm.parseKeyword((TokenKeyword *)class_kw); // TokenCLASS::parse → registers DataDefCLASS

    std::swap(pgm.compounds, saved_compounds);
    pgm.cur_func_name = saved_func;
    pgm.instantiating_canonical_spelling = saved_canon;

    datatype_map_iter now = pgm.datatype_map.find(mangled);
    if ( now == pgm.datatype_map.end() )
	pgm.Throw(tb) << "internal: template instantiation " << mangled << " did not register" << flush;
    DBG(std::cout << "instantiate_template_use(): instantiated " << mangled << std::endl);
    return use_site_type_token((TokenDataType *)now->second, tb);
}

static TokenDataType *resolve_declared_type_token(Program &pgm, TokenBase *tb,
						  bool consume_ns_tokens,
						  bool allow_lazy_types)
{
    if ( !tb )
	return NULL;
    if ( tb->type() == TokenType::ttDataType )
	return (TokenDataType *)tb;
    if ( !is_contextual_identifier_token(tb) )
	return NULL;

    std::string tname = contextual_identifier_name(tb);
    datatype_map_iter dmi = pgm.datatype_map.find(tname);
    if ( dmi != pgm.datatype_map.end() )
    {
	// dmi->second is the shared prototype token created at the typedef's
	// definition, carrying the definition-site position. Return a clone
	// stamped with this use-site token's position, so a typedef'd-type
	// usage (and any diagnostic or CIR node derived from it) maps to the
	// use site rather than the typedef definition.
	TokenDataType *use = (TokenDataType *)dmi->second->clone();
	use->file   = tb->file;
	use->line   = tb->line;
	use->column = tb->column;
	return use;
    }

    // `Name<ConcreteType>` where Name is a captured template: instantiate (or
    // reuse) the concrete class and return its type. Guarded on template_map +
    // a following '<', so non-template identifiers are unaffected.
    if ( TokenDataType *inst = instantiate_template_use(pgm, tname, tb) )
	return inst;

    // `Q1::Q2::...::Name<Args>` where Name is a captured template. Templates live
    // in template_map by BARE name (namespace_datatype_map holds only concrete
    // types), so skip the leading `ident ::` qualifier chain — including NESTED
    // namespaces such as std::__cxx11 — to the final `Name <`, then instantiate by
    // bare name. The template's defining_namespace (captured at template-parse
    // time) supplies the correct Itanium canonical spelling regardless of how the
    // use site qualified it, so e.g. std::__cxx11::basic_string<char,...> mangles
    // with the __cxx11 component. (A single qualifier — `std::char_traits<char>`,
    // or the relative `__cxx11::basic_string<...>` in a typedef — is the j==1 case.)
    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkNS )
    {
	// pgm.tokens[0] is the '::' after tb; a qualifier ident sits at index 1,
	// each followed by '::', until the template name (an ident) is followed
	// by '<'.
	size_t j = 1;
	while ( j + 1 < pgm.tokens.size()
	     && pgm.tokens[j] && is_contextual_identifier_token(pgm.tokens[j]) )
	{
	    if ( pgm.tokens[j+1]->id() == TokenID::tkLT
	      && pgm.template_map.count(contextual_identifier_name(pgm.tokens[j])) )
	    {
		std::string member_name = contextual_identifier_name(pgm.tokens[j]);
		for ( size_t k = 0; k < j; k++ )
		    pgm.nextToken();               // consume '::' + each (qualifier '::')
		TokenBase *name_tok = pgm.nextToken(); // consume the template name
		if ( TokenDataType *inst = instantiate_template_use(pgm, member_name, name_tok) )
		    return inst;
		break;
	    }
	    if ( pgm.tokens[j+1]->id() == TokenID::tkNS )
		j += 2;                            // skip a namespace qualifier
	    else
		break;
	}
    }

    if ( TokenDataType *ns_type = resolve_namespaced_type_token(pgm, tb, consume_ns_tokens) )
	return ns_type;

    if ( !allow_lazy_types || pgm.findVariable(tname) )
	return NULL;

    DataDef *dd = pgm.lazy_resolve_type(tname);
    if ( !dd )
	return NULL;
    return new TokenDataType(dd->name.c_str(), *dd);
}

// Functional construction `T(args)` / `Template<...>(args)` in expression position:
// build a TokenObjTemp constructing an anonymous temporary of class T. Recognized
// ONLY when the identifier names a CLASS type (a captured template-id we can
// instantiate, or a known class) IMMEDIATELY followed by '(' -- so ordinary
// function/variable references (resolved earlier in parseExpression) are untouched.
// Returns NULL when it is not a functional construction. General C++ feature; no
// per-class machinery (works for any class, incl. header-defined std:: classes).
static TokenObjTemp *try_parse_functional_ctor(Program &pgm, TokenBase *name_tb)
{
	std::string name = contextual_identifier_name(name_tb);
	if ( name.empty() )
		return NULL;
	DataDefCLASS *cdd = NULL;
	if ( pgm.template_map.count(name)
	  && pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkLT )
	{
		// Template-id: instantiate (consumes `<...>`), then require '('.
		if ( TokenDataType *inst = instantiate_template_use(pgm, name, name_tb) )
			cdd = dynamic_cast<DataDefCLASS *>(&inst->definition);
	}
	else
	{
		datatype_map_iter dmi = pgm.datatype_map.find(name);
		if ( dmi != pgm.datatype_map.end() )
			cdd = dynamic_cast<DataDefCLASS *>(&dmi->second->definition);
		if ( !cdd )
		{
			datadef_map_iter sdmi = pgm.struct_map.find(name);
			if ( sdmi != pgm.struct_map.end() )
				cdd = dynamic_cast<DataDefCLASS *>(sdmi->second);
		}
	}
	if ( !cdd )
		return NULL;
	if ( !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkOpBrk )
		return NULL;   // a bare type-id, not a construction
	pgm.nextToken(); // consume '('
	TokenObjTemp *ot = new TokenObjTemp(cdd);
	ot->file = name_tb->file; ot->line = name_tb->line; ot->column = name_tb->column;
	while ( pgm.peekToken() && pgm.peekToken()->id() != TokenID::tkClBrk )
	{
		ot->ctor_args.push_back(pgm.parseExpression(pgm.nextToken(), true));
		if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkComma )
			pgm.nextToken();
	}
	if ( !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkClBrk )
		pgm.Throw(name_tb) << "Expected ')' after constructor arguments" << flush;
	pgm.nextToken(); // consume ')'
	return ot;
}

// A type-name usage must carry its own source position, not the position of
// the shared prototype TokenDataType held in datatype_map (which is stamped at
// the typedef's definition site). Return a clone of `proto` positioned at the
// use-site token `at`, so declarations and diagnostics map to the usage.
static TokenDataType *use_site_type_token(TokenDataType *proto, TokenBase *at)
{
    if ( !proto || !at )
	return proto;
    TokenDataType *t = (TokenDataType *)proto->clone();
    t->file = at->file;
    t->line = at->line;
    t->column = at->column;
    return t;
}

static size_t query_datadef_measure(const DataDef *dd, bool want_alignof)
{
    if ( !dd )
	return 0;
    return want_alignof ? dd->alignment() : dd->size;
}

static TokenDataType *parse_typeof_datatype(Program &pgm, TokenBase *op_tb);
static size_t evaluate_type_query(Program &pgm, TokenBase *op_tb, const std::string &op_name);
static TokenBase *try_parse_dynamic_type_query(Program &pgm, TokenBase *op_tb,
					       const std::string &op_name);
static TokenBase *parse_static_assert_statement(Program &pgm, TokenBase *tb);
static TokenDataType *resolve_namespaced_type_token(Program &pgm, TokenBase *tb, bool consume_tokens);

static std::string canonical_builtin_simple_type_name(DataDef *dd)
{
    if ( !dd )
	return "";
    if ( dd == &ddDOUBLE )
	return "double";
    if ( DataDefENUM *enum_dd = dynamic_cast<DataDefENUM *>(dd) )
	return "enum:" + enum_dd->enum_name;
    if ( DataDefCOMPLEX *complex_dd = dynamic_cast<DataDefCOMPLEX *>(dd) )
    {
	std::string elem = canonical_builtin_simple_type_name(complex_dd->element_type);
	return elem.empty() ? "" : "complex(" + elem + ")";
    }
    if ( DataDefPTR *ptr_dd = dynamic_cast<DataDefPTR *>(dd) )
    {
	std::string base = canonical_builtin_simple_type_name(ptr_dd->base_type);
	return base.empty() ? "" : "ptr(" + base + ")";
    }
    if ( DataDefCArray *array_dd = dynamic_cast<DataDefCArray *>(dd) )
    {
	std::string elem = canonical_builtin_simple_type_name(array_dd->element_type);
	return elem.empty() ? "" : "array(" + elem + ")";
    }
    if ( dd->is_struct() )
	return "struct:" + dd->name;

    switch ( dd->rawtype() )
    {
	case DataType::dtVOID: return "void";
	case DataType::dtBOOL: return "bool";
	case DataType::dtUINT8: return "unsigned char";
	case DataType::dtINT8: return "char";
	case DataType::dtUINT16: return "unsigned short";
	case DataType::dtINT16: return "short";
	case DataType::dtUINT24: return "unsigned int24";
	case DataType::dtINT24: return "int24";
	case DataType::dtUINT32: return "unsigned int";
	case DataType::dtINT32: return "int";
	case DataType::dtUINT64: return "unsigned long";
	case DataType::dtINT64: return "long";
	case DataType::dtFLOAT: return "float";
	case DataType::dtDOUBLE: return "double";
	case DataType::dtLDOUBLE: return "long double";
	default: break;
    }
    return dd->name;
}

static void strip_top_level_type_qualifiers(std::string &sig)
{
    static const char *prefixes[] = {"const ", "restrict "};
    bool changed = true;
    while ( changed )
    {
	changed = false;
	for ( size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i )
	{
	    const char *prefix = prefixes[i];
	    size_t len = strlen(prefix);
	    if ( sig.compare(0, len, prefix) == 0 )
	    {
		sig.erase(0, len);
		changed = true;
	    }
	}
    }
}

static bool parse_builtin_types_compatible_operand(Program &pgm, TokenBase *type_tb,
						   std::string &sig)
{
    sig.clear();
    if ( !type_tb )
	return false;

    std::string leading_qualifiers;
    while ( is_type_qualifier_token(type_tb) )
    {
	if ( !leading_qualifiers.empty() )
	    leading_qualifiers += ' ';
	leading_qualifiers += ((TokenKeyword *)type_tb)->str;
	type_tb = pgm.nextToken();
	if ( !type_tb )
	    return false;
    }

    if ( type_tb->type() == TokenType::ttDataType )
    {
	TokenDataType *tdt = (TokenDataType *)type_tb;
	if ( tdt->str == "long double" )
	    sig = "long double";
	else
	    sig = canonical_builtin_simple_type_name(&tdt->definition);
    }
    else if ( type_tb->type() == TokenType::ttIdentifier )
    {
	std::string tname = ((TokenIdent *)type_tb)->str;
	if ( is_typeof_identifier(tname) )
	{
	    if ( !skip_expression_whitespace(pgm) || pgm.peekToken()->id() != TokenID::tkOpBrk )
		return false;
	    pgm.nextToken();
	    TokenBase *inner_tb = pgm.nextToken();
	    if ( !inner_tb )
		return false;
	    DataDef *inner_dd = NULL;
	    if ( inner_tb->type() == TokenType::ttIdentifier )
	    {
		std::string inner_name = ((TokenIdent *)inner_tb)->str;
		Variable *inner_var = pgm.findVariable(inner_name);
		if ( inner_var )
		    inner_dd = inner_var->type;
		else
		    inner_dd = resolve_named_datadef(pgm, inner_name);
	    }
	    else if ( inner_tb->type() == TokenType::ttDataType )
		inner_dd = &((TokenDataType *)inner_tb)->definition;
	    else if ( inner_tb->type() == TokenType::ttKeyword
		   && (inner_tb->id() == TokenID::tkSTRUCT || inner_tb->id() == TokenID::tkUNION) )
	    {
		TokenBase *tag_tb = pgm.nextToken();
		if ( !tag_tb || !is_contextual_identifier_token(tag_tb) )
		    return false;
		inner_dd = resolve_named_datadef(pgm, contextual_identifier_name(tag_tb));
	    }
	    if ( !inner_dd )
	    {
		TokenBase *expr = pgm.parseExpression(inner_tb, true, false, true, 1);
		inner_dd = expr ? expr->datadef() : NULL;
	    }
	    if ( !inner_dd )
		return false;
	    if ( skip_expression_whitespace(pgm) && pgm.peekToken()->id() == TokenID::tkClBrk )
		pgm.nextToken();
	    else if ( (!pgm.curToken() || pgm.curToken()->id() != TokenID::tkClBrk)
	       && (!pgm.prevToken() || pgm.prevToken()->id() != TokenID::tkClBrk) )
		return false;
	    sig = canonical_builtin_simple_type_name(inner_dd);
	}
	else
	{
	    DataDef *dd = resolve_named_datadef(pgm, tname);
	    sig = canonical_builtin_simple_type_name(dd);
	}
    }
    else if ( type_tb->type() == TokenType::ttKeyword
	   && (type_tb->id() == TokenID::tkSTRUCT || type_tb->id() == TokenID::tkUNION) )
    {
	TokenBase *tag_tb = pgm.nextToken();
	if ( !tag_tb || !is_contextual_identifier_token(tag_tb) )
	    return false;
	sig = (type_tb->id() == TokenID::tkSTRUCT ? "struct:" : "union:")
	    + contextual_identifier_name(tag_tb);
    }
    else if ( type_tb->type() == TokenType::ttKeyword && type_tb->id() == TokenID::tkENUM )
    {
	TokenBase *tag_tb = pgm.nextToken();
	if ( !tag_tb || !is_contextual_identifier_token(tag_tb) )
	    return false;
	sig = "enum:" + contextual_identifier_name(tag_tb);
    }
    else
	return false;

    if ( sig.empty() )
	return false;
    if ( !leading_qualifiers.empty() )
	sig = leading_qualifiers + " " + sig;

    bool wrapped = false;
    while ( pgm.peekToken() )
    {
	skip_expression_whitespace(pgm);
	if ( !pgm.peekToken() )
	    break;
	if ( pgm.peekToken()->id() == TokenID::tkMul )
	{
	    pgm.nextToken();
	    std::string ptr_qualifiers;
	    skip_expression_whitespace(pgm);
	    while ( pgm.peekToken() && is_type_qualifier_token(pgm.peekToken()) )
	    {
		if ( !ptr_qualifiers.empty() )
		    ptr_qualifiers += ' ';
		ptr_qualifiers += ((TokenKeyword *)pgm.nextToken())->str;
	    }
	    sig = ptr_qualifiers.empty()
		? "ptr(" + sig + ")"
		: ptr_qualifiers + " ptr(" + sig + ")";
	    wrapped = true;
	    continue;
	}
	if ( pgm.peekToken()->id() == TokenID::tkOpSqr )
	{
	    pgm.nextToken();
	    int depth = 1;
	    while ( depth > 0 )
	    {
		TokenBase *dim_tb = pgm.nextToken();
		if ( !dim_tb )
		    return false;
		if ( dim_tb->id() == TokenID::tkOpSqr )
		    ++depth;
		else if ( dim_tb->id() == TokenID::tkClSqr )
		    --depth;
	    }
	    sig = "array(" + sig + ")";
	    wrapped = true;
	    continue;
	}
	break;
    }

    if ( !wrapped )
	strip_top_level_type_qualifiers(sig);
    return true;
}

static Variable *resolve_c_identifier(Program &pgm, TokenIdent *ident_tb, bool expression_head)
{
	Variable *var = NULL;

	if ( expression_head && !pgm.current_namespace.empty() )
	{
	    namespace_map_t::iterator nsi = pgm.namespace_map.find(pgm.current_namespace);
	    if ( nsi != pgm.namespace_map.end() )
	    {
		variable_map_iter vmi = nsi->second.find(ident_tb->str);
		if ( vmi != nsi->second.end() )
		    var = vmi->second;
	    }
	}
	if ( !var )
	    var = pgm.findVariable(ident_tb->str);
	if ( !var && !expression_head && !pgm.current_namespace.empty() )
	{
	    namespace_map_t::iterator nsi = pgm.namespace_map.find(pgm.current_namespace);
	    if ( nsi != pgm.namespace_map.end() )
	    {
		variable_map_iter vmi = nsi->second.find(ident_tb->str);
		if ( vmi != nsi->second.end() )
		    var = vmi->second;
	    }
	}
	return var;
}

static bool read_constant_integer(Variable *var, int64_t &out)
{
    if ( !var || !var->is_constant() || !var->data || !var->type || !var->type->is_integer() )
	return false;
    switch ( var->type->rawtype() )
    {
	case DataType::dtINT8:   out = *((int8_t *)var->data);   return true;
	case DataType::dtUINT8:  out = *((uint8_t *)var->data);  return true;
	case DataType::dtINT16:  out = *((int16_t *)var->data);  return true;
	case DataType::dtUINT16: out = *((uint16_t *)var->data); return true;
	case DataType::dtINT24:  out = *((int16_t *)var->data);  return true;
	case DataType::dtUINT24: out = *((uint16_t *)var->data); return true;
	case DataType::dtINT32:  out = *((int32_t *)var->data);  return true;
	case DataType::dtUINT32: out = *((uint32_t *)var->data); return true;
	case DataType::dtINT64:  out = *((int64_t *)var->data);  return true;
	case DataType::dtUINT64: out = *((uint64_t *)var->data); return true;
	default:
	    return false;
    }
}

static void copy_token_location(TokenBase *dst, TokenBase *src)
{
    if ( !dst || !src )
	return;
    dst->file = src->file;
    dst->line = src->line;
    dst->column = src->column;
}

static TokenBase *make_expression_context_literal(Program &pgm,
						  const madc::value &resolved,
						  TokenBase *src)
{
    TokenBase *tb = NULL;

    switch ( resolved.type() )
    {
	case madc::value::kind::boolean:
	    tb = new TokenInt(resolved.as_boolean() ? 1 : 0);
	    break;
	case madc::value::kind::integer:
	    tb = new TokenInt(resolved.as_integer());
	    break;
	case madc::value::kind::real:
	    tb = new TokenReal(resolved.as_real());
	    break;
	case madc::value::kind::string:
	{
	    std::string literal = resolved.as_string();
	    Variable *literal_var = pgm.addLiteral(literal);
	    if ( !literal_var )
		return NULL;
	    tb = new TokenVar(*literal_var);
	    break;
	}
	default:
	    return NULL;
    }

    copy_token_location(tb, src);
    return tb;
}

static bool resolve_integer_constant(Program &pgm, TokenBase *tb, int64_t &out)
{
    if ( !tb )
	return false;
    if ( tb->type() == TokenType::ttInteger )
    {
	out = ((TokenInt *)tb)->get();
	return true;
    }
    if ( tb->type() == TokenType::ttChar )
    {
	out = ((TokenChar *)tb)->get();
	return true;
    }
    // Also accept contextual-identifier keywords (`class`, `try`, `catch`,
    // etc.) as integer constants when they resolve to one — this lets
    // `case class:` work for an enum that has `class` as one of its tags.
    std::string name;
    if ( tb->type() == TokenType::ttIdentifier )
	name = ((TokenIdent *)tb)->str;
    else if ( is_contextual_identifier_token(tb) )
	name = contextual_identifier_name(tb);
    else
	return false;
    // Scoped-enum constant in a constant expression: `Tag::Value` (e.g. a
    // `case Color::Green:` label). The enumerators live in the tag's
    // pseudo-namespace (see TokenENUM::parse), so resolve through it.
    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkNS )
    {
	namespace_map_t::iterator nsi = pgm.namespace_map.find(name);
	if ( nsi != pgm.namespace_map.end() )
	{
	    pgm.nextToken(); // consume '::'
	    TokenBase *member_tb = pgm.nextToken();
	    if ( !member_tb || !is_contextual_identifier_token(member_tb) )
		return false;
	    std::string member = contextual_identifier_name(member_tb);
	    variable_map_iter vmi = nsi->second.find(member);
	    if ( vmi == nsi->second.end() )
		return false;
	    return read_constant_integer(vmi->second, out);
	}
	return false;
    }
    Variable *var = pgm.findVariable(name);
    return read_constant_integer(var, out);
}

static bool is_shared_global_extern_reference(Program &pgm, TokenCpnd *code, Variable *var)
{
    return pgm.parsing_extern_decl
	&& code
	&& var
	&& var->is_global();
}

static int64_t parse_constant_integer_expression(Program &pgm);
static int64_t parse_constant_rel(Program &pgm);
static int64_t parse_constant_eq(Program &pgm);
static size_t query_fixed_array_sizeof_value(TokenVar *tv, bool want_alignof, bool deref);

static TokenBase *parse_parenthesized_expression(Program &pgm, const char *context,
						 bool stop_on_closing_paren)
{
    TokenBase *open = pgm.nextToken();
    if ( !open || open->id() != TokenID::tkOpBrk )
	pgm.Throw(open) << "expecting ( after " << context << flush;

    TokenBase *first = pgm.nextToken();
    TokenBase *expr = pgm.parseExpression(first, true, false, stop_on_closing_paren, 1);
    if ( !expr )
	pgm.Throw(first ? first : open) << "Failed to parse " << context << " expression" << flush;

    return expr;
}

static DataDef *resolve_type_query_datadef(Program &pgm, TokenBase *type_tb,
					   const std::string &op_name,
					   bool &have_value, size_t &query_value)
{
    bool want_alignof = is_alignof_identifier(op_name);
    DataDef *dd = NULL;
    Variable *var = NULL;

    if ( is_contextual_identifier_token(type_tb) )
    {
	std::string tname = contextual_identifier_name(type_tb);
	var = pgm.findVariable(tname);
	if ( var && pgm.peekToken()
	  && (pgm.peekToken()->id() == TokenID::tkOpSqr
	   || pgm.peekToken()->id() == TokenID::tkDot
	   || pgm.peekToken()->id() == TokenID::tkDeRef) )
	{
	    TokenBase *chain = pgm.parsePostfixChain(type_tb);
	    DataDef *cdd = chain ? chain->datadef() : NULL;
	    if ( !cdd )
		pgm.Throw(type_tb) << op_name << ": cannot determine type of expression" << flush;
	    query_value = query_datadef_measure(cdd, want_alignof);
	    if ( !want_alignof )
	    {
		if ( TokenMember *tm = dynamic_cast<TokenMember *>(chain) )
		{
		    if ( tm->is_fixed_array_member() )
		    {
			DataDef *otype = tm->object.type;
			if ( DataDefPTR *opt = dynamic_cast<DataDefPTR *>(otype) )
			    otype = opt->base_type;
			if ( DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(otype) )
			{
			    std::string mname = tm->var.name;
			    query_value *= sdd->m_count(mname);
			}
		    }
		}
	    }
	    have_value = true;
	    return NULL;
	}
	if ( var )
	{
	    query_value = query_datadef_measure(var->type, want_alignof);
	    if ( !want_alignof && var->is_fixed_array() )
		query_value *= var->total_elements();
	    have_value = true;
	    return NULL;
	}
    }

    if ( type_tb->type() == TokenType::ttDataType )
	dd = &((TokenDataType *)type_tb)->definition;
    else if ( type_tb->type() == TokenType::ttIdentifier )
    {
	std::string tname = ((TokenIdent *)type_tb)->str;
	var = pgm.findVariable(tname);
	if ( var && pgm.peekToken()
	  && (pgm.peekToken()->id() == TokenID::tkOpSqr
	   || pgm.peekToken()->id() == TokenID::tkDot
	   || pgm.peekToken()->id() == TokenID::tkDeRef) )
	{
	    TokenBase *chain = pgm.parsePostfixChain(type_tb);
	    DataDef *cdd = chain ? chain->datadef() : NULL;
	    if ( !cdd )
		pgm.Throw(type_tb) << op_name << ": cannot determine type of expression" << flush;
	    query_value = query_datadef_measure(cdd, want_alignof);
	    if ( !want_alignof )
	    {
		if ( TokenMember *tm = dynamic_cast<TokenMember *>(chain) )
		{
		    if ( tm->is_fixed_array_member() )
		    {
			DataDef *otype = tm->object.type;
			if ( DataDefPTR *opt = dynamic_cast<DataDefPTR *>(otype) )
			    otype = opt->base_type;
			if ( DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(otype) )
			{
			    std::string mname = tm->var.name;
			    query_value *= sdd->m_count(mname);
			}
		    }
		}
	    }
	    have_value = true;
	    var = NULL;
	}
	else if ( var )
	{
	    query_value = query_datadef_measure(var->type, want_alignof);
	    if ( !want_alignof && var->is_fixed_array() )
		query_value *= var->total_elements();
	    have_value = true;
	}
	else
	    dd = resolve_named_datadef(pgm, tname);
    }
    else if ( type_tb->type() == TokenType::ttKeyword
	   && (type_tb->id() == TokenID::tkSTRUCT || type_tb->id() == TokenID::tkUNION) )
    {
	TokenBase *tag_tb = pgm.nextToken();
	if ( tag_tb && tag_tb->type() == TokenType::ttIdentifier )
	    dd = resolve_named_datadef(pgm, ((TokenIdent *)tag_tb)->str);
	if ( !dd )
	    pgm.Throw(tag_tb) << "Unknown struct/union type in " << op_name << flush;
    }
    else if ( type_tb->type() == TokenType::ttKeyword && type_tb->id() == TokenID::tkENUM )
    {
	// sizeof(enum X) — enums are int-sized
	if ( pgm.peekToken() && is_contextual_identifier_token(pgm.peekToken()) )
	    pgm.nextToken(); // consume tag
	query_value = sizeof(int);
	have_value = true;
    }
    else if ( type_tb->type() == TokenType::ttKeyword && type_tb->id() == TokenID::tkCONST )
    {
	// sizeof(const type) — skip const qualifier
	TokenBase *inner = pgm.nextToken();
	return resolve_type_query_datadef(pgm, inner, op_name, have_value, query_value);
    }
    else if ( type_tb->id() == TokenID::tkMul )
    {
	TokenBase *deref_tb = pgm.nextToken();
	if ( !deref_tb || !is_contextual_identifier_token(deref_tb) )
	    pgm.Throw(type_tb) << "Expecting identifier after '*' in " << op_name << flush;
	DataDef *deref_base = NULL;
	if ( pgm.peekToken()
	  && (pgm.peekToken()->id() == TokenID::tkDot
	   || pgm.peekToken()->id() == TokenID::tkDeRef
	   || pgm.peekToken()->id() == TokenID::tkOpSqr) )
	{
	    TokenBase *chain = pgm.parsePostfixChain(deref_tb);
	    DataDef *cdd = chain ? chain->datadef() : NULL;
	    if ( !cdd )
		pgm.Throw(deref_tb) << op_name << "(*expr): cannot determine type" << flush;
	    if ( cdd->is_pointer() )
	    {
		DataDefPTR *cdp = dynamic_cast<DataDefPTR *>(cdd);
		deref_base = (cdp && cdp->base_type) ? cdp->base_type : &ddINT64;
	    }
	    else
		deref_base = cdd;
	}
	else
	{
	    std::string dname = contextual_identifier_name(deref_tb);
	    Variable *dvar = pgm.findVariable(dname);
	    if ( !dvar )
		pgm.Throw(deref_tb) << "undeclared identifier '" << dname << "' in " << op_name << "(*...)" << flush;
	    if ( dvar->is_fixed_array() )
		deref_base = dvar->type;
	    else if ( dvar->type->is_pointer() )
	    {
		DataDefPTR *dptr = dynamic_cast<DataDefPTR *>(dvar->type);
		deref_base = (dptr && dptr->base_type) ? dptr->base_type : &ddINT64;
	    }
	    else
		pgm.Throw(deref_tb) << op_name << "(*" << dname << "): not a pointer or array" << flush;
	}
	query_value = query_datadef_measure(deref_base, want_alignof);
	have_value = true;
    }

    return dd;
}

static size_t evaluate_type_query(Program &pgm, TokenBase *op_tb, const std::string &op_name)
{
    bool want_alignof = is_alignof_identifier(op_name);

    if ( pgm.peekToken() && pgm.peekToken()->id() != TokenID::tkOpBrk )
    {
	TokenBase *probe = pgm.peekToken();
	bool deref = (probe->id() == TokenID::tkMul);
	if ( deref )
	{
	    pgm.nextToken();
	    probe = pgm.peekToken();
	}
	// sizeof "literal" — string literal without parens
	if ( probe && probe->type() == TokenType::ttString )
	{
	    TokenStr *ts = static_cast<TokenStr *>(pgm.nextToken());
	    return ts->str.size() + 1; // include NUL terminator
	}
	if ( !probe || !is_contextual_identifier_token(probe) )
	    pgm.Throw(op_tb) << "Expecting '(' or identifier after " << op_name << flush;
	TokenBase *id_tb = pgm.nextToken();
	TokenBase *chain = pgm.parsePostfixChain(id_tb);
	DataDef *cdd = type_query_chain_datadef(chain);
	if ( !cdd )
	    pgm.Throw(id_tb) << op_name << ": cannot determine type of expression" << flush;
	size_t value = query_datadef_measure(cdd, want_alignof);
	if ( deref && cdd->is_pointer() )
	{
	    DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(cdd);
	    if ( pdd && pdd->base_type )
		value = query_datadef_measure(pdd->base_type, want_alignof);
	}
	else if ( TokenVar *tv = dynamic_cast<TokenVar *>(chain) )
	{
	    if ( tv->var.is_fixed_array() )
		value = query_fixed_array_sizeof_value(tv, want_alignof, deref);
	}
	return value;
    }

    if ( !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkOpBrk )
	pgm.Throw(op_tb) << "Expecting '(' after " << op_name << flush;
    pgm.nextToken();
    TokenBase *type_tb = pgm.nextToken();
    DataDef *dd = NULL;
    size_t value = 0;
    bool have_value = false;

    dd = resolve_type_query_datadef(pgm, type_tb, op_name, have_value, value);
    // Fallback: sizeof(expression) — parse as expression, use its type
    bool expr_fallback_consumed_paren = false;
    if ( !have_value && !dd )
    {
	// sizeof("literal") — C string literals are char arrays, not
	// std::string objects. Check type_tb before parseExpression
	// transforms it.
	if ( type_tb && type_tb->type() == TokenType::ttString )
	{
	    TokenStr *ts = static_cast<TokenStr *>(type_tb);
	    value = ts->str.size() + 1; // include NUL terminator
	    have_value = true;
	    dd = NULL;
	}
	else
	{
	    TokenBase *expr = pgm.parseExpression(type_tb, true, false, true, 1);
	    if ( expr && expr->datadef() )
	    {
		dd = expr->datadef();
		// For fixed arrays accessed as expressions, use element size
		if ( TokenVar *tv = dynamic_cast<TokenVar *>(expr) )
		    if ( tv->var.is_fixed_array() )
			value = query_fixed_array_sizeof_value(tv, want_alignof, false);
		if ( !value )
		    value = query_datadef_measure(dd, want_alignof);
		have_value = true;
		dd = NULL; // have_value is set, skip the pointer/array loop below
		expr_fallback_consumed_paren = true;
	    }
	    if ( !have_value )
		pgm.Throw(type_tb) << "Unknown type in " << op_name << flush;
	}
    }

    while ( !have_value && pgm.peekToken() )
    {
	if ( pgm.peekToken()->id() == TokenID::tkMul )
	{
	    pgm.nextToken();
	    dd = pgm.getPointerType(dd);
	    continue;
	}
	if ( pgm.peekToken()->id() == TokenID::tkOpBrk )
	{
	    TokenBase *open = pgm.nextToken();
	    TokenBase *star = pgm.nextToken();
	    if ( !star || star->id() != TokenID::tkMul )
		pgm.Throw(star ? star : open) << "Unsupported parenthesized declarator in " << op_name << flush;
	    dd = pgm.getPointerType(dd);
	    TokenBase *close = pgm.nextToken();
	    if ( !close || close->id() != TokenID::tkClBrk )
		pgm.Throw(close ? close : open) << "Expecting ')' in " << op_name << " function pointer type" << flush;
	    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrk )
	    {
		pgm.nextToken();
		int depth = 1;
		while ( depth > 0 )
		{
		    TokenBase *pt = pgm.nextToken();
		    if ( !pt )
			pgm.Throw(open) << "Unexpected end of input in " << op_name << " function pointer parameter list" << flush;
		    if ( pt->id() == TokenID::tkOpBrk )
			++depth;
		    else if ( pt->id() == TokenID::tkClBrk )
			--depth;
		}
	    }
	    continue;
	}
	break;
    }
    // The expression fallback (sizeof(expr)) already consumed the closing
    // paren via parseExpression's stop_on_closing_paren. Only consume it
    // here for the type-name / variable-name path.
    if ( !expr_fallback_consumed_paren )
    {
	if ( !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkClBrk )
	    pgm.Throw(type_tb) << "Expecting ')' after " << op_name << " type" << flush;
	pgm.nextToken();
    }

    if ( !have_value && dd )
	value = query_datadef_measure(dd, want_alignof);

    return value;
}

static bool is_runtime_sized_type(DataDef *dd)
{
    if ( !dd )
	return false;
    if ( DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(dd) )
	return sdd->has_runtime_size();
    if ( DataDefCArray *add = dynamic_cast<DataDefCArray *>(dd) )
	return add->has_runtime_size();
    return false;
}

static TokenBase *make_type_query_token(TokenBase *op_tb, DataDef *dd, bool want_alignof)
{
    TokenBase *result = NULL;
    if ( want_alignof || !is_runtime_sized_type(dd) )
    {
	TokenInt *ti = new TokenInt((int64_t)query_datadef_measure(dd, want_alignof));
	ti->setDataType(&ddUINT64);
	result = ti;
    }
    else
	result = new TokenTypeQuery(dd, want_alignof);
    result->file = op_tb ? op_tb->file : NULL;
    result->line = op_tb ? op_tb->line : 0;
    result->column = op_tb ? op_tb->column : 0;
    return result;
}

static size_t query_fixed_array_sizeof_value(TokenVar *tv, bool want_alignof, bool deref)
{
    if ( !tv || !tv->var.type )
	return 0;
    size_t value = query_datadef_measure(tv->var.type, want_alignof);
    if ( !want_alignof && !deref )
	value *= tv->var.total_elements();
    return value;
}

static bool try_eval_known_integer(TokenBase *tb, int64_t &out)
{
    if ( !tb )
	return false;
    if ( TokenInt *ti = dynamic_cast<TokenInt *>(tb) )
    {
	out = ti->ival();
	return true;
    }
    if ( TokenChar *tc = dynamic_cast<TokenChar *>(tb) )
    {
	out = tc->ival();
	return true;
    }
    if ( TokenVar *tv = dynamic_cast<TokenVar *>(tb) )
    {
	if ( tv->var.is_constant() )
	{
	    out = tv->var.get<int64_t>();
	    return true;
	}
	return false;
    }
    if ( TokenNeg *tn = dynamic_cast<TokenNeg *>(tb) )
    {
	int64_t rhs = 0;
	if ( try_eval_known_integer(tn->right, rhs) )
	{
	    out = -rhs;
	    return true;
	}
	return false;
    }
    if ( TokenAdd *ta = dynamic_cast<TokenAdd *>(tb) )
    {
	int64_t lhs = 0, rhs = 0;
	if ( try_eval_known_integer(ta->left, lhs) && try_eval_known_integer(ta->right, rhs) )
	{
	    out = lhs + rhs;
	    return true;
	}
	return false;
    }
    if ( TokenSub *ts = dynamic_cast<TokenSub *>(tb) )
    {
	int64_t lhs = 0, rhs = 0;
	if ( try_eval_known_integer(ts->left, lhs) && try_eval_known_integer(ts->right, rhs) )
	{
	    out = lhs - rhs;
	    return true;
	}
	return false;
    }
    return false;
}

static TokenBase *materialize_cast_literal_operand(Program &pgm, TokenBase *tb)
{
    if ( !tb )
	return tb;
    if ( tb->type() != TokenType::ttString )
	return tb;

    TokenStr *first = static_cast<TokenStr *>(tb);
    std::string literal = first->str;
    bool wide_literal = first->wide;
    while ( pgm.peekToken() && pgm.peekToken()->type() == TokenType::ttString )
    {
	TokenStr *next = static_cast<TokenStr *>(pgm.nextToken());
	if ( wide_literal || next->wide )
	{
	    if ( !wide_literal )
	    {
		std::string converted;
		for ( unsigned char c : literal )
		{
		    converted += (char)c;
		    converted += '\0';
		    converted += '\0';
		    converted += '\0';
		}
		literal = converted;
		wide_literal = true;
	    }
	    if ( next->wide )
		literal += next->str;
	    else
	    {
		for ( unsigned char c : next->str )
		{
		    literal += (char)c;
		    literal += '\0';
		    literal += '\0';
		    literal += '\0';
		}
	    }
	}
	else
	    literal += next->str;
    }

    Variable *var = wide_literal ? pgm.addWideLiteral(literal) : pgm.addLiteral(literal);
    TokenVar *tv = new TokenVar(*var);
    tv->file = tb->file;
    tv->line = tb->line;
    tv->column = tb->column;
    return tv;
}

static int64_t fixed_array_object_size(const Variable &var)
{
    if ( !var.is_fixed_array() || !var.type || !var.type->size )
	return -1;
    return (int64_t)var.type->size * (int64_t)var.total_elements();
}

static int64_t token_pointer_element_size(TokenBase *tb)
{
    if ( !tb )
	return 1;
    if ( TokenVar *tv = dynamic_cast<TokenVar *>(tb) )
    {
	if ( tv->var.is_fixed_array() && tv->var.type )
	    return tv->var.type->size ? (int64_t)tv->var.type->size : 1;
    }
    if ( TokenMember *tm = dynamic_cast<TokenMember *>(tb) )
    {
	if ( tm->is_fixed_array_member() && tm->var.type )
	    return tm->var.type->size ? (int64_t)tm->var.type->size : 1;
    }
    DataDef *dd = tb->datadef();
    if ( DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(dd) )
	return (pdd->base_type && pdd->base_type->size) ? (int64_t)pdd->base_type->size : 1;
    if ( DataDefCArray *add = dynamic_cast<DataDefCArray *>(dd) )
	return (add->element_type && add->element_type->size) ? (int64_t)add->element_type->size : 1;
    return (dd && dd->size) ? (int64_t)dd->size : 1;
}

static int64_t known_object_size_for_expr(TokenBase *expr, int mode);

static int64_t remaining_object_size(int64_t base_size, int64_t index, int64_t elem_size)
{
    if ( base_size < 0 || index < 0 || elem_size <= 0 )
	return -1;
    int64_t used = index * elem_size;
    if ( used >= base_size )
	return 0;
    return base_size - used;
}

static int64_t known_object_size_for_lvalue(TokenBase *expr, int mode)
{
    if ( !expr )
	return -1;
    if ( TokenVar *tv = dynamic_cast<TokenVar *>(expr) )
    {
	int64_t array_size = fixed_array_object_size(tv->var);
	if ( array_size >= 0 )
	    return array_size;
	return tv->var.type ? (int64_t)tv->var.type->size : -1;
    }
    if ( TokenMember *tm = dynamic_cast<TokenMember *>(expr) )
    {
	if ( tm->is_fixed_array_member() )
	{
	    DataDefSTRUCT *sdd = tm->owner_struct_type();
	    std::string mname = tm->var.name;
	    if ( sdd && tm->var.type )
		return (int64_t)sdd->m_count(mname) * (int64_t)tm->var.type->size;
	}
	return tm->var.type ? (int64_t)tm->var.type->size : -1;
    }
    if ( TokenSubscript *ts = dynamic_cast<TokenSubscript *>(expr) )
    {
	int64_t index = 0;
	if ( !try_eval_known_integer(ts->index, index) )
	    return -1;
	int64_t base_size = -1;
	if ( ts->object.is_fixed_array() )
	    base_size = fixed_array_object_size(ts->object);
	else
	    base_size = ts->object.object_size_hint;
	return remaining_object_size(base_size, index, ts->datadef() ? (int64_t)ts->datadef()->size : 1);
    }
    if ( TokenSubscriptExpr *tse = dynamic_cast<TokenSubscriptExpr *>(expr) )
    {
	int64_t index = 0;
	if ( !try_eval_known_integer(tse->index, index) )
	    return -1;
	int64_t base_size = known_object_size_for_expr(tse->base_expr, mode);
	return remaining_object_size(base_size, index, tse->datadef() ? (int64_t)tse->datadef()->size : 1);
    }
    return expr->datadef() ? (int64_t)expr->datadef()->size : -1;
}

static int64_t known_object_size_for_expr(TokenBase *expr, int mode)
{
    if ( !expr )
	return -1;
    if ( TokenVar *tv = dynamic_cast<TokenVar *>(expr) )
    {
	int64_t array_size = fixed_array_object_size(tv->var);
	if ( array_size >= 0 )
	    return array_size;
	return tv->var.object_size_hint;
    }
    if ( TokenMember *tm = dynamic_cast<TokenMember *>(expr) )
    {
	if ( tm->is_fixed_array_member() )
	{
	    DataDefSTRUCT *sdd = tm->owner_struct_type();
	    std::string mname = tm->var.name;
	    if ( sdd && tm->var.type )
		return (int64_t)sdd->m_count(mname) * (int64_t)tm->var.type->size;
	}
	return tm->var.object_size_hint;
    }
    if ( TokenAddrOf *tao = dynamic_cast<TokenAddrOf *>(expr) )
    {
	int64_t array_size = fixed_array_object_size(tao->var);
	if ( array_size >= 0 )
	    return array_size;
	return tao->var.type ? (int64_t)tao->var.type->size : -1;
    }
    if ( TokenAddrExpr *tae = dynamic_cast<TokenAddrExpr *>(expr) )
	return known_object_size_for_lvalue(tae->expr, mode);
    if ( TokenAdd *ta = dynamic_cast<TokenAdd *>(expr) )
    {
	int64_t offset = 0;
	if ( try_eval_known_integer(ta->right, offset) )
	{
	    int64_t base_size = known_object_size_for_expr(ta->left, mode);
	    return remaining_object_size(base_size, offset, token_pointer_element_size(ta->left));
	}
	if ( try_eval_known_integer(ta->left, offset) )
	{
	    int64_t base_size = known_object_size_for_expr(ta->right, mode);
	    return remaining_object_size(base_size, offset, token_pointer_element_size(ta->right));
	}
	return -1;
    }
    if ( TokenSub *ts = dynamic_cast<TokenSub *>(expr) )
    {
	int64_t offset = 0;
	if ( try_eval_known_integer(ts->right, offset) )
	{
	    int64_t base_size = known_object_size_for_expr(ts->left, mode);
	    return remaining_object_size(base_size, offset, token_pointer_element_size(ts->left));
	}
	return -1;
    }
    if ( TokenTerQ *tt = dynamic_cast<TokenTerQ *>(expr) )
    {
	int64_t true_size = known_object_size_for_expr(tt->true_expr, mode);
	int64_t false_size = known_object_size_for_expr(tt->false_expr, mode);
	if ( true_size < 0 || false_size < 0 )
	    return -1;
	return mode == 1 ? std::min(true_size, false_size)
			 : std::max(true_size, false_size);
    }
    if ( TokenCast *tc = dynamic_cast<TokenCast *>(expr) )
	return known_object_size_for_expr(tc->expr, mode);
    if ( TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(expr) )
    {
	if ( (tcf->var.name == "__builtin_alloca" || tcf->var.name == "alloca" || tcf->var.name == "malloc")
	  && tcf->argc() == 1 )
	{
	    int64_t size = 0;
	    if ( try_eval_known_integer(tcf->parameters[0], size) && size >= 0 )
		return size;
	}
    }
    return -1;
}

static void update_pointer_object_size_hints(TokenBase *expr)
{
    if ( !expr )
	return;
    if ( TokenAssign *ta = dynamic_cast<TokenAssign *>(expr) )
    {
	if ( TokenVar *lhs = dynamic_cast<TokenVar *>(ta->left) )
	{
	    if ( lhs->var.type && lhs->var.type->is_pointer() && !lhs->var.is_fixed_array() )
		lhs->var.object_size_hint = known_object_size_for_expr(ta->right, 0);
	}
	update_pointer_object_size_hints(ta->left);
	update_pointer_object_size_hints(ta->right);
	return;
    }
    if ( TokenComma *tc = dynamic_cast<TokenComma *>(expr) )
    {
	update_pointer_object_size_hints(tc->left);
	update_pointer_object_size_hints(tc->right);
	return;
    }
    if ( TokenTerQ *tt = dynamic_cast<TokenTerQ *>(expr) )
    {
	update_pointer_object_size_hints(tt->condition);
	update_pointer_object_size_hints(tt->true_expr);
	update_pointer_object_size_hints(tt->false_expr);
    }
}

static TokenBase *materialize_runtime_struct_size_captures(Program &pgm, TokenCpnd *code,
							    DataDefSTRUCT *dds, TokenBase *loc)
{
    if ( !code || !dds || !dds->has_runtime_size() )
	return NULL;

    static int runtime_size_capture_counter = 0;
    std::string cap_name = "__vla_type_size_" + std::to_string(runtime_size_capture_counter++);
    Variable *cap_var = pgm.addVariable(code, ddUINT64, cap_name, 1, NULL, false);
    TokenDecl *td = new TokenDecl(*cap_var);
    td->file = loc ? loc->file : NULL;
    td->line = loc ? loc->line : 0;
    td->column = loc ? loc->column : 0;

    TokenAssign *assign = new TokenAssign();
    assign->file = td->file;
    assign->line = td->line;
    assign->column = td->column;
    assign->left = new TokenVar(*cap_var);
    assign->right = new TokenTypeQuery(dds, false, false);
    td->initialize = assign;

    dds->runtime_size_expr = new TokenVar(*cap_var);
    return td;
}

static void configure_nested_function_captures(Program &pgm, FuncDef *func);

static TokenBase *try_parse_dynamic_type_query(Program &pgm, TokenBase *op_tb,
					       const std::string &op_name)
{
    bool want_alignof = is_alignof_identifier(op_name);
    if ( want_alignof )
	return NULL;

    auto consume_simple_named_type = [&](DataDef *dd) -> TokenBase * {
	if ( !is_runtime_sized_type(dd) )
	    return NULL;
	pgm.nextToken();
	return make_type_query_token(op_tb, dd, false);
    };

    if ( pgm.peekToken() && pgm.peekToken()->id() != TokenID::tkOpBrk )
    {
	TokenBase *probe = pgm.peekToken();
	if ( is_contextual_identifier_token(probe) )
	{
	    std::string name = contextual_identifier_name(probe);
	    if ( !pgm.findVariable(name) )
	    {
		DataDef *dd = resolve_named_datadef(pgm, name);
		if ( TokenBase *query = consume_simple_named_type(dd) )
		    return query;
	    }
	}
	return NULL;
    }

    if ( !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkOpBrk )
	return NULL;

    auto it = pgm.tokens.begin();
    if ( it == pgm.tokens.end() || (*it)->id() != TokenID::tkOpBrk )
	return NULL;
    ++it;
    if ( it == pgm.tokens.end() )
	return NULL;

    DataDef *dd = NULL;
    TokenBase *type_tb = *it;
    if ( type_tb->type() == TokenType::ttDataType )
	dd = &((TokenDataType *)type_tb)->definition;
    else if ( type_tb->type() == TokenType::ttIdentifier )
    {
	std::string name = ((TokenIdent *)type_tb)->str;
	if ( !pgm.findVariable(name) )
	    dd = resolve_named_datadef(pgm, name);
    }
    else if ( type_tb->type() == TokenType::ttKeyword
	   && (type_tb->id() == TokenID::tkSTRUCT || type_tb->id() == TokenID::tkUNION) )
    {
	++it;
	if ( it == pgm.tokens.end() || !is_contextual_identifier_token(*it) )
	    return NULL;
	dd = resolve_named_datadef(pgm, contextual_identifier_name(*it));
    }
    if ( !is_runtime_sized_type(dd) )
	return NULL;

    TokenBase *open = pgm.nextToken();
    (void)open;
    TokenBase *consumed = pgm.nextToken();
    if ( !consumed )
	return NULL;
    if ( consumed->type() == TokenType::ttKeyword
      && (consumed->id() == TokenID::tkSTRUCT || consumed->id() == TokenID::tkUNION) )
    {
	TokenBase *tag_tb = pgm.nextToken();
	if ( !tag_tb || !is_contextual_identifier_token(tag_tb) )
	    pgm.Throw(tag_tb ? tag_tb : consumed) << "Expecting struct/union tag in " << op_name << flush;
    }
    if ( !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkClBrk )
	pgm.Throw(consumed) << "Expecting ')' after " << op_name << " type" << flush;
    pgm.nextToken();
    return make_type_query_token(op_tb, dd, false);
}

static TokenDataType *parse_typeof_datatype(Program &pgm, TokenBase *op_tb)
{
    if ( !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkOpBrk )
	pgm.Throw(op_tb) << "Expecting '(' after typeof" << flush;
    pgm.nextToken();

    TokenBase *type_tb = pgm.nextToken();
    if ( !type_tb )
	pgm.Throw(op_tb) << "Unexpected end of input in typeof" << flush;

    DataDef *dd = NULL;
    bool closed_paren = false;
    if ( type_tb->type() == TokenType::ttDataType )
	dd = &((TokenDataType *)type_tb)->definition;
    else if ( type_tb->type() == TokenType::ttKeyword && type_tb->id() == TokenID::tkSTRUCT )
    {
	TokenBase *tag_tb = pgm.nextToken();
	if ( tag_tb && tag_tb->type() == TokenType::ttIdentifier )
	    dd = resolve_named_datadef(pgm, ((TokenIdent *)tag_tb)->str);
	if ( !dd )
	    pgm.Throw(tag_tb) << "Unknown struct type in typeof" << flush;
    }
    else if ( type_tb->type() == TokenType::ttIdentifier )
    {
	std::string tname = ((TokenIdent *)type_tb)->str;
	dd = resolve_named_datadef(pgm, tname);
	if ( !dd )
	{
	    TokenBase *expr = pgm.parseExpression(type_tb, true, false, true, 1);
	    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkClBrk )
		pgm.nextToken();
	    else if ( (!pgm.curToken() || pgm.curToken()->id() != TokenID::tkClBrk)
		   && (!pgm.prevToken() || pgm.prevToken()->id() != TokenID::tkClBrk) )
		pgm.Throw(type_tb) << "Expecting ')' after typeof expression" << flush;
	    dd = expr ? expr->datadef() : NULL;
	    closed_paren = true;
	}
    }
    else
    {
	TokenBase *expr = pgm.parseExpression(type_tb, true, false, true, 1);
	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkClBrk )
	    pgm.nextToken();
	else if ( (!pgm.curToken() || pgm.curToken()->id() != TokenID::tkClBrk)
	       && (!pgm.prevToken() || pgm.prevToken()->id() != TokenID::tkClBrk) )
	    pgm.Throw(type_tb) << "Expecting ')' after typeof expression" << flush;
	dd = expr ? expr->datadef() : NULL;
	closed_paren = true;
    }

    while ( !closed_paren && dd && pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkMul )
    {
	pgm.nextToken();
	dd = pgm.getPointerType(dd);
    }
    if ( !closed_paren && (!pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkClBrk) )
	pgm.Throw(type_tb) << "Expecting ')' after typeof" << flush;
    if ( !closed_paren )
	pgm.nextToken();

    if ( !dd )
	pgm.Throw(type_tb) << "typeof: cannot determine operand type" << flush;

    return new TokenDataType(dd->name.c_str(), *dd);
}

static TokenBase *parse_static_assert_statement(Program &pgm, TokenBase *tb)
{
    if ( !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkOpBrk )
	pgm.Throw(tb) << "Expecting '(' after " << ((TokenIdent *)tb)->str << flush;
    pgm.nextToken();

    int64_t cond = parse_constant_integer_expression(pgm);
    std::string message = "static assertion failed";
    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkComma )
    {
	pgm.nextToken();
	TokenBase *msg_tb = pgm.nextToken();
	if ( !msg_tb || msg_tb->type() != TokenType::ttString )
	    pgm.Throw(msg_tb ? msg_tb : tb) << "Expecting string literal in static assertion" << flush;
	message = ((TokenStr *)msg_tb)->str;
    }

    if ( !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkClBrk )
	pgm.Throw(tb) << "Expecting ')' after static assertion" << flush;
    pgm.nextToken();
    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
	pgm.nextToken();

    if ( !cond )
	pgm.Throw(tb) << message << flush;
    return NULL;
}

static bool try_parse_constant_offsetof_address(Program &pgm, int64_t &out)
{
    auto saved_tokens = pgm.tokens;
    size_t saved_diag_count = pgm.diagnostics.size();
    Program::ErrorInfo saved_error = pgm.last_error;

    auto fail = [&]() -> bool {
	pgm.tokens = saved_tokens;
	pgm.diagnostics.resize(saved_diag_count);
	pgm.last_error = saved_error;
	return false;
    };

    TokenBase *open_outer = pgm.nextToken();
    TokenBase *open_cast = pgm.nextToken();
    if ( !open_outer || open_outer->id() != TokenID::tkOpBrk
      || !open_cast || open_cast->id() != TokenID::tkOpBrk )
	return fail();

    DataDef *base_dd = NULL;
    TokenBase *type_tb = pgm.nextToken();
    if ( !type_tb )
	return fail();
    if ( type_tb->type() == TokenType::ttKeyword
      && (type_tb->id() == TokenID::tkSTRUCT || type_tb->id() == TokenID::tkUNION) )
    {
	TokenBase *tag_tb = pgm.nextToken();
	if ( !tag_tb || !is_contextual_identifier_token(tag_tb) )
	    return fail();
	base_dd = resolve_named_datadef(pgm, contextual_identifier_name(tag_tb));
    }
    else if ( type_tb->type() == TokenType::ttDataType )
	base_dd = &static_cast<TokenDataType *>(type_tb)->definition;
    else if ( is_contextual_identifier_token(type_tb) )
	base_dd = resolve_named_datadef(pgm, contextual_identifier_name(type_tb));
    if ( !base_dd )
	return fail();

    while ( pgm.peekToken() && pgm.peekToken()->id() != TokenID::tkClBrk )
	pgm.nextToken();
    TokenBase *close_cast = pgm.nextToken();
    if ( !close_cast || close_cast->id() != TokenID::tkClBrk )
	return fail();

    TokenBase *zero_tb = pgm.nextToken();
    int64_t zero_value = 0;
    if ( !resolve_integer_constant(pgm, zero_tb, zero_value) || zero_value != 0 )
	return fail();
    TokenBase *close_outer = pgm.nextToken();
    if ( !close_outer || close_outer->id() != TokenID::tkClBrk )
	return fail();

    TokenBase *access = pgm.nextToken();
    if ( !access || (access->id() != TokenID::tkDeRef && access->id() != TokenID::tkDot) )
	return fail();

    int64_t offset = 0;
    DataDef *current = base_dd;
    while ( true )
    {
	if ( DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(current) )
	    current = pdd->base_type;
	DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(current);
	if ( !sdd )
	    return fail();

	TokenBase *member_tb = pgm.nextToken();
	if ( !member_tb || !is_contextual_identifier_token(member_tb) )
	    return fail();
	std::string member_name = contextual_identifier_name(member_tb);
	ssize_t member_offset = sdd->m_offset(member_name);
	if ( member_offset < 0 )
	    return fail();
	offset += member_offset;
	current = sdd->m_type(member_name);
	if ( !current )
	    return fail();

	while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpSqr )
	{
	    pgm.nextToken();
	    int64_t index = parse_constant_integer_expression(pgm);
	    TokenBase *close_sq = pgm.nextToken();
	    if ( !close_sq || close_sq->id() != TokenID::tkClSqr )
		return fail();
	    size_t elem_size = current->size ? current->size : 1;
	    offset += index * (int64_t)elem_size;
	}

	if ( pgm.peekToken()
	  && (pgm.peekToken()->id() == TokenID::tkDeRef || pgm.peekToken()->id() == TokenID::tkDot) )
	{
	    pgm.nextToken();
	    continue;
	}
	break;
    }

    out = offset;
    return true;
}

static int64_t parse_constant_primary(Program &pgm)
{
    TokenBase *tb = pgm.nextToken();
    int64_t out = 0;

    if ( resolve_integer_constant(pgm, tb, out) )
	return out;
    if ( tb && tb->type() == TokenType::ttIdentifier )
    {
	std::string name = ((TokenIdent *)tb)->str;
	if ( name == "sizeof" || is_alignof_identifier(name) )
	    return (int64_t)evaluate_type_query(pgm, tb, name);
	if ( is_nullptr_identifier(name) )
	    return 0;
    }
    if ( tb && tb->id() == TokenID::tkNeg )
	return -parse_constant_primary(pgm);
    if ( tb && tb->id() == TokenID::tkAdd )
	return parse_constant_primary(pgm);
    if ( tb && tb->id() == TokenID::tkLnot )
	return !parse_constant_primary(pgm);
    if ( tb && tb->id() == TokenID::tkBand )
    {
	if ( try_parse_constant_offsetof_address(pgm, out) )
	    return out;
	pgm.Throw(tb) << "Unsupported address expression in constant expression" << flush;
    }
    if ( tb && tb->id() == TokenID::tkOpBrk )
    {
	// Check for cast: (type)value — e.g. (char)SB, (unsigned char)~0
	TokenBase *inner = pgm.peekToken();
	if ( inner && (inner->type() == TokenType::ttDataType
	  || inner->id() == TokenID::tkCONST
	  || inner->id() == TokenID::tkRESTRICT) )
	{
	    // Extract the cast target type for truncation
	    DataDef *cast_dd = NULL;
	    bool is_unsigned = false;
	    while ( pgm.peekToken()
	         && pgm.peekToken()->id() != TokenID::tkClBrk )
	    {
		TokenBase *ct = pgm.nextToken();
		if ( ct->type() == TokenType::ttDataType )
		    cast_dd = &((TokenDataType *)ct)->definition;
		if ( ct->type() == TokenType::ttIdentifier )
		{
		    std::string tname = ((TokenIdent *)ct)->str;
		    if ( tname == "unsigned" ) is_unsigned = true;
		}
	    }
	    if ( pgm.peekToken() )
		pgm.nextToken(); // consume ')'
	    int64_t val = parse_constant_primary(pgm);
	    // Apply truncation for the cast type
	    if ( cast_dd )
	    {
		int sz = cast_dd->size;
		if ( sz == 1 )
		    val = is_unsigned ? (int64_t)(uint8_t)val : (int64_t)(int8_t)val;
		else if ( sz == 2 )
		    val = is_unsigned ? (int64_t)(uint16_t)val : (int64_t)(int16_t)val;
		else if ( sz == 4 )
		    val = is_unsigned ? (int64_t)(uint32_t)val : (int64_t)(int32_t)val;
	    }
	    return val;
	}
	out = parse_constant_integer_expression(pgm);
	tb = pgm.nextToken();
	if ( !tb || tb->id() != TokenID::tkClBrk )
	    pgm.Throw(tb) << "Expecting ')' in constant expression" << flush;
	return out;
    }
    pgm.Throw(tb) << "Expecting integer constant expression" << flush;
    return 0;
}

static int64_t parse_constant_mul(Program &pgm)
{
    int64_t lhs = parse_constant_primary(pgm);

    while ( pgm.peekToken() )
    {
	TokenBase *op = pgm.peekToken();
	if ( op->id() != TokenID::tkMul && op->id() != TokenID::tkDiv && op->id() != TokenID::tkMod )
	    break;
	pgm.nextToken(); // consume operator
	int64_t rhs = parse_constant_primary(pgm);
	if ( op->id() == TokenID::tkMul ) lhs *= rhs;
	else if ( op->id() == TokenID::tkDiv )
	{
	    if ( rhs == 0 )
		pgm.Throw(op) << "Division by zero in constant expression" << flush;
	    lhs /= rhs;
	}
	else
	{
	    if ( rhs == 0 )
		pgm.Throw(op) << "Modulo by zero in constant expression" << flush;
	    lhs %= rhs;
	}
    }

    return lhs;
}

// additive: parse_constant_mul ([+-] parse_constant_mul)*
static int64_t parse_constant_add(Program &pgm)
{
    int64_t lhs = parse_constant_mul(pgm);

    while ( pgm.peekToken() )
    {
	TokenBase *op = pgm.peekToken();
	if ( op->id() != TokenID::tkAdd && op->id() != TokenID::tkSub && op->id() != TokenID::tkNeg )
	    break;
	pgm.nextToken(); // consume operator
	int64_t rhs = parse_constant_mul(pgm);
	if ( op->id() == TokenID::tkAdd ) lhs += rhs;
	else lhs -= rhs;
    }

    return lhs;
}

// shift: parse_constant_add ([<<>>] parse_constant_add)*
static int64_t parse_constant_shift(Program &pgm)
{
    int64_t lhs = parse_constant_add(pgm);

    while ( pgm.peekToken() )
    {
	TokenBase *op = pgm.peekToken();
	if ( op->id() != TokenID::tkBSL && op->id() != TokenID::tkBSR )
	    break;
	pgm.nextToken();
	int64_t rhs = parse_constant_add(pgm);
	if ( op->id() == TokenID::tkBSL ) lhs <<= rhs;
	else                              lhs >>= rhs;
    }

    return lhs;
}

// bitwise-and / xor / or: same precedence order as C.
static int64_t parse_constant_band(Program &pgm)
{
    int64_t lhs = parse_constant_eq(pgm);
    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkBand )
    {
	pgm.nextToken();
	lhs &= parse_constant_eq(pgm);
    }
    return lhs;
}

static int64_t parse_constant_bxor(Program &pgm)
{
    int64_t lhs = parse_constant_band(pgm);
    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkXor )
    {
	pgm.nextToken();
	lhs ^= parse_constant_band(pgm);
    }
    return lhs;
}

static int64_t parse_constant_bor(Program &pgm)
{
    int64_t lhs = parse_constant_bxor(pgm);
    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkBor )
    {
	pgm.nextToken();
	lhs |= parse_constant_bxor(pgm);
    }
    return lhs;
}

static int64_t parse_constant_rel(Program &pgm)
{
    int64_t lhs = parse_constant_shift(pgm);
    while ( pgm.peekToken() )
    {
	TokenBase *op = pgm.peekToken();
	if ( op->id() != TokenID::tkLT && op->id() != TokenID::tkGT
	  && op->id() != TokenID::tkLE && op->id() != TokenID::tkGE )
	    break;
	pgm.nextToken();
	int64_t rhs = parse_constant_shift(pgm);
	switch ( op->id() )
	{
	    case TokenID::tkLT: lhs = lhs < rhs; break;
	    case TokenID::tkGT: lhs = lhs > rhs; break;
	    case TokenID::tkLE: lhs = lhs <= rhs; break;
	    default:            lhs = lhs >= rhs; break;
	}
    }
    return lhs;
}

static int64_t parse_constant_eq(Program &pgm)
{
    int64_t lhs = parse_constant_rel(pgm);
    while ( pgm.peekToken() )
    {
	TokenBase *op = pgm.peekToken();
	if ( op->id() != TokenID::tkEquals && op->id() != TokenID::tkNotEq )
	    break;
	pgm.nextToken();
	int64_t rhs = parse_constant_rel(pgm);
	lhs = (op->id() == TokenID::tkEquals) ? (lhs == rhs) : (lhs != rhs);
    }
    return lhs;
}

static int64_t parse_constant_land(Program &pgm)
{
    int64_t lhs = parse_constant_bor(pgm);
    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkLand )
    {
	pgm.nextToken();
	lhs = lhs && parse_constant_bor(pgm);
    }
    return lhs;
}

static int64_t parse_constant_lor(Program &pgm)
{
    int64_t lhs = parse_constant_land(pgm);
    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkLor )
    {
	pgm.nextToken();
	lhs = lhs || parse_constant_land(pgm);
    }
    return lhs;
}

static int64_t parse_constant_ternary(Program &pgm)
{
    int64_t cond = parse_constant_lor(pgm);
    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkQmark )
    {
	pgm.nextToken(); // consume '?'
	int64_t true_val = parse_constant_integer_expression(pgm);
	TokenBase *colon = pgm.nextToken();
	if ( !colon || colon->id() != TokenID::tkColon )
	    pgm.Throw(colon) << "Expecting ':' in ternary constant expression" << flush;
	int64_t false_val = parse_constant_ternary(pgm);
	return cond ? true_val : false_val;
    }
    return cond;
}

static int64_t parse_constant_integer_expression(Program &pgm)
{
    return parse_constant_ternary(pgm);
}

static bool bracket_dim_constant_expression_parses(Program &pgm)
{
    auto saved_tokens = pgm.tokens;
    size_t saved_diag_count = pgm.diagnostics.size();
    Program::ErrorInfo saved_error = pgm.last_error;
    try
    {
	int64_t n = parse_constant_integer_expression(pgm);
	TokenBase *cl = pgm.nextToken();
	pgm.tokens = saved_tokens;
	pgm.diagnostics.resize(saved_diag_count);
	pgm.last_error = saved_error;
	return n >= 0 && cl && cl->id() == TokenID::tkClSqr;
    }
    catch ( ... )
    {
	pgm.tokens = saved_tokens;
	pgm.diagnostics.resize(saved_diag_count);
	pgm.last_error = saved_error;
	return false;
    }
}

static bool bracket_dim_uses_runtime_value(Program &pgm,
					   const std::set<std::string> *runtime_names = NULL)
{
    int depth = 1;
    for ( auto it = pgm.tokens.begin(); it != pgm.tokens.end() && depth > 0; ++it )
    {
	TokenBase *t = *it;
	if ( t->id() == TokenID::tkOpSqr ) { ++depth; continue; }
	if ( t->id() == TokenID::tkClSqr ) { --depth; continue; }
	if ( t->id() == TokenID::tkSemi || t->id() == TokenID::tkOpBrc ) break;
	// ++ or -- in a dimension expression is inherently runtime
	if ( t->id() == TokenID::tkInc || t->id() == TokenID::tkDec )
	    return true;
	std::string name;
	if ( t->type() == TokenType::ttIdentifier )
	    name = ((TokenIdent *)t)->str;
	else
	    continue;
	if ( runtime_names && runtime_names->count(name) )
	    return true;
	Variable *v = pgm.findVariable(name);
	if ( !v )
	    continue;
	if ( v->is_constant() )
	{
	    // A const-qualified var folds to a constant dimension ONLY if its
	    // value is known at compile time. A const with a runtime initializer
	    // (e.g. `const int len = atoi(argv[1]);`) carries no folded value
	    // (`var->data` is NULL) and is therefore still a VLA bound.
	    int64_t cval;
	    if ( read_constant_integer(v, cval) )
		continue;
	    return true;
	}
	return true;
    }
    return false;
}

static bool bracket_dim_has_constant_fold_query(Program &pgm)
{
    int depth = 1;
    for ( auto it = pgm.tokens.begin(); it != pgm.tokens.end() && depth > 0; ++it )
    {
	TokenBase *t = *it;
	if ( t->id() == TokenID::tkOpSqr ) { ++depth; continue; }
	if ( t->id() == TokenID::tkClSqr ) { --depth; continue; }
	if ( depth != 1 )
	    continue;
	if ( t->type() != TokenType::ttIdentifier )
	    continue;
	std::string name = ((TokenIdent *)t)->str;
	if ( name == "sizeof" || is_alignof_identifier(name) )
	    return true;
    }
    return false;
}

static bool bracket_dim_needs_runtime_value(Program &pgm,
					    const std::set<std::string> *runtime_names = NULL)
{
    if ( !bracket_dim_uses_runtime_value(pgm, runtime_names) )
	return false;
    if ( !bracket_dim_has_constant_fold_query(pgm) )
	return true;
    return !bracket_dim_constant_expression_parses(pgm);
}

static DataDef *parse_typedef_array_suffix(Program &pgm, DataDef *base_dd,
					   const std::string &alias_name,
					   TokenBase *err_tok)
{
    if ( !base_dd )
	return base_dd;
    if ( !(pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpSqr) )
	return base_dd;

    size_t alias_count = 1;
    TokenBase *alias_count_expr = NULL;
    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpSqr )
    {
	pgm.nextToken(); // consume '['
	TokenBase *cl = pgm.nextToken();
	if ( cl && cl->id() == TokenID::tkClSqr )
	{
	    alias_count = 0;
	    continue;
	}
	pgm.pushToken(cl);
	if ( !alias_count_expr && bracket_dim_needs_runtime_value(pgm) )
	{
	    alias_count_expr = pgm.parseExpression(pgm.nextToken(), true);
	    cl = pgm.nextToken();
	    if ( !cl || cl->id() != TokenID::tkClSqr )
		pgm.Throw(cl ? cl : err_tok) << "Expected ] in typedef array declaration" << flush;
	    continue;
	}
	int64_t n = parse_constant_integer_expression(pgm);
	if ( n < 0 )
	    pgm.Throw(err_tok) << "Typedef array dimension must be non-negative" << flush;
	cl = pgm.nextToken();
	if ( !cl || cl->id() != TokenID::tkClSqr )
	    pgm.Throw(cl ? cl : err_tok) << "Expected ] in typedef array declaration" << flush;
	if ( n == 0 )
	    alias_count = 0;
	else
	    alias_count *= (size_t)n;
    }
    return new DataDefCArray(*base_dd, alias_name, alias_count, alias_count_expr);
}

DataDefVOID ddVOID;
DataDefVOIDref ddVOIDref;
DataDefBOOL ddBOOL;
DataDefCHAR ddCHAR;
DataDefINT ddINT;
DataDefINT8 ddINT8;
DataDefINT16 ddINT16;
DataDefINT24 ddINT24;
DataDefINT32 ddINT32;
DataDefINT64 ddINT64;
DataDefUINT8 ddUINT8;
DataDefUINT16 ddUINT16;
DataDefUINT24 ddUINT24;
DataDefUINT32 ddUINT32;
DataDefUINT64 ddUINT64;
DataDefFLOAT ddFLOAT;
DataDefDOUBLE ddDOUBLE;
DataDefARRAY ddARRAY;
DataDefLPSTR ddLPSTR;
DataDefPTR ddVOIDptr(ddVOID), ddCHARptr(ddCHAR), ddINTptr(ddINT), ddINT32ptr(ddINT32);
DataDefAUTO ddAUTO;
DataDefTEST ddTESTSTRUCT;

void printuint32(uint32_t &i)
{
    std::cout << "i: " << i << std::endl << std::flush;
}

void printuint32(uint32_t i)
{
    std::cout << "i: " << i << std::endl << std::flush;
}

void EatSpaces(istream &is)
{
    while ( is.good() && !is.eof() && isspace(is.peek()) )
        is.get();
}


int64_t TokenAssign::ioperate() const
{
    DBG(std::cout << "TokenAssign" << std::endl);
    if ( left->type() != TokenType::ttVariable )
    {
	std::cerr << "TokenAssign::operate() left side not variable" << std::endl;
	return 0;
    }
    DBG(std::cout << "TokenAssign: " << dynamic_cast<TokenVar *>(left)->var.name << "=" << right->ival() << std::endl);
    left->set(right->ival());
    return right->ival();
}


// Variable constructor, will allocate data and initialize if requested
Variable::Variable(std::string n, DataDef &d, uint32_t c, void *init, bool alloc)
{
    name = n;
    type = &d;
    count = c;
    flags = 0;
    data = NULL;
    aot_data_offset = (size_t)-1;
    aot_cstr_offset = (size_t)-1;
    vla_size_expr = nullptr;
    param_vla_side_effect_expr = nullptr;
    if ( init ) { alloc = true; }
    if ( !alloc ) { flags |= vfSTACK; }
    // Size 0 (e.g. FuncDef, void) has no storage. Function-pointer types
    // (DataDefFPTR, size 8) are a pointer slot and DO need allocation.
    if ( alloc && count == 1
      && ((type->basetype() != BaseType::btFunct && type->size > 0)
	|| dynamic_cast<DataDefFPTR *>(type) != NULL) )
    {
	data = calloc(count, d.size);
	flags |= vfALLOC;
	DBG(std::cout << "Variable::Variable data = calloc(" << count << ", " << d.size << ") for " << n << std::endl);
	DBG(std::cout << "Data address: " << (uint64_t)data << std::endl);
    }
}

Variable::~Variable()
{
    if ( !(flags & vfALLOC) )
	return;

    DBG(std::cout << "Variable::~Variable(" << name << ") freeing data" << std::endl);

    free(data);
}

Variable *DataDefCLASS::findMethod(std::string &s)
{
    // check method_map first (unmangled names)
    std::map<std::string, Variable *>::iterator it = method_map.find(s);
    if ( it != method_map.end() )
	return it->second;

    // fallback: search methods vector by variable name
    for ( variable_vec_iter vvi = methods.begin(); vvi != methods.end(); ++vvi )
	if ( !s.compare((*vvi)->name) )
	    return *vvi;

    // search base class chain
    if ( base_class )
	return base_class->findMethod(s);

    return NULL;
}

Variable *DataDefCLASS::findMethodOverload(const std::string &name,
					  const std::vector<const DataDef *> &argtypes)
{
    Variable *best = NULL;
    int best_score = -1;
    bool any_named = false;
    // Scan ALL same-name overloads (the registration pushes each type-differing
    // overload into `methods` under the same display name); rank by arg type.
    for ( variable_vec_iter vvi = methods.begin(); vvi != methods.end(); ++vvi )
    {
	Variable *mv = *vvi;
	if ( !mv )
	    continue;
	FuncDef *fd = dynamic_cast<FuncDef *>(mv->type);
	if ( !fd )
	    continue;
	// Methods are keyed by their MANGLED name (`Box__take`, `Box__take__o2`);
	// match on the unmangled display name when the FuncDef records one, else
	// fall back to the raw name.
	const std::string &disp = fd->method_display_name.empty()
				? mv->name : fd->method_display_name;
	if ( name.compare(disp) != 0 )
	    continue;
	any_named = true;
	// Parameter count excluding the hidden __this (param 0).
	size_t pn = fd->parameters.empty() ? 0 : fd->parameters.size() - 1;
	if ( pn != argtypes.size() )
	    continue;
	int total = 0;
	bool ok = true;
	for ( size_t i = 0; i < argtypes.size(); i++ )
	{
	    size_t pi = i + 1;   // skip __this
	    DataDef *pt = pi < fd->parameters.size() ? fd->parameters[pi] : NULL;
	    bool refp = pi < fd->ref_params.size() && fd->ref_params[pi];
	    int s = score_arg_to_param(argtypes[i], pt, refp);
	    if ( s < 0 ) { ok = false; break; }
	    total += s;
	}
	if ( ok && total > best_score )
	{
	    best_score = total;
	    best = mv;
	}
    }
    if ( best )
	return best;
    // C++ name hiding: a same-name method declared in THIS class hides base
    // overloads. Only descend to the base when this class declares none.
    if ( any_named )
    {
	std::string n(name);
	return findMethod(n);   // no arity/type match: first by-name (>= findMethod)
    }
    if ( base_class )
	return base_class->findMethodOverload(name, argtypes);
    return NULL;
}

DataDef *DataDefCLASS::binary_operator_return_type(const std::string &opname)
{
    // Prefer a binary (params > 1 incl. __this) overload. Search the unmangled
    // name first (std::string registers operators under "operator+"), then the
    // mangled ClassName__operatorX family (user classes), then the base chain.
    FuncDef *any = NULL;
    for ( variable_vec_iter vvi = methods.begin(); vvi != methods.end(); ++vvi )
    {
	Variable *mv = *vvi;
	if ( !mv || opname.compare(mv->name) != 0 ) continue;
	FuncDef *fd = dynamic_cast<FuncDef *>(mv->type);
	if ( !fd ) continue;
	if ( !any ) any = fd;
	if ( fd->parameters.size() > 1 ) return &fd->returns;
    }
    std::string mangled = name + "__" + opname;
    std::string mangled_overload_prefix = mangled + "__o";
    for ( variable_vec_iter vvi = methods.begin(); vvi != methods.end(); ++vvi )
    {
	Variable *mv = *vvi;
	if ( !mv || (mangled.compare(mv->name) != 0
	  && mv->name.compare(0, mangled_overload_prefix.size(),
			      mangled_overload_prefix) != 0) ) continue;
	FuncDef *fd = dynamic_cast<FuncDef *>(mv->type);
	if ( !fd ) continue;
	if ( !any ) any = fd;
	if ( fd->parameters.size() > 1 ) return &fd->returns;
    }
    if ( any ) return &any->returns;
    if ( base_class ) return base_class->binary_operator_return_type(opname);
    return NULL;
}

DataDef *DataDefCLASS::unary_operator_return_type(const std::string &opname,
						  bool postfix)
{
    FuncDef *fallback = NULL;
    std::string mangled = name + "__" + opname;
    std::string mangled_un = mangled + "_un";
    std::string mangled_overload_prefix = mangled + "__o";
    for ( variable_vec_iter vvi = methods.begin(); vvi != methods.end(); ++vvi )
    {
	Variable *mv = *vvi;
	if ( !mv ) continue;
	FuncDef *fd = dynamic_cast<FuncDef *>(mv->type);
	if ( !fd ) continue;
	const std::string &disp = fd->method_display_name.empty()
				? mv->name : fd->method_display_name;
	if ( opname.compare(disp) != 0
	  && mangled.compare(mv->name) != 0
	  && mangled_un.compare(mv->name) != 0
	  && mv->name.compare(0, mangled_overload_prefix.size(),
			      mangled_overload_prefix) != 0 )
	    continue;
	bool parameterized = fd->parameters.size() > 1;
	if ( postfix == parameterized )
	    return &fd->returns;
	if ( !fallback )
	    fallback = fd;
    }
    if ( fallback )
	return &fallback->returns;
    if ( base_class )
	return base_class->unary_operator_return_type(opname, postfix);
    return NULL;
}

// Part A — type an operator expression whose LEFT operand is a class OBJECT that
// declares the matching operator, using that operator's RETURN type. Generic: it
// treats std::string and every user class identically (no per-type rule). Without
// it `a + b` on objects reports the default arithmetic datadef, so copy-init ctor
// selection, chained operator expressions, and `auto` all mis-resolve.
void Program::resolve_object_operator_type(TokenOperator *to)
{
    if ( !to ) return;
    bool unary = to->argc() == 1;
    bool postfix = unary && to->left != NULL;
    TokenBase *operand = unary ? (postfix ? to->left : to->right) : to->left;
    if ( !operand ) return;
    DataDefCLASS *lc = dynamic_cast<DataDefCLASS *>(operand->datadef());
    if ( !lc ) return;   // operand is not a class object
    const char *opsym;
    switch ( to->id() )
    {
	case TokenID::tkAdd: opsym = "+"; break;
	case TokenID::tkSub: opsym = "-"; break;
	case TokenID::tkNeg: opsym = "-"; break;
	case TokenID::tkMul: opsym = "*"; break;
	case TokenID::tkDiv: opsym = "/"; break;
	case TokenID::tkMod: opsym = "%"; break;
	case TokenID::tkLnot: opsym = "!"; break;
	case TokenID::tkBnot: opsym = "~"; break;
	case TokenID::tkInc: opsym = "++"; break;
	case TokenID::tkDec: opsym = "--"; break;
	case TokenID::tkBSL: opsym = "<<"; break;
	case TokenID::tkBSR: opsym = ">>"; break;
	default: return;     // comparison/bitwise results are already typed correctly
    }
    std::string opname = std::string("operator") + opsym;
    DataDef *rt = unary ? lc->unary_operator_return_type(opname, postfix)
			: lc->binary_operator_return_type(opname);
    if ( rt ) to->set_resolved_type(rt);
}

TokenCallMethod *Program::reselect_method_overload(TokenCallMethod *tc,
		Variable &recv, DataDefCLASS *cls, const std::string &id)
{
    if ( !tc || !cls )
	return tc;
    std::vector<const DataDef *> at;
    for ( TokenBase *p : tc->parameters )
	at.push_back(p ? p->datadef() : NULL);
    Variable *ov = cls->findMethodOverload(id, at);
    if ( !ov || ov == &tc->var )
	return tc;   // already the best (or single) overload — no change
    TokenCallMethod *tc2 = new TokenCallMethod(recv, *ov);
    tc2->parameters = tc->parameters;
    tc2->parent_expr = tc->parent_expr;
    tc2->file = tc->file;
    tc2->line = tc->line;
    tc2->column = tc->column;
    return tc2;
}

// Round `sz` up to alignment `a` (a is a power of two).
static inline size_t mi_align_up(size_t sz, size_t a) { return (sz + a - 1) & ~(a - 1); }

// Itanium-faithful record layout. See
// docs/superpowers/specs/2026-06-03-multiple-inheritance-design.md §2.
// Precondition: `members`/`member_offsets` hold OWN data members from offset 0
// (as addMember produced them); `size`/`max_align` are their packed size and
// strongest own-member alignment; `has_vtable` set iff polymorphic; `bases`
// populated (base, is_virtual). Postcondition: each BaseSpec.offset/is_primary,
// vbase_offset, secondary_vptr_owners, nvsize and size are final; own member
// offsets are shifted to sit after the vptr+non-virtual-bases block.
void DataDefCLASS::compute_layout()
{
    size_t cur = 0;
    size_t maxalign = 8; // class alignment accumulator (>= pointer align)

    // 1. vptr: a class carries a vptr if it has virtual methods OR any (transitive)
    //    virtual base (Itanium: virtual inheritance needs a vptr even with no virtual
    //    methods). The vptr sits at offset 0 unless a primary base (non-virtual base
    //    that itself carries a vptr) already provides one.
    std::vector<DataDefCLASS *> all_vbases;
    std::set<DataDefCLASS *> vseen;
    collect_vbases(all_vbases, vseen);
    bool needs_vptr = is_polymorphic() || !all_vbases.empty();
    has_vptr_slot = needs_vptr;
    bool have_primary = false;
    for ( auto &bs : bases )
    {
	if ( !bs.is_virtual && bs.base->has_vptr_slot )
	{
	    bs.is_primary = true; have_primary = true; break;
	}
    }
    bool own_vptr = needs_vptr && !have_primary;
    if ( own_vptr ) cur += 8;   // __vptr at 0

    // 2. non-virtual bases in declaration order (primary first, at 0). A base
    //    contributes its NON-VIRTUAL size (nvsize); its vbases are hoisted.
    for ( auto &bs : bases )
    {
	if ( bs.is_virtual ) continue;
	size_t balign = 8;
	cur = mi_align_up(cur, balign);
	bs.offset = bs.is_primary ? 0 : cur;
	if ( bs.is_primary )
	    cur = bs.base->nvsize;             // shares vptr@0; advance past its nvsize
	else
	{
	    if ( bs.base->has_vptr_slot ) secondary_vptr_owners.push_back(bs.base);
	    cur += bs.base->nvsize;
	}
	if ( balign > maxalign ) maxalign = balign;
    }

    // 3. own data members begin after the non-virtual bases. Record the boundary
    //    only; apply_member_layout() does the member_offsets rewrite (it knows each
    //    member's origin). addMember left `size` = the packed own-members size
    //    (the live flatten resets size to 0 before adding own members; the unit
    //    tests add only own members) and `max_align` = strongest own alignment.
    if ( max_align > maxalign ) maxalign = max_align;
    own_block_off = mi_align_up(cur, max_align ? max_align : 1);
    cur = own_block_off + size;   // size = own packed size on entry

    // 4. nvsize = end of the non-virtual portion.
    nvsize = mi_align_up(cur, maxalign);

    // 5. virtual bases: appended once at the end, in canonical (collected) order.
    std::vector<DataDefCLASS *> vbs;
    std::set<DataDefCLASS *> seen;
    collect_vbases(vbs, seen);
    size_t end = nvsize;
    for ( DataDefCLASS *vb : vbs )
    {
	end = mi_align_up(end, 8);
	vbase_offset[vb] = end;
	end += vb->nvsize;        // vbase contributes its non-virtual size
	if ( 8 > maxalign ) maxalign = 8;
    }
    size = mi_align_up(end, maxalign);
}

// Rewrite each member's final offset from its origin: own members (origin -1, or
// no origin recorded) sit at own_block_off + their own-local offset; an inherited
// member sits at its base subobject offset (vbase_offset for a virtual base, else
// bases[origin].offset) + its offset-within-that-base. Run AFTER compute_layout().
void DataDefCLASS::apply_member_layout()
{
    for ( size_t i = 0; i < member_offsets.size(); i++ )
    {
	int origin = (i < member_origin.size()) ? member_origin[i] : -1;
	if ( origin < 0 )
	    member_offsets[i] = own_block_off + member_offsets[i];
	else
	{
	    size_t boff = 0;
	    if ( origin < (int)bases.size() )
		boff = bases[origin].is_virtual
			 ? vbase_offset[bases[origin].base]
			 : bases[origin].offset;
	    member_offsets[i] = boff + member_offsets[i];
	}
    }
}

// Subobject offset of `target` within this class. A (transitive) virtual base
// lives at this class's hoisted vbase_offset; a non-virtual base at its bs.offset
// (plus the offset within it, transitively). Returns (size_t)-1 if not a base.
// Build the grouped vtable: group 0 = primary (this class's flat vtable_slots,
// already inheritance-merged), then one group per secondary polymorphic base
// (its own slots, at its subobject offset). Run after compute_layout (so
// secondary_vptr_owners + base offsets are known).
void DataDefCLASS::build_vtable_groups()
{
    vtable_groups.clear();
    if ( !has_vtable && !has_vptr_slot ) return;

    // The primary group holds the PRIMARY-chain slots: the primary base's slots
    // (overrides resolved by name at emit time) plus this class's own newly
    // introduced virtuals. Secondary bases' slots live in their own groups, so
    // exclude them from the primary group.
    DataDefCLASS *prim = NULL;
    for ( auto &b : bases ) if ( b.is_primary ) { prim = b.base; break; }
    std::set<std::string> secondary_slots;
    for ( DataDefCLASS *o : secondary_vptr_owners )
	for ( auto &s : o->vtable_slots ) secondary_slots.insert(s);

    VtableGroup primary{this, 0, std::vector<std::string>(), 0};
    if ( prim ) primary.slots = prim->vtable_slots;
    for ( auto &s : vtable_slots )
    {
	bool in_prim = false;
	if ( prim )
	    for ( auto &ps : prim->vtable_slots ) if ( ps == s ) { in_prim = true; break; }
	if ( !in_prim && !secondary_slots.count(s) )
	    primary.slots.push_back(s);
    }
    vtable_groups.push_back(primary);

    for ( DataDefCLASS *o : secondary_vptr_owners )
    {
	size_t off = 0;
	for ( auto &b : bases ) if ( b.base == o ) { off = b.offset; break; }
	vtable_groups.push_back(VtableGroup{o, off, o->vtable_slots, 0});
    }

    // Address point of each group = the index in the flat Cls__vtable[] array
    // (sub-tables emitted back-to-back) of the group's first FUNCTION slot. Each
    // address point is preceded by the Itanium 2-word prologue
    // [offset_to_top, &_ZTI<cls>] (emitted in class_vtable_def), so the running
    // index advances by PROLOGUE before each group. Computed here so emission /
    // dispatch / vptr-init agree regardless of pass order. (S5a)
    const size_t PROLOGUE = 2;
    size_t ap = 0;
    for ( auto &g : vtable_groups )
    {
	ap += PROLOGUE;          // this group's prologue precedes its address point
	g.addr_point = ap;
	ap += g.slots.size();
    }
}

size_t DataDefCLASS::base_offset_of(const DataDefCLASS *target) const
{
    if ( target == this ) return 0;
    for ( const auto &vb : vbase_offset )
	if ( vb.first == target ) return vb.second;
    for ( const auto &bs : bases )
    {
	if ( bs.is_virtual ) continue; // virtual bases handled via vbase_offset above
	if ( bs.base == target ) return bs.offset;
	size_t inner = bs.base->base_offset_of(target);
	if ( inner != (size_t)-1 ) return bs.offset + inner;
    }
    return (size_t)-1;
}

DataDefCLASS::TypeInfoFlavor DataDefCLASS::typeinfo_flavor() const
{
    if ( bases.empty() )
	return TI_CLASS;
    // __si only when there is exactly one base, public (access 0), non-virtual.
    if ( bases.size() == 1 && !bases[0].is_virtual && bases[0].access == 0 )
	return TI_SI;
    return TI_VMI;
}

bool DataDefCLASS::is_unique_public_nonvirtual_base(DataDefCLASS *b, size_t *off) const
{
    size_t found_off = 0;
    int count = 0;
    for ( const BaseSpec &bs : bases )
    {
	if ( bs.is_virtual || bs.access != 0 ) continue; // public, non-virtual only
	if ( bs.base == b )
	{
	    found_off = bs.offset;
	    count++;
	}
	else
	{
	    size_t sub = 0;
	    if ( bs.base->is_unique_public_nonvirtual_base(b, &sub) )
	    {
		found_off = bs.offset + sub;
		count++;
	    }
	}
    }
    if ( count == 1 )
    {
	if ( off ) *off = found_off;
	return true;
    }
    return false;
}

void DataDefCLASS::collect_vbases(std::vector<DataDefCLASS *> &out,
				  std::set<DataDefCLASS *> &seen) const
{
    for ( const auto &bs : bases )
    {
	// a base's own (transitive) virtual bases come first, then the base if virtual
	bs.base->collect_vbases(out, seen);
	if ( bs.is_virtual && seen.insert(bs.base).second )
	    out.push_back(bs.base);
    }
}

DataDef *FuncDef::findParameter(std::string &s)
{
    DBG(cout << "FuncDef[" << name << "]::findParameter(" << s << ')' << endl);
    for ( datadef_vec_iter dvi = parameters.begin(); dvi != parameters.end(); ++dvi )
	if ( !s.compare((*dvi)->name) )
	    return *dvi;

    return NULL;
}

Variable *TokenCpnd::getParameter(unsigned int i)
{
    DBG(cout << "TokenCpnd::getParameter(" << i << ") method: " << (method ? method->returns.name : "NULL") << endl);
    return method ? method->getParameter(i) : NULL;
}

Variable *TokenCpnd::findParameter(std::string &id)
{
    DBG(cout << "TokenCpnd::findParameter(" << id << ") method: " << (method ? method->returns.name : "NULL") << endl);
    return method ? method->findParameter(id) : NULL;
}

// recursively search for local variables up the codeblock
Variable *TokenCpnd::findVariable(std::string &id)
{
    DBG(cout << "TokenCpnd::findVariable(" << id << ") method: " << (method ? method->returns.name : "NULL") << endl);
    for ( variable_vec_iter vvi = variables.begin(); vvi != variables.end(); ++vvi )
	if ( !id.compare((*vvi)->name) )
	    return *vvi;
    if ( parent )
	return parent->findVariable(id);

    return NULL;
}

Variable *Method::findVariable(std::string &s)
{
    for ( variable_vec_iter vvi = variables.begin(); vvi != variables.end(); ++vvi )
	if ( !s.compare((*vvi)->name) )
	    return *vvi;

    return NULL;
}

Variable *Method::findParameter(std::string &s)
{
    for ( variable_vec_iter vvi = parameters.begin(); vvi != parameters.end(); ++vvi )
	if ( !s.compare((*vvi)->name) )
	    return *vvi;

    return NULL;
}

void printinteger(int i)
{
    std::cout << i << std::endl;
}

void printuinteger(uint64_t i)
{
    std::cout << i << std::endl;
}

// some debugging functions
void printdouble(double d)
{
    std::cout << std::setprecision(16) << d << std::endl;
}

// some debugging functions
void printfloat(float f)
{
    std::cout << std::setprecision(8) << f << std::endl;
}

int64_t madc_dlopen(void *filename)
{
    const char *fn = (const char *)filename;
    void *handle = dlopen(fn, RTLD_LAZY);
    if ( !handle )
	std::cerr << "dlopen: " << dlerror() << std::endl;
    return (int64_t)handle;
}

int64_t madc_dlsym(int64_t handle, void *name)
{
    const char *n = (const char *)name;
    void *sym = dlsym((void *)handle, n);
    if ( !sym )
	std::cerr << "dlsym: " << dlerror() << std::endl;
    return (int64_t)sym;
}

void madc_dlclose(int64_t handle)
{
    if ( handle )
	dlclose((void *)handle);
}

int64_t madc_system(void *cmd)
{
    return (int64_t)system((const char *)cmd);
}

void *madc_getenv(void *result, void *name)
{
    std::string &out = *(std::string *)result;
    const char *value = getenv((const char *)name);
    out = value ? value : "";
    return result;
}

const char *madc_get_argv(int64_t argv_ptr, int64_t index)
{
    char **argv = (char **)argv_ptr;
    return argv[index];
}

// Extern "C" wrapper so transpiled code (c2mir) can resolve get_argv via dlsym
extern "C" const char *get_argv(int64_t argv_ptr, int64_t index)
{
    return madc_get_argv(argv_ptr, index);
}

extern "C" void *__madc_getenv(void *result, void *name)
{
    return madc_getenv(result, name);
}

void madc_setenv(void *name, void *value)
{
    setenv((const char *)name, (const char *)value, 1);
}

void madc_unsetenv(void *name)
{
    unsetenv((const char *)name);
}

static void register_std_namespace_spec(Program &pgm)
{
    pgm.add_namespaces();
}

static void register_madc_namespace_spec(Program &pgm)
{
    pgm.add_madc_namespace();
}

int MadcTeeBuf::overflow(int ch)
{
    if ( ch == EOF )
	return sync() == 0 ? 0 : EOF;

    int primary_result = primary ? primary->sputc((char)ch) : ch;
    int secondary_result = secondary ? secondary->sputc((char)ch) : ch;
    if ( (primary && primary_result == EOF) || (secondary && secondary_result == EOF) )
	return EOF;
    return ch;
}

std::streamsize MadcTeeBuf::xsputn(const char *s, std::streamsize n)
{
    std::streamsize primary_written = primary ? primary->sputn(s, n) : n;
    std::streamsize secondary_written = secondary ? secondary->sputn(s, n) : n;
    return primary_written < secondary_written ? primary_written : secondary_written;
}

int MadcTeeBuf::sync()
{
    int primary_sync = primary ? primary->pubsync() : 0;
    int secondary_sync = secondary ? secondary->pubsync() : 0;
    return (primary_sync == 0 && secondary_sync == 0) ? 0 : -1;
}

Program::Program()
    : _braces(0),
      _prv_token(NULL),
      _cur_token(NULL),
      engine(NULL),
      runtime_scope_prev(NULL),
      input_stream(&std::cin),
      output_stream(&std::cout),
      error_stream(&std::cerr),
      expression_context_root(NULL),
      tkProgram(NULL),
      tkFunction(NULL),
      script_argc(0),
      script_argv(NULL),
      _include_iostream(false),
      _include_stdio(false),
      _include_string(false),
      colors(false),
      language_std(STD_MADC),
      aot_tracking(false),
      instrument_functions(false),
      skip_includes(false),
      root_fn(NULL)
{
}

Program::Program(MadcEngine *eng)
    : _braces(0),
      _prv_token(NULL),
      _cur_token(NULL),
      engine(NULL),
      runtime_scope_prev(NULL),
      input_stream(&std::cin),
      output_stream(&std::cout),
      error_stream(&std::cerr),
      expression_context_root(NULL),
      tkProgram(NULL),
      tkFunction(NULL),
      script_argc(0),
      script_argv(NULL),
      _include_iostream(false),
      _include_stdio(false),
      _include_string(false),
      colors(false),
      language_std(STD_MADC),
      aot_tracking(false),
      instrument_functions(false),
      skip_includes(false),
      root_fn(NULL)
{
    attach_engine(eng);
}

bool Program::set_language_standard(const std::string &standard)
{
    if ( standard == "madc" )
	language_std = STD_MADC;
    else if ( standard == "c78" )
	language_std = STD_C78;
    else if ( standard == "c86" )
	language_std = STD_C86;
    else if ( standard == "c88" )
	language_std = STD_C88;
    else if ( standard == "c89" || standard == "c90" )
	language_std = STD_C89;
    else if ( standard == "c94" )
	language_std = STD_C94;
    else if ( standard == "c95" )
	language_std = STD_C95;
    else if ( standard == "c99" )
	language_std = STD_C99;
    else if ( standard == "c" || standard == "c11" )
	language_std = STD_C11;
    else if ( standard == "c17" )
	language_std = STD_C17;
    else if ( standard == "c23" )
	language_std = STD_C23;
    else if ( standard == "c++98" || standard == "cpp98" )
	language_std = STD_CPP98;
    else if ( standard == "c++03" || standard == "cpp03" )
	language_std = STD_CPP03;
    else if ( standard == "c++" || standard == "cpp"
	   || standard == "c++11" || standard == "cpp11" )
	language_std = STD_CPP11;
    else if ( standard == "c++14" || standard == "cpp14" )
	language_std = STD_CPP14;
    else if ( standard == "c++17" || standard == "cpp17" )
	language_std = STD_CPP17;
    else if ( standard == "c++20" || standard == "cpp20" )
	language_std = STD_CPP20;
    else if ( standard == "c++23" || standard == "cpp23" )
	language_std = STD_CPP23;
    else if ( standard == "c++26" || standard == "cpp26" )
	language_std = STD_CPP26;
    else
	return false;

    return true;
}

bool Program::set_language_standard_option(const std::string &arg)
{
    const std::string prefix("--std=");
    if ( arg.compare(0, prefix.size(), prefix) != 0 )
	return false;
    return set_language_standard(arg.substr(prefix.size()));
}

void Program::attach_engine(MadcEngine *eng)
{
    engine = eng;
    if ( !engine )
	return;
    engine->populate_default_registries();
    registration_policy = engine->registration_policy;
    builtin_registry = engine->builtin_registry;
    namespace_registry = engine->namespace_registry;
}

bool Program::is_builtin_disabled(const std::string &name) const
{
    return disabled_builtin_names.count(name) != 0;
}

void Program::clear_error()
{
    last_error = ErrorInfo();
}

std::istream &Program::input()
{
    if ( engine )
	return engine->input();
    return input_stream ? *input_stream : std::cin;
}

std::ostream &Program::output()
{
    if ( engine )
	return engine->output();
    return output_stream ? *output_stream : std::cout;
}

std::ostream &Program::error()
{
    if ( engine )
	return engine->error();
    return error_stream ? *error_stream : std::cerr;
}

void Program::clear_diagnostics()
{
    diagnostics.clear();
}

const Program::Diagnostic *Program::last_diagnostic() const
{
    if ( diagnostics.empty() )
	return NULL;
    return &diagnostics.back();
}

void Program::add_diagnostic(DiagnosticSeverity severity, DiagnosticPhase phase, const std::string &message, const char *file, int line, int column)
{
    Diagnostic diag;
    diag.severity = severity;
    diag.phase = phase;
    diag.message = message;
    diag.file = file ? file : "";
    diag.line = line;
    diag.column = column;
    diagnostics.push_back(diag);
}

void Program::report_warning(DiagnosticPhase phase, const std::string &message, const char *file, int line, int column)
{
    add_diagnostic(DiagnosticSeverity::warning, phase, message, file, line, column);
}

void Program::report_error(DiagnosticPhase phase, const std::string &message, const char *file, int line, int column)
{
    add_diagnostic(DiagnosticSeverity::error, phase, message, file, line, column);
}

void Program::set_error(DiagnosticPhase phase, const std::string &message, const char *file, int line, int column)
{
    report_error(phase, message, file, line, column);
    last_error.has_error = true;
    last_error.message = message;
    last_error.file = file ? file : "";
    last_error.line = line;
    last_error.column = column;
}

void Program::set_error(const std::string &message, const char *file, int line, int column)
{
    set_error(DiagnosticPhase::unknown, message, file, line, column);
}

const char *Program::diagnostic_severity_name(DiagnosticSeverity severity) const
{
    switch ( severity )
    {
	case DiagnosticSeverity::warning: return "warning";
	case DiagnosticSeverity::error:   return "error";
    }
    return "diagnostic";
}

const char *Program::diagnostic_phase_name(DiagnosticPhase phase) const
{
    switch ( phase )
    {
	case DiagnosticPhase::lexer:    return "lexer";
	case DiagnosticPhase::parser:   return "parser";
	case DiagnosticPhase::compiler: return "compiler";
	case DiagnosticPhase::runtime:  return "runtime";
	case DiagnosticPhase::unknown:  return "unknown";
    }
    return "unknown";
}

bool Program::can_show_diagnostic_source(const Diagnostic &diag) const
{
    return !diag.file.empty()
	&& diag.line > 0
	&& diag.column > 0
	&& source.fname()
	&& diag.file == source.fname();
}

void Program::print_diagnostic(std::ostream &os, const Diagnostic &diag, const char *suffix)
{
    if ( !diag.file.empty() )
	os << ANSI_WHITE << diag.file << ':' << diag.line << ':' << diag.column;
    else
	os << ANSI_WHITE << ':';
    os << ": \e[1;31m" << diagnostic_severity_name(diag.severity)
       << ":\e[1;37m " << diag.message;
    if ( suffix && *suffix )
	os << ' ' << suffix;
    os << ANSI_RESET << std::endl;
    if ( can_show_diagnostic_source(diag) )
	source.showerror(diag.line, diag.column);
}

void Program::print_last_diagnostic(std::ostream &os, const char *suffix)
{
    const Diagnostic *diag = last_diagnostic();
    if ( diag )
	print_diagnostic(os, *diag, suffix);
}

MadcEngine::MadcEngine()
    : input_stream(&std::cin),
      output_stream(&std::cout),
      error_stream(&std::cerr),
      default_input_buf(std::cin.rdbuf()),
      default_output_buf(std::cout.rdbuf()),
      default_error_buf(std::cerr.rdbuf()),
      log_timestamps(false),
      log_level_prefixes(true),
      log_threshold(LogLevel::debug),
      log_to_error_stream(true),
      syslog_active(false),
      syslog_ident("madc"),
      syslog_option(-1),
      syslog_facility(-1),
      file_sink_active(false),
      log_file_max_bytes(0),
      log_file_max_files(5),
      json_sink_active(false)
{
}

MadcEngine::~MadcEngine()
{
    reset_standard_streams();
    disable_file_sink();
    disable_json_sink();
    disable_syslog_sink();
}

std::istream &MadcEngine::input()
{
    return input_stream ? *input_stream : std::cin;
}

std::ostream &MadcEngine::output()
{
    return output_stream ? *output_stream : std::cout;
}

std::ostream &MadcEngine::error()
{
    return error_stream ? *error_stream : std::cerr;
}

void MadcEngine::bind_input_stream(std::istream &is)
{
    input_stream = &is;
    std::cin.rdbuf(is.rdbuf());
}

void MadcEngine::bind_output_stream(std::ostream &os)
{
    output_stream = &os;
    std::cout.rdbuf(os.rdbuf());
}

void MadcEngine::bind_error_stream(std::ostream &os)
{
    error_stream = &os;
    std::cerr.rdbuf(os.rdbuf());
}

void MadcEngine::bind_input_string(const std::string &text)
{
    owned_input_buffer.reset(new std::istringstream(text));
    bind_input_stream(*owned_input_buffer);
}

void MadcEngine::capture_output_to_buffer()
{
    owned_output_buffer.reset(new std::ostringstream());
    bind_output_stream(*owned_output_buffer);
}

void MadcEngine::capture_error_to_buffer()
{
    owned_error_buffer.reset(new std::ostringstream());
    bind_error_stream(*owned_error_buffer);
}

void MadcEngine::tee_output_stream(std::ostream &os)
{
    output_tee_buf.reset(new MadcTeeBuf(std::cout.rdbuf(), os.rdbuf()));
    output_stream = &std::cout;
    std::cout.rdbuf(output_tee_buf.get());
}

void MadcEngine::tee_error_stream(std::ostream &os)
{
    error_tee_buf.reset(new MadcTeeBuf(std::cerr.rdbuf(), os.rdbuf()));
    error_stream = &std::cerr;
    std::cerr.rdbuf(error_tee_buf.get());
}

void MadcEngine::tee_output_to_buffer()
{
    owned_output_buffer.reset(new std::ostringstream());
    tee_output_stream(*owned_output_buffer);
}

void MadcEngine::tee_error_to_buffer()
{
    owned_error_buffer.reset(new std::ostringstream());
    tee_error_stream(*owned_error_buffer);
}

const char *MadcEngine::log_level_name(LogLevel level) const
{
    switch ( level )
    {
	case LogLevel::emerg:  return "emerg";
	case LogLevel::alert:  return "alert";
	case LogLevel::crit:   return "crit";
	case LogLevel::err:    return "err";
	case LogLevel::warn:   return "warn";
	case LogLevel::notice: return "notice";
	case LogLevel::info:   return "info";
	case LogLevel::debug:  return "debug";
    }
    return "info";
}

std::string MadcEngine::format_log_message(LogLevel level, const std::string &message) const
{
    std::ostringstream os;
    if ( log_timestamps )
    {
	time_t now = time(NULL);
	struct tm tm_now;
	localtime_r(&now, &tm_now);
	char tsbuf[32];
	strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", &tm_now);
	os << tsbuf << ' ';
    }
    if ( log_level_prefixes )
	os << '[' << log_level_name(level) << "] ";
    os << message;
    return os.str();
}

bool MadcEngine::should_log(LogLevel level) const
{
    return static_cast<int>(level) <= static_cast<int>(log_threshold);
}

void MadcEngine::write_log(LogLevel level, const std::string &message)
{
    if ( !should_log(level) )
	return;
    if ( log_to_error_stream )
	error() << format_log_message(level, message) << std::endl;
    write_builtin_sinks(level, message);
    for ( auto &sink : log_sinks )
    {
	if ( sink )
	    sink(level, message);
    }
}

void MadcEngine::write_builtin_sinks(LogLevel level, const std::string &message)
{
    write_syslog_sink(level, message);
    write_file_sink(level, message);
    write_json_sink(level, message);
}

void MadcEngine::add_log_sink(LogSink sink)
{
    if ( sink )
	log_sinks.push_back(std::move(sink));
}

void MadcEngine::clear_log_sinks()
{
    log_sinks.clear();
}

int MadcEngine::syslog_priority_for(LogLevel level)
{
    switch ( level )
    {
	case LogLevel::emerg:  return LOG_EMERG;
	case LogLevel::alert:  return LOG_ALERT;
	case LogLevel::crit:   return LOG_CRIT;
	case LogLevel::err:    return LOG_ERR;
	case LogLevel::warn:   return LOG_WARNING;
	case LogLevel::notice: return LOG_NOTICE;
	case LogLevel::info:   return LOG_INFO;
	case LogLevel::debug:  return LOG_DEBUG;
    }
    return LOG_INFO;
}

void MadcEngine::enable_syslog_sink(const char *ident, int option, int facility)
{
    if ( syslog_active )
	disable_syslog_sink();
    int resolved_option   = (option   < 0) ? LOG_PID  : option;
    int resolved_facility = (facility < 0) ? LOG_USER : facility;
    openlog(ident, resolved_option, resolved_facility);
    syslog_active = true;
    syslog_ident = ident ? ident : "madc";
    syslog_option = resolved_option;
    syslog_facility = resolved_facility;
}

void MadcEngine::disable_syslog_sink()
{
    if ( !syslog_active )
	return;
    syslog_active = false;
    closelog();
}

void MadcEngine::write_syslog_sink(LogLevel level, const std::string &message)
{
    if ( syslog_active )
	::syslog(syslog_priority_for(level), "%s", message.c_str());
}

bool MadcEngine::enable_file_sink(const std::string &path,
				  size_t max_bytes,
				  int max_files)
{
    if ( file_sink_active )
	disable_file_sink();
    log_file.reset(new std::ofstream(path.c_str(), std::ios::app));
    if ( !log_file->is_open() )
    {
	log_file.reset();
	return false;
    }
    file_sink_active = true;
    log_file_path = path;
    log_file_max_bytes = max_bytes;
    log_file_max_files = max_files < 1 ? 1 : max_files;
    return true;
}

void MadcEngine::disable_file_sink()
{
    if ( !file_sink_active )
	return;
    file_sink_active = false;
    if ( log_file )
    {
	log_file->flush();
	log_file->close();
	log_file.reset();
    }
    log_file_path.clear();
    log_file_max_bytes = 0;
}

void MadcEngine::rotate_log_file()
{
    if ( log_file_path.empty() )
	return;
    if ( log_file )
    {
	log_file->flush();
	log_file->close();
    }
    for ( int i = log_file_max_files; i >= 2; --i )
    {
	std::string from = log_file_path + "." + std::to_string(i - 1);
	std::string to   = log_file_path + "." + std::to_string(i);
	::rename(from.c_str(), to.c_str());
    }
    std::string first = log_file_path + ".1";
    ::rename(log_file_path.c_str(), first.c_str());
    log_file.reset(new std::ofstream(log_file_path.c_str(), std::ios::app));
    if ( !log_file->is_open() )
    {
	log_file.reset();
	file_sink_active = false;
    }
}

void MadcEngine::reopen_log_file()
{
    if ( log_file_path.empty() )
	return;
    if ( log_file )
    {
	log_file->flush();
	log_file->close();
    }
    log_file.reset(new std::ofstream(log_file_path.c_str(), std::ios::app));
    if ( !log_file->is_open() )
    {
	log_file.reset();
	file_sink_active = false;
    }
}

void MadcEngine::write_file_sink(LogLevel level, const std::string &message)
{
    if ( !(file_sink_active && log_file && log_file->is_open()) )
	return;
    const std::string formatted = format_log_message(level, message);
    if ( log_file_max_bytes > 0 )
    {
	log_file->flush();
	std::streampos pos = log_file->tellp();
	if ( pos != std::streampos(-1) &&
	     static_cast<size_t>(pos) + formatted.size() + 1 > log_file_max_bytes )
	    rotate_log_file();
    }
    if ( file_sink_active && log_file && log_file->is_open() )
	(*log_file) << formatted << std::endl;
}

std::string MadcEngine::json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for ( char c : s )
    {
	switch ( c )
	{
	    case '"':  out += "\\\""; break;
	    case '\\': out += "\\\\"; break;
	    case '\b': out += "\\b";  break;
	    case '\f': out += "\\f";  break;
	    case '\n': out += "\\n";  break;
	    case '\r': out += "\\r";  break;
	    case '\t': out += "\\t";  break;
	    default:
		if ( static_cast<unsigned char>(c) < 0x20 )
		{
		    char buf[8];
		    snprintf(buf, sizeof(buf), "\\u%04x", c);
		    out += buf;
		}
		else
		    out += c;
		break;
	}
    }
    return out;
}

std::string MadcEngine::format_json_log_line(LogLevel level, const std::string &message) const
{
    std::ostringstream os;
    os << "{";
    if ( log_timestamps )
    {
	time_t now = time(NULL);
	struct tm tm_now;
	localtime_r(&now, &tm_now);
	char tsbuf[32];
	strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%dT%H:%M:%S", &tm_now);
	os << "\"ts\":\"" << tsbuf << "\",";
    }
    os << "\"level\":\"" << log_level_name(level) << "\",";
    os << "\"message\":\"" << json_escape(message) << "\"";
    os << "}";
    return os.str();
}

bool MadcEngine::enable_json_sink(const std::string &path)
{
    if ( json_sink_active )
	disable_json_sink();
    json_file.reset(new std::ofstream(path.c_str(), std::ios::app));
    if ( !json_file->is_open() )
    {
	json_file.reset();
	return false;
    }
    json_sink_active = true;
    json_file_path = path;
    return true;
}

void MadcEngine::disable_json_sink()
{
    if ( !json_sink_active )
	return;
    json_sink_active = false;
    if ( json_file )
    {
	json_file->flush();
	json_file->close();
	json_file.reset();
    }
    json_file_path.clear();
}

void MadcEngine::write_json_sink(LogLevel level, const std::string &message)
{
    if ( json_sink_active && json_file && json_file->is_open() )
	(*json_file) << format_json_log_line(level, message) << '\n';
}

bool MadcEngine::apply_log_config(const Config &cfg)
{
    log_threshold       = cfg.threshold;
    log_timestamps      = cfg.timestamps;
    log_level_prefixes  = cfg.level_prefixes;
    log_to_error_stream = cfg.error_stream;

    bool ok = true;

    if ( cfg.file_sink )
    {
	if ( !enable_file_sink(cfg.file_path, cfg.file_max_bytes, cfg.file_max_files) )
	    ok = false;
    }
    else
	disable_file_sink();

    if ( cfg.syslog_sink )
	enable_syslog_sink(cfg.syslog_ident.c_str(), cfg.syslog_option, cfg.syslog_facility);
    else
	disable_syslog_sink();

    if ( cfg.json_sink )
    {
	if ( !enable_json_sink(cfg.json_path) )
	    ok = false;
    }
    else
	disable_json_sink();

    return ok;
}

bool MadcEngine::has_output_buffer() const
{
    return owned_output_buffer.get() != NULL;
}

bool MadcEngine::has_error_buffer() const
{
    return owned_error_buffer.get() != NULL;
}

std::string MadcEngine::output_buffer_str() const
{
    return owned_output_buffer ? owned_output_buffer->str() : "";
}

std::string MadcEngine::error_buffer_str() const
{
    return owned_error_buffer ? owned_error_buffer->str() : "";
}

void MadcEngine::clear_output_buffer()
{
    if ( owned_output_buffer )
    {
	owned_output_buffer->str("");
	owned_output_buffer->clear();
    }
}

void MadcEngine::clear_error_buffer()
{
    if ( owned_error_buffer )
    {
	owned_error_buffer->str("");
	owned_error_buffer->clear();
    }
}

void MadcEngine::reset_standard_streams()
{
    input_stream = &std::cin;
    output_stream = &std::cout;
    error_stream = &std::cerr;
    if ( default_input_buf )
	std::cin.rdbuf(default_input_buf);
    if ( default_output_buf )
	std::cout.rdbuf(default_output_buf);
    if ( default_error_buf )
	std::cerr.rdbuf(default_error_buf);
    output_tee_buf.reset();
    error_tee_buf.reset();
    owned_input_buffer.reset();
    owned_output_buffer.reset();
    owned_error_buffer.reset();
}

void MadcEngine::populate_default_registries()
{
    if ( builtin_registry.defaults_loaded && namespace_registry.defaults_loaded )
	return;

    Program seed;
    seed.populate_builtin_registry();
    seed.populate_namespace_registry();
    builtin_registry = seed.builtin_registry;
    namespace_registry = seed.namespace_registry;
}

void MadcEngine::configure_program(Program &pgm) const
{
    pgm.engine = const_cast<MadcEngine *>(this);
    pgm.input_stream = input_stream;
    pgm.output_stream = output_stream;
    pgm.error_stream = error_stream;
    pgm.registration_policy = registration_policy;
    pgm.builtin_registry = builtin_registry;
    pgm.namespace_registry = namespace_registry;
}

std::unique_ptr<Program> MadcEngine::create_program()
{
    populate_default_registries();
    std::unique_ptr<Program> pgm(new Program());
    configure_program(*pgm);
    return pgm;
}

void MadcEngine::bind_log_streams()
{
    madc::emerg.set_engine(this);
    madc::alert.set_engine(this);
    madc::crit.set_engine(this);
    madc::err.set_engine(this);
    madc::warn.set_engine(this);
    madc::notice.set_engine(this);
    madc::info.set_engine(this);
    madc::debug.set_engine(this);
}

void MadcEngine::unbind_log_streams()
{
    madc::emerg.set_engine(NULL);
    madc::alert.set_engine(NULL);
    madc::crit.set_engine(NULL);
    madc::err.set_engine(NULL);
    madc::warn.set_engine(NULL);
    madc::notice.set_engine(NULL);
    madc::info.set_engine(NULL);
    madc::debug.set_engine(NULL);
}

MadcLogStreambuf::MadcLogStreambuf(MadcEngine::LogLevel lvl)
    : _level(lvl), _engine(NULL)
{
}

void MadcLogStreambuf::flush_line()
{
    if ( _line.empty() )
	return;
    if ( _engine )
	_engine->write_log(_level, _line);
    else
    {
	MadcEngine fallback;
	std::cerr << fallback.format_log_message(_level, _line) << std::endl;
    }
    _line.clear();
}

int MadcLogStreambuf::overflow(int ch)
{
    if ( _engine && !_engine->should_log(_level) )
    {
	if ( !_line.empty() )
	    _line.clear();
	return ch == EOF ? 0 : ch;
    }
    if ( ch == EOF )
    {
	flush_line();
	return 0;
    }
    if ( ch == '\n' )
	flush_line();
    else
	_line.push_back((char)ch);
    return ch;
}

std::streamsize MadcLogStreambuf::xsputn(const char *s, std::streamsize n)
{
    if ( _engine && !_engine->should_log(_level) )
	return n;
    for ( std::streamsize i = 0; i < n; ++i )
    {
	if ( s[i] == '\n' )
	    flush_line();
	else
	    _line.push_back(s[i]);
    }
    return n;
}

int MadcLogStreambuf::sync()
{
    flush_line();
    return 0;
}

MadcLogStream::MadcLogStream(MadcEngine::LogLevel lvl)
    : std::ostream(&_buf), _buf(lvl)
{
}

namespace madc
{
    MadcLogStream emerg(MadcEngine::LogLevel::emerg);
    MadcLogStream alert(MadcEngine::LogLevel::alert);
    MadcLogStream crit(MadcEngine::LogLevel::crit);
    MadcLogStream err(MadcEngine::LogLevel::err);
    MadcLogStream warn(MadcEngine::LogLevel::warn);
    MadcLogStream notice(MadcEngine::LogLevel::notice);
    MadcLogStream info(MadcEngine::LogLevel::info);
    MadcLogStream debug(MadcEngine::LogLevel::debug);
}

bool Program::load_file(const char *fname)
{
    TokenProgram *tp = tokenize(fname);
    if ( !tp )
	return false;
    if ( !parse(tp) )
	return false;
    return compile();
}

bool Program::load_buffer(const std::string &source_text,
			  const std::string &display_name)
{
    TokenProgram *tp = tokenize_buffer(source_text, display_name);
    if ( !tp )
	return false;
    if ( !parse(tp) )
	return false;
    return compile();
}

void Program::BuiltinRegistry::add_core_function(const std::string &id, const datatype_vec_t &params, fVOIDFUNC extfunc, bool is_method)
{
    core_functions.push_back({id, params, extfunc, is_method});
}

void Program::BuiltinRegistry::add_process_function(const std::string &id, const datatype_vec_t &params, fVOIDFUNC extfunc, bool is_method)
{
    process_functions.push_back({id, params, extfunc, is_method});
}

void Program::BuiltinRegistry::add_dlfcn_function(const std::string &id, const datatype_vec_t &params, fVOIDFUNC extfunc, bool is_method)
{
    dlfcn_functions.push_back({id, params, extfunc, is_method});
}

void Program::NamespaceRegistry::add_namespace(const std::string &name, namespace_init_fn_t init)
{
    specs.push_back({name, init});
}

void Program::populate_builtin_registry()
{
    if ( builtin_registry.defaults_loaded )
	return;

    builtin_registry.add_core_function("puts",	 datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtCHAR)}, (fVOIDFUNC)puts);
    builtin_registry.add_core_function("printstr", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtCHAR)}, (fVOIDFUNC)NULL);
    builtin_registry.add_core_function("puti",	 datatype_vec_t{DataType::dtVOID, DataType::dtINT}, (fVOIDFUNC)printinteger);
    builtin_registry.add_core_function("putu",	 datatype_vec_t{DataType::dtVOID, DataType::dtUINT64}, (fVOIDFUNC)printuinteger);
    builtin_registry.add_core_function("putd",	 datatype_vec_t{DataType::dtVOID, DataType::dtDOUBLE}, (fVOIDFUNC)printdouble);
    builtin_registry.add_core_function("putf",	 datatype_vec_t{DataType::dtVOID, DataType::dtFLOAT}, (fVOIDFUNC)printfloat);
    builtin_registry.add_core_function("putchar", datatype_vec_t{DataType::dtINT,  DataType::dtINT}, (fVOIDFUNC)putchar);
    builtin_registry.add_core_function("__builtin_memcpy", datatype_vec_t{rtPtr(DataType::dtVOID), rtPtr(DataType::dtVOID), rtPtr(DataType::dtVOID), DataType::dtUINT64}, (fVOIDFUNC)memcpy);
    builtin_registry.add_core_function("__builtin_frame_address", datatype_vec_t{rtPtr(DataType::dtCHAR), DataType::dtINT}, (fVOIDFUNC)NULL);
    // __builtin_va_start(ap): c2mir intrinsic (lowered to MIR_VA_START), no real
    // symbol — register with a NULL pointer like __builtin_frame_address so the
    // call parses; cir_builder emits N_CALL(__builtin_va_start, ap) and c2mir
    // intrinsic-lowers it. The arg is the user's va_list (array -> pointer).
    builtin_registry.add_core_function("__builtin_va_start", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtVOID)}, (fVOIDFUNC)NULL);
    // __destroy(ptr): compiler intrinsic that destructs the pointed-to object.
    // No real symbol (NULL pointer, like __builtin_va_start) — the parser
    // accepts the call and cir_builder lowers it to the element type's class
    // destructor (mangled ~basic_string for std::string, Cls___dtor for a
    // user class), or to nothing for a scalar/pointer element type. Used by
    // the std:: container headers (<vector>/<map>/<set>) to destruct live
    // elements before free(); generic, element-type-driven, never string-
    // special-cased. See CirBuilder::translate_expr's __destroy handling.
    builtin_registry.add_core_function("__destroy", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtVOID)}, (fVOIDFUNC)NULL);
    // alloca() is a compiler intrinsic, not a real libc function.
    // Map to malloc for now (true stack alloca needs JIT intrinsic support).
    builtin_registry.add_core_function("alloca", datatype_vec_t{rtPtr(DataType::dtVOID), DataType::dtUINT64}, (fVOIDFUNC)malloc);
    builtin_registry.add_core_function("__builtin_memset", datatype_vec_t{rtPtr(DataType::dtVOID), rtPtr(DataType::dtVOID), DataType::dtINT, DataType::dtUINT64}, (fVOIDFUNC)memset);
    // copysign family: GCC provides these as always-available builtins (no
    // <math.h> required). The lexer maps __builtin_copysign* -> the bare libm
    // name, but without an <math.h> include the bare name would resolve via the
    // dlsym fallback with a long return — so a properly-signed double result
    // (returned in xmm0) is read from the integer register as garbage. Register
    // the real double/float/long-double signatures here so `__builtin_copysign`
    // works without an explicit include, matching GCC.
    builtin_registry.add_core_function("copysign",  datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE, DataType::dtDOUBLE}, (fVOIDFUNC)(double(*)(double,double))copysign);
    builtin_registry.add_core_function("copysignf", datatype_vec_t{DataType::dtFLOAT, DataType::dtFLOAT, DataType::dtFLOAT}, (fVOIDFUNC)(float(*)(float,float))copysignf);
    builtin_registry.add_core_function("copysignl", datatype_vec_t{DataType::dtLDOUBLE, DataType::dtLDOUBLE, DataType::dtLDOUBLE}, (fVOIDFUNC)(long double(*)(long double,long double))copysignl);
    builtin_registry.add_process_function("system", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtCHAR)}, (fVOIDFUNC)madc_system);
    builtin_registry.add_process_function("getenv", datatype_vec_t{rtPtr(DataType::dtVOID), rtPtr(DataType::dtVOID), rtPtr(DataType::dtCHAR)}, (fVOIDFUNC)__madc_getenv);
    builtin_registry.add_process_function("get_argv", datatype_vec_t{DataType::dtCHARptr, DataType::dtINT64, DataType::dtINT64}, (fVOIDFUNC)madc_get_argv);
    builtin_registry.add_process_function("setenv", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtCHAR), rtPtr(DataType::dtCHAR)}, (fVOIDFUNC)madc_setenv);
    builtin_registry.add_process_function("unsetenv", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtCHAR)}, (fVOIDFUNC)madc_unsetenv);
    builtin_registry.add_process_function("__errno_location", datatype_vec_t{rtPtr(DataType::dtINT32)}, (fVOIDFUNC)__errno_location);

    builtin_registry.add_dlfcn_function("dlopen", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtCHAR)}, (fVOIDFUNC)madc_dlopen);
    builtin_registry.add_dlfcn_function("dlsym", datatype_vec_t{DataType::dtINT64, DataType::dtINT64, rtPtr(DataType::dtCHAR)}, (fVOIDFUNC)madc_dlsym);
    builtin_registry.add_dlfcn_function("dlclose", datatype_vec_t{DataType::dtVOID, DataType::dtINT64}, (fVOIDFUNC)madc_dlclose);
    builtin_registry.add_dlfcn_function("dlcall", datatype_vec_t{DataType::dtINT64}, (fVOIDFUNC)NULL);

    builtin_registry.defaults_loaded = true;
}

void Program::populate_namespace_registry()
{
    if ( namespace_registry.defaults_loaded )
	return;

    namespace_registry.add_namespace("std", register_std_namespace_spec);
    namespace_registry.add_namespace("madc", register_madc_namespace_spec);
    namespace_registry.defaults_loaded = true;
}

void Program::register_function_specs(const std::vector<FunctionRegistrationSpec> &specs)
{
    for ( std::vector<FunctionRegistrationSpec>::const_iterator it = specs.begin(); it != specs.end(); ++it )
	addFunction(it->id, it->params, it->extfunc, it->is_method);
}

void Program::add_core_functions()
{
    register_function_specs(builtin_registry.core_functions);
}

void Program::add_process_functions()
{
    // glibc's errno is a 4-byte int. Registering this as int*-to-int64 made
    // madc's `*p` deref read 8 bytes and silently broke errno comparisons.
    for ( std::vector<FunctionRegistrationSpec>::const_iterator it = builtin_registry.process_functions.begin();
	  it != builtin_registry.process_functions.end(); ++it )
    {
	Variable *var = addFunction(it->id, it->params, it->extfunc, it->is_method);
	if ( var && it->id == "getenv" )
	{
	    FuncDef *fd = dynamic_cast<FuncDef *>(var->type);
	    if ( fd )
	    {
		fd->nested_emit_name = "__madc_getenv";
		funcdef_map[fd->nested_emit_name] = fd;
	    }
	}
    }
}

void Program::add_dlfcn_functions()
{
    for ( std::vector<FunctionRegistrationSpec>::const_iterator it = builtin_registry.dlfcn_functions.begin();
	  it != builtin_registry.dlfcn_functions.end(); ++it )
    {
	if ( !is_dynamic_symbol_allowed(it->id) )
	    continue;
	addFunction(it->id, it->params, it->extfunc, it->is_method);
    }
}

void Program::register_namespace_specs()
{
    for ( std::vector<NamespaceRegistrationSpec>::const_iterator it = namespace_registry.specs.begin(); it != namespace_registry.specs.end(); ++it )
    {
	if ( !is_namespace_registration_enabled(it->name) )
	    continue;
	if ( it->init )
	    it->init(*this);
    }
}

void Program::ensure_registration_config()
{
    if ( engine )
	return;

    populate_builtin_registry();
    populate_namespace_registry();
}

// add system library functions
void Program::add_functions()
{
    if ( registration_policy.enable_core_functions )
	add_core_functions();
    if ( registration_policy.enable_process_functions )
	add_process_functions();
    if ( registration_policy.enable_dlfcn_functions )
	add_dlfcn_functions();
}

// define some global variables
void Program::add_globals()
{
    // The release version is exposed as the built-in MADC_VERSION macro (a
    // const char* string literal substituted by the lexer's define_map), not a
    // std::string global — see lexer.cpp. Nothing to register here yet.
}

enum { LAZY_STDIO = 2, LAZY_MATHH = 3 };

// populates lazy_map — symbols are registered on first use via lazy_resolve()
void Program::add_iostream()
{
}

void Program::add_stdio()
{
    // printf family available via dlsym fallback (libc is always loaded).
    // Register stdin/stdout/stderr for lazy resolution — each becomes an
    // int64 global whose data holds libc's current FILE* value.
    lazy_map["stdin"]  = {LAZY_STDIO, Program::lkVariable};
    lazy_map["stdout"] = {LAZY_STDIO, Program::lkVariable};
    lazy_map["stderr"] = {LAZY_STDIO, Program::lkVariable};
}

// on-demand variable/function registration — called from parseExpression()
Variable *Program::lazy_resolve(const std::string &name)
{
    std::map<std::string, LazyEntry>::iterator it = lazy_map.find(name);
    if ( it == lazy_map.end() )
	return NULL;
    if ( it->second.kind != lkVariable && it->second.kind != lkFunction )
	return NULL; // not a variable/function — leave for lazy_resolve_type

    Variable *var = NULL;
    int header = it->second.header;
    lazy_map.erase(it);

    if ( header == LAZY_STDIO )
    {
	// dlsym the libc symbol and copy the current FILE* value into our
	// backing slot. Note: dlsym("stderr") returns the address of the
	// libc `FILE *stderr;` variable — one deref yields the FILE*.
	void **sym = NULL;
	if ( name == "stdin" || name == "stdout" || name == "stderr" )
	    sym = (void **)dlsym(RTLD_DEFAULT, name.c_str());
	if ( sym )
	{
	    var = addGlobal(ddINT64, name, 1, NULL);
	    if ( var && var->data )
		*(void **)var->data = *sym;
	}
    }

    DBG(if (var) std::cout << "lazy_resolve(" << name << ") registered" << std::endl);
    return var;
}

// on-demand type/struct registration — called from type lookup paths
DataDef *Program::lazy_resolve_type(const std::string &name)
{
    std::map<std::string, LazyEntry>::iterator it = lazy_map.find(name);
    if ( it == lazy_map.end() )
	return NULL;
    if ( it->second.kind != lkType && it->second.kind != lkStruct )
	return NULL;

    DataDef *dd = NULL;
    lazy_map.erase(it);

    // future: register struct layouts, typedefs from embedded headers
    // e.g. if ( header == LAZY_TIME_H && name == "time_t" ) dd = &ddINT64;

    DBG(if (dd) std::cout << "lazy_resolve_type(" << name << ") registered" << std::endl);
    return dd;
}

void Program::add_namespaces()
{
    namespace_map["std"];
}

void Program::add_madc_namespace()
{
    variable_map_t &madc_ns = namespace_map["madc"];
    Variable *var;
    DataType objp = rtPtr(DataType::dtVOID);
    DataType cstr = rtPtr(DataType::dtCHAR);

    // register array type as madc::array
    std::string id = "__madc_array";
    var = new Variable(id, ddARRAY, 1, NULL, false);
    var->flags |= vfSTATIC;
    madc_ns["array"] = var;

    var = addFunction("__madc_regex_match", datatype_vec_t{DataType::dtINT64, cstr, cstr}, (fVOIDFUNC)__madc_regex_match);
    if (var) madc_ns["regex_match"] = var;

    var = addFunction("__madc_regex_search", datatype_vec_t{DataType::dtINT64, cstr, cstr}, (fVOIDFUNC)__madc_regex_search);
    if (var) madc_ns["regex_search"] = var;

    var = addFunction("__madc_regex_replace", datatype_vec_t{objp, objp, cstr, cstr, cstr}, (fVOIDFUNC)__madc_regex_replace);
    if (var) madc_ns["regex_replace"] = var;

    DBG(std::cout << "add_madc_namespace() registered madc:: with " << madc_ns.size() << " members" << std::endl);
}

void Program::_parser_init()
{
    ensure_registration_config();
    add_functions();
    add_globals();
    // populate lazy_map for included headers (actual registration deferred to first use)
    if ( _include_iostream ) add_iostream();
    if ( _include_stdio )   add_stdio();
    register_namespace_specs();
    _braces = 0;
}

bool Program::is_namespace_registration_enabled(const std::string &name) const
{
    if ( name == "std" ) return registration_policy.enable_std_namespace;
    if ( name == "madc" ) return registration_policy.enable_madc_namespace;
    if ( name == "php" ) return registration_policy.enable_php_namespace;
    if ( name == "perl" ) return registration_policy.enable_perl_namespace;
    if ( name == "python" ) return registration_policy.enable_python_namespace;
    if ( name == "ruby" ) return registration_policy.enable_ruby_namespace;
    if ( name == "js" ) return registration_policy.enable_js_namespace;
    if ( name == "rust" ) return registration_policy.enable_rust_namespace;
    return true;
}

bool Program::is_dynamic_library_loading_enabled() const
{
    return registration_policy.enable_dlfcn_functions;
}

bool Program::is_dynamic_symbol_fallback_enabled() const
{
    return registration_policy.enable_dlfcn_functions;
}

bool Program::is_runtime_eval_source_scope_access_enabled() const
{
    return registration_policy.enable_runtime_eval_source_scope_access;
}

bool Program::is_runtime_eval_expression_scope_access_enabled() const
{
    return registration_policy.enable_runtime_eval_expression_scope_access;
}

bool Program::is_embedded_header_allowed(const std::string &name) const
{
    if ( !registration_policy.restrict_headers_to_allowlist
      && registration_policy.allowed_headers.empty() )
	return true;
    for ( std::size_t i = 0; i < registration_policy.allowed_headers.size(); ++i )
    {
	if ( registration_policy.allowed_headers[i] == name )
	    return true;
    }
    return false;
}

bool Program::is_dynamic_symbol_allowed(const std::string &name) const
{
    if ( !registration_policy.restrict_dlfcn_symbols_to_allowlist
      && registration_policy.allowed_dlfcn_symbols.empty() )
	return true;
    for ( std::size_t i = 0; i < registration_policy.allowed_dlfcn_symbols.size(); ++i )
    {
	if ( registration_policy.allowed_dlfcn_symbols[i] == name )
	    return true;
    }
    return false;
}

// Some libc symbols resolved via dlsym fallback return a pointer, not an
// int. Declaring them as dtINT64 leaves CIR emitting `long f()` and then
// assigning the result to a pointer (a spurious int-to-pointer warning,
// and a real truncation risk on any 32-bit-int return path). Map the
// known string-returning ones to dtCHARptr so the prototype is accurate.
//
// Likewise, the void / noreturn libc functions (abort, exit, free, ...)
// genuinely return `void`. Declaring them as dtINT64 makes the dlsym
// prototype `long f()`, so a comma-expression / ternary branch ending in
// such a call is typed `long` rather than `void` — e.g. the assert idiom
// `(e) ? (void)0 : (printf(...), abort())` then trips c2mir's
// "incompatible types in true and false parts of cond-expression". Map the
// known void-returning ones to dtVOID so the prototype matches GCC/libc.
static DataType dynamic_symbol_fallback_return_type(const std::string &name)
{
    static const std::set<std::string> cstr_returners = {
	"asctime", "ctime"
    };
    static const std::set<std::string> void_returners = {
	"abort", "exit", "_exit", "_Exit", "quick_exit",
	"free", "perror", "srand", "srandom",
	"__assert_fail", "__assert", "__assert_perror_fail"
    };
    if ( cstr_returners.count(name) )
	return DataType::dtCHARptr;
    if ( void_returners.count(name) )
	return DataType::dtVOID;
    return DataType::dtINT64;
}

Variable *Program::runtime_eval_scope_target(Variable *var) const
{
    if ( !var )
	return var;

    if ( is_runtime_eval_source_helper_name(var->name)
      && !is_runtime_eval_source_scope_access_enabled() )
	return var;

    if ( is_runtime_eval_expression_helper_name(var->name)
      && !is_runtime_eval_expression_scope_access_enabled() )
	return var;

    std::string target_name;
    if ( var->name == "__madc_eval_runtime" )
	target_name = "__madc_eval_ctx_runtime";
    else if ( var->name == "__madc_eval_bool_runtime" )
	target_name = "__madc_eval_bool_ctx_runtime";
    else if ( var->name == "__madc_eval_int_runtime" )
	target_name = "__madc_eval_int_ctx_runtime";
    else if ( var->name == "__madc_eval_double_runtime" )
	target_name = "__madc_eval_double_ctx_runtime";
    else if ( var->name == "__madc_eval_string_runtime" )
	target_name = "__madc_eval_string_ctx_runtime";
    else if ( var->name == "__madc_eval_expression_runtime" )
	target_name = "__madc_eval_expression_ctx_runtime";
    else if ( var->name == "__madc_eval_expression_bool_runtime" )
	target_name = "__madc_eval_expression_bool_ctx_runtime";
    else if ( var->name == "__madc_eval_expression_int_runtime" )
	target_name = "__madc_eval_expression_int_ctx_runtime";
    else if ( var->name == "__madc_eval_expression_double_runtime" )
	target_name = "__madc_eval_expression_double_ctx_runtime";
    else if ( var->name == "__madc_eval_expression_string_runtime" )
	target_name = "__madc_eval_expression_string_ctx_runtime";
    else
	return var;

    std::string lookup = target_name;
    Variable *mapped = tkProgram ? tkProgram->findVariable(lookup) : NULL;
    return mapped ? mapped : var;
}

namespace {

bool is_runtime_eval_scope_supported_variable(Variable *var)
{
    if ( !var || !var->type )
	return false;
    if ( var->type->is_function() )
	return false;
    if ( var->count != 1 || var->is_fixed_array() || var->is_vla() )
	return false;
    if ( var->name.compare(0, 7, "__madc_") == 0 )
	return false;
    if ( var->name.compare(0, 11, "__literal__") == 0 )
	return false;
    if ( is_runtime_eval_scope_helper_name(var->name) )
	return false;

    DataType raw = var->type->rawtype();
    return raw == DataType::dtBOOL
	|| var->type->is_integer()
	|| var->type->is_real()
	|| raw == DataType::dtARRAY;
}

void append_runtime_eval_scope_variable(std::vector<Variable *> &out,
					std::set<std::string> &seen,
					Variable *var)
{
    if ( !is_runtime_eval_scope_supported_variable(var) )
	return;
    if ( seen.insert(var->name).second )
	out.push_back(var);
}

} // namespace

void Program::collect_runtime_eval_scope_variables(std::vector<Variable *> &out) const
{
    out.clear();
    std::set<std::string> seen;
    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();

    for ( TokenCpnd *scope = code; scope; scope = scope->parent )
    {
	for ( variable_vec_iter it = scope->variables.begin(); it != scope->variables.end(); ++it )
	    append_runtime_eval_scope_variable(out, seen, *it);
    }

    if ( code && code->method )
    {
	for ( variable_vec_iter it = code->method->parameters.begin();
	      it != code->method->parameters.end(); ++it )
	    append_runtime_eval_scope_variable(out, seen, *it);
    }

    if ( tkProgram )
    {
	for ( variable_vec_iter it = tkProgram->variables.begin(); it != tkProgram->variables.end(); ++it )
	    append_runtime_eval_scope_variable(out, seen, *it);
    }
}

// find variable matching id anywhere accessable from codeblock
Variable *Program::findVariable(TokenCpnd *code, std::string &id)
{
    Variable *var;
    const char *debug_var = ::getenv("MADC_DEBUG_AOT_VAR");

    if ( code )
    {
	if ( (var=code->findVariable(id)) )
	{
	    if ( debug_var && id == debug_var )
		std::fprintf(stderr,
		    "[aot] findVariable local id=%s var=%p data=%p flags=%u count=%u fixed=%d code=%p\n",
		    id.c_str(), (void *)var, var ? var->data : NULL,
		    var ? (unsigned)var->flags : 0, var ? (unsigned)var->count : 0,
		    (var && var->is_fixed_array()) ? 1 : 0, (void *)code);
	    return var;
	}
	if ( (var=code->findParameter(id)) )
	    return var;
    }
    if ( !(var=tkProgram->findVariable(id)) )
    {
	DBG(std::cout << "Program::findVariable(code, " << id << ") not found" << std::endl);
	return NULL;
    }

    if ( debug_var && id == debug_var )
	std::fprintf(stderr,
	    "[aot] findVariable global id=%s var=%p data=%p flags=%u count=%u fixed=%d code=%p\n",
	    id.c_str(), (void *)var, var ? var->data : NULL,
	    var ? (unsigned)var->flags : 0, var ? (unsigned)var->count : 0,
	    (var && var->is_fixed_array()) ? 1 : 0, (void *)code);

    DBG(std::cout << "Program::findVariable(code, " << id << ") found ptr: " << var << std::endl);

    return var;
}


Variable *Program::findVariable(std::string &s)
{
    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
    variable_map_iter vmi;
    Variable *var;

    if ( code /*&& code->type() != TokenType::ttProgram*/ )
	return findVariable(code, s);

    if ( !(var=tkProgram->findVariable(s)) )
    {
	DBG(std::cout << "Program::findVariable(" << s << ") not found" << std::endl);
	return NULL;
    }

    DBG(std::cout << "Program::findVariable(" << s << ") found ptr: " << var << std::endl);

    return var;
}

bool Program::is_known_namespace(const std::string &name) const
{
    return name == "c" || namespace_map.find(name) != namespace_map.end();
}

void Program::set_namespace_preference(const std::vector<std::string> &order, TokenBase *tb)
{
    if ( order.empty() )
	Throw(tb) << "Expecting at least one namespace name in prefer directive" << flush;
    for ( size_t i = 0; i < order.size(); ++i )
    {
	if ( !is_known_namespace(order[i]) )
	    Throw(tb) << "Unknown namespace '" << order[i] << "' in prefer directive" << flush;
    }
    namespace_preference = order;
}

Variable *Program::find_namespace_member(const std::string &ns_name, const std::string &member_name)
{
    namespace_map_t::const_iterator nsi = namespace_map.find(ns_name);
    if ( nsi == namespace_map.end() )
	return NULL;
    variable_map_t::const_iterator vmi = nsi->second.find(member_name);
    return vmi == nsi->second.end() ? NULL : vmi->second;
}

Variable *Program::resolve_preferred_identifier(TokenIdent *ident_tb, bool expression_head)
{
    if ( !ident_tb ) return NULL;

    if ( expression_head && !current_namespace.empty() )
    {
	Variable *var = find_namespace_member(current_namespace, ident_tb->str);
	if ( var ) return var;
    }

    if ( namespace_preference.empty() )
	return resolve_c_identifier(*this, ident_tb, expression_head);

    for ( size_t i = 0; i < namespace_preference.size(); ++i )
    {
	const std::string &pref = namespace_preference[i];
	if ( pref == "c" )
	{
	    Variable *var = resolve_c_identifier(*this, ident_tb, expression_head);
	    if ( var ) return var;
	    continue;
	}
	Variable *var = find_namespace_member(pref, ident_tb->str);
	if ( var ) return var;
    }

    return resolve_c_identifier(*this, ident_tb, expression_head);
}

void Program::set_expression_context_root(const madc::value *root)
{
    expression_context_root = root;
}

void Program::clear_expression_context_root()
{
    expression_context_root = NULL;
}

bool Program::has_expression_context_root() const
{
    return expression_context_root != NULL && expression_context_root->is_object();
}

void Program::push_runtime_scope()
{
    runtime_scope_prev = g_runtime_program;
    g_runtime_program = this;
}

void Program::pop_runtime_scope()
{
    if ( g_runtime_program == this )
	g_runtime_program = runtime_scope_prev;
    runtime_scope_prev = NULL;
}

Program *Program::active_runtime_program()
{
    return g_runtime_program;
}

bool Program::runtime_eval_source(const std::string &source_text,
				  madc::value &result,
				  const std::string &display_name,
				  const madc::value *context,
				  const char *wrapper_return_type)
{
    return madc::internal_program_runtime_eval_source(*this, source_text, result, display_name, context, wrapper_return_type);
}

bool Program::runtime_eval_expression(const std::string &expression,
				      madc::value &result,
				      const std::string &display_name,
				      const madc::value *context)
{
    return madc::internal_program_runtime_eval_expression(*this,
							 expression,
							 result,
							 display_name,
							 context);
}

TokenBase *Program::resolve_expression_context_identifier(TokenIdent *ident_tb)
{
    if ( !ident_tb || !has_expression_context_root() )
	return NULL;

    const std::map<std::string, madc::value> &fields = expression_context_root->as_object();
    std::map<std::string, madc::value>::const_iterator it = fields.find(ident_tb->str);
    if ( it == fields.end() )
	return NULL;

    if ( it->second.is_object() )
    {
	TokenExprContextObject *obj = new TokenExprContextObject(ident_tb->str, &it->second);
	copy_token_location(obj, ident_tb);
	return obj;
    }

    return make_expression_context_literal(*this, it->second, ident_tb);
}

TokenBase *Program::resolve_expression_context_member(TokenBase *lhs, TokenIdent *member_tb)
{
    if ( !lhs || !member_tb )
	return NULL;

    TokenExprContextObject *obj = dynamic_cast<TokenExprContextObject *>(lhs);
    if ( !obj || !obj->context_value || !obj->context_value->is_object() )
	return NULL;

    const std::map<std::string, madc::value> &fields = obj->context_value->as_object();
    std::map<std::string, madc::value>::const_iterator it = fields.find(member_tb->str);
    if ( it == fields.end() )
	Throw(member_tb) << "context path '" << obj->path << "." << member_tb->str
			 << "' cannot find field '" << member_tb->str << "'" << flush;

    if ( it->second.is_object() )
    {
	TokenExprContextObject *next = new TokenExprContextObject(obj->path + "." + member_tb->str,
								  &it->second);
	copy_token_location(next, member_tb);
	return next;
    }

    TokenBase *resolved = make_expression_context_literal(*this, it->second, member_tb);
    if ( !resolved )
	Throw(member_tb) << "context path '" << obj->path << "." << member_tb->str
			 << "' resolves to unsupported value kind '"
			 << madc::value::kind_name(it->second.type()) << "'" << flush;
    return resolved;
}

// creates global variable named after string,
// duplicate strings will share same variable
Variable *Program::addLiteral(std::string &s)
{
    variable_map_iter vmi;
    Variable *var;

    string id = "__literal__";
    id.append(s);

    if ( (var=tkProgram->findVariable(id)) )
	return var;

    // A bare string literal has type `const char *` (C/C++ canon: a `char[]`
    // decaying to a pointer), NOT std::string. The std::string CLASS ingests a
    // literal via its const-char* constructor / `operator=` where a `string`
    // object is required (`string s = "x"` == `string("x")` == `s.assign("x")`)
    // — that lowering lives in the class path, not in the literal's own type.
    // The literal bytes live in the variable name suffix (`__literal__<text>`);
    // the CIR builder emits them via str(). `data` holds a stable char* for any
    // parse-time / AOT reader.
    var = new Variable(id, ddCHARptr, 1, NULL, true);
    var->makeconstant();
    if ( var->data )
	*(const char **)var->data = strdup(s.c_str());
    tkProgram->variables.push_back(var);

    return var;
}

Variable *Program::addWideLiteral(std::string &s)
{
    variable_map_iter vmi;
    Variable *var;

    string id = "__wliteral__";
    id.append(s);

    if ( (var=tkProgram->findVariable(id)) )
	return var;

    size_t chars = s.size() / 4;
    uint32_t count = (uint32_t)chars + 1;
    var = new Variable(id, ddINT32, count, NULL, true);
    var->dims.push_back(count);
    var->flags |= vfFIXEDARRAY;
    var->makeconstant();
    var->object_size_hint = (int64_t)count * (int64_t)ddINT32.size;
    var->data = calloc(count ? count : 1, ddINT32.size ? ddINT32.size : 4);
    var->flags |= vfALLOC;

    int32_t *dst = (int32_t *)var->data;
    for ( size_t i = 0; i < chars; ++i )
    {
	size_t p = i * 4;
	uint32_t cp = (uint8_t)s[p]
	    | ((uint32_t)(uint8_t)s[p + 1] << 8)
	    | ((uint32_t)(uint8_t)s[p + 2] << 16)
	    | ((uint32_t)(uint8_t)s[p + 3] << 24);
	dst[i] = (int32_t)cp;
    }
    tkProgram->variables.push_back(var);

    return var;
}

Variable *Program::addVariable(TokenCpnd *code, DataDef &dd, std::string &id, int c, void *init, bool alloc)
{
    Variable *var;

    if ( code )
    {
	// Function-local `extern T name;` is not a fresh local — it's a
	// reference to the file-scope global with that name.  When a
	// local with the same name already exists in an enclosing scope,
	// we must add an alias in the *current* block that points to the
	// global's storage so subsequent lookups in this block find the
	// global instead of the local shadow.
	if ( parsing_extern_decl )
	{
	    Variable *global_var = tkProgram->findVariable(id);
	    if ( global_var )
	    {
		// If the current block's own variables already include this
		// name, return it (already declared extern earlier in this
		// same block).  Otherwise create a local alias that shares
		// the global's data pointer, so findVariable in this block
		// finds it before any parent-scope shadow.
		for ( variable_vec_iter vvi = code->variables.begin();
		      vvi != code->variables.end(); ++vvi )
		    if ( (*vvi)->name == id )
			return *vvi;
		var = new Variable(id, *global_var->type, global_var->count,
				   NULL, false);
		var->flags = global_var->flags;
		var->data  = global_var->data;
		code->variables.push_back(var);
		return var;
	    }
	}
	// Reuse an existing symbol ONLY if it was declared in THIS block.
	// code->findVariable recurses into parent scopes, so using it here
	// would make an inner-block redeclaration (`{ long x; }` shadowing an
	// outer `struct tiny x;`) silently reuse — and later mutate — the
	// outer variable's type. A same-name decl in an inner scope must
	// create a fresh shadowing local instead.
	for ( variable_vec_iter vvi = code->variables.begin();
	      vvi != code->variables.end(); ++vvi )
	    if ( (*vvi)->name == id )
		return *vvi;
	var = new Variable(id, dd, c, init, alloc);
	var->flags |= vfLOCAL;
	// The extern flag may only be set on a freshly-created symbol (here), never
	// on an existing one — so a redundant `extern` declaration can never demote
	// an already-defined variable back to a mere declaration.
	if ( parsing_extern_decl )
	    var->flags |= vfEXTERN;
	code->variables.push_back(var);
	DBG(std::cout << "Added new variable type: " << dd.name << " size: "
		<< dd.size << " name: " << id << " ptr: " << var << " to codeblock: " << code << std::endl);
	DBG(std::cout << "Alloc: " << (alloc ? "true" : "false") << std::endl);
	DBG(std::cout << "Data address: " << (uint64_t)var->data << std::endl);
	return var;
    }
    if ( (var=tkProgram->findVariable(id)) )
    {
	if ( var->flags & vfEXTERN )
	{
	    var->type = &dd;
	    var->count = c;
	    if ( !parsing_extern_decl )
		var->flags &= ~vfEXTERN;
	}
	if ( !current_namespace.empty() )
	    namespace_map[current_namespace][id] = var;
	return var;
    }
    var = new Variable(id, dd, c, init, alloc);
    // Extern is set only at creation (see the local-create path above): a later
    // redundant `extern` declaration finds the existing var via the branch
    // above and leaves its storage class untouched.
    if ( parsing_extern_decl )
	var->flags |= vfEXTERN;
    tkProgram->variables.push_back(var);
    if ( !current_namespace.empty() )
	namespace_map[current_namespace][id] = var;

    DBG(std::cout << "Added new global variable type: " << dd.name << " size: "
		<< dd.size << " name: " << id << " ptr: " << var << " flags: " << var->flags << std::endl);
    DBG(std::cout << "Data address: " << (uint64_t)var->data << std::endl);

    return var;
}

Variable *Program::resolve_global_storage_variable(Variable *var) const
{
    if ( !var || !tkProgram || !var->is_global() )
	return var;

    std::set<Variable *> seen;
    Variable *resolved = var;
    while ( resolved && !resolved->storage_alias_name.empty()
	 && !seen.count(resolved) )
    {
	seen.insert(resolved);
	Variable *target = tkProgram->findVariable(resolved->storage_alias_name);
	if ( !target || target == resolved )
	    break;
	resolved = target;
    }
    return resolved;
}

// get or create a pointer-to-T DataDef
DataDefPTR *Program::getPointerType(DataDef *base)
{
    // check cache first
    auto it = ptr_type_cache.find(base);
    if ( it != ptr_type_cache.end() )
	return it->second;

    // return well-known globals for common types
    if ( base == &ddVOID )  return &ddVOIDptr;
    if ( base == &ddCHAR )  return &ddCHARptr;
    if ( base == &ddINT )   return &ddINTptr;
    if ( base == &ddINT32 ) return &ddINT32ptr;

    // create and cache a new pointer DataDef
    DataDefPTR *ptr = new DataDefPTR(*base);
    ptr_type_cache[base] = ptr;
    DBG(std::cout << "getPointerType() created " << ptr->name << " for base " << base->name << std::endl);
    return ptr;
}

// add a function definition
Variable *Program::addFunction(std::string id, datatype_vec_t params, fVOIDFUNC extfunc, bool isMethod)
{
    variable_map_iter vmi;
    funcdef_map_iter fmi;
    FuncDef *func;
    Variable *var;
    DataDef *dd;

    auto resolve_data_type = [this](const typespec_t &spec) -> DataDef *
    {
	// A type named directly by DataDef* wins; no enum word is involved. A
	// T* / T& form (ptr_of/ref_of)
	// is resolved the same way the DataType-offset path resolves a pointer /
	// reference: getPointerType for a pointer; the base DataDef for a ref.
	if ( spec.dd )
	{
	    if ( spec.ref == RefType::rtPointer )  return getPointerType(spec.dd);
	    if ( spec.ref == RefType::rtReference ) return spec.dd;
	    return spec.dd;
	}
	DataType dt = spec.dt;
	if ( DataDef::rawtype(dt) != dt )
	{
	    DataType raw = DataDef::rawtype(dt);
	    DataDef *base = NULL;
	    switch(raw)
	    {
		default:		   base = NULL;		break;
		case DataType::dtVOID:   base = &ddVOID;	break;
		case DataType::dtCHAR:   base = &ddCHAR;	break;
		case DataType::dtBOOL:   base = &ddBOOL;	break;
		case DataType::dtUINT8:  base = &ddUINT8;	break;
		case DataType::dtINT16:  base = &ddINT16;	break;
		case DataType::dtUINT16: base = &ddUINT16;	break;
		case DataType::dtINT32:  base = &ddINT32;	break;
		case DataType::dtUINT32: base = &ddUINT32;	break;
		case DataType::dtINT64:  base = &ddINT64;	break;
		case DataType::dtUINT64: base = &ddUINT64;	break;
		case DataType::dtARRAY:  base = &ddARRAY;	break;
		case DataType::dtFLOAT:  base = &ddFLOAT;	break;
		case DataType::dtDOUBLE: base = &ddDOUBLE;	break;
	    }
	    if ( !base )
		return &ddVOID;
	    if ( static_cast<uint32_t>(dt) >= 20000 )
		return base;
	    return getPointerType(base);
	}

	switch(dt)
	{
	    default:	 	  return &ddVOID;
	    case DataType::dtCHAR:    return &ddCHAR;
	    case DataType::dtBOOL:    return &ddBOOL;
	    case DataType::dtUINT8:   return &ddUINT8;
	    case DataType::dtINT16:   return &ddINT16;
	    case DataType::dtUINT16:  return &ddUINT16;
	    case DataType::dtINT32:   return &ddINT32;
	    case DataType::dtUINT32:  return &ddUINT32;
	    case DataType::dtINT64:	  return &ddINT64;
	    case DataType::dtUINT64:  return &ddUINT64;
	    case DataType::dtARRAY:   return &ddARRAY;
	    case DataType::dtFLOAT:   return &ddFLOAT;
	    case DataType::dtDOUBLE:  return &ddDOUBLE;
	}
    };

    // may have already been declared
    if ( !isMethod && (fmi=funcdef_map.find(id)) != funcdef_map.end() )
    {
	DBG(std::cout << "addFunction() already declared: " << id << std::endl);
	return NULL;
    }

    dd = resolve_data_type(params[0]);

    func = new FuncDef(*dd);
    if ( !isMethod )
	funcdef_map[id] = func;
    DBG(std::cout << "addFunction() Added new function declaration name: " << id << " numparams: " << params.size()-1  << " x86code: " << (uint64_t)extfunc << " returns " << dd->name << std::endl);

    // func->parameters.push_back(&pb->definition);

    for ( uint32_t i = 1; i < params.size(); ++i )
    {
	dd = resolve_data_type(params[i]);

	DBG(std::cout << dd->name);
	DBG(if (i < params.size()-1) std::cout << ", ");
	func->parameters.push_back(dd);
	func->param_typedef_names.push_back("");
    }
    DBG(std::cout << endl);

    Method *method;

    if ( isMethod )
    {
	var = new Variable(id, *func, 1, NULL, false);
	method = new Method(*var);
	var->data = (void *)method;
	method->x86code = (void *)extfunc;
	if ( extfunc )
	{
	    Dl_info _dli;
	    if ( dladdr((void *)extfunc, &_dli) && _dli.dli_sname && _dli.dli_sname[0] )
		external_symbol_map[reinterpret_cast<uintptr_t>(extfunc)] = _dli.dli_sname;
	    else
		external_symbol_map[reinterpret_cast<uintptr_t>(extfunc)] = id;
	}

	return var;
    }

    // check if this variable was already defined
    if ( (var=tkProgram->findVariable(id)) )
    {
	method = (Method *)var->data;
    }
    else
    {
	var = addVariable(NULL, *func, id, false);
	method = new Method(*var);
	var->data = (void *)method;
    }
    method->x86code = (void *)extfunc;
    if ( extfunc )
    {
	Dl_info _dli;
	if ( dladdr((void *)extfunc, &_dli) && _dli.dli_sname && _dli.dli_sname[0] )
	    external_symbol_map[reinterpret_cast<uintptr_t>(extfunc)] = _dli.dli_sname;
	else
	    external_symbol_map[reinterpret_cast<uintptr_t>(extfunc)] = id;
    }

    return var;
}


void Program::pushCompound()
{
    TokenCpnd *tc = new TokenCpnd;

    ++_braces;

    if ( compounds.empty() )
    {
	DBG(std::cout << "pushCompound(" << _braces << ") function" << std::endl);
	tc->parent = NULL;
    }
    else
    {
	DBG(std::cout << "pushCompound(" << _braces << ") nested" << std::endl);
	tc->parent = compounds.top();
	tc->method = compounds.top()->method;
	compounds.top()->child = tc;
    }
    compounds.push(tc);
}

void Program::popCompound()
{
    DBG(std::cout << "popCompound(" << _braces << ')' << std::endl);
    --_braces;
    if ( !compounds.empty() )
	compounds.pop();
}

void Program::popOperator(stack<TokenBase *> &opStack, stack<TokenBase *> &exStack)
{
    DBG(cout << "popOperator() size: " << opStack.size() << " TOP" << endl);
    TokenOperator *to;

    switch(opStack.top()->type())
    {
	case TokenType::ttOperator:
	case TokenType::ttMultiOp:
	    to = (TokenOperator *)opStack.top();
	    if ( to->type() == TokenType::ttOperator )
		DBG(cout << "popOperator() got operator: " << (char)to->get() << " id() " << (int)to->id() << endl);
	    else
		DBG(cout << "popOperator() got operator: " << ((TokenMultiOp *)to)->str << endl);
	    if ( to->argc() > 0 )
	    {
		if ( !to->right )
		{
		    if ( exStack.empty() )
		    {
			DBG(cerr << "got operator, but exStack is empty!" << endl);
			Throw(to) << "Missing operand" << flush;
		    }
		    to->right = exStack.top(); exStack.pop(); DBG(cout << "popped " << to->right->ival() << endl);
		}
		if ( to->argc() > 1 )
		{
		    if ( !to->left )
		    {
			if ( exStack.empty() )
			{
			    DBG(cerr << "got operator, but exStack is empty!" << endl);
			    Throw(to) << "Missing operand" << flush;
			}
			to->left = exStack.top(); exStack.pop();  DBG(cout << "popped " << to->left->ival() << endl);
		    }
		}
	    }
	    // Type a class-object operator expression with the operator's RETURN type
	    // (generic operator-overload support — applies to every class identically).
	    // Authoritative over the built-in pointer/arithmetic heuristics via
	    // TokenOperator::resolved_type.
	    resolve_object_operator_type(to);
	    DBG(cout << "Popping " << (char)to->get() << "[" << (to->left ? to->left->ival() : 0) << ", " << (to->right ? to->right->ival() : 0) << "] from opStack and onto exStack" << endl);
	    opStack.pop();
	    exStack.push(to);
	    break;
	case TokenType::ttCallFunc:
	    DBG(cout << "popOperator() got ttCallFunc" << endl);
	    exStack.push(opStack.top());
	    opStack.pop();
	    break;
	case TokenType::ttCallMethod:
	    DBG(cout << "popOperator() got ttCallMethod" << endl);
	    exStack.push(opStack.top());
	    opStack.pop();
	    break;
	default:
	    DBG(cerr << "popOperator() throwing opStack.top()" << endl);
	    Throw(opStack.top()) << "unexpected token type " << (int)opStack.top()->type() << flush;
    } // end switch
    DBG(cout << "popOperator() size: " << opStack.size() << " END" << endl);
}

#if 0
// parse a function call and it's parameters
// parameters are individually parsed by parseExpression
// returns ending token
TokenBase *Program::parseCallFunc(TokenCallFunc *tc)
{
    TokenBase *tb;

    DBG(std::cout << tc->line << ':' << tc->column << ":Program::parseCallFunc(" << tc->var.name << ')' << std::endl);
    int brackets = 1;
    size_t paramcnt = 0;

    if ( !(tb=peekToken()) )
	throw "Unexpected end of input";
    while ( brackets && tb->id() != TokenID::tkSemi )
    {
	tb = nextToken();
	if ( tb->id() == TokenID::tkClBrk ) { --brackets; continue; }
	if ( tb->id() == TokenID::tkComma )
	{
	    if ( ++paramcnt >= ((FuncDef *)tc->var.type)->parameters.size() )
		throw "Too many parameters";
	    continue;
	}
	if ( tb->id() == TokenID::tkSemi ) { DBG(cout << "Got ;" << endl); break; }
	DBG(cout << "parseCallFunc() brackets: " << brackets << " tokenID(" << (char)tb->get() << "): " << (int)tb->id() << " calling parseExpression" << endl);
	if ( !(tb=parseExpression(tb, true)) ) { DBG(cout << "parseExp return NULL" << endl); break; }
	if ( tb->id() == TokenID::tkClBrk ) { --brackets; continue; }
	DBG(cout << "parseExpression returned type(): " << (int)tb->type() << " id(): " << (int)tb->id() << endl);
	DBG(cout << "calling tc(" << tc->var.name << ")[" << (uint64_t)tc << "]->parameters.push_back(tb[" << (uint64_t)tb << "]) brackets: " << brackets << endl);
	tc->parameters.push_back(tb);
    }
    if ( tb->id() == TokenID::tkSemi )
	DBG(cout << "parseCallFunc() while ended on semicolon" << endl);
    // (need check for optional parameters)
    if ( tc->argc() != ((FuncDef *)tc->var.type)->parameters.size() )
    {
	// (debug removed)
	DBG(std::cout << "parseCallFunc(" << tc->var.name << "): argument count: " << tc->argc() << " expected: " << ((FuncDef *)tc->var.type)->parameters.size() << " (paramcnt: " << paramcnt << ") brackets: " << brackets << std::endl);
	throw "Incorrect number of parameters";
    }

    return tb;
}
#else
static bool call_accepts_extra_args(TokenCallFunc *tc)
{
    FuncDef *fd = (FuncDef *)tc->var.type;
    Method *md = (Method *)tc->var.data;

    if ( fd->is_varargs )
	return true;
    if ( fd->parameters.empty() && md && (md->x86code || tc->var.name == "dlcall") )
	return true;
    return false;
}

// parse a function call and it's parameters
// parameters are individually parsed by parseExpression
// returns ending token
TokenBase *Program::parseCallFunc(TokenCallFunc *tc)
{
    TokenBase *tb;

    DBG(std::cout << tc->line << ':' << tc->column << ":Program::parseCallFunc(" << tc->var.name << ')' << std::endl);
#if 0
    tb = nextToken();
    if ( tb->id() != TokenID::tkOpBrk )
    {
	DBG(std::cout << "Program::parseCallFunc() no parameters" << std::endl);
	return tb;
    }
#endif
    int brackets = 1;
    size_t paramcnt = 0;

    while ( brackets )
    {
	tb = peekToken();
	if ( tb->id() == TokenID::tkSemi )  { return tb; }
	tb = nextToken();
	if ( tb->id() == TokenID::tkClBrk ) { --brackets; continue; }
	if ( tb->id() == TokenID::tkComma )
	{
	    FuncDef *_fd = (FuncDef *)tc->var.type;
	    if ( !_fd->parameters.empty()
	    &&   !call_accepts_extra_args(tc)
	    &&   ++paramcnt >= _fd->parameters.size() )
		Throw(tb) << "Too many parameters" << flush;
	    continue;
	}
	if ( tb->id() == TokenID::tkSemi ) { break; }
	DBG(cout << "parseCallFunc() brackets: " << brackets << " tokenID(" << (char)tb->get() << "): " << (int)tb->id() << " calling parseExpression" << endl);
	std::string saved_namespace = current_namespace;
	current_namespace.clear();
	tb = parseExpression(tb, true);
	current_namespace = saved_namespace;
	if ( !tb ) { break; }
	if ( tb->id() == TokenID::tkClBrk ) { --brackets; continue; }
	DBG(cout << "parseExpression returned type(): " << (int)tb->type() << " id(): " << (int)tb->id() << endl);
	DBG(cout << "calling tc(" << tc->var.name << ")[" << (uint64_t)tc << "]->parameters.push_back(tb[" << (uint64_t)tb << "])" << endl);
	tc->parameters.push_back(tb);
    }

    bool needs_runtime_scope_context = tc->auto_scope_context;
    if ( !needs_runtime_scope_context && is_runtime_eval_scope_ctx_helper_name(tc->var.name) )
    {
	FuncDef *fd = dynamic_cast<FuncDef *>(tc->var.type);
	Method *md = (Method *)tc->var.data;
	if ( fd && !fd->is_varargs
	  && !(fd->parameters.empty() && md && (md->x86code || tc->var.name == "dlcall")) )
	{
	    // capture params (nested fn / [&] lambda) are NOT in fd->parameters —
	    // the CIR builder appends them; the user-visible arity is the full count.
	    size_t expected = fd->parameters.size()
		- (md && md->owner_class ? 1 : 0);
	    needs_runtime_scope_context = (tc->argc() + 1 == expected);
	}
    }

    if ( needs_runtime_scope_context )
    {
	TokenCpnd *scope = compounds.empty() ? NULL : compounds.top();
	if ( !scope )
	    Throw(tc) << "runtime eval scope capture requires an active compound scope" << flush;
	static uint64_t runtime_eval_scope_serial = 0;
	std::string ctx_name = "__madc_eval_scope_ctx_"
	    + std::to_string(++runtime_eval_scope_serial);
	Variable *ctx_var = addVariable(scope, ddARRAY, ctx_name, 1, NULL, false);
	TokenScopeContext *ctx_token = new TokenScopeContext(*ctx_var);
	collect_runtime_eval_scope_variables(ctx_token->scope_vars);
	tc->parameters.push_back(ctx_token);
    }

    // (need check for optional parameters)
    // skip arg count check for dlopen functions (0 declared params = variadic-like)
    {
	// function pointer variable: type is DataDefFPTR, get target FuncDef
	if ( tc->var.type->is_function() && tc->var.type->is_numeric() )
	{
	    DataDefFPTR *fptr = static_cast<DataDefFPTR *>(tc->var.type);
	    FuncDef *fd = fptr->target;
	    // K&R: empty param list (not void) accepts any number of args
	    if ( !(fd->parameters.empty() && !fd->is_void_params) )
	    {
		// Capture params (nested fn / [&] lambda) live only in the CIR
		// lowering, not fd->parameters — user arity is the full count.
		size_t expected = fd->parameters.size()
		    - (fd->is_varargs ? 1 : 0);
		if ( fd->is_varargs ? (tc->argc() < expected) : (tc->argc() != expected) )
		    Throw(tc) << "Incorrect number of parameters for '" << tc->var.name << "': expected " << expected << " got " << tc->argc() << flush;
	    }
	}
	else
	{
	    FuncDef *fd = (FuncDef *)tc->var.type;
	    Method *md = (Method *)tc->var.data;
	    // C++ default arguments: fill omitted TRAILING args from the parameter
	    // defaults (param_defaults is index-aligned with parameters, hidden
	    // __this/__retbuf/__va_args slots hold NULL). Stop at the first param
	    // without a default so the arity check still reports a genuine shortfall.
	    if ( !fd->is_varargs && !fd->param_defaults.empty() )
	    {
		size_t hidden = (md && md->owner_class) ? 1 : 0;   // __this
		for ( size_t i = hidden + tc->argc(); i < fd->parameters.size(); ++i )
		{
		    if ( i < fd->param_defaults.size() && fd->param_defaults[i] )
			tc->parameters.push_back(fd->param_defaults[i]);
		    else
			break;
		}
	    }
	    // In C, f() with no params accepts any number of arguments (K&R style).
	    // Only f(void) means exactly zero. Skip the check for empty-param functions.
	    if ( !(fd->parameters.empty() && !fd->is_void_params) )
	    {
		size_t expected = fd->parameters.size()
			- (md && md->owner_class ? 1 : 0)
			- (fd->is_varargs ? 1 : 0);
		// varargs functions accept expected or more args; fixed functions require exact match
		if ( fd->is_varargs ? (tc->argc() < expected) : (tc->argc() != expected) )
		{
		    DBG(std::cout << "parseCallFunc: argument count: " << tc->argc() << " expected: " << expected << std::endl);
		    Throw(tc) << "Incorrect number of parameters for '" << tc->var.name
		        << "': expected " << expected << " got " << tc->argc() << flush;
		}
	    }
	}
    }

    return tb;
}
#endif

// parse a method call and it's parameters
// parameters are individually parsed by parseExpression
// returns ending token
TokenBase *Program::parseCallMethod(TokenCallMethod *tc)
{
    TokenBase *tb;

    DBG(std::cout << tc->line << ':' << tc->column << ":Program::parseCallMethod(" << tc->var.name << ')' << std::endl);
    int brackets = 1;
    size_t paramcnt = 1;

    while ( brackets )
    {
	tb = peekToken();
	if ( tb->id() == TokenID::tkSemi )  { return tb; }
	tb = nextToken();
	if ( tb->id() == TokenID::tkClBrk ) { --brackets; continue; }
	if ( tb->id() == TokenID::tkComma )
	{
	    if ( !((FuncDef *)tc->var.type)->is_varargs
	    &&   ++paramcnt >= ((FuncDef *)tc->var.type)->parameters.size() )
		Throw(tb) << "Too many parameters" << flush;
	    continue;
	}
	if ( tb->id() == TokenID::tkSemi ) { break; }
	DBG(cout << "parseCallMethod() brackets: " << brackets << " tokenID(" << (char)tb->get() << "): " << (int)tb->id() << " calling parseExpression" << endl);
	std::string saved_namespace = current_namespace;
	current_namespace.clear();
	tb = parseExpression(tb, true);
	current_namespace = saved_namespace;
	if ( !tb ) { break; }
	if ( tb->id() == TokenID::tkClBrk ) { --brackets; continue; }
	DBG(cout << "parseExpression returned type(): " << (int)tb->type() << " id(): " << (int)tb->id() << endl);
	DBG(cout << "calling tc(" << tc->var.name << ")[" << (uint64_t)tc << "]->parameters.push_back(tb[" << (uint64_t)tb << "])" << endl);
	tc->parameters.push_back(tb);
    }
    // (need check for optional parameters)
    if ( ((FuncDef *)tc->var.type)->is_varargs
      ? (tc->argc()+1 < ((FuncDef *)tc->var.type)->parameters.size()-1)
      : (tc->argc()+1 != ((FuncDef *)tc->var.type)->parameters.size()) )
    {
	DBG(std::cout << "parseCallMethod: argument count: " << tc->argc() << " expected: " << ((FuncDef *)tc->var.type)->parameters.size() << std::endl);
	Throw(tc) << "Incorrect number of parameters: expected " << ((FuncDef *)tc->var.type)->parameters.size() << " got " << tc->argc()+1 << flush;
    }

    return tb;
}


// parse one complete expression
// for expression: x = 5, sum(5, 5), ++x, etc
// a "conditional" expression stops when brackets are equalized
// Parse an identifier followed by any chain of postfix operators
// (->ident, .ident, [expr]) and return the resulting node. Stops at the
// first non-postfix token (binary operator, comma, semicolon, etc.);
// that token remains consumable by the caller on the next nextToken().
// Used by unary `*` to avoid parseExpression's greedy consumption of
// trailing binary operators such as `== '$'`.
TokenBase *Program::parsePostfixChain(TokenBase *head)
{
    if ( !head || (!is_contextual_identifier_token(head)
		 && head->type() != TokenType::ttIdentifier) )
	return NULL;

    std::string name = contextual_identifier_name(head);
    // Scoped-enum value `Tag::Value` (e.g. inside a cast `(int)Size::Large`).
    // The enumerators live in the tag's pseudo-namespace (see TokenENUM::parse).
    if ( peekToken() && peekToken()->id() == TokenID::tkNS )
    {
	namespace_map_t::iterator nsi = namespace_map.find(name);
	if ( nsi != namespace_map.end() )
	{
	    nextToken(); // consume '::'
	    TokenBase *member_tb = nextToken();
	    if ( !member_tb || !is_contextual_identifier_token(member_tb) )
		Throw(head) << "Expecting identifier after '" << name << "::'" << flush;
	    std::string member = contextual_identifier_name(member_tb);
	    variable_map_iter vmi = nsi->second.find(member);
	    if ( vmi == nsi->second.end() )
		Throw(member_tb) << "'" << member << "' is not a member of '" << name << "'" << flush;
	    TokenBase *r = new TokenVar(*vmi->second);
	    r->file = head->file;
	    r->line = head->line;
	    r->column = head->column;
	    return r;
	}
    }
    Variable *var = findVariable(name);
    TokenBase *result = NULL;
    if ( var )
    {
	result = new TokenVar(*var);
	result->file = head->file;
	result->line = head->line;
	result->column = head->column;
    }
    else
    {
	result = resolve_expression_context_identifier((TokenIdent *)head);
	if ( !result )
	    Throw(head) << "undeclared identifier '" << name << "'" << flush;
    }

    while ( peekToken() )
    {
	TokenID pid = peekToken()->id();
	if ( pid == TokenID::tkDeRef || pid == TokenID::tkDot )
	{
	    bool is_arrow = (pid == TokenID::tkDeRef);
	    TokenBase *op_tb = nextToken();
	    TokenBase *mtb = nextToken();
	    if ( !mtb || !is_contextual_identifier_token(mtb) )
		Throw(mtb ? mtb : op_tb) << "expected member name after '"
		    << (is_arrow ? "->" : ".") << "'" << flush;
	    std::string mname = contextual_identifier_name(mtb);
	    TokenIdent member_ident(mname);
	    copy_token_location(&member_ident, mtb);

	    if ( !is_arrow )
	    {
		if ( TokenBase *ctx_member = resolve_expression_context_member(result, &member_ident) )
		{
		    result = ctx_member;
		    continue;
		}
	    }

	    DataDef *obj_type = result->datadef();
	    bool fixed_array_arrow = false;
	    if ( is_arrow )
	    {
		if ( TokenVar *tv = dynamic_cast<TokenVar *>(result) )
		    fixed_array_arrow = tv->var.is_fixed_array();
		if ( !fixed_array_arrow )
		    obj_type = effective_pointer_type_for_member_access(*this, result);
		if ( !fixed_array_arrow && (!obj_type || !obj_type->is_pointer()) )
		    Throw(mtb) << "expression before '->' must be a pointer" << flush;
		if ( !fixed_array_arrow )
		{
		    DataDefPTR *pt = dynamic_cast<DataDefPTR *>(obj_type);
		    if ( !pt || !pt->base_type )
			Throw(mtb) << "expression before '->' is not a typed pointer" << flush;
		    obj_type = pt->base_type;
		}
	    }
	    if ( !obj_type || (!obj_type->is_struct() && !obj_type->is_object()) )
		Throw(mtb) << "member reference type is not a structure or union" << flush;
	    DataDefSTRUCT *sdd = static_cast<DataDefSTRUCT *>(obj_type);
	    ssize_t ofs = sdd->m_offset(mname);
	    if ( ofs == -1 )
		Throw(mtb) << "no member named '" << mname << "'" << flush;
	    DataDef *mtype = sdd->m_type(mname);
	    Variable *mvar = new Variable(mname, *mtype, 1, NULL, false);
	    mvar->flags = member_proxy_flags(var->flags);

	    TokenMember *tm;
	    if ( result->type() == TokenType::ttVariable )
	    {
		TokenVar *tv = dynamic_cast<TokenVar *>(result);
		tm = new TokenMember(tv->var, *mvar, ofs);
	    }
	    else
	    {
		tm = new TokenMember(*var, *mvar, ofs, result);
	    }
	    tm->file = op_tb->file;
	    tm->line = op_tb->line;
	    tm->column = op_tb->column;
	    result = tm;
	    var = mvar;
	    continue;
	}
	if ( pid == TokenID::tkOpSqr )
	{
	    TokenBase *open = nextToken();
	    TokenBase *idx_tb = nextToken();
	    TokenBase *idx_expr = parseExpression(idx_tb, true);
	    TokenBase *close = nextToken();
	    if ( !close || close->id() != TokenID::tkClSqr )
		Throw(close ? close : open) << "expected ']' in subscript" << flush;
	    DataDef *base_type = result->datadef();
	    DataDef *elem_type = &ddINT64;
	    // Fixed-array subscripts preserve the element type directly (the
	    // `struct node *` in `struct node *arr[2]` is already the element),
	    // whereas raw-pointer subscripts dereference one level.
	    bool handled_fixed_array = false;
	    TokenMember *tm = dynamic_cast<TokenMember *>(result);
	    if ( tm && tm->is_fixed_array_member() && tm->var.type )
	    {
		DataDefSTRUCT *sdd = tm->owner_struct_type();
		const std::vector<uint32_t> *dims = sdd ? sdd->m_dims(tm->var.name) : NULL;
		elem_type = (dims && !dims->empty())
		    ? build_fixed_array_query_type(tm->var.type, *dims, 1)
		    : tm->var.type;
		handled_fixed_array = true;
	    }
	    else if ( TokenVar *tv = dynamic_cast<TokenVar *>(result) )
	    {
		if ( tv->var.is_fixed_array() )
		{
		    elem_type = build_fixed_array_query_type(tv->var.type, tv->var.dims, 1);
		    handled_fixed_array = true;
		}
	    }
	    if ( !handled_fixed_array && base_type && base_type->is_simd() )
	    {
		DataDefSIMD *vdd = static_cast<DataDefSIMD *>(base_type);
		elem_type = vdd->element_type ? vdd->element_type : &ddINT64;
	    }
	    else if ( !handled_fixed_array && base_type && (base_type->is_pointer() || dynamic_cast<DataDefCArray *>(base_type) != NULL) )
		elem_type = unwrap_subscript_element_type(base_type);
	    result = new TokenSubscriptExpr(result, idx_expr, elem_type);
	    continue;
	}
	break;
    }
    return result;
}

static TokenBase *parse_cast_unary_deref_operand(Program &pgm, TokenBase *star)
{
    if ( !star || star->id() != TokenID::tkMul )
	return NULL;

    TokenBase *deref_tb = pgm.nextToken();
    if ( !deref_tb )
	pgm.Throw(star) << "expecting pointer expression after '*'" << flush;

    if ( deref_tb->id() == TokenID::tkOpBrk )
    {
	TokenBase *inner_tb = pgm.nextToken();
	TokenBase *inner_expr = pgm.parseExpression(inner_tb, true, false, true, 1);
	if ( !inner_expr )
	    pgm.Throw(deref_tb) << "expecting pointer expression after '*('" << flush;
	DataDef *dtype = effective_pointer_type_for_member_access(pgm, inner_expr);
	if ( !dtype )
	    dtype = inner_expr->datadef();
	if ( !dtype || !dtype->is_pointer() )
	    pgm.Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
	return new TokenDerefExpr(inner_expr, unwrap_subscript_element_type(dtype));
    }

    if ( deref_tb->type() != TokenType::ttIdentifier )
    {
	pgm.pushToken(deref_tb);
	return NULL;
    }

    TokenBase *peek = pgm.peekToken();
    if ( peek && peek->id() == TokenID::tkOpBrk )
    {
	pgm.pushToken(deref_tb);
	return NULL;
    }

    TokenBase *pointer_expr = NULL;
    if ( peek && (peek->id() == TokenID::tkDeRef
	       || peek->id() == TokenID::tkDot
	       || peek->id() == TokenID::tkOpSqr) )
    {
	pointer_expr = pgm.parsePostfixChain(deref_tb);
	DataDef *dtype = effective_pointer_type_for_member_access(pgm, pointer_expr);
	if ( !dtype )
	    dtype = pointer_expr ? pointer_expr->datadef() : NULL;
	if ( !dtype )
	    pgm.Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
	if ( dtype->is_function() && dtype->is_numeric() )
	    return pointer_expr;
	if ( !dtype->is_pointer() )
	{
	    if ( TokenMember *tm = dynamic_cast<TokenMember *>(pointer_expr) )
		if ( tm->is_fixed_array_member() )
		    return new TokenDerefExpr(pointer_expr, dtype);
	    pgm.Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
	}
	return new TokenDerefExpr(pointer_expr, unwrap_subscript_element_type(dtype));
    }

    std::string name = ((TokenIdent *)deref_tb)->str;
    Variable *var = pgm.findVariable(name);
    if ( !var )
	pgm.Throw(deref_tb) << "undeclared identifier '" << name << "'" << flush;
    if ( var->type->is_function() && var->type->is_numeric() )
	return new TokenVar(*var);
    if ( dynamic_cast<DataDefFPTR *>(var->type) != NULL )
	return new TokenVar(*var);
    DataDef *base = deref_type_for_variable(var);
    if ( !base )
	pgm.Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
    return new TokenDeref(*var, base);
}

static TokenBase *parse_cast_function_call_operand(Program &pgm, TokenBase *head)
{
    if ( !head || head->type() != TokenType::ttIdentifier )
	return NULL;
    if ( !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkOpBrk )
	return NULL;

    TokenIdent *ident = static_cast<TokenIdent *>(head);
    Variable *var = pgm.findVariable(ident->str);
    if ( !var || !var->type || !var->type->is_function() )
	return NULL;

    Variable *call_var = pgm.runtime_eval_scope_target(var);
    TokenCallFunc *tc = new TokenCallFunc(*call_var);
    tc->auto_scope_context = is_runtime_eval_scope_ctx_helper_name(call_var->name)
	&& is_runtime_eval_scope_public_name(ident->str);

    TokenBase *open = pgm.nextToken();
    tc->file = open->file;
    tc->line = open->line;
    tc->column = open->column;
    pgm.parseCallFunc(tc);
    return tc;
}

TokenFunc *Program::build_expression_function(TokenProgram *tp,
					      TokenBase *expr,
					      DataDef *return_type,
					      const std::string &function_name,
					      bool have_result,
					      const std::string &result_name)
{
    if ( !expr || !return_type || function_name.empty() )
	return NULL;

    std::string fn_name = function_name;
    FuncDef *func = new FuncDef(*return_type);
    Variable *fn_var = addVariable(NULL, *func, fn_name, 1, NULL, false);
    Method *method = new Method(*fn_var);
    fn_var->data = (void *)method;

    TokenFunc *tf = new TokenFunc(*fn_var);
    tf->method = method;
    copy_token_location(tf, expr);

    if ( have_result )
    {
	std::string local_result_name = result_name;
	Variable *result_var = addVariable(tf, *return_type, local_result_name, 1, NULL, true);
	TokenAssign *assign = new TokenAssign();
	TokenVar *lhs = new TokenVar(*result_var);
	copy_token_location(lhs, expr);
	assign->left = lhs;
	assign->right = expr;
	copy_token_location(assign, expr);
	lhs->parent = assign;
	expr->parent = assign;
	tf->statements.push_back((TokenStmt *)assign);

	TokenRETURN *ret = new TokenRETURN();
	TokenVar *ret_value = new TokenVar(*result_var);
	copy_token_location(ret_value, expr);
	ret->returns = ret_value;
	copy_token_location(ret, expr);
	ret_value->parent = ret;
	tf->statements.push_back((TokenStmt *)ret);
    }
    else
    {
	expr->parent = tf;
	tf->statements.push_back((TokenStmt *)expr);
	TokenRETURN *ret = new TokenRETURN();
	copy_token_location(ret, expr);
	tf->statements.push_back((TokenStmt *)ret);
    }

    ast.push(tp);
    ast.push(tf);
    pending_funcs.push_back(tf);
    return tf;
}

static bool token_starts_type_name(Program &pgm, TokenBase *tb)
{
    if ( !tb )
	return false;
    if ( tb->type() == TokenType::ttDataType )
	return true;
    if ( tb->id() == TokenID::tkSTRUCT || tb->id() == TokenID::tkUNION
      || tb->id() == TokenID::tkENUM || tb->id() == TokenID::tkCONST )
	return true;
    if ( tb->type() == TokenType::ttIdentifier )
	return pgm.datatype_map.count(((TokenIdent *)tb)->str) != 0;
    return false;
}

static bool next_parenthesized_type_is_compound_literal(Program &pgm)
{
    std::vector<TokenBase *> saved;
    TokenBase *open = pgm.nextToken();
    if ( !open )
	return false;
    saved.push_back(open);
    if ( open->id() != TokenID::tkOpBrk )
    {
	for ( std::vector<TokenBase *>::reverse_iterator it = saved.rbegin();
	      it != saved.rend(); ++it )
	    pgm.pushToken(*it);
	return false;
    }

    TokenBase *head = pgm.nextToken();
    if ( head )
	saved.push_back(head);
    bool type_head = token_starts_type_name(pgm, head);
    int depth = 1;
    while ( type_head && depth > 0 )
    {
	TokenBase *t = pgm.nextToken();
	if ( !t )
	    break;
	saved.push_back(t);
	if ( t->id() == TokenID::tkOpBrk )
	    ++depth;
	else if ( t->id() == TokenID::tkClBrk )
	    --depth;
    }

    bool is_compound_literal = type_head && depth == 0
	&& pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrc;
    for ( std::vector<TokenBase *>::reverse_iterator it = saved.rbegin();
	  it != saved.rend(); ++it )
	pgm.pushToken(*it);
    return is_compound_literal;
}

static bool is_addressable_expression(TokenBase *expr)
{
    if ( TokenComplexPart *tcp = dynamic_cast<TokenComplexPart *>(expr) )
	return tcp->expr && tcp->expr->datadef() && tcp->expr->datadef()->is_complex();
    return dynamic_cast<TokenMember *>(expr)
	|| dynamic_cast<TokenDeref *>(expr)
	|| dynamic_cast<TokenSubscript *>(expr)
	|| dynamic_cast<TokenSubscriptExpr *>(expr)
	|| dynamic_cast<TokenDerefExpr *>(expr);
}

TokenBase *Program::parseAddressOfExpression(TokenBase *ampersand)
{
    bool paren = false;
    if ( peekToken() && peekToken()->id() == TokenID::tkOpBrk
      && next_parenthesized_type_is_compound_literal(*this) )
    {
	TokenBase *compound = parseExpression(nextToken(), true, false, false);
	// `&(T){...}` is the address of the unnamed compound-literal object.
	// The CIR backend emits the literal as an N_COMPOUND_LITERAL value (an
	// addressable lvalue), so wrap it — like any other lvalue — in
	// TokenAddrExpr to yield N_ADDR(compound), a pointer to the object.
	DataDefPTR *aptr = getPointerType(compound->datadef());
	return new TokenAddrExpr(compound, aptr);
    }
    if ( peekToken() && peekToken()->id() == TokenID::tkOpBrk )
    {
	nextToken(); // consume '('
	paren = true;
    }

    TokenBase *addr_tb = nextToken();
    TokenBase *addr_expr = NULL;
    if ( paren )
    {
	addr_expr = parseExpression(addr_tb, true, false, true, 1);
	if ( is_addressable_expression(addr_expr) )
	{
	    DataDefPTR *aptr = getPointerType(addr_expr->datadef());
	    return new TokenAddrExpr(addr_expr, aptr);
	}
	if ( TokenVar *tv = dynamic_cast<TokenVar *>(addr_expr) )
	{
	    if ( tv->var.type && tv->var.type->is_function() )
		return tv;
	    tv->var.flags |= vfADDRTAKEN;
	    DataDefPTR *aptr = getPointerType(tv->var.type);
	    return new TokenAddrOf(tv->var, aptr);
	}
	Throw(addr_tb) << "expecting addressable expression after '&('" << flush;
    }

    bool postfix_chain = is_contextual_identifier_token(addr_tb)
	&& peekToken()
	&& (peekToken()->id() == TokenID::tkDot
	 || peekToken()->id() == TokenID::tkDeRef
	 || peekToken()->id() == TokenID::tkOpSqr);
    if ( !postfix_chain && addr_tb->type() == TokenType::ttString
      && peekToken() && peekToken()->id() == TokenID::tkOpSqr )
    {
	addr_expr = parseExpression(addr_tb, true, false, false, 0);
	if ( is_addressable_expression(addr_expr) )
	{
	    DataDefPTR *aptr = getPointerType(addr_expr->datadef());
	    return new TokenAddrExpr(addr_expr, aptr);
	}
	Throw(addr_tb) << "expecting addressable string subscript after '&'" << flush;
    }
    if ( postfix_chain )
    {
	if ( addr_tb->type() == TokenType::ttIdentifier )
	    addr_expr = parsePostfixChain(addr_tb);
	else
	    addr_expr = parseExpression(addr_tb, true, false, false, 0);
	if ( is_addressable_expression(addr_expr) )
	{
	    DataDefPTR *aptr = getPointerType(addr_expr->datadef());
	    return new TokenAddrExpr(addr_expr, aptr);
	}
	if ( TokenVar *tv = dynamic_cast<TokenVar *>(addr_expr) )
	{
	    tv->var.flags |= vfADDRTAKEN;
	    DataDefPTR *aptr = getPointerType(tv->var.type);
	    return new TokenAddrOf(tv->var, aptr);
	}
	Throw(addr_tb) << "expecting addressable expression after '&'" << flush;
    }

    if ( !is_contextual_identifier_token(addr_tb) )
	Throw(addr_tb) << "expecting variable name after '&'" << flush;
    std::string aname = contextual_identifier_name(addr_tb);
    Variable *avar = findVariable(aname);
    if ( !avar && is_dynamic_symbol_fallback_enabled()
      && is_dynamic_symbol_allowed(aname) )
    {
	void *sym = dlsym(RTLD_DEFAULT, aname.c_str());
	if ( sym )
	    avar = addFunction(aname,
		datatype_vec_t{dynamic_symbol_fallback_return_type(aname)},
		(fVOIDFUNC)sym);
    }
    if ( !avar )
	Throw(addr_tb) << "undeclared identifier '" << aname << "'" << flush;
    if ( avar->type && avar->type->is_function() )
	return new TokenVar(*avar);
    avar->flags |= vfADDRTAKEN;
    DataDefPTR *aptr = getPointerType(avar->type);
    return new TokenAddrOf(*avar, aptr);
}

// Is `cls` the same as `target`, or derived (single-inheritance chain) from it?
// Used for protected-member access: a protected member is reachable from the
// declaring class AND any class derived from it.
static bool class_is_or_derives(DataDefCLASS *cls, DataDefCLASS *target)
{
    return cls && cls->is_or_derives_from(target);
}

// Access-control core (P2.5 / P2.5c). Given a member's access flag `acc`
// (0=public, vfPRIVATE, vfPROTECTED), the class that OWNS it, the member name,
// and the class whose method body we are currently parsing (NULL outside any
// method), return a human-readable violation string, or empty when access is
// allowed. Public is always allowed; private only from the SAME class's
// methods; protected from the declaring class or a derived class's methods.
// Used by BOTH the data-member path (flag from member_access) and the
// method-call path (flag from the method Variable's flags) — one rule, two
// callers (I6). `friend`, access-changing using-declarations, and private
// inheritance are out of scope.
static std::string access_flag_violation(uint32_t acc, DataDefCLASS *owner_cls,
					 const std::string &name,
					 DataDefCLASS *cur_class)
{
    if ( acc == 0 || !owner_cls )
	return std::string();   // public, or no class owner -> always allowed
    if ( acc == vfPRIVATE )
    {
	if ( cur_class == owner_cls )
	    return std::string();
	return "'" + name + "' is a private member of '" + owner_cls->name + "'";
    }
    // vfPROTECTED: reachable from the declaring class or any derived class.
    if ( cur_class && class_is_or_derives(cur_class, owner_cls) )
	return std::string();
    return "'" + name + "' is a protected member of '" + owner_cls->name + "'";
}

// Data-member access check: the access flag comes from the owner struct's
// per-member member_access vector. Only class objects carry non-zero flags (a
// plain C struct leaves every entry 0=public), so this is a no-op for structs.
static std::string member_access_violation(DataDef *owner_type,
					    const std::string &name,
					    DataDefCLASS *cur_class)
{
    DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(owner_type);
    if ( !sdd )
	return std::string();
    return access_flag_violation(sdd->m_access(name),
				 dynamic_cast<DataDefCLASS *>(owner_type),
				 name, cur_class);
}

// Method-call access check (P2.5c): the access flag lives on the resolved
// method Variable's flags (set in TokenCLASS::parse alongside data members).
// `owner_type` is the static class the call is made on; `method` is the
// Variable findMethod resolved; `display_name` is the UNMANGLED name the user
// wrote (the method Variable carries a mangled name, which makes for a poor
// diagnostic). Returns a violation string or empty.
static std::string method_access_violation(DataDef *owner_type,
					   const Variable *method,
					   const std::string &display_name,
					   DataDefCLASS *cur_class)
{
    if ( !method )
	return std::string();
    uint32_t acc = method->flags & (vfPRIVATE | vfPROTECTED);
    return access_flag_violation(acc, dynamic_cast<DataDefCLASS *>(owner_type),
				 display_name, cur_class);
}

TokenBase *Program::parseExpression(TokenBase *tb, bool conditional, bool ternary_branch,
				    bool stop_on_closing_paren, int initial_brackets,
				    bool push_back_comma)
{
    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
    TokenOperator *to;
    stack<TokenBase *> exStack;
    stack<TokenBase *> opStack;
    TokenDataType *bt;
    Variable *var;
    bool done = false;
    int brackets = initial_brackets;
    auto awaiting_prefix_step_operand = [&]() -> bool
    {
	return exStack.empty()
	    && !opStack.empty()
	    && (opStack.top()->id() == TokenID::tkInc
	     || opStack.top()->id() == TokenID::tkDec);
    };

    DBG(std::cout << tb->line << ':' << tb->column << ":Program::parseExpression(" << tb->get() << " type: " << (int)tb->type() << ") start" << (conditional ? " conditional" : "") << std::endl);

    while ( !done && tb )
    {
	redo_expression_token:
	switch(tb->type())
	{
	    case TokenType::ttInteger:
	        DBG(cout << "Pushing integer: " << (int)tb->get() << " onto exStack" << endl);
		exStack.push(tb); // exStack.push(tb->clone());
		break;
	    case TokenType::ttReal:
	        DBG(cout << "Pushing number: " << ((TokenReal *)tb)->dval() << " onto exStack" << endl);
		exStack.push(tb);
		break;
	    case TokenType::ttSymbol:
	    	if ( tb->id() == TokenID::tkSemi )
		{
		    DBG(cout << "parseExpression: found semicolon" << endl);
		    done = true;
		}
	    	if ( tb->id() == TokenID::tkComma )
		{
		    // C comma operator inside parenthesized expression:
		    // build a real comma-expression node so left-side side
		    // effects are preserved and the whole expression returns
		    // the right-side value.
		    if ( brackets > 0 )
		    {
			DBG(cout << "parseExpression: comma operator (brackets=" << brackets << ")" << endl);
			// Flush pending operators above the last '(' so the
			// left-side expression is fully reduced, then push the
			// comma operator at the lowest precedence within the
			// parenthesized sub-expression.
			while ( !opStack.empty() && opStack.top()->get() != '(' )
			    popOperator(opStack, exStack);
			TokenOperator *comma = (TokenOperator *)tb;
			comma->left = NULL;
			comma->right = NULL;
			opStack.push(comma);
			break;
		    }
		    DBG(cout << "parseExpression: found comma" << endl);
		    if ( push_back_comma ) pushToken(tb);
		    done = true;
		}
		break;
	    case TokenType::ttMultiOp:
	    case TokenType::ttOperator:
	    	if ( tb->id() == TokenID::tkComma )
		{
		    // C comma operator inside parenthesized expression
		    if ( brackets > 0 )
		    {
			DBG(cout << "parseExpression: comma operator (brackets=" << brackets << ")" << endl);
			while ( !opStack.empty() && opStack.top()->get() != '(' )
			    popOperator(opStack, exStack);
			TokenOperator *comma = (TokenOperator *)tb;
			comma->left = NULL;
			comma->right = NULL;
			opStack.push(comma);
			break;
		    }
		    DBG(cout << "parseExpression: found comma" << endl);
		    if ( push_back_comma ) pushToken(tb);
		    done = true;
		    break;
		}
		// subscript: var[index] or lambda: [](params) { body }
		if ( tb->id() == TokenID::tkOpSqr )
		{
		    // Chained subscript on a multi-dim C fixed-size array: arr[i][j]
		    // Append to the existing TokenSubscript's extra_indices vector.
		    if ( !exStack.empty() && exStack.top()->type() == TokenType::ttSubscript )
		    {
			TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(exStack.top());
			if ( tsub && tsub->object.is_fixed_array()
			  && tsub->extra_indices.size() + 2 <= tsub->object.dims.size() )
			{
			    TokenBase *idx = parseExpression(nextToken());
			    TokenBase *clsqr = nextToken(); // consume ]
			    if ( !clsqr || clsqr->id() != TokenID::tkClSqr )
				Throw(tb) << "Expected ] in subscript expression" << flush;
			    tsub->extra_indices.push_back(idx);
			    break;
			}
		    }
		    // if top of exStack is a variable, treat [ as subscript operator
		    if ( !exStack.empty() && exStack.top()->type() == TokenType::ttVariable )
		    {
			TokenVar *tv = dynamic_cast<TokenVar *>(exStack.top());
			exStack.pop();
			// parse index expression (stops at ] via peek-stop below)
			TokenBase *idx = parseExpression(nextToken());
			TokenBase *clsqr = nextToken(); // consume ]
			if ( !clsqr || clsqr->id() != TokenID::tkClSqr )
			    Throw(tb) << "Expected ] in subscript expression" << flush;
			// Swapped subscript: N[ptr] where N is a plain
			// integer and the index is a pointer.  C defines
			// a[b] as *(a+b), so N[ptr] == ptr[N].  Swap so
			// the pointer is the base and the integer is the
			// index.
			if ( tv->var.type->is_numeric()
			  && !tv->var.is_fixed_array()
			  && !tv->var.type->is_pointer()
			  && idx->datadef()
			  && idx->datadef()->is_pointer() )
			{
			    DataDef *elem_type = unwrap_subscript_element_type(idx->datadef());
			    DBG(cout << "parseExpression: swapped subscript "
				     << tv->var.name << "[ptr]" << endl);
			    exStack.push(new TokenSubscriptExpr(idx, new TokenVar(tv->var), elem_type));
			    break;
			}
			DBG(cout << "parseExpression: subscript on " << tv->var.name << endl);
			exStack.push(new TokenSubscript(tv->var, idx));
			break;
		    }
		    // Widened detection: complex value-producing expressions
		    // whose datadef() reports a pointer (TokenAdd / TokenSub /
		    // TokenAssign — see their datadef() overrides). Lets
		    // `(p + n)[i]`, `(q = p + 1)[i]`, `(mud = imc_mudof(arg))[0]`
		    // parse as subscript-on-pointer rather than lambda.
		    // TokenSubscriptExpr::compile branches on the base_expr type
		    // and uses compile() (not operand()) for these complex bases.
		    bool top_is_complex_ptr_expr = false;
		    if ( !exStack.empty() )
		    {
			TokenBase *xt = exStack.top();
			if ( dynamic_cast<TokenAdd *>(xt) != NULL
			  || dynamic_cast<TokenSub *>(xt) != NULL
			  || dynamic_cast<TokenAssign *>(xt) != NULL )
			{
			    // Direct datadef() check first — covers
			    // `(char *p + n)[i]` where p is a real pointer.
			    DataDef *xd = xt->datadef();
			    if ( xd && xd->is_pointer() )
				top_is_complex_ptr_expr = true;
			    // Fallback: a TokenVar of a fixed-array decays
			    // to a pointer in arithmetic context, but its
			    // raw datadef reports the element type. Walk
			    // into TokenAdd/Sub operands to find such a
			    // base. Surfaced by `(buf + strlen(buf))[i]`
			    // where buf is `char[N]`.
			    if ( !top_is_complex_ptr_expr )
			    {
				TokenBase *op_l = NULL, *op_r = NULL;
				if ( TokenAdd *ta = dynamic_cast<TokenAdd *>(xt) )
				    { op_l = ta->left; op_r = ta->right; }
				else if ( TokenSub *ts = dynamic_cast<TokenSub *>(xt) )
				    { op_l = ts->left; op_r = ts->right; }
				else if ( TokenAssign *tas = dynamic_cast<TokenAssign *>(xt) )
				    { op_l = tas->left; op_r = tas->right; }
				auto is_fixed_array_var = [](TokenBase *t) -> bool {
				    if ( !t ) return false;
				    if ( TokenVar *tv = dynamic_cast<TokenVar *>(t) )
					return tv->var.is_fixed_array();
				    return false;
				};
				if ( is_fixed_array_var(op_l) || is_fixed_array_var(op_r) )
				    top_is_complex_ptr_expr = true;
			    }
			}
		    }
		    bool top_is_cast_subscriptable = false;
		    if ( !exStack.empty() )
		    {
			if ( TokenCast *tc = dynamic_cast<TokenCast *>(exStack.top()) )
			{
			    DataDef *cd = tc->datadef();
			    if ( cd && (cd->is_pointer() || cd->is_simd()) )
				top_is_cast_subscriptable = true;
			}
		    }
		    if ( !exStack.empty()
		      && (exStack.top()->type() == TokenType::ttMember
		       || exStack.top()->type() == TokenType::ttSubscript
		       || dynamic_cast<TokenDerefExpr *>(exStack.top()) != NULL
		       || dynamic_cast<TokenDeref *>(exStack.top()) != NULL
		       || top_is_complex_ptr_expr
		       || top_is_cast_subscriptable) )
		    {
			TokenBase *base_expr = exStack.top();
			exStack.pop();
			TokenBase *idx = parseExpression(nextToken());
			TokenBase *clsqr = nextToken(); // consume ]
			if ( !clsqr || clsqr->id() != TokenID::tkClSqr )
			    Throw(tb) << "Expected ] in subscript expression" << flush;
			DataDef *elem_type = base_expr->datadef();
			// Distinguish a fixed-array struct member (e.g.
			// `SKILLTYPE *arr[N]` — array of pointers) from a
			// stored pointer member (`SKILLTYPE *p`). For the
			// first, `arr[i]` yields the declared element type
			// as-is (still a SKILLTYPE *); for the second, `p[i]`
			// derefs the pointer to its base type.
			TokenMember *tm = dynamic_cast<TokenMember *>(base_expr);
			bool member_is_fixed_array = tm && tm->is_fixed_array_member();
			if ( !member_is_fixed_array && elem_type
			  && (elem_type->is_pointer() || dynamic_cast<DataDefCArray *>(elem_type) != NULL) )
			    elem_type = unwrap_subscript_element_type(elem_type);
			// SIMD vector subscript: v[i] yields the lane element type.
			if ( elem_type && elem_type->is_simd() )
			    elem_type = static_cast<DataDefSIMD *>(elem_type)->element_type;
			// Fixed-array decay: when the base was widened via the
			// fixed-array fallback, the chain's datadef() reports the
			// element type (TokenVar of fixed-array does so), and
			// is_pointer is false — so the unwrap above didn't fire.
			// Walk into TokenAdd/Sub/Assign operands to recover the
			// fixed-array's element type. Surfaced by `(buf + N)[i]`
			// where buf is `char[K]`.
			if ( top_is_complex_ptr_expr
			  && (!base_expr->datadef() || !base_expr->datadef()->is_pointer()) )
			{
			    TokenBase *op_l = NULL, *op_r = NULL;
			    if ( TokenAdd *ta = dynamic_cast<TokenAdd *>(base_expr) )
				{ op_l = ta->left; op_r = ta->right; }
			    else if ( TokenSub *ts = dynamic_cast<TokenSub *>(base_expr) )
				{ op_l = ts->left; op_r = ts->right; }
			    else if ( TokenAssign *tas = dynamic_cast<TokenAssign *>(base_expr) )
				{ op_l = tas->left; op_r = tas->right; }
			    auto fixed_array_var = [](TokenBase *t) -> Variable * {
				if ( !t ) return NULL;
				if ( TokenVar *tv = dynamic_cast<TokenVar *>(t) )
				    return tv->var.is_fixed_array() ? &tv->var : NULL;
				return NULL;
			    };
			    Variable *fav = fixed_array_var(op_l);
			    if ( !fav ) fav = fixed_array_var(op_r);
			    if ( fav )
				elem_type = fav->type;
			}
			DBG(cout << "parseExpression: subscript on expression base" << endl);
			exStack.push(new TokenSubscriptExpr(base_expr, idx, elem_type));
			break;
		    }
		    // General fallback: any expression with a pointer datadef
		    // can be subscripted — covers ternary results, function
		    // call results, and other complex expressions.
		    if ( !exStack.empty() )
		    {
			DataDef *dd = exStack.top()->datadef();
			TokenComma *top_comma = dynamic_cast<TokenComma *>(exStack.top());
			TokenVar *comma_fixed_array = NULL;
			if ( top_comma )
			    comma_fixed_array = dynamic_cast<TokenVar *>(top_comma->right);
			bool comma_returns_fixed_array = comma_fixed_array
			    && comma_fixed_array->var.is_fixed_array()
			    && comma_fixed_array->var.type;
			if ( dd && (dd->is_pointer()
			  || dd->type() == DataType::dtCHARptr
			  || comma_returns_fixed_array) )
			{
			    TokenBase *base_expr = exStack.top();
			    exStack.pop();
			    TokenBase *idx = parseExpression(nextToken());
			    TokenBase *clsqr = nextToken();
			    if ( !clsqr || clsqr->id() != TokenID::tkClSqr )
				Throw(tb) << "Expected ] in subscript expression" << flush;
			    DataDef *elem_type = comma_returns_fixed_array
				? comma_fixed_array->var.type : dd;
			    if ( elem_type && elem_type->is_pointer() )
			    {
				DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(elem_type);
				elem_type = (pdd && pdd->base_type) ? pdd->base_type : &ddINT64;
			    }
			    DBG(cout << "parseExpression: subscript on generic expression" << endl);
			    exStack.push(new TokenSubscriptExpr(base_expr, idx, elem_type));
			    break;
			}
		    }
		    DBG(cout << "parseExpression: detected [ — parsing lambda" << endl);
		    TokenBase *lambda = parseLambda();
		    exStack.push(lambda);
		    break;
		}
		if ( tb->id() == TokenID::tkOpBrk )
		{
		    if ( peekToken() && peekToken()->id() == TokenID::tkOpBrc )
		    {
			nextToken(); // consume '{'
			pushCompound();
			TokenBase *stmt_expr = parseCompound();
			// Mark it a statement-expression so translate_stmt emits it
			// as an expression-statement (value-producing), not a plain
			// block — required when it is the last item of an enclosing
			// `({...})` (a nested stmt-expr `({ ({...}); })`).
			if ( TokenCpnd *secp = dynamic_cast<TokenCpnd *>(stmt_expr) )
			    secp->is_stmt_expr = true;
			TokenBase *close = nextToken();
			if ( !close || close->id() != TokenID::tkClBrk )
			    Throw(close ? close : tb) << "Expected ')' after statement expression" << flush;
			exStack.push(stmt_expr);
			if ( stop_on_closing_paren && initial_brackets == 0 )
			    return stmt_expr;
			break;
		    }
		    // check for cast expression: (TYPE [*...]) expr
		    TokenBase *peek1 = peekToken();
		    DataDef *cast_dd = NULL;
		    std::string cast_typedef_name;
		    TokenBase *cast_qualifier = NULL;
		    if ( peek1 )
		    {
			if ( peek1->id() == TokenID::tkCONST
			  || peek1->id() == TokenID::tkVOLATILE )
			{
			    cast_qualifier = nextToken();
			    peek1 = peekToken();
			}
			// __attribute__((vector_size(N))) type — inline SIMD type in cast/compound literal
			size_t cast_vector_bytes = 0;
			if ( peek1 && is_attribute_identifier_token(peek1) )
			{
			    TokenBase *attr_tok = nextToken();
			    TokenBase *after_attr = consume_gnu_attributes(*this, attr_tok, NULL, NULL, NULL, &cast_vector_bytes);
			    if ( after_attr )
			    {
				pushToken(after_attr);
				peek1 = peekToken();
			    }
			}
			if ( peek1->type() == TokenType::ttDataType )
			{
			    cast_dd = &((TokenDataType *)peek1)->definition;
			    if ( cast_vector_bytes > 0 )
				cast_dd = new DataDefSIMD(cast_dd, "", cast_vector_bytes);
			}
			else if ( peek1->id() == TokenID::tkSTRUCT || peek1->id() == TokenID::tkUNION )
			{
			    // (struct/union Tag *) — peek further
			    TokenBase *save1 = nextToken(); // consume 'struct' / 'union'
			    TokenBase *save2 = peekToken();
			    if ( save2 && save2->type() == TokenType::ttIdentifier )
			    {
				std::string sname = ((TokenIdent *)save2)->str;
				datadef_map_iter sdmi = struct_map.find(sname);
				if ( sdmi != struct_map.end() )
				{
				    nextToken(); // consume tag name
				    cast_dd = sdmi->second;
				}
				else
				{
				    pushToken(save1); // push 'struct' back
				}
			    }
			    else
				pushToken(save1);
			}
			else if ( peek1->id() == TokenID::tkENUM )
			{
			    nextToken(); // consume enum
			    if ( peekToken() && is_contextual_identifier_token(peekToken()) )
				nextToken(); // consume optional tag
			    cast_dd = &ddINT32;
			}
			else if ( peek1->type() == TokenType::ttIdentifier )
			{
			    std::string tname = ((TokenIdent *)peek1)->str;
			    datatype_map_iter tdmi = datatype_map.find(tname);
			    if ( tdmi != datatype_map.end() )
			    {
				cast_dd = &tdmi->second->definition;
				cast_typedef_name = tname;
			    }
			}
		    }
		    if ( !cast_dd && cast_qualifier )
			pushToken(cast_qualifier);
		    if ( cast_dd )
		    {
			// speculatively consume the type token (if not struct, which was already consumed)
			if ( peekToken() && (peekToken()->type() == TokenType::ttDataType
			||  (peekToken()->type() == TokenType::ttIdentifier && datatype_map.count(((TokenIdent *)peekToken())->str))) )
			    nextToken();
			// consume pointer stars (skip const/restrict qualifiers)
			while ( peekToken()
			     && (peekToken()->id() == TokenID::tkMul
			      || peekToken()->id() == TokenID::tkCONST
			      || peekToken()->id() == TokenID::tkVOLATILE
			      || peekToken()->id() == TokenID::tkRESTRICT) )
			{
			    TokenBase *pt = nextToken();
			    if ( pt->id() == TokenID::tkMul )
				cast_dd = getPointerType(cast_dd);
			    // const/restrict are skipped — no JIT effect
			}
			// Array declarator in cast/compound literal: (int []){...}
			// or (int [3]){...}. Consume [N] and record the element
			// type so the compound literal path can build a synthetic
			// struct with N elements of that type.
			DataDef *array_elem_dd = NULL;
			int64_t array_explicit_count = 0;
			if ( peekToken() && peekToken()->id() == TokenID::tkOpSqr )
			{
			    array_elem_dd = cast_dd;
			    nextToken(); // consume '['
			    if ( peekToken() && peekToken()->id() != TokenID::tkClSqr )
			    {
				// explicit count: (int [3]){...}
				TokenBase *cnt = nextToken();
				if ( cnt->type() == TokenType::ttInteger )
				    array_explicit_count = ((TokenInt *)cnt)->ival();
			    }
			    TokenBase *csq = nextToken();
			    if ( !csq || csq->id() != TokenID::tkClSqr )
				Throw(csq ? csq : tb) << "Expected ']' after array size in cast" << flush;
			    // For plain casts (not compound literals), treat as pointer
			    cast_dd = getPointerType(cast_dd);
			}
			// Function-pointer cast: `(RET (*)(PARAMS)) expr`. After the
			// return type (plus any pointer stars) we may see `(*)` and
			// then a parameter list. Reuse parseFnPtrParams() to build the
			// FuncDef, then wrap in DataDefFPTR.
			if ( peekToken() && peekToken()->id() == TokenID::tkOpBrk )
			{
			    TokenBase *open = nextToken();    // consume '('
			    TokenBase *star = peekToken();
			    if ( star && star->id() == TokenID::tkMul )
			    {
				nextToken(); // consume '*'
				TokenBase *close1 = nextToken();
				if ( !close1 || close1->id() != TokenID::tkClBrk )
				    Throw(close1 ? close1 : open) << "expected ')' after '(*' in function pointer cast" << flush;
				TokenBase *open2 = nextToken();
				if ( !open2 || open2->id() != TokenID::tkOpBrk )
				    Throw(open2 ? open2 : open) << "expected '(' to introduce parameter list in function pointer cast" << flush;
				FuncDef *func = parseFnPtrParams(*cast_dd);
				cast_dd = new DataDefFPTR(func);
			    }
			    else
			    {
				// not a function-pointer cast — push '(' back and fall
				// through to the regular close-paren handling below.
				pushToken(open);
			    }
			}
			// must have closing )
			if ( peekToken() && peekToken()->id() == TokenID::tkClBrk )
			{
			    nextToken(); // consume )
			    // Array compound literal: (int []){0,1,2}
			    // Build a synthetic DataDefSTRUCT with N uniform
			    // elements so existing struct-init code handles it.
			    if ( array_elem_dd
			      && peekToken() && peekToken()->id() == TokenID::tkOpBrc )
			    {
				int64_t n = array_explicit_count;
				if ( n <= 0 )
				{
				    // Count top-level comma-separated items in {...}
				    std::vector<TokenBase *> peek_buf;
				    TokenBase *ob = nextToken();
				    peek_buf.push_back(ob);
				    n = 1;
				    int depth = 1;
				    while ( depth > 0 )
				    {
					TokenBase *t = nextToken();
					if ( !t )
					    break;
					peek_buf.push_back(t);
					if ( t->id() == TokenID::tkOpBrc )
					    ++depth;
					else if ( t->id() == TokenID::tkClBrc )
					    --depth;
					else if ( depth == 1 && t->id() == TokenID::tkComma )
					    ++n;
				    }
				    for ( std::vector<TokenBase *>::reverse_iterator it = peek_buf.rbegin();
					  it != peek_buf.rend(); ++it )
					pushToken(*it);
				}
				DataDefSTRUCT *arr_sdd = new DataDefSTRUCT("__compound_array", 0);
				for ( int64_t i = 0; i < n; i++ )
				    arr_sdd->addMember("_" + std::to_string(i), *array_elem_dd, 1);
				arr_sdd->finalize();
				cast_dd = arr_sdd;
			    }
			    // C99 compound literal: (type){ init_list }
			    // After (type), if '{' follows, parse as a temporary
			    // struct/array initialization rather than a cast.
				    if ( peekToken() && peekToken()->id() == TokenID::tkOpBrc )
				    {
					std::function<TokenStructLit *(DataDefSTRUCT *)> read_compound_struct_lit;
					read_compound_struct_lit = [&](DataDefSTRUCT *current_sdd) -> TokenStructLit * {
					    nextToken(); // consume '{'
					    TokenStructLit *slit = new TokenStructLit();
					    while ( true )
					    {
						TokenBase *look = peekToken();
						if ( !look )
						    Throw(tb) << "Unexpected end of data in compound literal" << flush;
						if ( look->id() == TokenID::tkClBrc )
						{
						    nextToken();
						    break;
						}
						if ( look->id() == TokenID::tkOpBrc )
						{
						    DataDefSTRUCT *elem_sdd = NULL;
						    if ( current_sdd )
						    {
							size_t idx = slit->inits.size();
							if ( idx < current_sdd->members.size() )
							    elem_sdd = dynamic_cast<DataDefSTRUCT *>(current_sdd->members[idx].second);
						    }
						    slit->inits.push_back(read_compound_struct_lit(elem_sdd));
						}
						else
						{
						TokenBase *elem = nextToken();
						if ( elem->id() == TokenID::tkDot
						  || (current_sdd && is_contextual_identifier_token(elem)
						   && peekToken() && peekToken()->id() == TokenID::tkTerC) )
						{
						    std::vector<std::string> field_path;
						    TokenBase *field_tok = elem;
						    if ( elem->id() == TokenID::tkDot )
						    {
							field_tok = nextToken();
							if ( !is_contextual_identifier_token(field_tok) )
							    Throw(field_tok) << "Expecting field name in compound literal designator" << flush;
							field_path.push_back(contextual_identifier_name(field_tok));
							while ( peekToken() && peekToken()->id() == TokenID::tkDot )
							{
							    nextToken();
							    TokenBase *nested_field = nextToken();
							    if ( !is_contextual_identifier_token(nested_field) )
								Throw(nested_field) << "Expecting field name in compound literal designator" << flush;
							    field_path.push_back(contextual_identifier_name(nested_field));
							}
							TokenBase *eq = nextToken();
							if ( !eq || eq->id() != TokenID::tkAssign )
							    Throw(eq ? eq : field_tok) << "Expecting '=' after compound literal designator" << flush;
						    }
						    else
						    {
							field_path.push_back(contextual_identifier_name(field_tok));
							nextToken(); // consume ':'
						    }
						    TokenBase *value_tok = nextToken();
						    std::vector<TokenBase *> *target_inits = &slit->inits;
						    DataDefSTRUCT *target_sdd = current_sdd;
							size_t field_index = 0;
							for ( size_t pi = 0; pi < field_path.size(); ++pi )
							{
							    const std::string &field_name = field_path[pi];
							    field_index = find_struct_member_index(target_sdd, field_name);
							    if ( !target_sdd || field_index >= target_sdd->members.size() )
								Throw(field_tok) << "Unknown field '" << field_name << "' in compound literal designator" << flush;
							    if ( target_inits->size() <= field_index )
								target_inits->resize(field_index + 1, NULL);
							    if ( pi + 1 == field_path.size() )
								break;
							    DataDefSTRUCT *nested_sdd = dynamic_cast<DataDefSTRUCT *>(target_sdd->members[field_index].second);
							    if ( !nested_sdd )
								Throw(field_tok) << "Field '" << field_name << "' is not a struct in compound literal designator" << flush;
							    TokenStructLit *nested_lit = dynamic_cast<TokenStructLit *>((*target_inits)[field_index]);
							    if ( !nested_lit )
							    {
								nested_lit = new TokenStructLit();
								(*target_inits)[field_index] = nested_lit;
							    }
							    target_inits = &nested_lit->inits;
							    target_sdd = nested_sdd;
							}
							if ( value_tok && value_tok->id() == TokenID::tkOpBrc )
							{
							    pushToken(value_tok);
							    DataDefSTRUCT *nested_sdd = target_sdd
								? dynamic_cast<DataDefSTRUCT *>(target_sdd->members[field_index].second)
								: NULL;
							    (*target_inits)[field_index] = read_compound_struct_lit(nested_sdd);
							}
							else
							    (*target_inits)[field_index] = parseExpression(value_tok);
						    }
						    else
							slit->inits.push_back(parseExpression(elem));
						}
						if ( peekToken() && peekToken()->id() == TokenID::tkComma )
						    nextToken();
					    }
					    return slit;
					};
					TokenStructLit *slit = read_compound_struct_lit(dynamic_cast<DataDefSTRUCT *>(cast_dd));
					slit->setDataType(cast_dd);
					if ( !array_elem_dd )
					    slit->typedef_name = cast_typedef_name;
					else
					{
					    slit->array_elem_dd = array_elem_dd;
					    // The element type's typedef alias (e.g. `(S[]){...}`
					    // where `typedef struct S {...} S`) so the CIR array
					    // path can emit ID("S") instead of mis-rendering the
					    // struct as a scalar int (pr98366).
					    slit->typedef_name = cast_typedef_name;
					}
					TokenBase *lit_expr = slit;
					// Array compound literals decay to pointer.
					// Wrap in a cast so the expression type is
					// ptr-to-element for subscript/assign purposes.
					if ( array_elem_dd )
					{
					    DataDef *ptr_dd = getPointerType(array_elem_dd);
					    lit_expr = new TokenCast(ptr_dd, slit);
					}
					exStack.push(lit_expr);
					break;
				    }
			    TokenBase *cast_expr_tb = nextToken();
			    // Null out _prv_token so unary operators at the head of
			    // the cast body (`&addr`, `*ptr`, `-x`) see a unary
			    // position. Otherwise the cast's close-paren leaks into
			    // isUnaryPosition and they mis-parse as binary ops.
			    _prv_token = NULL;
			    TokenBase *cast_expr = NULL;
			    // Casts in C bind tighter than binary operators: `(long)q
			    // - n` means `((long)q) - n`, not `(long)(q - n)`.
			    // Identifier operands, including calls, stop before any
			    // trailing binary operator so `(uint64)f() << 32` widens
			    // before the shift.
		    if ( cast_expr_tb
		      && cast_expr_tb->type() == TokenType::ttIdentifier
		      && peekToken() && peekToken()->id() == TokenID::tkOpBrk
		      && (cast_expr = parse_cast_function_call_operand(*this, cast_expr_tb)) )
		    {
		    }
		    else if ( cast_expr_tb
		      && cast_expr_tb->type() == TokenType::ttIdentifier
		      && !(peekToken() && peekToken()->id() == TokenID::tkOpBrk) )
		    {
			cast_expr = parsePostfixChain(cast_expr_tb);
		    }
		    else if ( cast_expr_tb && cast_expr_tb->id() == TokenID::tkMul )
		    {
			cast_expr = parse_cast_unary_deref_operand(*this, cast_expr_tb);
			if ( !cast_expr )
			    cast_expr = parseExpression(cast_expr_tb, true);
		    }
		    else if ( cast_expr_tb && cast_expr_tb->id() == TokenID::tkBand )
		    {
			cast_expr = parseAddressOfExpression(cast_expr_tb);
		    }
		    else if ( cast_expr_tb && cast_expr_tb->id() == TokenID::tkOpBrk )
		    {
				// The cast body starts with `(`.  Two possibilities:
				// a) Chained cast: `(long)(int)x` — the inner `(`
				//    starts another `(type)expr` cast. Detected by
				//    peeking for a type token inside the parens.
				// b) Parenthesized expression: `(int)(a - b)`.
				// For (a), push `(` back and let parseExpression
				// handle the inner cast naturally. For (b), consume
				// `(` and use stop_on_closing_paren.
				TokenBase *inner_peek = peekToken();
				bool inner_is_cast = inner_peek
				    && (inner_peek->type() == TokenType::ttDataType
					|| inner_peek->id() == TokenID::tkCONST
					|| (inner_peek->type() == TokenType::ttIdentifier
					    && (datatype_map.count(((TokenIdent *)inner_peek)->str)
						|| struct_map.count(((TokenIdent *)inner_peek)->str))));
				if ( inner_is_cast )
				{
				    // Push `(` back — parseExpression will handle it
				    // as a cast via the normal `(type)expr` path.
				    pushToken(cast_expr_tb);
				    _prv_token = NULL;
				    cast_expr = parseExpression(nextToken(), true);
				}
				else
				{
				    TokenBase *first_inner = nextToken();
				    cast_expr = parseExpression(first_inner, true, false, true, 1);
				}
			    }
			    else if ( cast_expr_tb
				   && (cast_expr_tb->type() == TokenType::ttInteger
				    || cast_expr_tb->type() == TokenType::ttReal
				    || cast_expr_tb->type() == TokenType::ttChar
				    || cast_expr_tb->type() == TokenType::ttString) )
			    {
				// Simple literal: cast binds tightly.
				// (double)5 consumes only 5, not < 3.0.
				cast_expr = materialize_cast_literal_operand(*this, cast_expr_tb);
			    }
			    else if ( cast_expr_tb
				   && (cast_expr_tb->id() == TokenID::tkBnot  // ~
				    || cast_expr_tb->id() == TokenID::tkNeg   // -
				    || cast_expr_tb->id() == TokenID::tkLnot  // !
				    || cast_expr_tb->id() == TokenID::tkAdd)  // +
				   && peekToken()
				   && (peekToken()->type() == TokenType::ttInteger
				    || peekToken()->type() == TokenType::ttReal) )
			    {
				// Unary operator + literal: cast binds tightly.
				// (unsigned char)~0 consumes only ~0, not * ' '.
				TokenBase *operand_tb = nextToken();
				TokenOperator *uop = dynamic_cast<TokenOperator *>(cast_expr_tb);
				if ( uop ) { uop->right = operand_tb; cast_expr = uop; }
				else cast_expr = operand_tb;
			    }
			    else
			    {
				cast_expr = parseExpression(cast_expr_tb, true);
			    }
			    exStack.push(new TokenCast(cast_dd, cast_expr));
			    DBG(cout << "parseExpression: cast to " << cast_dd->name << endl);
			    // Caller wants only the cast group, not whatever
			    // follows. Without this the inner parseExpression
			    // (e.g. the `*(CAST)X` deref-of-cast detector) keeps
			    // consuming through a following `=` etc., returning
			    // a TokenAssign with the cast as its left — and the
			    // outer wrapper ends up holding the assignment
			    // instead of the bare cast.
			    //
			    // Restricted to initial_brackets == 0 so the if/while
			    // pre-paren callers (parse_parenthesized_expression
			    // passes initial_brackets=1) keep parsing the rest of
			    // the condition. SMAUG `if ((int)a == b == 0)` from
			    // QUICKMATCH macro expansion regressed when this
			    // returned early there.
			    if ( stop_on_closing_paren && opStack.empty() && initial_brackets == 0 )
				return exStack.top();
			    break;
			}
			// not a cast after all — fall through to grouping
			// (this shouldn't happen in practice for valid C code)
		    }
		    // Direct invocation through a struct-member function pointer,
		    // e.g. `cmd.fn(3, 4)` or `tab[i].fn(ch, arg)`. Detected when the
		    // top of exStack is a TokenMember whose datadef is DataDefFPTR.
		    // The `(` must IMMEDIATELY follow the member access — if any
		    // tighter-than-assignment operator has been pushed onto opStack
		    // since the member was parsed (e.g. `ch->fn && (something_else)`),
		    // the `(` belongs to the next sub-expression, not a call through
		    // the fn-ptr. We only count operators with precedence < 14
		    // (anything tighter than `=`); `=` itself is the OUTER context
		    // for declaration init like `int v = (*flfunc)(args)` and must
		    // not block the call.
		    TokenMember *member_call_base = NULL;
		    bool opstack_has_pending_op = false;
		    {
			std::stack<TokenBase *> tmp = opStack;
			while ( !tmp.empty() )
			{
			    TokenBase *t = tmp.top();
			    if ( t->id() == TokenID::tkOpBrk ) { tmp.pop(); continue; }
			    if ( t->is_operator() )
			    {
				int p = ((TokenOperator *)t)->precedence();
				if ( p < 14 )
				{
				    opstack_has_pending_op = true;
				    break;
				}
			    }
			    tmp.pop();
			}
		    }
		    // A member appearing as the LHS of an assignment is NOT
		    // callable through `(`: `ch->fn = (...)` opens an RHS
		    // paren, not a call through fn. prevToken at this point
		    // for that case is the assignment op; for the legitimate
		    // call site (`tab[i].fn(args)`, `cmd.fn(arg)`) it's the
		    // member-name identifier.
		    TokenBase *prev_for_member = prevToken();
		    bool member_is_assign_lhs = prev_for_member
			&& (prev_for_member->id() == TokenID::tkAssign
			 || prev_for_member->id() == TokenID::tkAddEq
			 || prev_for_member->id() == TokenID::tkSubEq
			 || prev_for_member->id() == TokenID::tkMulEq
			 || prev_for_member->id() == TokenID::tkDivEq
			 || prev_for_member->id() == TokenID::tkModEq
			 || prev_for_member->id() == TokenID::tkXorEq
			 || prev_for_member->id() == TokenID::tkBandEq
			 || prev_for_member->id() == TokenID::tkBorEq
			 || prev_for_member->id() == TokenID::tkBSLEq
			 || prev_for_member->id() == TokenID::tkBSREq);
		    // P2.1b gap 1 — functor call `obj(args)`: when the exStack top
		    // is a class OBJECT (a plain object variable) whose class declares
		    // operator(), route the `(` to a method call on its operator().
		    // C++ canon: `obj(args)` == `obj.operator()(args)`. The callee is a
		    // class OBJECT, not a function — distinguished here from a normal
		    // function call (those resolve the identifier to a FuncDef variable
		    // up front and never push an object onto exStack before the `(`).
		    // Reuses the existing class-method dispatch (TokenCallMethod →
		    // ClassName__operator()); no parallel codegen (I6).
		    {
			TokenVar *obj_call_base = NULL;
			DataDefCLASS *fcls = NULL;
			Variable *fmethod = NULL;
			std::string functor_name("operator()");
			// The `(` must IMMEDIATELY follow the object (prevToken is the
			// object identifier). Unlike member-fptr calls we do NOT gate on
			// opstack_has_pending_op: in `cout << m(7)` the `<<` is pending but
			// the `(` still binds to `m` (the just-pushed exStack object), a
			// tighter call. Immediacy of prevToken is the discriminator.
			bool paren_follows_object = prev_for_member
			    && prev_for_member->type() == TokenType::ttIdentifier;
			if ( !exStack.empty()
			  && paren_follows_object
			  && !member_is_assign_lhs
			  && exStack.top()->type() == TokenType::ttVariable
			  && (obj_call_base = dynamic_cast<TokenVar *>(exStack.top())) != NULL
			  && obj_call_base->var.type
			  && obj_call_base->var.type->is_object()
			  && (fcls = dynamic_cast<DataDefCLASS *>(obj_call_base->var.type)) != NULL
			  && (fmethod = fcls->findMethod(functor_name)) != NULL )
			{
			    TokenCallMethod *tc = new TokenCallMethod(obj_call_base->var, *fmethod);
			    exStack.pop();
			    tc->file = tb->file;
			    tc->line = tb->line;
			    tc->column = tb->column;
			    tb = parseCallMethod(tc);
			    DBG(cout << "functor call through " << fcls->name << "::operator()" << endl);
			    opStack.push(tc);
			    if ( tb && tb->id() == TokenID::tkSemi )
				done = true;
			    break;
			}
		    }
		    if ( !exStack.empty()
		      && !opstack_has_pending_op
		      && !member_is_assign_lhs
		      && exStack.top()->type() == TokenType::ttMember
		      && (member_call_base = dynamic_cast<TokenMember *>(exStack.top())) != NULL
		      && dynamic_cast<DataDefFPTR *>(member_call_base->var.type) )
		    {
			TokenMember *tmem = member_call_base;
			exStack.pop();
			TokenCallFunc *tc = new TokenCallFunc(tmem->var);
			tc->src_node = tmem;
			tc->file = tb->file;
			tc->line = tb->line;
			tc->column = tb->column;
			tb = parseCallFunc(tc);
			DBG(cout << "member fptr call through " << tmem->var.name << endl);
			opStack.push(tc);
			if ( tb && tb->id() == TokenID::tkSemi )
			    done = true;
			break;
		    }
		    // Direct invocation through a function-pointer value produced by
		    // subscript syntax, e.g. `funcs[i](...)`, `tab[i].fn(...)`, or
		    // `(*(table[i]))(...)`. The top expression already materializes
		    // the function-pointer value; route the call through src_node so
		    // compile() loads that value instead of looking for storage on
		    // the placeholder Variable.
		    TokenBase *subscript_call_base = NULL;
		    DataDefFPTR *subscript_call_type = NULL;
		    if ( !exStack.empty()
		      && !opstack_has_pending_op
		      && !member_is_assign_lhs
		      && (dynamic_cast<TokenSubscript *>(exStack.top()) != NULL
		       || dynamic_cast<TokenSubscriptExpr *>(exStack.top()) != NULL)
		      && (subscript_call_base = exStack.top()) != NULL
		      && (subscript_call_type = dynamic_cast<DataDefFPTR *>(subscript_call_base->datadef())) != NULL )
		    {
			exStack.pop();
			Variable *call_var = new Variable("__subscript_fptr", *subscript_call_type, 1, NULL, false);
			TokenCallFunc *tc = new TokenCallFunc(*call_var);
			tc->src_node = subscript_call_base;
			tc->file = tb->file;
			tc->line = tb->line;
			tc->column = tb->column;
			tb = parseCallFunc(tc);
			DBG(cout << "subscript fptr call" << endl);
			opStack.push(tc);
			if ( tb && tb->id() == TokenID::tkSemi )
			    done = true;
			break;
		    }
		    // Direct invocation through a function-pointer variable wrapped
		    // in parens: `(*flfunc)(args)` and `(flfunc)(args)`. After the
		    // inner expression resolved, exStack top is a TokenVar whose
		    // type is DataDefFPTR. The `(` here begins the call args.
		    TokenVar *var_call_base = NULL;
		    if ( !exStack.empty()
		      && !opstack_has_pending_op
		      && exStack.top()->type() == TokenType::ttVariable
		      && (var_call_base = dynamic_cast<TokenVar *>(exStack.top())) != NULL
		      && dynamic_cast<DataDefFPTR *>(var_call_base->var.type)
		      && !var_call_base->var.is_constant()
		      && var_call_base->var.name.compare(0, 11, "__literal__") != 0 /* skip string literals */
		      && var_call_base->var.name[0] != '(' /* skip grouped exprs */ )
		    {
			TokenVar *tv = var_call_base;
			exStack.pop();
			TokenCallFunc *tc = new TokenCallFunc(tv->var);
			tc->src_node = tv;
			tc->file = tb->file;
			tc->line = tb->line;
			tc->column = tb->column;
			tb = parseCallFunc(tc);
			DBG(cout << "var fptr call through " << tv->var.name << endl);
			opStack.push(tc);
			if ( tb && tb->id() == TokenID::tkSemi )
			    done = true;
			break;
		    }
		    // Generic expression-as-function-pointer call:
		    // `(c ? foo : bar)()`, `(expr)(args)` — when the
		    // exStack top is a ternary or explicit DataDefFPTR
		    // expression. Only trigger for ternary expressions
		    // and already-typed function pointers, NOT for plain
		    // identifiers that happen to have a function type
		    // (which would be a normal function call or a
		    // parenthesized expression starting with a func name).
		    if ( !exStack.empty() && !opstack_has_pending_op )
		    {
			TokenBase *call_expr = exStack.top();
			DataDef *call_dd = call_expr->datadef();
			DataDefFPTR *fptr_type = dynamic_cast<DataDefFPTR *>(call_dd);
			// For ternary: check if either branch is a function reference.
			TokenTerQ *terq = dynamic_cast<TokenTerQ *>(call_expr);
			if ( terq )
			{
			    DataDef *td = terq->true_expr ? terq->true_expr->datadef() : NULL;
			    DataDef *fd = terq->false_expr ? terq->false_expr->datadef() : NULL;
			    if ( td && td->is_function() ) call_dd = td;
			    else if ( fd && fd->is_function() ) call_dd = fd;
			    // For ternary, wrap in FPTR if needed
			    if ( !fptr_type && call_dd && call_dd->is_function() )
			    {
				FuncDef *func = dynamic_cast<FuncDef *>(call_dd);
				if ( func )
				    fptr_type = new DataDefFPTR(func);
			    }
			}
			// Only trigger for genuine fptr patterns: ternary dispatch,
			// member fptr, or deref fptr (*fptr)(args).
			// Do NOT trigger for subscript expressions or plain
			// identifiers — those are normal expressions followed by
			// grouping parens, not function pointer invocations.
			// (Subscript fptr calls like table[i](args) are rare and
			// were not supported pre-v0.14; they cause false matches
			// on SMAUG's DO_FUN/SPEC_FUN typedef system.)
			if ( fptr_type && !terq )
			{
			    bool is_genuine_fptr =
				call_expr->type() == TokenType::ttMember
				|| dynamic_cast<TokenDerefExpr *>(call_expr) != NULL;
			    if ( !is_genuine_fptr )
				fptr_type = NULL;
			}
			// Only trigger for: ternary dispatch or deref fptr.
			// Members and subscripts are handled by their own dedicated
			// fptr paths earlier in the code. The generic path's
			// is_function() check is too aggressive for those.
			if ( fptr_type && !terq
			  && dynamic_cast<TokenDerefExpr *>(call_expr) == NULL )
			    fptr_type = NULL;
			if ( fptr_type )
			{
			    exStack.pop();
			    Variable *call_var = new Variable("__expr_fptr", *fptr_type, 1, NULL, false);
			    TokenCallFunc *tc = new TokenCallFunc(*call_var);
			    tc->src_node = call_expr;
			    tc->file = tb->file;
			    tc->line = tb->line;
			    tc->column = tb->column;
			    tb = parseCallFunc(tc);
			    DBG(cout << "expression fptr call" << endl);
			    opStack.push(tc);
			    if ( tb && tb->id() == TokenID::tkSemi )
				done = true;
			    break;
			}
		    }
		    ++brackets;
		    DBG(cout << "Got (, pushing onto opStack" << endl);
		    opStack.push(tb); // opStack.push(tb->clone());
		    break;
		}
		// colon stops expression (ternary false branch, case label, range-for)
		if ( tb->id() == TokenID::tkTerC && !brackets )
		{
		    pushToken(tb); // put : back for caller to consume
		    done = true;
		    break;
		}
		if ( tb->id() == TokenID::tkClBrk )
		{
		    if ( !brackets )
		    {
			DBG(cout << "Hit ), no prior brackets, might be end of function?" << endl);
			done = true;
			break;
		    }
		    --brackets;
		    DBG(cout << "Got ), clearing opStack until (" << endl);
		    while ( !opStack.empty() && opStack.top()->get() != '(' )
		    {
			opStack.top()->setFlag(tfBRACKETED);
			popOperator(opStack, exStack);
		    }
		    if ( !opStack.empty() )
			opStack.pop(); // pop off '('
		    if ( conditional && !brackets )
		    {
			TokenBase *next = peekToken();
			// A postfix-chain operator after the close paren means
			// the parenthesized expression is a SUB-expression
			// (e.g. `&((ch)->pcdata->ice_listen)` — the inner
			// `(ch)` closes here but `->pcdata` continues the
			// outer chain). Don't end on stop_on_closing_paren in
			// that case.
			// `(` after `)` is a postfix call only when the
			// expression has a function/function-pointer type.
			bool paren_is_call = next
			    && next->id() == TokenID::tkOpBrk
			    && !exStack.empty()
			    && exStack.top()->datadef()
			    && (exStack.top()->datadef()->is_function()
			     || dynamic_cast<DataDefFPTR *>(exStack.top()->datadef()));
			bool postfix_follows = next
			    && (next->id() == TokenID::tkDot
			     || next->id() == TokenID::tkDeRef
			     || next->id() == TokenID::tkOpSqr
			     || paren_is_call);
			bool ends_conditional = !postfix_follows && (stop_on_closing_paren || !next
			    || next->id() == TokenID::tkComma
			    || next->id() == TokenID::tkClBrk
			    || next->id() == TokenID::tkClSqr
			    || next->id() == TokenID::tkOpBrc
			    || next->id() == TokenID::tkSemi
			    || next->type() == TokenType::ttKeyword
			    || (ternary_branch && next->id() == TokenID::tkTerC));
			if ( ends_conditional )
			{
			    // Flush any remaining operators before returning so
			    // expressions like `c = -(2)` complete the pending
			    // unary `-` and assignment before the conditional-end
			    // short-circuit. Otherwise exStack may hold only the
			    // inner paren's value, losing the outer operator chain.
			    while ( !opStack.empty() )
				popOperator(opStack, exStack);
			    DBG(std::cout << "Program::parseExpression() conditional end exStack:" << exStack.size() << std::endl);
			    return exStack.empty() ? NULL : exStack.top();
			}
		    }
		    break;
		}
		// ternary operator: condition ? true_expr : false_expr
		if ( tb->id() == TokenID::tkTerQ )
		{
		    DBG(cout << "parseExpression: ternary operator ?" << endl);
		    // pop operators with higher or equal precedence than ? (13)
		    // but NOT assignment (14) or lower precedence operators.
		    // Pending function/method call nodes must also be flushed here so
		    // `func(...) ? a : b` presents the call result as the condition.
		    while ( !opStack.empty() && opStack.top()->get() != '('
			    && ((opStack.top()->type() == TokenType::ttCallFunc
			      || opStack.top()->type() == TokenType::ttCallMethod)
			     || (dynamic_cast<TokenOperator *>(opStack.top())
			      && dynamic_cast<TokenOperator *>(opStack.top())->precedence() <= 13)) )
			popOperator(opStack, exStack);
		    if ( exStack.empty() )
			Throw(tb) << "Missing condition before ?" << flush;
		    TokenTerQ *ternary = (TokenTerQ *)tb;
		    ternary->condition = exStack.top();
		    exStack.pop();
		    // parse true expression — use conditional mode so it stops at : or )
		    // but : is an operator, not ), so we parse then check for :
		    // GNU extension: `a ?: b` means `a ? a : b` — the middle
		    // operand is omitted and `:` appears immediately after `?`.
		    TokenBase *texpr = nextToken();
		    TokenBase *colon;
		    if ( texpr->id() == TokenID::tkTerC )
		    {
			// GNU `?:` — reuse the condition as the true branch.
			ternary->true_expr = ternary->condition;
			colon = texpr;
		    }
		    else
		    {
			ternary->true_expr = parseExpression(texpr, true, true);
			// after conditional parseExpression, expect : next
			colon = nextToken();
		    }
		    if ( colon->id() != TokenID::tkTerC )
			Throw(colon) << "Expecting : in ternary expression" << flush;
		    // parse false expression
		    TokenBase *fexpr = nextToken();
		    ternary->false_expr = parseExpression(fexpr, conditional);
		    // Propagate branch datadef up to the ternary so downstream
		    // type-directed paths (e.g. TokenAssign's std::string → char*
		    // coercion) can see a meaningful datadef(). Prefer the true
		    // branch's type; fall back to the false branch if that's
		    // richer (non-NULL / non-int).
		    //
		    // Mixed string-literal/pointer ternary: when one branch is
		    // a string literal (std::string) and the other is a real
		    // char*-yielding expression (pointer type), prefer the
		    // pointer type. The merge needs both branches to land as
		    // raw char* in the merge slot; labeling the result as an
		    // object would make downstream coercion dereference the
		    // function's char* return incorrectly. Closes SMAUG boards.c:1615
		    // `feof(fp) ? "End" : fread_word(fp)`.
		    DataDef *tdd = ternary->true_expr  ? ternary->true_expr->datadef()  : NULL;
		    DataDef *fdd = ternary->false_expr ? ternary->false_expr->datadef() : NULL;
		    DataDef *ternary_dd = tdd;
		    if ( (!ternary_dd || ternary_dd == &ddINT) && fdd && fdd != &ddINT )
			ternary_dd = fdd;

		    // C ternary type unification for pointer-flavored
		    // branches. Each branch has an *effective* type:
		    //   - real pointer types stay as-is
		    //   - std::string (string literals) → char* equivalent
		    //   - fixed-array variables / members → char* equivalent
		    //     (their datadef reports the element type, but
		    //     the value decays to pointer-to-element)
		    // When both branches are pointer-flavored and their
		    // raw datadefs disagree, unify on a real pointer if
		    // available, else on `char *` (ddLPSTR). Without this,
		    // the merge slot inherits a single-byte / std::string /
		    // mixed type and the IR coerce step rejects the other
		    // branch. Closes SMAUG boards.c:1615
		    // `feof(fp) ? "End" : fread_word(fp)`, fight.c:4298
		    // `IS_NPC(victim) ? buf2 : ""`, player.c:1883
		    // `(x == lvl) ? buf : (x == lvl+1) ? buf2 : " exp"`,
		    // and act_wiz.c do_mstat's many
		    // `obj ? obj->name : "(none)"` calls.
		    auto charptr_like = [](TokenBase *expr, DataDef *dd) {
			if ( !dd )
			    return false;
			if ( dd->is_pointer() )
			    return true;
			if ( dynamic_cast<DataDefCArray *>(dd) )
			    return true;
			if ( TokenVar *tv = dynamic_cast<TokenVar *>(expr) )
			    if ( tv->var.is_fixed_array() )
				return true;
			if ( TokenMember *tm = dynamic_cast<TokenMember *>(expr) )
			    if ( tm->is_fixed_array_member() )
				return true;
			return false;
		    };
		    bool tptr = charptr_like(ternary->true_expr,  tdd);
		    bool fptr = charptr_like(ternary->false_expr, fdd);
		    if ( tptr && fptr && tdd != fdd )
		    {
			DataDef *pick = NULL;
			if ( tdd && tdd->is_pointer() )
			    pick = tdd;
			else if ( fdd && fdd->is_pointer() )
			    pick = fdd;
			if ( !pick )
			    pick = &ddLPSTR;
			ternary_dd = pick;
		    }
		    if ( ternary_dd )
			ternary->setDataType(ternary_dd);
		    // push ternary result onto exStack
		    exStack.push(ternary);
		    // only stop if not inside brackets — inside () we need
		    // to continue to find the closing )
		    if ( brackets == 0 )
			done = true;
		    break;
		}
		// see if we need to convert TokenNeg to TokenSub (binary context)
		if ( tb->id() == TokenID::tkNeg && isPostfixPosition() )
		{
		    DBG(std::cout << "parseExpression() converting TokenNeg to TokenSub, prevToken id: " << (int)prevToken()->id() << " prevToken->is_operator: " << (prevToken()->is_operator() ? "true" : "false") << std::endl);
		    TokenSub *ts = new TokenSub();

		    ts->file = tb->file;
		    ts->line = tb->line;
		    ts->column = tb->column;
		    // should we delete tb ?
		    tb = ts;
		}
		// Unary `+` (no-op): just consume and continue.
		// Treat as unary when there's no value-producing operand
		// on exStack, or when the previous token is a binary/assign
		// operator (so `x = +20` works). Exclude postfix `++`/`--`
		// so `x++ + 10` stays binary.
		if ( tb->id() == TokenID::tkAdd && (isUnaryPosition() || awaiting_prefix_step_operand())
		  && (exStack.empty()
		   || (_prv_token && (_prv_token->id() == TokenID::tkAssign
		     || _prv_token->id() == TokenID::tkComma
		     || _prv_token->id() == TokenID::tkOpBrk
		     || _prv_token->id() == TokenID::tkOpSqr
		     || _prv_token->id() == TokenID::tkSemi))) )
		    break;
		// & address-of in unary position
		if ( tb->id() == TokenID::tkBand && (isUnaryPosition() || awaiting_prefix_step_operand()) )
		{
		    exStack.push(parseAddressOfExpression(tb));
		    break;
		}
		// GNU computed-goto label address: `&&label`
		if ( tb->id() == TokenID::tkLand && (isUnaryPosition() || awaiting_prefix_step_operand()) )
		{
		    TokenBase *label_tb = nextToken();
		    if ( !label_tb || !is_contextual_identifier_token(label_tb) )
			Throw(label_tb ? label_tb : tb) << "expected label name after '&&'" << flush;
		    exStack.push(new TokenLabelAddr(
			contextual_identifier_name(label_tb),
			getPointerType(&ddVOID)));
		    break;
		}
			// * dereference in unary position
			if ( tb->id() == TokenID::tkMul && (isUnaryPosition() || awaiting_prefix_step_operand()) )
			{
			    TokenBase *deref_tb = nextToken();
			    if ( deref_tb->id() == TokenID::tkOpBrk )
		    {
			// Check whether the inner expression is a cast
			// signature (`(TYPE*) expr`). If so, delegate the
			// whole `(...)` to parseExpression so its cast
			// detection fires and `TYPE` gets resolved against
			// `datatype_map` instead of being sent through the
			// identifier/variable lookup path — which fails for
			// typedef'd struct names like `EXT_BV` in
			// `*(EXT_BV*)vd.data`. Delegation consumes the `)`
			// itself, so the subsequent nextToken() is skipped
			// on the cast path.
			TokenBase *peek_inner = peekToken();
			bool inner_is_statement_expr =
			    peek_inner && peek_inner->id() == TokenID::tkOpBrc;
			bool inner_is_cast_head =
			    !inner_is_statement_expr
			    && peek_inner
			    && ( peek_inner->type() == TokenType::ttDataType
			      || peek_inner->id() == TokenID::tkSTRUCT
			      || peek_inner->id() == TokenID::tkCLASS
			      || peek_inner->id() == TokenID::tkCONST
			      || peek_inner->id() == TokenID::tkRESTRICT
			      || peek_inner->id() == TokenID::tkENUM
			      || ( peek_inner->type() == TokenType::ttIdentifier
				&& datatype_map.count(((TokenIdent *)peek_inner)->str) ) );
			TokenBase *deref_expr;
			if ( inner_is_cast_head || inner_is_statement_expr )
			{
			    // stop_on_closing_paren=true so the matching `)` of
			    // the cast/statement-expression group ends parsing —
			    // otherwise conditional mode would chase past it into
			    // a following `=` or operator chain (closing the SMAUG
			    // `*(EXT_BV *)p = fread_bitvector(...)` family).
			    // Delegation consumes the `)` itself, so no follow-up
			    // nextToken() is needed.
			    deref_expr = parseExpression(deref_tb, true, false, true);
			}
			else
			{
			    TokenBase *inner_tb = nextToken();
			    deref_expr = parseExpression(inner_tb, true);
			    TokenBase *close = nextToken();
			    if ( !close || close->id() != TokenID::tkClBrk )
				Throw(close ? close : deref_tb) << "expected ')' after *(expr)" << flush;
			}
			DataDef *dtype = effective_pointer_type_for_member_access(*this, deref_expr);
			if ( !dtype )
			    dtype = deref_expr->datadef();
			if ( !dtype )
			    Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
			if ( dynamic_cast<DataDefFPTR *>(dtype) != NULL )
			{
			    exStack.push(deref_expr);
			    break;
			}
			if ( dtype->is_function() && dtype->is_numeric() )
			{
			    exStack.push(deref_expr);
			    break;
			}
			if ( !dtype->is_pointer() )
			{
			    // Allow dereference of fixed-array struct members —
			    // they decay to pointers (e.g. *edit->line for char line[N])
			    TokenMember *tm_deref = dynamic_cast<TokenMember *>(deref_expr);
			    if ( tm_deref && tm_deref->is_fixed_array_member() )
			    {
				exStack.push(new TokenDerefExpr(deref_expr, dtype));
				break;
			    }
			    Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
			}
			DataDefPTR *dptr = dynamic_cast<DataDefPTR *>(dtype);
			DataDef *base = dptr ? dptr->base_type : &ddINT64;
			if ( peekToken()
			  && (peekToken()->id() == TokenID::tkInc
			   || peekToken()->id() == TokenID::tkDec) )
			{
			    // Postfix ++/-- binds tighter than the outer unary `*`:
			    // `*(*x)++` is `*(((*x)++))`, not `(*(*x))++`.
			    TokenBase *step_tb = nextToken();
			    TokenOperator *step;
			    if ( step_tb->id() == TokenID::tkInc )
				step = new TokenInc();
			    else
				step = new TokenDec();
			    step->left = deref_expr;
			    step->right = NULL;
			    exStack.push(new TokenDerefExpr(step, base));
			    break;
			}
			exStack.push(new TokenDerefExpr(deref_expr, base));
			    }
			    else
			    {
				if ( deref_tb->type() == TokenType::ttIdentifier
				  && !(peekToken()
				    && (peekToken()->id() == TokenID::tkOpBrk
				     || peekToken()->id() == TokenID::tkDeRef
				     || peekToken()->id() == TokenID::tkDot
				     || peekToken()->id() == TokenID::tkOpSqr)) )
				{
				    std::string dname = ((TokenIdent *)deref_tb)->str;
				    Variable *dvar = findVariable(dname);
				    if ( !dvar && dname == "this"
				      && code && code->method && code->method->owner_class )
				    {
					std::string thisid = "__this";
					dvar = code->method->findParameter(thisid);
				    }
				    if ( !dvar )
					Throw(deref_tb) << "undeclared identifier '" << dname << "'" << flush;
				    // C function-to-pointer decay reverses through `*`:
				    // `*fp` (where fp is a function pointer) IS the
				    // function — still callable as `(*fp)(args)`. Push
				    // the variable as a value and let the call-site
				    // logic dispatch normally.
				    if ( dvar->type->is_function() && dvar->type->is_numeric() )
				    {
					exStack.push(new TokenVar(*dvar));
					break;
				    }
				    if ( dynamic_cast<DataDefFPTR *>(dvar->type) != NULL )
				    {
					exStack.push(new TokenVar(*dvar));
					break;
				    }
				    DataDef *base = deref_type_for_variable(dvar);
				    if ( !base )
					Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
				    if ( peekToken() && (peekToken()->id() == TokenID::tkInc || peekToken()->id() == TokenID::tkDec) )
				    {
					TokenBase *step_tb = nextToken();
					TokenBase *step_expr = new TokenDerefStep(*dvar, base, step_tb->id() == TokenID::tkInc);
					exStack.push(step_expr);
					_cur_token = step_expr;
				    }
				    else
					exStack.push(new TokenDeref(*dvar, base));
				}
			    else
			    {
				TokenBase *deref_expr = NULL;
				if ( deref_tb->id() == TokenID::tkMul )
				{
				    TokenBase *inner_tb = nextToken();
				    if ( !inner_tb )
					Throw(deref_tb) << "expecting pointer expression after '*'" << flush;
				    if ( inner_tb->id() == TokenID::tkMul )
				    {
					// Multi-level dereference: ***p, ****p, etc.
					// Collect all the `*` tokens, then the final
					// operand, and build the deref chain bottom-up.
					std::vector<TokenBase *> stars;
					stars.push_back(deref_tb);  // the current `*`
					stars.push_back(inner_tb);  // one more `*`
					TokenBase *operand_tb = nextToken();
					while ( operand_tb && operand_tb->id() == TokenID::tkMul )
					{
					    stars.push_back(operand_tb);
					    operand_tb = nextToken();
					}
					// operand_tb is the identifier or '(' expr
					if ( !operand_tb )
					    Throw(deref_tb) << "expecting pointer expression after '*'" << flush;
					// Build the innermost deref from the variable
					if ( operand_tb->type() == TokenType::ttIdentifier )
					{
					    std::string vname = ((TokenIdent *)operand_tb)->str;
					    Variable *var = findVariable(vname);
					    if ( !var )
						Throw(operand_tb) << "undeclared identifier '" << vname << "'" << flush;
					    DataDef *base = deref_type_for_variable(var);
					    if ( !base )
						Throw(operand_tb) << "cannot dereference non-pointer type" << flush;
					    // Postfix ++/-- on the innermost variable:
					    // `**pp++` = `*(*(pp++))`.  Wrap the variable
					    // in a postfix step before building the deref
					    // chain so the increment targets `pp`, not a
					    // dereferenced value.
					    if ( peekToken()
					      && (peekToken()->id() == TokenID::tkInc
					       || peekToken()->id() == TokenID::tkDec) )
					    {
						DBG(std::cout << "multi-deref: postfix step on " << vname << std::endl);
						TokenBase *step_tb = nextToken();
						deref_expr = new TokenDerefStep(*var, base,
						    step_tb->id() == TokenID::tkInc);
					    }
					    else
						deref_expr = new TokenDeref(*var, base);
					}
					else if ( operand_tb->id() == TokenID::tkOpBrk )
					{
					    TokenBase *inner_expr_tb = nextToken();
					    deref_expr = parseExpression(inner_expr_tb, true);
					    TokenBase *close = nextToken();
					    if ( !close || close->id() != TokenID::tkClBrk )
						Throw(close ? close : operand_tb) << "expected ')' in multi-deref" << flush;
					}
					else
					    Throw(operand_tb) << "expecting identifier or '(' after multi-level '*'" << flush;
					// Now wrap in TokenDerefExpr for each additional `*`
					// (skip the first star since TokenDeref already derefs once)
					for ( size_t si = 1; si < stars.size(); ++si )
					{
					    DataDef *dtype = effective_pointer_type_for_member_access(*this, deref_expr);
					    if ( !dtype )
						dtype = deref_expr->datadef();
					    if ( !dtype || !dtype->is_pointer() )
						Throw(stars[si]) << "cannot dereference non-pointer type" << flush;
					    DataDefPTR *dptr = dynamic_cast<DataDefPTR *>(dtype);
					    DataDef *base = dptr ? dptr->base_type : &ddINT64;
					    deref_expr = new TokenDerefExpr(deref_expr, base);
					}
				    }
				    else if ( inner_tb->id() == TokenID::tkOpBrk )
				    {
					TokenBase *inner_expr_tb = nextToken();
					TokenBase *inner_expr = parseExpression(inner_expr_tb, true);
					TokenBase *close = nextToken();
					if ( !close || close->id() != TokenID::tkClBrk )
					    Throw(close ? close : inner_tb) << "expected ')' after *(expr)" << flush;
					DataDef *inner_dtype = effective_pointer_type_for_member_access(*this, inner_expr);
					if ( !inner_dtype )
					    inner_dtype = inner_expr ? inner_expr->datadef() : NULL;
					if ( !inner_dtype || !inner_dtype->is_pointer() )
					    Throw(inner_tb) << "cannot dereference non-pointer type" << flush;
					DataDefPTR *inner_dptr = dynamic_cast<DataDefPTR *>(inner_dtype);
					DataDef *inner_base = inner_dptr ? inner_dptr->base_type : &ddINT64;
					if ( peekToken()
					  && (peekToken()->id() == TokenID::tkInc
					   || peekToken()->id() == TokenID::tkDec) )
					{
					    // Postfix ++/-- binds tighter than the outer unary
					    // `*`: `*(*x)++` is `*(((*x)++))`, not `(*(*x))++`.
					    // Lower it the same way as the explicit `*p++` fast
					    // path: wrap the pointer-valued inner expression in a
					    // postfix step node, then dereference the OLD pointer
					    // result.
					    TokenBase *step_tb = nextToken();
					    TokenOperator *step;
					    if ( step_tb->id() == TokenID::tkInc )
						step = new TokenInc();
					    else
						step = new TokenDec();
					    step->left = inner_expr;
					    step->right = NULL;
					    deref_expr = step;
					}
					else
					    deref_expr = new TokenDerefExpr(inner_expr, inner_base);
				    }
				    else if ( inner_tb->type() == TokenType::ttIdentifier
					  && !(peekToken()
					    && (peekToken()->id() == TokenID::tkOpBrk
					     || peekToken()->id() == TokenID::tkDeRef
					     || peekToken()->id() == TokenID::tkDot
					     || peekToken()->id() == TokenID::tkOpSqr)) )
				    {
					std::string inner_name = ((TokenIdent *)inner_tb)->str;
					Variable *inner_var = findVariable(inner_name);
					if ( !inner_var )
					    Throw(inner_tb) << "undeclared identifier '" << inner_name << "'" << flush;
					DataDef *inner_base = deref_type_for_variable(inner_var);
					if ( !inner_base )
					    Throw(inner_tb) << "cannot dereference non-pointer type" << flush;
					// Postfix ++/-- on inner var: `**pp++`
					// = `*(*(pp++))`.  Use TokenDerefStep so
					// the increment targets pp.
					if ( peekToken()
					  && (peekToken()->id() == TokenID::tkInc
					   || peekToken()->id() == TokenID::tkDec) )
					{
					    DBG(std::cout << "two-level deref: postfix step on " << inner_name << std::endl);
					    TokenBase *step_tb = nextToken();
					    deref_expr = new TokenDerefStep(*inner_var, inner_base,
						step_tb->id() == TokenID::tkInc);
					}
					else
					    deref_expr = new TokenDeref(*inner_var, inner_base);
				    }
				    else
					Throw(inner_tb) << "expecting pointer expression after '*'" << flush;
				}
				else if ( deref_tb->type() == TokenType::ttIdentifier
				   && peekToken()
				   && (peekToken()->id() == TokenID::tkDeRef
				    || peekToken()->id() == TokenID::tkDot
				    || peekToken()->id() == TokenID::tkOpSqr) )
				{
				    // Postfix chain (e.g. `res->name`, `p.x`, `tab[i]`)
				    // — parse only the chain so trailing binary operators
				    // like `*p->name == '$'` don't get swallowed.
				    deref_expr = parsePostfixChain(deref_tb);
				}
				else if ( deref_tb->type() == TokenType::ttIdentifier
				   && peekToken()
				   && peekToken()->id() == TokenID::tkOpBrk )
				{
				    // *func(args) — parse just the call expression so
				    // trailing `=` stays for the outer assignment, e.g.
				    // `*foo(&c) = 2`. Push the deref_tb back and let
				    // parseExpression handle the identifier+call, but
				    // use stop_on_closing_paren at bracket depth 0 so
				    // the parse ends after the call's `)`.
				    std::string fname = ((TokenIdent *)deref_tb)->str;
				    Variable *fvar = findVariable(fname);
				    if ( fvar )
				    {
					TokenCallFunc *tcf = new TokenCallFunc(*fvar);
					nextToken(); // consume '('
					parseCallFunc(tcf);
					deref_expr = tcf;
				    }
				    else
					deref_expr = parseExpression(deref_tb, true);
				}
				else if ( (deref_tb->id() == TokenID::tkInc
				        || deref_tb->id() == TokenID::tkDec)
				    && peekToken()
				    && peekToken()->type() == TokenType::ttIdentifier )
				{
				    // Pre-increment / pre-decrement of a pointer:
				    // `*++p`, `*--p`. The recursive parseExpression
				    // path would happily consume any trailing binary
				    // operator (`*++p == 'e'` would parse as
				    // `*(++p == 'e')`), so handle the unary step
				    // explicitly: build a `TokenInc(right=p)` (pre-
				    // increment, which mutates p and yields its new
				    // value), then wrap it in a `TokenDerefExpr` so
				    // the deref reads through the post-step pointer.
				    TokenBase *id_tb = nextToken();
				    std::string id_name = ((TokenIdent *)id_tb)->str;
				    Variable *id_var = findVariable(id_name);
				    if ( !id_var )
					Throw(id_tb) << "undeclared identifier '" << id_name << "'" << flush;
				    if ( !id_var->type->is_pointer() )
					Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
				    DataDefPTR *idptr = dynamic_cast<DataDefPTR *>(id_var->type);
				    DataDef *base = (idptr && idptr->base_type) ? idptr->base_type : &ddINT64;
				    TokenOperator *step;
				    if ( deref_tb->id() == TokenID::tkInc )
					step = new TokenInc();
				    else
					step = new TokenDec();
				    step->left = NULL;
				    step->right = new TokenVar(*id_var);
				    deref_expr = new TokenDerefExpr(step, base);
				    DataDef *dtype = id_var->type;
				    if ( !dtype || !dtype->is_pointer() )
					Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
				    exStack.push(deref_expr);
				    break;
				}
				else if ( deref_tb->id() == TokenID::tkOpBrk )
				{
				    TokenBase *inner_expr_tb = nextToken();
				    TokenBase *inner_expr = parseExpression(inner_expr_tb, true);
				    TokenBase *close = nextToken();
				    if ( !close || close->id() != TokenID::tkClBrk )
					Throw(close ? close : deref_tb) << "expected ')' after *(expr)" << flush;
				    if ( !inner_expr )
					Throw(deref_tb) << "expecting pointer expression after '*('" << flush;
				    DataDef *inner_dtype = effective_pointer_type_for_member_access(*this, inner_expr);
				    if ( !inner_dtype )
					inner_dtype = inner_expr->datadef();
				    if ( !inner_dtype || !inner_dtype->is_pointer() )
					Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
				    DataDefPTR *inner_dptr = dynamic_cast<DataDefPTR *>(inner_dtype);
				    DataDef *inner_base = inner_dptr ? inner_dptr->base_type : &ddINT64;
				    if ( peekToken()
				      && (peekToken()->id() == TokenID::tkInc
				       || peekToken()->id() == TokenID::tkDec) )
				    {
					// Postfix ++/-- binds tighter than the outer unary
					// `*`: `*(*x)++` is `*(((*x)++))`, not `(*(*x))++`.
					TokenBase *step_tb = nextToken();
					TokenOperator *step;
					if ( step_tb->id() == TokenID::tkInc )
					    step = new TokenInc();
					else
					    step = new TokenDec();
					step->left = inner_expr;
					step->right = NULL;
					exStack.push(new TokenDerefExpr(step, inner_base));
					break;
				    }
				    else
					deref_expr = new TokenDerefExpr(inner_expr, inner_base);
				}
				else
				    deref_expr = parseExpression(deref_tb, true);
				if ( !deref_expr )
				    Throw(deref_tb) << "expecting pointer expression after '*'" << flush;
				DataDef *dtype = effective_pointer_type_for_member_access(*this, deref_expr);
				if ( !dtype )
				    dtype = deref_expr->datadef();
				if ( !dtype )
				    Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
				if ( dtype->is_function() && dtype->is_numeric() )
				{
				    exStack.push(deref_expr);
				    break;
				}
				if ( !dtype->is_pointer() )
				{
				    // Fixed-array struct members decay to pointers
				    TokenMember *tm_d = dynamic_cast<TokenMember *>(deref_expr);
				    if ( tm_d && tm_d->is_fixed_array_member() )
				    {
					exStack.push(new TokenDerefExpr(deref_expr, dtype));
					break;
				    }
				    // Multi-dim array subscripts decay to pointers:
				    // *argv[i] where argv is char[N][M]
				    TokenSubscript *ts_d = dynamic_cast<TokenSubscript *>(deref_expr);
				    if ( ts_d && ts_d->object.is_fixed_array() )
				    {
					exStack.push(new TokenDerefExpr(deref_expr, dtype));
					break;
				    }
				    // Also handle TokenSubscriptExpr from parsePostfixChain
				    TokenSubscriptExpr *tse_d = dynamic_cast<TokenSubscriptExpr *>(deref_expr);
				    if ( tse_d )
				    {
					TokenVar *base_tv = dynamic_cast<TokenVar *>(tse_d->base_expr);
					if ( base_tv && base_tv->var.is_fixed_array() )
					{
					    exStack.push(new TokenDerefExpr(deref_expr, dtype));
					    break;
					}
				    }
				    Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
				}
				DataDefPTR *dptr = dynamic_cast<DataDefPTR *>(dtype);
				DataDef *base = dptr ? dptr->base_type : &ddINT64;
				exStack.push(new TokenDerefExpr(deref_expr, base));
				}
			    }
			    break;
			}
		if ( tb->id() == TokenID::tkDec || tb->id() == TokenID::tkInc )
		{
		    DBG(cout << "parseExpression: Got operator: " << (char)tb->get() << (char)tb->get() << endl);
		    to = (TokenOperator *)tb;
		    to->left = NULL;
		    to->right = NULL;
		    if ( isPostfixPosition() && !exStack.empty() )
		    {
			to->left = exStack.top(); exStack.pop(); DBG(cout << "popped " << to->left->ival() << endl);
			resolve_object_operator_type(to);
			exStack.push(to);
		    }
		    else
			opStack.push(to);
		    break;
		}
		DBG(cout << "parseExpression: Got operator: " << (char)tb->get() << " id() " << (int)tb->id() << endl);
		to = (TokenOperator *)tb; // ->clone();
		to->left = NULL;
		to->right = NULL;
		// whiile: there is a function at the top of the operator stack)
		// or (there is an operator at the top of the operator stack with greater precedence)
		// or (the operator at the top of the operator stack has equal precedence and is left associative))
		// and (the operator at the top of the operator stack is not a left parenthesis):
		// (Note: we don't put functions in the stack right now)
		// Same-precedence left-associative chains must reduce the stack
		// top before pushing the new operator. The general operator>
		// comparison is strict for equal precedence, so spell out the
		// left-associative arithmetic/shift levels here.
		while ( !opStack.empty() && opStack.top()->id() != TokenID::tkOpBrk
		&&      (opStack.top()->type() == TokenType::ttCallFunc || opStack.top()->type() == TokenType::ttCallMethod
		||      (opStack.top()->is_operator() && (*((TokenOperator *)opStack.top()) > *to))
		||      (opStack.top()->is_operator()
			 && ((TokenOperator *)opStack.top())->precedence() == to->precedence()
			 && (to->precedence() == 3 || to->precedence() == 4
			  || to->precedence() == 5))) )
		{
		    DBG(cout << "Operator(" << (char)opStack.top()->get() << ") has precedence over operator(" << (char)to->get() << ')' << endl);
		    popOperator(opStack, exStack);
		}
		DBG(cout << "Pushing " << (char)tb->get() << " onto opStack" << endl);
		opStack.push(to);
		break;
            case TokenType::ttDataType:
		bt = (TokenDataType *)tb;
		// If the previous token is '.' or '->', the type name is a
		// struct member name (e.g. `a.array[1]` where `array` is
		// both a madc keyword and a union member).  Re-inject as
		// a plain identifier and let the ttIdentifier path resolve it.
		if ( prevToken()
		  && (prevToken()->id() == TokenID::tkDot || prevToken()->id() == TokenID::tkDeRef)
		  && is_contextual_identifier_token(bt) )
		{
		    std::string ctx_name = contextual_identifier_name(bt);
		    tb = new TokenIdent(ctx_name);
		    tb->line = bt->line;
		    tb->column = bt->column;
		    goto redo_expression_token;
		}
		// If the next token isn't an identifier, the user probably
		// meant the data type *name* as a contextual identifier — a
		// parameter or local variable named e.g. `string`. Look it up
		// as a variable first; only treat as inline declaration if we
		// don't find one and the next token is an identifier.
		if ( is_contextual_identifier_token(bt) )
		{
		    std::string ctx_name = contextual_identifier_name(bt);
		    Variable *ctx_var = findVariable(ctx_name);
		    if ( ctx_var
			 && (!peekToken() || peekToken()->type() != TokenType::ttIdentifier) )
		    {
			DBG(cout << "ttDataType " << ctx_name << " resolves to variable" << endl);
			exStack.push(new TokenVar(*ctx_var));
			break;
		    }
		}
		tb = nextToken();
		if ( tb->type() != TokenType::ttIdentifier ) { Throw(tb) << "Expecting identifier" << flush; }
		var = addVariable(code, bt->definition, ((TokenIdent *)tb)->str);
		DBG(cout << "Pushing newly declared variable: " << var->name << " onto exStack" << endl);
		exStack.push(new TokenVar(*var));
		break;
	    case TokenType::ttString:
		if ( ((TokenStr *)tb)->wide )
		    var = addWideLiteral(((TokenStr *)tb)->str);
		else
		    var = addLiteral(((TokenIdent *)tb)->str);
		DBG(cout << "Pushing new variable of literal: " << var->name << " onto exStack" << endl);
		exStack.push(new TokenVar(*var));
		break;
	    case TokenType::ttKeyword:
		// const/volatile/restrict qualifiers are compile-time
		// annotations — skip them in expression context (casts,
		// sizeof, etc.)
		if ( tb->id() == TokenID::tkCONST
		  || tb->id() == TokenID::tkRESTRICT
		  || tb->id() == TokenID::tkVOLATILE )
		{
		    tb = nextToken();
		    if ( !tb ) { done = true; break; }
		    continue;
		}
		// new-expression in expression context. After `.` or `->`, C code
		// can use `new` as a member name. The new-expression forms:
		//   new ClassName(args)   -> next token is an identifier
		//   new int(v)/string(v)  -> next token is a builtin type
		//   new (addr) Type(args) -> next token is '(' (placement new)
		if ( tb->id() == TokenID::tkNEW
		  && (!prevToken()
		   || (prevToken()->id() != TokenID::tkDot
		    && prevToken()->id() != TokenID::tkDeRef))
		  && peekToken()
		  && (peekToken()->type() == TokenType::ttIdentifier
		   || peekToken()->type() == TokenType::ttDataType
		   || peekToken()->id() == TokenID::tkOpBrk) )
		{
		    TokenBase *new_node = ((TokenKeyword *)tb)->parse(*this);
		    if ( new_node )
			exStack.push(new_node);
		    break;
		}
		if ( !is_contextual_identifier_token(tb) )
		    Throw(tb) << "Unexpected keyword in expression" << flush;
	    case TokenType::ttIdentifier:
	    {
		std::string contextual_name = contextual_identifier_name(tb);
		TokenIdent contextual_ident(contextual_name);
		TokenIdent *ident_tb = tb->type() == TokenType::ttIdentifier
		    ? (TokenIdent *)tb
		    : &contextual_ident;
		bool expression_head = exStack.empty() && opStack.empty();
		if ( ident_tb->str == "__FUNCTION__" || ident_tb->str == "__func__"
		  || ident_tb->str == "__PRETTY_FUNCTION__" )
		{
		    var = addLiteral(cur_func_name);
		    exStack.push(new TokenVar(*var));
		    break;
		}
		if ( is_realpart_identifier(ident_tb->str)
		  || is_imagpart_identifier(ident_tb->str) )
		{
		    bool want_imag = is_imagpart_identifier(ident_tb->str);
		    TokenBase *next_tb = nextToken();
		    if ( !next_tb )
			Throw(tb) << "Expecting expression after " << ident_tb->str << flush;
		    TokenBase *component_expr = NULL;
		    if ( next_tb->id() == TokenID::tkOpBrk )
		    {
			TokenBase *inner_tb = nextToken();
			component_expr = parseExpression(inner_tb, true, false, true, 1);
		    }
		    else if ( is_contextual_identifier_token(next_tb) || next_tb->type() == TokenType::ttIdentifier )
		    {
			bool has_postfix_chain = peekToken()
			    && (peekToken()->id() == TokenID::tkDot
			     || peekToken()->id() == TokenID::tkDeRef
			     || peekToken()->id() == TokenID::tkOpSqr);
			if ( has_postfix_chain )
			    component_expr = parsePostfixChain(next_tb);
			else
			{
			    std::string name = contextual_identifier_name(next_tb);
			    TokenIdent component_ident(name);
			    copy_token_location(&component_ident, next_tb);
			    Variable *component_var = findVariable(name);
			    if ( component_var )
			    {
				component_expr = new TokenVar(*component_var);
				copy_token_location(component_expr, next_tb);
			    }
			    else
				component_expr = resolve_expression_context_identifier(&component_ident);
			}
			TokenVar *tv = dynamic_cast<TokenVar *>(component_expr);
			if ( tv && peekToken() && peekToken()->id() == TokenID::tkOpBrk
			  && tv->var.type && tv->var.type->is_function() )
			{
			    Variable *call_var = runtime_eval_scope_target(&tv->var);
			    TokenCallFunc *tc = new TokenCallFunc(*call_var);
			    TokenBase *call_tb = nextToken();
			    tc->line = call_tb->line;
			    tc->column = call_tb->column;
			    parseCallFunc(tc);
			    component_expr = tc;
			}
		    }
		    if ( !component_expr )
			Throw(next_tb) << "Unsupported operand for " << ident_tb->str << flush;
		    exStack.push(make_complex_component_token(component_expr, want_imag));
		    break;
		}
		// sizeof / alignof — resolve to integer constant at parse time.
		if ( ident_tb->str == "sizeof" || is_alignof_identifier(ident_tb->str) )
		{
		    if ( TokenBase *query_tb = try_parse_dynamic_type_query(*this, tb, ident_tb->str) )
			exStack.push(query_tb);
		    else
		    {
			size_t query_value = evaluate_type_query(*this, tb, ident_tb->str);
			TokenInt *ti = new TokenInt((int64_t)query_value);
			ti->setDataType(&ddUINT64);
			ti->file = tb->file;
			ti->line = tb->line;
			ti->column = tb->column;
			exStack.push(ti);
		    }
		    break;
		}
		// dynamic_cast < TYPE * > ( EXPR )   (S5c)
		if ( ident_tb->str == "dynamic_cast" )
		{
		    skip_expression_whitespace(*this);
		    if ( !peekToken() || peekToken()->id() != TokenID::tkLT )
			Throw(tb) << "Expecting '<' after dynamic_cast" << flush;
		    nextToken(); // consume '<'
		    skip_expression_whitespace(*this);
		    TokenBase *type_tb = nextToken();
		    TokenDataType *tdt = resolve_declared_type_token(*this, type_tb, true, true);
		    if ( !tdt )
			Throw(type_tb ? type_tb : tb) << "dynamic_cast target is not a type" << flush;
		    DataDef *tgt = &tdt->definition;
		    skip_expression_whitespace(*this);
		    bool is_ptr = false;
		    if ( peekToken() && peekToken()->id() == TokenID::tkMul )
		    { nextToken(); is_ptr = true; }
		    else if ( peekToken() && peekToken()->id() == TokenID::tkBand )
			Throw(tb) << "dynamic_cast to a reference type is not yet supported (use the pointer form)" << flush;
		    skip_expression_whitespace(*this);
		    if ( !peekToken() || peekToken()->id() != TokenID::tkGT )
			Throw(tb) << "Expecting '>' to close dynamic_cast<...>" << flush;
		    nextToken(); // consume '>'
		    skip_expression_whitespace(*this);
		    if ( !peekToken() || peekToken()->id() != TokenID::tkOpBrk )
			Throw(tb) << "Expecting '(' after dynamic_cast<...>" << flush;
		    nextToken(); // consume '('
		    TokenBase *inner_first = nextToken();
		    TokenBase *inner = parseExpression(inner_first, false, false, false, 0, true);
		    skip_expression_whitespace(*this);
		    TokenBase *close_tb = nextToken();
		    if ( !close_tb || close_tb->id() != TokenID::tkClBrk )
			Throw(close_tb ? close_tb : tb) << "Expecting ')' to close dynamic_cast" << flush;
		    TokenDynamicCast *dc = new TokenDynamicCast();
		    dc->target_type = tgt;
		    dc->target_is_ptr = is_ptr;
		    dc->operand = inner;
		    dc->setDataType(is_ptr ? (DataDef *)getPointerType(tgt) : tgt);
		    dc->file = tb->file; dc->line = tb->line; dc->column = tb->column;
		    exStack.push(dc);
		    break;
		}
		// typeid ( EXPR | TYPE )   (S5d)
		if ( ident_tb->str == "typeid" )
		{
		    skip_expression_whitespace(*this);
		    if ( !peekToken() || peekToken()->id() != TokenID::tkOpBrk )
			Throw(tb) << "Expecting '(' after typeid" << flush;
		    nextToken(); // consume '('
		    skip_expression_whitespace(*this);
		    TokenBase *first = nextToken();
		    TokenTypeid *ttd = new TokenTypeid();
		    // Type form iff the operand resolves to a type AND is immediately
		    // followed by ')'. resolve_declared_type_token returns NULL for a
		    // variable/expression (it checks findVariable), so typeid(obj) and
		    // typeid(*p) fall to the expression form.
		    TokenDataType *tdt = resolve_declared_type_token(*this, first, false, true);
		    skip_expression_whitespace(*this);
		    if ( tdt && peekToken() && peekToken()->id() == TokenID::tkClBrk )
		    {
			ttd->static_type = &tdt->definition;
			nextToken(); // consume ')'
		    }
		    else
		    {
			ttd->operand = parseExpression(first, false, false, false, 0, true);
			skip_expression_whitespace(*this);
			TokenBase *close_tb = nextToken();
			if ( !close_tb || close_tb->id() != TokenID::tkClBrk )
			    Throw(close_tb ? close_tb : tb) << "Expecting ')' after typeid(...)" << flush;
		    }
		    // Result type: const std::type_info& — modeled as std::type_info*.
		    // The <typeinfo> header registers `class type_info` in the global
		    // datatype_map under its bare name (header-defined classes are not
		    // namespace-keyed); fall back to the std namespace map.
		    DataDef *tinfo = NULL;
		    datatype_map_iter gdti = datatype_map.find("type_info");
		    if ( gdti != datatype_map.end() )
			tinfo = &gdti->second->definition;
		    if ( !tinfo )
		    {
			namespace_datatype_map_t::iterator nti = namespace_datatype_map.find("std");
			if ( nti != namespace_datatype_map.end() )
			{
			    datatype_map_iter dti = nti->second.find("type_info");
			    if ( dti != nti->second.end() )
				tinfo = &dti->second->definition;
			}
		    }
		    if ( !tinfo )
			Throw(tb) << "typeid requires #include <typeinfo>" << flush;
		    ttd->setDataType(getPointerType(tinfo));
		    ttd->file = tb->file; ttd->line = tb->line; ttd->column = tb->column;
		    exStack.push(ttd);
		    break;
		}
		if ( is_nullptr_identifier(ident_tb->str) )
		{
		    TokenNullptr *tnp = new TokenNullptr();
		    tnp->file = tb->file;
		    tnp->line = tb->line;
		    tnp->column = tb->column;
		    exStack.push(tnp);
		    break;
		}
		if ( ident_tb->str == "__builtin_types_compatible_p" )
		{
		    if ( !skip_expression_whitespace(*this) || peekToken()->id() != TokenID::tkOpBrk )
			Throw(tb) << "Expecting '(' after __builtin_types_compatible_p" << flush;
		    nextToken();
		    skip_expression_whitespace(*this);
		    TokenBase *lhs_tb = nextToken();
		    std::string lhs_sig;
		    if ( !parse_builtin_types_compatible_operand(*this, lhs_tb, lhs_sig) )
			Throw(lhs_tb ? lhs_tb : tb) << "Invalid first type in __builtin_types_compatible_p" << flush;
		    skip_expression_whitespace(*this);
		    TokenBase *comma_tb = nextToken();
		    if ( !comma_tb || comma_tb->id() != TokenID::tkComma )
			Throw(comma_tb ? comma_tb : tb) << "Expecting ',' in __builtin_types_compatible_p" << flush;
		    skip_expression_whitespace(*this);
		    TokenBase *rhs_tb = nextToken();
		    std::string rhs_sig;
		    if ( !parse_builtin_types_compatible_operand(*this, rhs_tb, rhs_sig) )
			Throw(rhs_tb ? rhs_tb : tb) << "Invalid second type in __builtin_types_compatible_p" << flush;
		    skip_expression_whitespace(*this);
		    TokenBase *close_tb = nextToken();
		    if ( !close_tb || close_tb->id() != TokenID::tkClBrk )
			Throw(close_tb ? close_tb : tb) << "Expecting ')' after __builtin_types_compatible_p" << flush;
		    TokenInt *ti = new TokenInt(lhs_sig == rhs_sig ? 1 : 0);
		    ti->setDataType(&ddINT);
		    ti->file = tb->file;
		    ti->line = tb->line;
		    ti->column = tb->column;
		    exStack.push(ti);
		    break;
		}
		// va_arg(ap, type) — compiler intrinsic for reading variadic args
		if ( ident_tb->str == "va_arg" )
		{
		    if ( !peekToken() || peekToken()->id() != TokenID::tkOpBrk )
			Throw(tb) << "Expecting '(' after va_arg" << flush;
		    nextToken(); // consume (
		    // first arg: the va_list expression — may be a simple
		    // identifier, *ptr, aps[4], or any expression that yields
		    // a va_list. Parse as a general expression; comma stops it.
		    TokenBase *ap_first = nextToken();
		    TokenBase *ap_expr = parseExpression(ap_first, false, false, false, 0, true);
		    // For backward compat, extract the Variable* when the
		    // expression is a simple variable or deref — but NOT for
		    // struct member (TokenMember) or subscript expressions,
		    // which need the full expression for proper Mem write-back.
		    Variable *ap_var = NULL;
		    if ( dynamic_cast<TokenMember *>(ap_expr)
		      || dynamic_cast<TokenSubscriptExpr *>(ap_expr) )
			; // leave ap_var NULL — compiler uses ap_expr->operand()
		    else if ( TokenVar *tv = dynamic_cast<TokenVar *>(ap_expr) )
			ap_var = &tv->var;
		    else if ( TokenDeref *td = dynamic_cast<TokenDeref *>(ap_expr) )
			ap_var = &td->var;
		    // consume comma
		    TokenBase *comma_tb = nextToken();
		    if ( comma_tb->id() != TokenID::tkComma )
			Throw(comma_tb) << "Expecting ',' after va_list expression in va_arg" << flush;
		    // second arg: type name
		    TokenBase *type_tb = nextToken();
		    while ( is_type_qualifier_token(type_tb) )
		    {
			type_tb = nextToken();
			if ( !type_tb )
			    Throw(comma_tb) << "Expecting type after qualifier in va_arg" << flush;
		    }
		    DataDef *target_dd = NULL;
		    if ( type_tb->type() == TokenType::ttDataType )
			target_dd = &((TokenDataType *)type_tb)->definition;
		    else if ( type_tb->type() == TokenType::ttIdentifier
		      && ((TokenIdent *)type_tb)->str == "typeof" )
		    {
			TokenDataType *typeof_dt = parse_typeof_datatype(*this, type_tb);
			target_dd = typeof_dt ? &typeof_dt->definition : NULL;
		    }
		    else if ( type_tb->type() == TokenType::ttIdentifier )
		    {
			std::string tname = ((TokenIdent *)type_tb)->str;
			datatype_map_iter tdmi = datatype_map.find(tname);
			if ( tdmi != datatype_map.end() )
			    target_dd = &tdmi->second->definition;
			if ( !target_dd )
			{
			    datadef_map_iter sdmi = struct_map.find(tname);
			    if ( sdmi != struct_map.end() )
				target_dd = sdmi->second;
			}
		    }
		    // handle 'struct Tag' or 'union Tag' as va_arg type
		    if ( !target_dd && type_tb->type() == TokenType::ttKeyword
			&& (type_tb->id() == TokenID::tkSTRUCT || type_tb->id() == TokenID::tkUNION) )
		    {
			TokenBase *tag_tb = nextToken();
			if ( tag_tb && tag_tb->type() == TokenType::ttIdentifier )
			{
			    std::string sname = ((TokenIdent *)tag_tb)->str;
			    datadef_map_iter sdmi = struct_map.find(sname);
			    if ( sdmi != struct_map.end() )
				target_dd = sdmi->second;
			}
		    }
		    // handle 'enum Tag' — treat as int
		    if ( !target_dd && type_tb->type() == TokenType::ttKeyword
			&& type_tb->id() == TokenID::tkENUM )
		    {
			nextToken(); // consume tag name
			target_dd = &ddINT;
		    }
		    // handle compound type specifiers: unsigned, long, etc.
		    if ( !target_dd && type_tb->type() == TokenType::ttDataType )
			target_dd = &((TokenDataType *)type_tb)->definition;
		    if ( !target_dd )
			Throw(type_tb) << "Unknown type in va_arg" << flush;
		    // handle pointer: va_arg(ap, char *)
		    while ( peekToken() && peekToken()->id() == TokenID::tkMul )
		    {
			nextToken(); // consume '*'
			target_dd = getPointerType(target_dd);
		    }
		    // consume closing ) unless a nested typeof/expression parse
		    // already balanced the token stream to the outer close-paren
		    if ( peekToken() && peekToken()->id() == TokenID::tkClBrk )
			nextToken();
		    else if ( (!curToken() || curToken()->id() != TokenID::tkClBrk)
		       && (!prevToken() || prevToken()->id() != TokenID::tkClBrk) )
			Throw(type_tb) << "Expecting ')' after va_arg type" << flush;
		    exStack.push(new TokenVaArg(ap_var, ap_expr, target_dd));
		    break;
		}
	    	if ( prevToken() && prevToken()->id() == TokenID::tkDot )
		{
#if 0
		    DBG(cout << "parseExpression() prevToken is tkDot, pushing TokenIdent " << ((TokenIdent *)tb)->str << endl);
		    exStack.push(tb);
#else
		    if ( exStack.empty() )
			Throw(tb) << "expected expression" << flush;
		    // Accept TokenVar, TokenMember, TokenSubscript, or a
		    // compound literal as LHS for dot access. Subscript case:
		    // tab[i].member for an array of structs.
		    TokenBase *lhs_dot = exStack.top();
		    if ( TokenBase *ctx_member = resolve_expression_context_member(lhs_dot, ident_tb) )
		    {
			exStack.pop();
			exStack.push(ctx_member);
			if ( !opStack.empty() && opStack.top()->id() == TokenID::tkDot )
			    opStack.pop();
			break;
		    }
		    if ( lhs_dot->type() != TokenType::ttVariable
		      && lhs_dot->type() != TokenType::ttMember
		      && lhs_dot->type() != TokenType::ttSubscript
		      && lhs_dot->type() != TokenType::ttCompound
		      && lhs_dot->type() != TokenType::ttStructLit
		      && lhs_dot->type() != TokenType::ttCallFunc
		      && lhs_dot->type() != TokenType::ttOperator   // (a + b).member — operator result object
		      && lhs_dot->id() != TokenID::tkObjTemp        // T(args).member — functional-ctor temp
		      && lhs_dot->id() != TokenID::tkTypeid )   // typeid(x).name() (S5d)
			Throw(tb) << "member reference is not a structure or union" << flush;
		    Variable *tv_var;
		    DataDef  *struct_type;
		    if ( lhs_dot->type() == TokenType::ttVariable )
		    {
			TokenVar *tv = dynamic_cast<TokenVar *>(lhs_dot);
			tv_var      = &tv->var;
			struct_type =  tv->var.type;
			// A class-REFERENCE parameter (`A &a`) is lowered to a
			// pointer (vfREFERENCE, type DataDefPTR(A)) — like a numeric
			// `T&`. Member access `a.member` is therefore `a->member`:
			// resolve struct_type to the pointed-to class so member /
			// method lookup works, while tv_var keeps the pointer-typed
			// reference var so the CIR member codegen emits N_DEREF_FIELD.
			// The hidden `__this` parameter is the same shape: a
			// pointer-to-class. `this.member` (madc's dot form) is thus
			// `this->member` — unwrap to the class for lookup, keep the
			// pointer var for codegen. (`this->member` already takes the
			// arrow path; this clause covers only the dot spelling.)
			if ( ((tv->var.flags & vfREFERENCE) || tv->var.name == "__this")
			  && struct_type && struct_type->is_pointer() )
			{
			    DataDefPTR *rp = dynamic_cast<DataDefPTR *>(struct_type);
			    if ( rp && rp->base_type
			      && (rp->base_type->is_struct()
			       || rp->base_type->is_object()) )
				struct_type = rp->base_type;
			}
		    }
		    else if ( lhs_dot->type() == TokenType::ttMember )
		    {
			TokenMember *tm = dynamic_cast<TokenMember *>(lhs_dot);
			if ( tm )
			{
			    tv_var      = &tm->var;
			    struct_type =  tm->var.type;
			}
			else if ( TokenDeref *tdl = dynamic_cast<TokenDeref *>(lhs_dot) )
			{
			    // (*p).member — logically equivalent to p->member.
			    // Route the dot through the pointer variable so the
			    // normal TokenMember pointer-in-Gp path compiles it
			    // as [p + offset] at codegen time.
			    tv_var      = &tdl->var;
			    struct_type =  tdl->deref_type;
			}
			else if ( TokenDerefExpr *tdxl = dynamic_cast<TokenDerefExpr *>(lhs_dot) )
			{
			    // (*expr).member — expr yields a pointer whose target
			    // is a struct. Resolve member lookup against the
			    // dereferenced struct type; codegen uses the expr's
			    // pointer value as base via TokenMember's parent_expr
			    // path.
			    tv_var      = new Variable("__deref_expr", *tdxl->deref_type, 1, NULL, false);
			    struct_type =  tdxl->deref_type;
			}
			else
			    Throw(tb) << "member reference '.' on unsupported deref expression" << flush;
		    }
		    else
		    {
			TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(lhs_dot);
			if ( tsub )
			{
			    tv_var      = &tsub->object;
			    struct_type =  tsub->datadef(); // element type
			}
			else if ( TokenSubscriptExpr *tse = dynamic_cast<TokenSubscriptExpr *>(lhs_dot) )
			{
			    // expr[i].member — subscript LHS is an arbitrary
			    // pointer/array-producing expression (e.g.
			    // `ch->pcdata->killed[x].vnum`). Synthesize a
			    // struct-typed object variable and route codegen
			    // through the TokenSubscriptExpr as parent_expr;
			    // TokenMember::operand's dot-chain path handles
			    // [base + idx*shift + offset].
			    DataDef *elem_type = tse->datadef();
			    if ( !elem_type )
				Throw(tb) << "subscript expression has no element type" << flush;
			    tv_var      = new Variable("__sub_expr", *elem_type, 1, NULL, false);
			    struct_type =  elem_type;
			}
			else if ( TokenStructLit *slit = dynamic_cast<TokenStructLit *>(lhs_dot) )
			{
			    struct_type = slit->datadef();
			    if ( !struct_type )
				Throw(tb) << "compound literal has no type" << flush;
			    tv_var = new Variable("__compound_lit", *struct_type, 1, NULL, false);
			}
			else if ( lhs_dot->type() == TokenType::ttCompound )
			{
			    DataDef *stmt_type = lhs_dot->datadef();
			    if ( !stmt_type )
				Throw(tb) << "statement expression has no type for member access" << flush;
			    struct_type = stmt_type;
			    tv_var = new Variable("__stmt_expr", *struct_type, 1, NULL, false);
			}
			else if ( lhs_dot->type() == TokenType::ttCallFunc )
			{
			    // f().member — function returning a struct, immediate member access.
			    // The function call's return type is the struct type.
			    TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(lhs_dot);
			    DataDef *ret_type = tcf ? tcf->returns() : NULL;
			    if ( !ret_type )
				Throw(tb) << "function call has no return type for member access" << flush;
			    struct_type = ret_type;
			    tv_var = new Variable("__call_expr", *struct_type, 1, NULL, false);
			}
			else if ( lhs_dot->type() == TokenType::ttOperator
			       || lhs_dot->id() == TokenID::tkObjTemp )
			{
			    // (a + b).member or T(args).member — a class-object rvalue
			    // (an overloaded-operator result typed by
			    // resolve_object_operator_type, or a functional-construction
			    // temp TokenObjTemp). The rvalue is materialized at codegen via
			    // parent_expr (class_this_arg -> translate_expr -> the temp).
			    DataDef *op_type = lhs_dot->datadef();
			    if ( !op_type || !op_type->is_object() )
				Throw(tb) << "member reference is not a structure or union" << flush;
			    struct_type = op_type;
			    tv_var = new Variable("__op_expr", *struct_type, 1, NULL, false);
			}
			else if ( lhs_dot->id() == TokenID::tkTypeid )
			{
			    // typeid(x).name() — the typeid result is std::type_info*
			    // (a reference modeled as a pointer); resolve member lookup
			    // against the pointed-to type_info class and route codegen
			    // through the typeid as parent_expr (class_this_arg passes
			    // the pointer straight through as `this`).
			    DataDef *rt = lhs_dot->datadef();
			    DataDefPTR *rp = dynamic_cast<DataDefPTR *>(rt);
			    struct_type = rp ? rp->base_type : rt;
			    if ( !struct_type )
				Throw(tb) << "typeid result has no type for member access" << flush;
			    tv_var = new Variable("__typeid_expr", *struct_type, 1, NULL, false);
			}
			else
			    Throw(tb) << "member reference '.' on unsupported subscript form" << flush;
		    }
		    if ( !struct_type->is_struct() && !struct_type->is_object() )
			Throw(tb) << "member reference is not a structure or union" << flush;
		    var = NULL;
		    string id = ident_tb->str;
		    if ( struct_type->is_object() && (var=((DataDefCLASS *)struct_type)->findMethod(id)) )
		    {
			// Access control (P2.5c): reject a private/protected METHOD
			// call from outside the (derived) class. Same context + rule
			// as the data-member check.
			{
			    DataDefCLASS *cur_class =
				(code && code->method) ? code->method->owner_class : NULL;
			    std::string av =
				method_access_violation(struct_type, var, id, cur_class);
			    if ( !av.empty() )
				Throw(tb) << av << flush;
			}
			// Method call on a class-object receiver. The common case is a
			// bare object variable (`s.length()`). A subscript element
			// (`v[i].length()` where v's operator[] returns a class element)
			// is also a valid receiver: build the TokenCallMethod with the
			// subscript as parent_expr so the CIR side (class_this_arg)
			// addresses the element via class_subscript_addr. Other chained
			// receivers (member-of-member returning an object, etc.) are not
			// yet supported.
			// (Both TokenSubscript and TokenSubscriptExpr report
			// ttSubscript.)
			TokenBase *recv_parent = NULL;
			if ( lhs_dot->type() == TokenType::ttSubscript )
			    recv_parent = lhs_dot;
			else if ( lhs_dot->id() == TokenID::tkTypeid )
			    recv_parent = lhs_dot;   // typeid(x).name() (S5d)
			else if ( lhs_dot->type() == TokenType::ttOperator
			       || lhs_dot->id() == TokenID::tkObjTemp )
			    recv_parent = lhs_dot;   // (a + b).method() — operator result object
			else if ( lhs_dot->type() != TokenType::ttVariable )
			    Throw(tb) << "chained method call not yet supported" << flush;
			TokenCallMethod *tc = new TokenCallMethod(*tv_var, *var);
			if ( recv_parent )
			    tc->parent_expr = recv_parent;
			tb = nextToken();
			tc->line = tb->line;
			tc->column = tb->column;
			// if bracket, parse params
			if ( tb->id() == TokenID::tkOpBrk )
			{
			    // delete tb?
			    tb = parseCallMethod(tc);
			    DBG(cout << "parseCallMethod returned with token " << (char)tb->get() << endl);
			}
			// Now that the argument types are known, re-bind to the
			// overload they actually select (findMethod above took the
			// first by-name match).
			tc = reselect_method_overload(tc, *tv_var,
				(DataDefCLASS *)struct_type, id);
			var = &tc->var;
			// remove object TokenVar from exStack
			exStack.pop();
			// remove TokenDot from opStack
			if ( !opStack.empty() && opStack.top()->id() == TokenID::tkDot )
			{
			    DBG(cout << "parseCallMethod, removing tkDot from opStack" << endl);
			    opStack.pop();
			}
			DBG(cout << "Pushing found method call: " << var->name << "() onto opStack" << endl);
			opStack.push(tc);
			// I'm not sure why I need to do this TODO: figure this out
			if ( tb->id() == TokenID::tkSemi )
			    done = true;
			break;
		    }
		    // get offset
		    ssize_t ofs = ((DataDefSTRUCT *)struct_type)->m_offset(id);
		    if ( ofs == -1 )
			Throw(tb) << "Unidentified member" << flush;
		    // Access control (P2.5): reject private/protected member access
		    // from outside the (derived) class. cur_class = the class whose
		    // method body we are parsing (NULL outside any method).
		    {
			DataDefCLASS *cur_class =
			    (code && code->method) ? code->method->owner_class : NULL;
			std::string av =
			    member_access_violation(struct_type, id, cur_class);
			if ( !av.empty() )
			    Throw(tb) << av << flush;
		    }
		    DataDef *mtype = ((DataDefSTRUCT *)struct_type)->m_type(id);
		    // create new variable
		    var = new Variable(id, *mtype, 1, NULL, false);
		    var->flags = member_proxy_flags(tv_var->flags);
		    if ( tv_var->data )
			var->data = (void *)((char *)tv_var->data + ofs);
		    // remove LHS from exStack
		    exStack.pop();
		    // When LHS carries its own base-pointer (TokenMember for dot/arrow
		    // chains, or TokenSubscript for array-of-structs), pass it as
		    // parent_expr so TokenMember::operand() can resolve the base at
		    // codegen time.
		    //
		    // TokenDeref reports ttMember for LHS-compat reasons but is
		    // not a real TokenMember — for `(*p).x` we already routed
		    // tv_var to the pointer `p`, so the normal no-parent_expr path
		    // compiles it as `p->x` via voperand's pointer-in-Gp branch.
		    //
		    // TokenDerefExpr is the opposite: `(*expr).x` where expr is a
		    // pointer-producing expression. Pass the TokenDerefExpr as
		    // parent_expr so TokenMember::operand calls expr->compile to
		    // materialize the pointer value at codegen, then accesses
		    // [ptr + offset] via the struct-value ("dot chain") branch.
		    bool is_deref_lhs = (dynamic_cast<TokenDeref *>(lhs_dot) != NULL);
		    bool is_derefexpr_lhs = (dynamic_cast<TokenDerefExpr *>(lhs_dot) != NULL);
		    bool is_compound_lit_lhs = (dynamic_cast<TokenStructLit *>(lhs_dot) != NULL);
		    bool is_stmt_expr_lhs = (lhs_dot->type() == TokenType::ttCompound);
		    bool is_callfunc_lhs = (lhs_dot->type() == TokenType::ttCallFunc);
		    if ( is_derefexpr_lhs || is_compound_lit_lhs || is_stmt_expr_lhs || is_callfunc_lhs )
			exStack.push(new TokenMember(*tv_var, *var, ofs, lhs_dot));
		    else if ( !is_deref_lhs
		      && (lhs_dot->type() == TokenType::ttMember
		       || lhs_dot->type() == TokenType::ttSubscript) )
			exStack.push(new TokenMember(*tv_var, *var, ofs, lhs_dot));
		    else
			exStack.push(new TokenMember(*tv_var, *var, ofs));
		    // remove TokenDot from opStack
		    if ( !opStack.empty() && opStack.top()->id() == TokenID::tkDot )
			opStack.pop();
#endif
		    break;
		}
		// -> pointer member access: ptr->member
		if ( prevToken() && prevToken()->id() == TokenID::tkDeRef )
		{
		    if ( exStack.empty() )
			Throw(tb) << "expected expression before '->'" << flush;

		    // get the pointer-valued LHS — from TokenVar/TokenMember or a
		    // pointer-returning subscript expression such as tab[i]->field
		    TokenBase *lhs = exStack.top();
		    Variable *obj_var = NULL;
		    DataDef *obj_type = NULL;
		    bool expr_backed_lhs = false;
		    bool fixed_array_arrow = false;
		    bool arrow_via_op = false;
		    // P2.1b gap 2 — operator->: when the LHS is a class OBJECT (not a
		    // pointer) whose class declares operator->, C++ canon rewrites
		    // `obj->m` as `(obj.operator->())->m`: call operator-> (which
		    // returns a pointer), then apply the real `->` to that result.
		    // Reuses the class-method dispatch — no parallel codegen (I6).
		    if ( lhs->type() == TokenType::ttVariable )
		    {
			TokenVar *otv = dynamic_cast<TokenVar *>(lhs);
			DataDefCLASS *ocls = (otv && otv->var.type && otv->var.type->is_object())
			    ? dynamic_cast<DataDefCLASS *>(otv->var.type) : NULL;
			std::string arrow_name("operator->");
			Variable *arrow_m = ocls ? ocls->findMethod(arrow_name) : NULL;
			if ( arrow_m && !otv->var.type->is_pointer() )
			{
			    TokenCallMethod *opcall = new TokenCallMethod(otv->var, *arrow_m);
			    opcall->file = tb->file;
			    opcall->line = tb->line;
			    opcall->column = tb->column;
			    DataDef *ret = opcall->datadef();
			    if ( !ret || !ret->is_pointer() )
				Throw(tb) << "operator-> must return a pointer" << flush;
			    exStack.pop();
			    exStack.push(opcall);
			    lhs = opcall;
			    obj_type = ret;
			    obj_var = new Variable("__arrow_op", *obj_type, 1, NULL, false);
			    expr_backed_lhs = true;
			    arrow_via_op = true;
			}
		    }
		    if ( arrow_via_op )
			; // LHS already resolved to operator-> call result above
		    else if ( lhs->type() == TokenType::ttVariable )
		    {
			TokenVar *tv_lhs = dynamic_cast<TokenVar *>(lhs);
			obj_var = &tv_lhs->var;
			obj_type = obj_var->type;
			fixed_array_arrow = obj_var->is_fixed_array();
		    }
		    else if ( lhs->type() == TokenType::ttMember )
		    {
			// TokenDeref and TokenDerefExpr also report ttMember (reuse
			// member type for assignment compat) but are not TokenMember
			// instances. When the cast fails, fall through to the
			// expression-backed path using the node's reported datadef
			// instead of throwing.
			TokenMember *tm = dynamic_cast<TokenMember *>(lhs);
			if ( tm )
			{
			    obj_var = &tm->var;
			    obj_type = tm->var.type;
			}
			else if ( lhs->datadef() && lhs->datadef()->is_pointer() )
			{
			    obj_type = lhs->datadef();
			    obj_var = new Variable("__arrow_expr", *obj_type, 1, NULL, false);
			    expr_backed_lhs = true;
			}
			else
			    Throw(tb) << "expression before '->' must be a pointer to struct" << flush;
		    }
		    else if ( effective_pointer_type_for_member_access(*this, lhs) )
		    {
			obj_type = effective_pointer_type_for_member_access(*this, lhs);
			obj_var = new Variable("__arrow_expr", *obj_type, 1, NULL, false);
			expr_backed_lhs = true;
		    }
		    else
			Throw(tb) << "expression before '->' must be a pointer" << flush;

		    if ( !fixed_array_arrow && !obj_type->is_pointer() )
			Throw(tb) << "expression before '->' must be a pointer" << flush;

		    // get the pointed-to type
		    DataDef *base = obj_type;
		    if ( !fixed_array_arrow )
		    {
			DataDefPTR *ptr_type = dynamic_cast<DataDefPTR *>(obj_type);
			if ( !ptr_type || !ptr_type->base_type )
			    Throw(tb) << "expression before '->' is not a typed pointer" << flush;
			base = ptr_type->base_type;
		    }
		    if ( !base->is_struct() && !base->is_object() )
			Throw(tb) << "member reference type is not a structure or union" << flush;

		    string id = ident_tb->str;

		    // get member offset and type — or method
		    ssize_t ofs = ((DataDefSTRUCT *)base)->m_offset(id);
		    if ( ofs == -1 && base->is_object() )
		    {
			// Check if it's a method call on a class pointer
			var = ((DataDefCLASS *)base)->findMethod(id);
			if ( var )
			{
			    // Access control (P2.5c): reject a private/protected
			    // METHOD call via `->` from outside the (derived) class.
			    {
				DataDefCLASS *cur_class =
				    (code && code->method) ? code->method->owner_class : NULL;
				std::string av =
				    method_access_violation(base, var, id, cur_class);
				if ( !av.empty() )
				    Throw(tb) << av << flush;
			    }
			    // arrow method call: ptr->method(args). The common LHS is a
			    // pointer variable / member. A SUBSCRIPT element that is a
			    // pointer-to-class (`v[i]` where v is vector<Base*>) is also a
			    // valid receiver: build the TokenCallMethod with the subscript
			    // as parent_expr so the CIR side (class_this_arg) passes the
			    // element POINTER VALUE as __this (recv_is_ptr -> pass the
			    // pointer directly, NOT its address — unlike the dot case where
			    // the element is an object and __this = &element). Other chained
			    // receivers (member-of-call, etc.) are not yet supported.
			    TokenBase *recv_parent = NULL;
			    if ( lhs->type() == TokenType::ttSubscript )
				recv_parent = lhs;
			    else if ( arrow_via_op )
				// operator-> result is a pointer VALUE; pass it as
				// __this directly (recv_is_ptr), like a pointer element.
				recv_parent = lhs;
			    else if ( lhs->type() != TokenType::ttVariable
				   && lhs->type() != TokenType::ttMember )
				Throw(tb) << "chained arrow method call not yet supported" << flush;
			    TokenCallMethod *tc = new TokenCallMethod(*obj_var, *var);
			    if ( recv_parent )
				tc->parent_expr = recv_parent;
			    tb = nextToken();
			    tc->line = tb->line;
			    tc->column = tb->column;
			    if ( tb->id() == TokenID::tkOpBrk )
				tb = parseCallMethod(tc);
			    // Re-bind to the overload the argument types select.
			    tc = reselect_method_overload(tc, *obj_var,
				    (DataDefCLASS *)base, id);
			    var = &tc->var;
			    exStack.pop();
			    // remove TokenDeRef from opStack
			    if ( !opStack.empty() && opStack.top()->id() == TokenID::tkDeRef )
				opStack.pop();
			    opStack.push(tc);
			    break;
			}
		    }
		    if ( ofs == -1 )
			Throw(tb) << "no member named '" << id << "'" << flush;
		    // Access control (P2.5): reject private/protected member access
		    // via `->` from outside the (derived) class.
		    {
			DataDefCLASS *cur_class =
			    (code && code->method) ? code->method->owner_class : NULL;
			std::string av =
			    member_access_violation(base, id, cur_class);
			if ( !av.empty() )
			    Throw(tb) << av << flush;
		    }
		    DataDef *mtype = ((DataDefSTRUCT *)base)->m_type(id);

		    // create variable for the member
		    var = new Variable(id, *mtype, 1, NULL, false);
		    var->flags = member_proxy_flags(obj_var->flags);

		    // remove LHS from exStack
		    exStack.pop();
		    // for chained -> (lhs was a TokenMember), pass it as parent_expr so
		    // operand() can compile the intermediate pointer at codegen time
		    if ( lhs->type() == TokenType::ttMember || expr_backed_lhs )
			exStack.push(new TokenMember(*obj_var, *var, ofs, lhs));
		    else
			exStack.push(new TokenMember(*obj_var, *var, ofs));
		    // remove TokenDeRef from opStack
		    if ( !opStack.empty() && opStack.top()->id() == TokenID::tkDeRef )
			opStack.pop();
		    break;
		}
		// namespace resolution: identifier :: member
		if ( peekToken() && peekToken()->id() == TokenID::tkNS )
		{
		    std::string ns_name = ident_tb->str;
		    namespace_map_t::iterator nsi = namespace_map.find(ns_name);
		    if ( nsi == namespace_map.end() )
			Throw(tb) << "Unknown namespace '" << ns_name << "'" << flush;
		    nextToken(); // consume '::'
		    TokenBase *member_tb = nextToken(); // consume member identifier
		    if ( !member_tb || member_tb->type() != TokenType::ttIdentifier )
			Throw(tb) << "Expecting identifier after '" << ns_name << "::'" << flush;
		    std::string member_name = ((TokenIdent *)member_tb)->str;
		    variable_map_iter vmi = nsi->second.find(member_name);
		    if ( vmi == nsi->second.end() )
		    {
			// try dlsym fallback if this namespace was loaded via #load
			std::map<std::string, void *>::iterator dli = dlopen_map.find(ns_name);
			if ( dli == dlopen_map.end() )
			    Throw(member_tb) << "'" << member_name << "' is not a member of namespace '" << ns_name << "'" << flush;
			if ( !is_dynamic_symbol_fallback_enabled() )
			    Throw(member_tb) << "dynamic symbol fallback is disabled by registration policy" << flush;
			if ( !is_dynamic_symbol_allowed(member_name) )
			    Throw(member_tb) << "dynamic symbol '" << member_name
					     << "' is not allowed by registration policy" << flush;
			void *sym = dlsym(dli->second, member_name.c_str());
			if ( !sym )
			    Throw(member_tb) << "dlsym failed for '" << member_name << "' in '" << ns_name << "': " << dlerror() << flush;
			// create function with int64 return, no declared params (variadic-like)
			// actual args are passed through at compile time
			std::string func_id = "__dl_" + ns_name + "_" + member_name;
			var = addFunction(func_id,
			    datatype_vec_t{DataType::dtINT64},
			    (fVOIDFUNC)sym);
			if ( !var )
			    Throw(member_tb) << "Failed to register dlsym function '" << member_name << "'" << flush;
			nsi->second[member_name] = var; // cache for next call
			DBG(cout << "parseExpression() dlsym resolved " << ns_name << "::" << member_name << " at " << (uint64_t)sym << endl);
		    }
		    else
			var = vmi->second;
		    DBG(cout << "parseExpression() resolved " << ns_name << "::" << member_name << endl);
		    tb = member_tb; // update tb for line/col tracking below
		    goto ns_resolved;
		}
		// Statement-head `ns::member(...)` should resolve the callee from
		// the active namespace first, but once we're inside that call's
		// arguments / subexpressions, lexical scope should win so locals
		// can shadow same-named namespace members (`ruby::chars(chars, s)`).
		var = resolve_preferred_identifier(ident_tb, expression_head);
		// `this` keyword: inside a class method body it names the hidden
		// __this parameter (a `ClassName*`). Resolving it here lets
		// `this->member` / `this.member` / `this->method()` flow through the
		// EXISTING pointer-member-access path, and `this` as a value (e.g.
		// `return this;`) be the pointer itself. Outside a method it stays
		// unresolved -> the usual "undeclared identifier" error.
		if ( !var && ident_tb->str == "this"
		     && code && code->method && code->method->owner_class )
		{
		    std::string thisid = "__this";
		    var = code->method->findParameter(thisid);
		}
		// class method: resolve unqualified member name through __this
		if ( !var && code && code->method && code->method->owner_class )
		{
		    DataDefCLASS *cls = code->method->owner_class;
		    std::string mname = ident_tb->str;
		    ssize_t ofs = cls->m_offset(mname);
		    if ( ofs >= 0 )
		    {
			DataDef *mtype = cls->m_type(mname);
			// find __this parameter
			std::string thisid = "__this";
			Variable *thisvar = code->method->findParameter(thisid);
			if ( thisvar )
			{
			    Variable *member = new Variable(mname, *mtype, 1, NULL, false);
			    exStack.push(new TokenMember(*thisvar, *member, ofs));
			    break;
			}
		    }
		}
		// lazy-load check: symbol registered by #include but not yet created
		if ( !var )
		    var = lazy_resolve(ident_tb->str);
		if ( !var )
		{
		    TokenBase *ctx_value = resolve_expression_context_identifier(ident_tb);
		    if ( ctx_value )
		    {
			exStack.push(ctx_value);
			break;
		    }
		}
		if ( !var && peekToken() && peekToken()->id() == TokenID::tkOpBrk )
		{
		    std::string fname = ident_tb->str;
		    // dlsym fallback: resolve known libc/system functions early.
		    // If that fails, C89 still permits an implicit `int f()`
		    // declaration for calls to functions defined later in the file.
		    if ( !var && is_implicit_complex_builtin_name(fname) )
		    {
			FuncDef *implicit_func = make_implicit_complex_builtin_func(fname);
			var = addVariable(NULL, *implicit_func, fname, 1, NULL, false);
			Method *implicit_method = new Method(*var);
			var->data = (void *)implicit_method;
			// Register in funcdef_map so the CIR proto pass emits an
			// extern prototype carrying the real (often _Complex) return
			// type. Without a prototype c2mir defaults an undeclared call
			// to `int (...)`, so a _Complex-returning builtin (conjf, …)
			// returned through the wrong (scalar/variadic) ABI.
			funcdef_map[fname] = implicit_func;
			DBG(cout << "parseExpression() created builtin complex helper declaration for " << fname << endl);
		    }
		    if ( is_dynamic_symbol_fallback_enabled()
		      && !is_implicit_complex_builtin_name(fname)
		      && is_dynamic_symbol_allowed(fname) )
		    {
			void *sym = dlsym(RTLD_DEFAULT, fname.c_str());
			if ( sym )
			{
			    var = addFunction(fname,
				datatype_vec_t{dynamic_symbol_fallback_return_type(fname)},
				(fVOIDFUNC)sym);
			    DBG(if (var) cout << "parseExpression() dlsym fallback resolved " << fname << " at " << (uint64_t)sym << endl);
			}
		    }
		    if ( !var && is_implicit_complex_builtin_name(fname) )
		    {
			FuncDef *implicit_func = make_implicit_complex_builtin_func(fname);
			var = addVariable(NULL, *implicit_func, fname, 1, NULL, false);
			Method *implicit_method = new Method(*var);
			var->data = (void *)implicit_method;
			funcdef_map[fname] = implicit_func;  /* emit a real prototype (see above) */
			DBG(cout << "parseExpression() created builtin complex helper declaration for " << fname << endl);
		    }
		    if ( !var && token_origin_allows_c89_implicit_function(ident_tb) )
		    {
			FuncDef *implicit_func = new FuncDef(ddINT32);
			// No prototype exists yet, so accept any argument count.
			// The real later definition replaces var->type before
			// TokenCallFunc compiles calls to this variable.
			implicit_func->is_varargs = true;
			implicit_func->parameters.push_back(&ddINT64);
			implicit_func->param_typedef_names.push_back("");
			var = addVariable(NULL, *implicit_func, fname, 1, NULL, false);
			Method *implicit_method = new Method(*var);
			var->data = (void *)implicit_method;
			DBG(cout << "parseExpression() created implicit function declaration for " << fname << endl);
		    }
		}
		if ( !var )
		{
		    // Functional construction `T(args)`: only when the unresolved
		    // identifier names a class type followed by '(' (ordinary calls
		    // resolved to `var` above). General; no per-class machinery.
		    if ( TokenObjTemp *ot = try_parse_functional_ctor(*this, tb) )
		    {
			exStack.push(ot);
			break;
		    }
		    DBG(cerr << "parseExpression() failed to resolve identifier " << ident_tb->str << endl);
		    Throw(tb) << "use of undeclared identifier '" << ident_tb->str << '\'' << flush;
		}
		ns_resolved:
		if ( var->type->is_function() )
		{
		    // function pointer variable (DataDefFPTR) — different from regular functions
		    if ( var->type->is_numeric() )
		    {
			// FPTR variable: if followed by (, call through pointer
			if ( peekToken() && peekToken()->id() == TokenID::tkOpBrk )
			{
			    TokenCallFunc *tc = new TokenCallFunc(*var);
			    tb = nextToken();
			    tc->line = tb->line;
			    tc->column = tb->column;
			    tb = parseCallFunc(tc);
			    opStack.push(tc);
			    if ( tb->id() == TokenID::tkSemi )
				done = true;
			}
			else
			{
			    // FPTR variable as value — push onto exStack
			    exStack.push(new TokenVar(*var));
			}
			break;
		    }
		    // Regular function identifier.
		    // C function-to-pointer decay: a bare function name used as an
		    // rvalue (RHS of assignment) becomes its address, so
		    // `fptr = func_name;` writes the function's address into fptr.
		    // Other contexts (e.g. `cout << endl;`, where BSL consumes a
		    // no-arg ostream-taking function) keep the pre-decay behavior.
		    {
			TokenBase *peek_after = peekToken();
			TokenID peek_id = peek_after ? peek_after->id() : TokenID::tkBase;
			bool followed_by_paren = (peek_id == TokenID::tkOpBrk);
			// Value-context followers: struct/array-init element end,
			// call-arg end, ternary branch separator - a bare function
			// name in these positions is passing/returning its address.
			bool followed_by_value_end =
			    peek_id == TokenID::tkComma || peek_id == TokenID::tkClBrk
			 || peek_id == TokenID::tkClSqr || peek_id == TokenID::tkClBrc
			 || peek_id == TokenID::tkTerC
			 // `;` follower with empty opStack: a bare function name
			 // followed by semicolon at the top of an expression is
			 // function-to-pointer decay (`return func;`). The
			 // empty-opStack guard preserves operator-consuming
			 // patterns like `cout << endl;` where BSL on opStack
			 // wants the no-arg function call form, not the address.
			 // Closes the SMAUG `tables.c:skill_function` `return
			 // do_aassign;` family where TokenCallFunc was being
			 // built for a void-returning function, yielding an
			 // empty Operand back into TokenRETURN.
			 || (peek_id == TokenID::tkSemi && opStack.empty())
			 // Binary comparison / logical / bitwise operators: a bare
			 // function name on either side of these is its address
			 // (function-to-pointer decay), not a call. Closes patterns
			 // like `t->fn == do_cast && tmp->...` where do_cast was
			 // previously pushed as a TokenCallFunc, then the operator
			 // was silently consumed and the next token mis-parsed.
			 || peek_id == TokenID::tkEquals || peek_id == TokenID::tkNotEq
			 || peek_id == TokenID::tkLT     || peek_id == TokenID::tkLE
			 || peek_id == TokenID::tkGT     || peek_id == TokenID::tkGE
			 || peek_id == TokenID::tkLand   || peek_id == TokenID::tkLor
			 || peek_id == TokenID::tkBand   || peek_id == TokenID::tkBor
			 || peek_id == TokenID::tkXor;
			bool in_assign_context = false;
			if ( !opStack.empty() )
			{
			    TokenID opid = opStack.top()->id();
			    if ( opid == TokenID::tkAssign
			      || opid == TokenID::tkAddEq || opid == TokenID::tkSubEq
			      || opid == TokenID::tkMulEq  || opid == TokenID::tkDivEq
			      || opid == TokenID::tkModEq  || opid == TokenID::tkXorEq
			      || opid == TokenID::tkBandEq || opid == TokenID::tkBorEq
			      || opid == TokenID::tkBSLEq  || opid == TokenID::tkBSREq )
				in_assign_context = true;
			}
			if ( !followed_by_paren && (in_assign_context || followed_by_value_end) )
			{
			    DBG(cout << "Pushing function address (decay): " << var->name << " onto exStack" << endl);
			    exStack.push(new TokenVar(*var));
			    break;
			}
		    }
		    Variable *call_var = runtime_eval_scope_target(var);
		    TokenCallFunc *tc = new TokenCallFunc(*call_var);
		    tc->auto_scope_context = is_runtime_eval_scope_ctx_helper_name(call_var->name)
			&& is_runtime_eval_scope_public_name(ident_tb->str);
		    tb = nextToken();
		    tc->line = tb->line;
		    tc->column = tb->column;
		    if ( tb->id() == TokenID::tkOpBrk )
		    {
			tb = parseCallFunc(tc);
			DBG(cout << "parseCallFunc returned with token " << (char)tb->get() << endl);
		    }
		    DBG(cout << "Pushing found function call: " << var->name << "() onto opStack" << endl);
		    opStack.push(tc);
		    if ( tb->id() == TokenID::tkSemi )
			done = true;
		    break;
		}
		if ( var->type->is_integer() )
		    DBG(cout << "Pushing found variable: " << var->name << '=' << (int)var->get<int>() << " onto exStack" << endl);
		else
		if ( var->type->is_real() )
		    DBG(cout << "Pushing found variable: " << var->name << '=' << (double)var->get<double>() << " onto exStack" << endl);
		else
		    DBG(cout << "Pushing found variable: " << var->name << " onto exStack" << endl);
		exStack.push(new TokenVar(*var));
		break;
	    }
	    case TokenType::ttVariable:
		var = &dynamic_cast<TokenVar *>(tb)->var;
		if ( var->type->is_integer() )
		    DBG(cout << "Pushing direct variable: " << var->name << '=' << (int)var->get<int>() << " onto exStack" << endl);
		else
		if ( var->type->is_real() )
		    DBG(cout << "Pushing direct variable: " << var->name << '=' << (double)var->get<double>() << " onto exStack" << endl);
		else
		    DBG(cout << "Pushing direct variable: " << var->name << " onto exStack" << endl);
		exStack.push(tb);
		break;
	    case TokenType::ttFunction:
		Throw(tb) << "Got function!" << flush;
		break;
	    case TokenType::ttCallFunc:
		Throw(tb) << "Got call function!" << flush;
		break;
	    case TokenType::ttCallMethod:
		Throw(tb) << "Got call method!" << flush;
		break;
	    case TokenType::ttChar:
	        DBG(cout << "Pushing char: " << (int)tb->get() << " onto exStack" << endl);
		exStack.push(tb);
		break;
	    default:
		DBG(std::cerr << "parseExpression() primary switch throwing token" << std::endl);
		Throw(tb) << "unexpected token type " << (int)opStack.top()->type() << flush;
	}
	if ( done ) { break; /* prevent eating next token */ }
	tb = peekToken();
	if ( tb->id() == TokenID::tkClBrk && !brackets )
	{
	    DBG(cout << "Hit ), no prior brackets, might be end of function?" << endl);
	    break;
	}
	if ( tb->id() == TokenID::tkClSqr )
	{
	    DBG(cout << "Hit ], end of subscript index" << endl);
	    break; // stop without consuming: let the subscript handler consume ]
	}
	if ( tb->id() == TokenID::tkClBrc )
	{
	    DBG(cout << "Hit }, end of expression (initializer or block terminator)" << endl);
	    break; // stop without consuming: caller handles }
	}
	// in conditional mode, stop at ; without consuming it
	// (needed for cast expressions: (TYPE *)expr; must not eat the ;)
	if ( conditional && tb->id() == TokenID::tkSemi )
	    break;
	// in conditional mode, stop at , without consuming — the caller
	// (parseCallFunc, for-init/incr, nested cast) uses the comma as an
	// argument / clause separator and needs it left in the stream.
	// Without this, `strcpy((char *)h + 8, "x")` lets the cast's inner
	// parseExpression eat the comma, and the outer parseExpression then
	// merges `"x"` into the first arg's expression.
	if ( conditional && tb->id() == TokenID::tkComma && !brackets )
	    break;
	tb = nextToken();
    }

    if ( !opStack.empty() )
	DBG(cout << "Emptying operator stack" << endl);

    while ( !opStack.empty() )
	popOperator(opStack, exStack);

    DBG(cout << "parseExpression() exStack size: " << exStack.size() << endl);
    DBG(if ( !exStack.empty() ) std::cout << " exStack.top()->type() = " << (int)exStack.top()->type() << endl);

    DBG(std::cout << "Program::parseExpression() end" << std::endl);

    return exStack.empty() ? NULL : exStack.top();
}

// parse a structure definition
//
// forms:
// struct tag { type member; ... };
// struct { type member; } variable;
// struct tag { type member; } variable;
// struct tag variable;
// typedef struct tag alias;
// typedef struct tag { type member; } alias;
// typedef struct { type member; } alias;
// parse a `namespace NAME { ... }` definition block.
//   namespace std { template<typename T> class vector { ... }; }
// Sets pgm.current_namespace for the duration of the body so definitions
// declared inside (templates, classes, functions) register under the
// namespace, then restores the previous namespace. Body statements that
// yield a node (e.g. a global initializer) are appended to the program.
TokenBase *TokenNAMESPACE::parse(Program &pgm)
{
    DBG(std::cout << "TokenNAMESPACE::parse() top" << std::endl);

    TokenBase *tn = pgm.nextToken();
    if ( !tn || tn->type() != TokenType::ttIdentifier )
	pgm.Throw(tn) << "Expecting namespace name after 'namespace'" << flush;
    std::string ns_name = ((TokenIdent *)tn)->str;

    tn = pgm.nextToken();
    if ( !tn || tn->id() != TokenID::tkOpBrc )
	pgm.Throw(tn) << "Expecting '{' after namespace name" << flush;

    // Nested namespaces concatenate (`namespace a { namespace b { ... } }`
    // → "a::b"); restore the outer namespace on exit.
    std::string saved_namespace = pgm.current_namespace;
    pgm.current_namespace = saved_namespace.empty()
			  ? ns_name : (saved_namespace + "::" + ns_name);

    while ( (tn = pgm.nextToken()) )
    {
	if ( tn->id() == TokenID::tkClBrc )
	    break;
	if ( tn->id() == TokenID::tkSemi )
	    continue;
	TokenBase *stmt = pgm.parseStatement(tn);
	if ( stmt && pgm.tkProgram )
	    pgm.tkProgram->statements.push_back((TokenStmt *)stmt);
    }

    pgm.current_namespace = saved_namespace;
    DBG(std::cout << "TokenNAMESPACE::parse() end of namespace '" << ns_name
	<< "'" << std::endl);
    return NULL;
}

// parse 'using' statement
// forms:
// using namespace std;       — import all members of std into global scope
// using std::cout;            — import single member
TokenBase *TokenUSING::parse(Program &pgm)
{
    TokenBase *tn;

    DBG(std::cout << std::endl << "TokenUSING::parse() top" << std::endl);

    tn = pgm.nextToken();
    if ( !tn )
	pgm.Throw << "Unexpected end of input after 'using'" << flush;

    auto import_namespace_member = [&](const std::string &name, Variable *src)
    {
	std::string alias_name = name;
	if ( pgm.findVariable(alias_name) )
	    return;
	if ( src->name != name )
	{
	    Variable *alias = new Variable();
	    alias->name = name;
	    alias->type = src->type;
	    alias->data = src->data;
	    alias->count = src->count;
	    alias->flags = src->flags;
	    alias->storage_alias_name = src->storage_alias_name.empty()
				      ? src->name : src->storage_alias_name;
	    pgm.tkProgram->variables.push_back(alias);
	    return;
	}
	pgm.tkProgram->variables.push_back(src);
    };
    auto import_namespace_type = [&](const std::string &name, TokenDataType *src)
    {
	if ( !src )
	    return;
	if ( pgm.datatype_map.find(name) != pgm.datatype_map.end() )
	    return;
	pgm.datatype_map[name] = src;
    };

    // using namespace std;
    if ( tn->id() == TokenID::tkNAMESPACE )
    {
	tn = pgm.nextToken(); // namespace name
	if ( !tn || tn->type() != TokenType::ttIdentifier )
	    pgm.Throw(tn) << "Expecting namespace name after 'using namespace'" << flush;
	std::string ns_name = ((TokenIdent *)tn)->str;
	namespace_map_t::iterator nsi = pgm.namespace_map.find(ns_name);
	if ( nsi == pgm.namespace_map.end() )
	    pgm.Throw(tn) << "Unknown namespace '" << ns_name << "'" << flush;
	// import all members into global scope
	for ( variable_map_iter vmi = nsi->second.begin(); vmi != nsi->second.end(); ++vmi )
	{
	    std::string name = vmi->first;
	    // only import if not already defined
	    if ( !pgm.findVariable(name) )
		import_namespace_member(name, vmi->second);
	    DBG(std::cout << "TokenUSING::parse() imported " << ns_name << "::" << name << std::endl);
	}
	namespace_datatype_map_t::iterator nti = pgm.namespace_datatype_map.find(ns_name);
	if ( nti != pgm.namespace_datatype_map.end() )
	    for ( datatype_map_iter dti = nti->second.begin(); dti != nti->second.end(); ++dti )
		import_namespace_type(dti->first, dti->second);
	// expect semicolon
	tn = pgm.nextToken();
	if ( !tn || tn->id() != TokenID::tkSemi )
	    pgm.Throw(tn) << "Expecting ';' after using declaration" << flush;
	return NULL;
    }

    // using std::cout;
    if ( tn->type() == TokenType::ttIdentifier )
    {
	std::string ns_name = ((TokenIdent *)tn)->str;
	tn = pgm.nextToken(); // should be ::
	if ( !tn || tn->id() != TokenID::tkNS )
	    pgm.Throw(tn) << "Expecting '::' in using declaration" << flush;
	tn = pgm.nextToken(); // member name
	if ( !tn || tn->type() != TokenType::ttIdentifier )
	    pgm.Throw(tn) << "Expecting member name in using declaration" << flush;
	std::string member_name = ((TokenIdent *)tn)->str;
	namespace_map_t::iterator nsi = pgm.namespace_map.find(ns_name);
	if ( nsi == pgm.namespace_map.end() )
	    pgm.Throw(tn) << "Unknown namespace '" << ns_name << "'" << flush;
	variable_map_iter vmi = nsi->second.find(member_name);
	namespace_datatype_map_t::iterator nti = pgm.namespace_datatype_map.find(ns_name);
	datatype_map_iter dti = nti == pgm.namespace_datatype_map.end()
	    ? datatype_map_t::iterator()
	    : nti->second.find(member_name);
	if ( vmi == nsi->second.end()
	  && (nti == pgm.namespace_datatype_map.end() || dti == nti->second.end()) )
	    pgm.Throw(tn) << "'" << member_name << "' is not a member of namespace '" << ns_name << "'" << flush;
	// import into global scope / datatype map
	std::string name = member_name;
	if ( vmi != nsi->second.end() && !pgm.findVariable(name) )
	    import_namespace_member(name, vmi->second);
	if ( nti != pgm.namespace_datatype_map.end() && dti != nti->second.end() )
	    import_namespace_type(name, dti->second);
	// expect semicolon
	tn = pgm.nextToken();
	if ( !tn || tn->id() != TokenID::tkSemi )
	    pgm.Throw(tn) << "Expecting ';' after using declaration" << flush;
	return NULL;
    }

    pgm.Throw(tn) << "Unexpected token in using declaration" << flush;
    return NULL;
}

TokenBase *TokenPREFER::parse(Program &pgm)
{
    TokenBase *tn;
    std::vector<std::string> order;

    for (;;)
    {
	tn = pgm.nextToken();
	if ( !tn || tn->type() != TokenType::ttIdentifier )
	    pgm.Throw(tn) << "Expecting namespace name in prefer directive" << flush;
	order.push_back(((TokenIdent *)tn)->str);

	tn = pgm.nextToken();
	if ( !tn )
	    pgm.Throw << "Unexpected end of input in prefer directive" << flush;
	if ( tn->id() == TokenID::tkSemi )
	    break;
	if ( tn->id() != TokenID::tkComma )
	    pgm.Throw(tn) << "Expecting ',' or ';' after namespace name in prefer directive" << flush;
    }

    pgm.set_namespace_preference(order, this);
    return NULL;
}


// parse a structure definition
//
// forms:
// struct tag { type member; ... };
// struct { type member; } variable;
// struct tag { type member; } variable;
// struct tag variable;
// typedef struct tag alias;
// typedef struct tag { type member; } alias;
// typedef struct { type member; } alias;
// Create a tagless `struct {..}` / `union {..}` DataDefSTRUCT with a UNIQUE
// synthetic tag. C anonymous aggregates have no tag, but the CIR/c2mir layer
// needs a real, distinct, referenceable tag per aggregate: sharing the literal
// name "anonymous" made every anonymous struct collide in the by-name dedup
// (emitted_structs / struct_def_points), so the SECOND `typedef struct {...} Y`
// degraded to an incomplete `struct anonymous` reference. A unique `__anon_N`
// name keeps each distinct AND emittable; is_anonymous marks it for the inline
// declarator paths that must NOT forward-reference it across translation.
static DataDefSTRUCT *new_anon_struct(size_t size = 0)
{
    static size_t anon_tag_counter = 0;
    DataDefSTRUCT *dds = new DataDefSTRUCT(
	std::string("__anon_") + std::to_string(++anon_tag_counter), size);
    dds->is_anonymous = true;
    return dds;
}

TokenBase *TokenSTRUCT::parse(Program &pgm)
{
    TokenIdent *tag = NULL;
    TokenBase *tn;
    TokenDataType *tdt;
    bool do_typedef = pgm.parsing_typedef_decl
	|| (pgm.prevToken() ? pgm.prevToken()->id() == TokenID::tkTYPEDEF : false);
    bool is_union = id() == TokenID::tkUNION;
    const char *aggregate_kw = is_union ? "union" : "struct";
    datatype_map_iter bmi; // TokenDataType map
    datadef_map_iter dmi;  // DataDef map

    DBG(std::cout << std::endl << "TokenSTRUCT::parse() top" << std::endl);
    if ( !(tn=pgm.peekToken()) )
	pgm.Throw << "Unexpected end of input" << flush;

    auto scoped_struct_tag = [&](const std::string &name) -> std::string
    {
	TokenCpnd *scope = pgm.compounds.empty() ? NULL : pgm.compounds.top();
	if ( scope && scope != pgm.tkProgram && !pgm.cur_func_name.empty() )
	    return pgm.cur_func_name + "::" + name;
	return name;
    };
    auto find_visible_struct_tag = [&](const std::string &name) -> datadef_map_iter
    {
	std::string scoped = scoped_struct_tag(name);
	if ( scoped != name )
	{
	    datadef_map_iter scoped_it = pgm.struct_map.find(scoped);
	    if ( scoped_it != pgm.struct_map.end() )
		return scoped_it;
	}
	return pgm.struct_map.find(name);
    };

    // Record source-ordered top-level declarations for the CIR backend.
    // Inert for the legacy JIT/MIR paths (the returned nodes' compile()
    // is a no-op and MIR re-parses prog->tokens). record_struct logs a
    // struct/union definition; record_typedef logs a typedef alias.
    auto record_struct = [&](DataDefSTRUCT *sdd, TokenBase *otok = nullptr)
    {
	Program::TopDecl td;
	// Use the recorded struct's OWN layout — a nested union inside a struct
	// (or vice versa) must record its own kind, not the enclosing aggregate's.
	td.kind = sdd->union_layout ? Program::DeclKind::dkUnion : Program::DeclKind::dkStruct;
	td.name = sdd->name;
	td.dd = sdd;
	td.file = TokenBase::_parse_file;
	td.line = TokenBase::_parse_line;
	td.origin = otok;
	pgm.top_decls.push_back(td);
    };
    auto record_typedef = [&](const std::string &alias, DataDef *dd, TokenDataType *tdt_, TokenBase *otok = nullptr, bool defines_body = false)
    {
	pgm.user_typedef_names.insert(alias);
	if ( tdt_ && !pgm.current_namespace.empty() )
	    pgm.namespace_datatype_map[pgm.current_namespace][alias] = tdt_;
	// A typedef inside a function body is block-scoped: the CIR builder emits
	// it in-place from the TokenTypedefDecl in the statement stream (the only
	// correct scope for a VLA typedef whose bound references locals). Only
	// file-scope typedefs are recorded for top-level emission.
	if ( !pgm.compounds.empty() )
	    return;
	Program::TopDecl td;
	td.kind = Program::DeclKind::dkTypedef;
	td.name = alias;
	td.dd = dd;
	td.tdt = tdt_;
	td.file = TokenBase::_parse_file;
	td.line = TokenBase::_parse_line;
	td.origin = otok;
	td.struct_body = defines_body;
	pgm.top_decls.push_back(td);
    };

    // check for __attribute__((packed)) before or after tag
    bool is_packed = false;
    size_t explicit_align = 0;
    bool have_scalar_storage_order = false;
    bool reverse_scalar_storage = false;
    auto consume_attribute = [&]()
    {
	while ( is_attribute_identifier_token(tn) )
	{
	    pgm.nextToken(); // consume __attribute__
	    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrk )
	    {
		pgm.nextToken(); // consume first (
		if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrk )
		{
		    pgm.nextToken(); // consume second (
		    while ( pgm.peekToken() && pgm.peekToken()->id() != TokenID::tkClBrk )
		    {
			TokenBase *attr = pgm.nextToken();
			if ( attr->id() == TokenID::tkComma )
			    continue;
			if ( attr->type() == TokenType::ttIdentifier
			  && ((TokenIdent *)attr)->str == "packed" )
			    is_packed = true;
			else if ( attr->type() == TokenType::ttIdentifier
			       && ((TokenIdent *)attr)->str == "aligned" )
			{
			    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrk )
			    {
				pgm.nextToken();
				if ( pgm.peekToken() && pgm.peekToken()->id() != TokenID::tkClBrk )
				{
				    int64_t aval = parse_constant_integer_expression(pgm);
				    if ( aval > 0 )
					explicit_align = (size_t)aval;
				}
				if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkClBrk )
				    pgm.nextToken();
			    }
			}
			else if ( attr->type() == TokenType::ttIdentifier
			       && ((TokenIdent *)attr)->str == "scalar_storage_order" )
			{
			    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrk )
			    {
				pgm.nextToken();
				TokenBase *order_tb = pgm.nextToken();
				if ( order_tb && order_tb->type() == TokenType::ttString )
				{
				    have_scalar_storage_order = true;
				    reverse_scalar_storage =
					reverse_scalar_storage_requested(((TokenStr *)order_tb)->str);
				}
				if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkClBrk )
				    pgm.nextToken();
			    }
			}
			else if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrk )
			{
			    int attr_depth = 0;
			    do {
				TokenBase *skip = pgm.nextToken();
				if ( !skip ) break;
				if ( skip->id() == TokenID::tkOpBrk ) ++attr_depth;
				else if ( skip->id() == TokenID::tkClBrk ) --attr_depth;
			    } while ( attr_depth > 0 );
			}
		    }
		    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkClBrk )
			pgm.nextToken(); // consume first )
		    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkClBrk )
			pgm.nextToken(); // consume second )
		}
	    }
	    tn = pgm.peekToken();
	}
    };

    // __attribute__ can appear before the tag name
    consume_attribute();

    // optional struct tag name
    if ( is_contextual_identifier_token(tn) )
    {
	TokenBase *tag_tb = pgm.nextToken(); // consume tag
	std::string tag_name = contextual_identifier_name(tag_tb);
	tag = new TokenIdent(tag_name);
	tag->file = tag_tb->file;
	tag->line = tag_tb->line;
	tag->column = tag_tb->column;
	DBG(cout << "TokenSTRUCT::parse() got tag " << tag->str << endl);
	tn = pgm.peekToken(); // peek at what follows the tag
	if ( !tn )
	    pgm.Throw << "Unexpected end of input after " << aggregate_kw << " tag" << flush;
    }

    // __attribute__ can also appear after the tag name
    consume_attribute();

    // if no brace, then this structure type must already be defined
    // and we are either doing a typedef, or a variable declaration
    if ( tn->id() != TokenID::tkOpBrc )
    {
	if ( !tag )
	    pgm.Throw(tn) << "Expecting '{' or identifier after " << aggregate_kw << flush;
	dmi = find_visible_struct_tag(tag->str);

	// plain forward declaration: struct tag;
	if ( tn->id() == TokenID::tkSemi )
	{
	    DataDefSTRUCT *fwd;
	    if ( dmi == pgm.struct_map.end() )
	    {
		fwd = new DataDefSTRUCT(tag->str, 0);
		fwd->union_layout = is_union;
		pgm.struct_map[scoped_struct_tag(tag->str)] = fwd;
	    }
	    else
		fwd = static_cast<DataDefSTRUCT *>(dmi->second);
	    pgm.nextToken(); // consume ';'
	    DBG(cout << "TokenSTRUCT::parse() forward declaration of struct " << tag->str << endl);
	    // Do NOT record a pure forward declaration into top_decls: an
	    // incomplete struct emits nothing, but its position would anchor the
	    // emission order at the FORWARD site (top_decls emits each tag at its
	    // first occurrence). The full definition — which DOES record_struct at
	    // its own, later position — must drive emission order so a member whose
	    // type is defined between the forward decl and the definition is laid
	    // out first (pr41463: `union tree_node;` ... `struct other_tree{}` ...
	    // `union tree_node { struct other_tree othr; }`). An undefined-only tag
	    // is a valid incomplete type for pointer use and needs no top_decl.
	    return new TokenStructDef(fwd, is_union);
	}

	// forward typedef: typedef struct tag_name alias; (struct not yet defined)
	if ( dmi == pgm.struct_map.end() )
	{
	    // create placeholder struct (size 0, no members) for forward declaration
	    DataDefSTRUCT *fwd = new DataDefSTRUCT(tag->str, 0);
	    fwd->union_layout = is_union;
	    pgm.struct_map[scoped_struct_tag(tag->str)] = fwd;
	    dmi = find_visible_struct_tag(tag->str);
	    DBG(cout << "TokenSTRUCT::parse() forward declaration of struct " << tag->str << endl);
	}
	// typedef struct tag alias
	if ( do_typedef )
	{
	    DataDef *alias_dd = dmi->second;
	    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkMul )
	    {
		pgm.nextToken();
		alias_dd = pgm.getPointerType(alias_dd);
	    }
	    tn = pgm.nextToken(); // consume the alias identifier
	    if ( !is_contextual_identifier_token(tn) )
		pgm.Throw(tn) << "Expecting identifier after struct tag in typedef" << flush;
	    std::string alias_name = contextual_identifier_name(tn);
	    alias_dd = parse_typedef_array_suffix(pgm, alias_dd, alias_name, tn);
	    if ( (bmi=pgm.datatype_map.find(alias_name)) != pgm.datatype_map.end() )
	    {
		// C allows identical typedef redeclarations — silently accept
		// when the alias maps to the same underlying type.
		DataDef *existing = &bmi->second->definition;
		if ( existing == alias_dd
		  || (existing->is_pointer() && alias_dd->is_pointer()
		      && existing->rawtype() == alias_dd->rawtype()) )
		{
		    // consume trailing ';' and return
		    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
			pgm.nextToken();
		    return NULL;
		}
		pgm.Throw(tn) << "Identifier already defined" << flush;
	    }
	    tdt = new TokenDataType(alias_name.c_str(), *alias_dd);
	    pgm.datatype_map[alias_name] = tdt;
	    // also register tag in struct_map so "struct tag" works
	    if ( !alias_dd->is_pointer() )
		pgm.struct_map[alias_name] = dmi->second;
	    record_typedef(alias_name, alias_dd, tdt, tn);
	    return new TokenTypedefDecl(alias_name, alias_dd);
	}

	// struct tag variable; — declare variable of existing struct type
	string tname(aggregate_kw);
	tname.append(" ");
	tname.append(tag->str);
	tdt = new TokenDataType(tname.c_str(), *dmi->second);
	return pgm.parseDeclaration(tdt);
    }

    // ---- defining a new structure: struct [tag] { type member; ... } ----

    pgm.nextToken(); // consume '{'

    DataDefSTRUCT *dds = tag ? new DataDefSTRUCT(tag->str, 0) : new_anon_struct();
    std::string tag_store_key = tag ? tag->str : "";
    if ( tag )
    {
	std::string scoped = scoped_struct_tag(tag->str);
	if ( scoped != tag->str && pgm.struct_map.find(tag->str) != pgm.struct_map.end() )
	    tag_store_key = scoped;
    }
    dds->union_layout = is_union;
    if ( is_packed || pgm.pack_stack_top() == 1 )
	dds->pack = 1;
    else if ( pgm.pack_stack_top() > 0 )
	dds->pack = pgm.pack_stack_top();
    if ( have_scalar_storage_order )
	dds->setReverseScalarStorage(reverse_scalar_storage);
    if ( explicit_align > dds->max_align )
	dds->max_align = explicit_align;
    if ( explicit_align > dds->tag_explicit_align )
	dds->tag_explicit_align = explicit_align;	// __attribute__((aligned(N))) on the tag
    DBG(cout << "TokenSTRUCT::parse() defining struct " << dds->name << endl);

    auto parse_bitfield_width = [&](TokenBase *loc, DataDef *member_dd, bool named) -> size_t
    {
	if ( !member_dd || member_dd->is_pointer() || !member_dd->is_integer() )
	    pgm.Throw(loc) << "Bit-field type must be an integer type" << flush;
	int64_t width = parse_constant_integer_expression(pgm);
	if ( width < 0 )
	    pgm.Throw(loc) << "Bit-field width must be non-negative" << flush;
	if ( named && width == 0 )
	    pgm.Throw(loc) << "Named bit-field width must be positive" << flush;
	size_t storage_bits = dds->bitfield_storage_size(*member_dd) * 8;
	if ( (size_t)width > storage_bits )
	    pgm.Throw(loc) << "Bit-field width exceeds storage type width" << flush;
	return (size_t)width;
    };

    // Pre-register the tag (if any) before parsing the body so fields like
    // `struct hashstr_data *next;` inside `struct hashstr_data { ... }` can
    // resolve the self-reference. The struct is treated as "incomplete" at
    // this point (size 0 members none); pointer-to-incomplete works because
    // DataDefPTR only needs an 8-byte pointer size.
    bool was_pre_registered = false;
    if ( tag && pgm.struct_map.find(tag_store_key) == pgm.struct_map.end() )
    {
	pgm.struct_map[tag_store_key] = dds;
	was_pre_registered = true;
	DBG(cout << "TokenSTRUCT::parse() pre-registered " << tag_store_key << " for self-reference" << endl);
    }

    while ( (tn=pgm.peekToken()) && tn->id() != TokenID::tkClBrc )
    {
	while ( is_attribute_identifier_token(tn) )
	{
	    tn = consume_gnu_attributes(pgm, pgm.nextToken());
	    if ( tn )
		pgm.pushToken(tn);
	    tn = pgm.peekToken();
	}
	while ( tn && (tn->id() == TokenID::tkCONST
	            || tn->id() == TokenID::tkVOLATILE) )
	{
	    pgm.nextToken(); // consume qualifier
	    tn = pgm.peekToken();
	}

	// expect a data type token (or typedef'd identifier, or 'struct Tag')
	TokenDataType *mtype = NULL;
	if ( tn->type() == TokenType::ttDataType )
	    mtype = (TokenDataType *)pgm.nextToken();
	else if ( tn->type() == TokenType::ttIdentifier )
	{
	    std::string tname = ((TokenIdent *)tn)->str;
	    datatype_map_iter tdmi = pgm.datatype_map.find(tname);
	    if ( tdmi != pgm.datatype_map.end() )
	    {
		pgm.nextToken(); // consume the identifier
		mtype = tdmi->second;
	    }
	    else
		pgm.Throw(tn) << "Expecting type in struct definition, got '" << tname << "'" << flush;
	}
	else if ( tn->id() == TokenID::tkSTRUCT || tn->id() == TokenID::tkUNION )
	{
	    bool nested_union_kw = tn->id() == TokenID::tkUNION;
	    pgm.nextToken(); // consume 'struct' / 'union'
	    auto consume_nested_attributes = [&]() -> bool
	    {
		bool packed = false;
		while ( is_attribute_identifier_token(pgm.peekToken()) )
		{
		    pgm.nextToken();
		    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrk )
		    {
			int depth = 0;
			do {
			    TokenBase *at = pgm.nextToken();
			    if ( !at ) break;
			    if ( at->id() == TokenID::tkOpBrk ) ++depth;
			    else if ( at->id() == TokenID::tkClBrk ) --depth;
			    else if ( at->type() == TokenType::ttIdentifier
				   && ((TokenIdent *)at)->str == "packed" )
				packed = true;
			} while ( depth > 0 );
		    }
		}
		return packed;
	    };
	    bool nested_packed = consume_nested_attributes();
	    TokenBase *stag = pgm.peekToken();
	    std::function<void(DataDefSTRUCT *, TokenBase *)> parse_nested_aggregate_body;
	    parse_nested_aggregate_body = [&](DataDefSTRUCT *inner, TokenBase *loc) -> void
	    {
		while ( (tn = pgm.peekToken()) && tn->id() != TokenID::tkClBrc )
		{
		    while ( is_attribute_identifier_token(tn) )
		    {
			tn = consume_gnu_attributes(pgm, pgm.nextToken());
			if ( tn )
			    pgm.pushToken(tn);
			tn = pgm.peekToken();
		    }
		    TokenDataType *inner_type = NULL;
		    if ( tn->type() == TokenType::ttDataType )
			inner_type = (TokenDataType *)pgm.nextToken();
		    else if ( tn->type() == TokenType::ttIdentifier )
		    {
			std::string tname = ((TokenIdent *)tn)->str;
			datatype_map_iter tdmi = pgm.datatype_map.find(tname);
			if ( tdmi == pgm.datatype_map.end() )
			    pgm.Throw(tn) << "Expecting type in anonymous struct definition, got '" << tname << "'" << flush;
			pgm.nextToken();
			inner_type = tdmi->second;
		    }
		    else if ( tn->id() == TokenID::tkSTRUCT || tn->id() == TokenID::tkUNION )
		    {
			bool inner_union_kw = tn->id() == TokenID::tkUNION;
			pgm.nextToken();
			bool inner_packed = consume_nested_attributes();
			TokenBase *inner_tag = pgm.peekToken();
			if ( inner_tag && inner_tag->id() == TokenID::tkOpBrc )
			{
			    pgm.nextToken();
			    DataDefSTRUCT *nested = new_anon_struct();
			    nested->union_layout = inner_union_kw;
			    if ( inner_packed )
				nested->pack = 1;
			    parse_nested_aggregate_body(nested, inner_tag);
			    inner_type = new TokenDataType("anonymous", *nested);
			}
			else
			{
			    inner_tag = pgm.nextToken();
			    if ( !inner_tag || inner_tag->type() != TokenType::ttIdentifier )
				pgm.Throw(inner_tag ? inner_tag : tn) << "Expecting struct name" << flush;
			    std::string sname = ((TokenIdent *)inner_tag)->str;
			    datadef_map_iter sdmi = pgm.struct_map.find(sname);
			    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrc )
			    {
				DataDefSTRUCT *nested = NULL;
				if ( sdmi == pgm.struct_map.end() )
				{
				    nested = new DataDefSTRUCT(sname, 0);
				    if ( inner_packed )
					nested->pack = 1;
				    pgm.struct_map[sname] = nested;
				}
				else
				{
				    nested = static_cast<DataDefSTRUCT *>(sdmi->second);
				    if ( nested->size != 0 || !nested->members.empty() )
					pgm.Throw(inner_tag) << "Struct '" << sname << "' already defined" << flush;
				}
				nested->union_layout = inner_union_kw;
				if ( inner_packed )
				    nested->pack = 1;
				pgm.nextToken();
				parse_nested_aggregate_body(nested, inner_tag);
				// Named inline-defined nested struct/union — visible at enclosing
				// scope in C; record as a standalone top-level def (see outer
				// member loop). Handles deeper nesting (`struct A{struct B{struct C
				// {..}c;}b;}a;`).
				record_struct(nested, inner_tag);
				inner_type = new TokenDataType(sname.c_str(), *nested);
			    }
			    else
			    {
				if ( sdmi == pgm.struct_map.end() )
				{
				    DataDefSTRUCT *fwd = new DataDefSTRUCT(sname, 0);
				    fwd->union_layout = inner_union_kw;
				    pgm.struct_map[sname] = fwd;
				    sdmi = pgm.struct_map.find(sname);
				}
				inner_type = new TokenDataType(sname.c_str(), *sdmi->second);
			    }
			}
		    }
		    else if ( tn->id() == TokenID::tkENUM )
		    {
			pgm.nextToken();
			if ( pgm.peekToken() && is_contextual_identifier_token(pgm.peekToken()) )
			    pgm.nextToken();
			inner_type = new TokenDataType("enum", ddUINT32);
		    }
		    else
			pgm.Throw(tn) << "Expecting type in anonymous struct definition" << flush;

		    DataDef *inner_member_dd = &inner_type->definition;
		    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkMul )
		    {
			pgm.nextToken();
			inner_member_dd = pgm.getPointerType(inner_member_dd);
		    }

		    // Check for unnamed bitfield: `int : 4;`
		    tn = pgm.nextToken();
		    if ( tn && tn->id() == TokenID::tkSemi )
		    {
			if ( DataDefSTRUCT *anon = dynamic_cast<DataDefSTRUCT *>(inner_member_dd) )
			{
			    inner->addAnonymousAggregate(*anon);
			    continue;
			}
			pgm.Throw(tn) << "Expecting member name in anonymous struct definition" << flush;
		    }
		    if ( tn && tn->id() == TokenID::tkColon )
		    {
			size_t bit_width = parse_bitfield_width(tn, inner_member_dd, false);
			inner->addUnnamedBitField(*inner_member_dd, bit_width);
			tn = pgm.nextToken();
			if ( !tn || (tn->id() != TokenID::tkSemi && tn->id() != TokenID::tkComma) )
			    pgm.Throw(tn ? tn : loc) << "Expecting ';' after unnamed bit-field" << flush;
			if ( tn->id() == TokenID::tkComma )
			    continue;
			// semicolon consumed — fall through to done_members check
		    }
		    else
		    {
		    if ( !is_contextual_identifier_token(tn) )
			pgm.Throw(tn) << "Expecting member name in anonymous struct definition" << flush;
		    std::string inner_name = contextual_identifier_name(tn);

		    size_t inner_count = 1;
		    TokenBase *inner_count_expr = NULL;
		    bool inner_is_array_decl = false;
		    // Per-dimension shape, mirroring the top-level struct-member path's
		    // member_dims: needed so a trailing flexible (`T x[]`) or GNU
		    // zero-length (`T x[0]`) array member keeps its 0 dimension
		    // (m_is_flexible_array keys on member_dims) instead of collapsing
		    // to a scalar.
		    std::vector<uint32_t> inner_dims;
		    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpSqr )
		    {
			inner_is_array_decl = true;
			pgm.nextToken();
			TokenBase *cl = pgm.nextToken();
			if ( cl && cl->id() == TokenID::tkClSqr )
			{
			    inner_dims.push_back(0);
			    inner_count = 0;
			    break;
			}
			pgm.pushToken(cl);
			if ( !inner_count_expr && bracket_dim_needs_runtime_value(pgm) )
			{
			    inner_count_expr = pgm.parseExpression(pgm.nextToken(), true);
			    cl = pgm.nextToken();
			    if ( !cl || cl->id() != TokenID::tkClSqr )
				pgm.Throw(cl ? cl : tn) << "Expected ']' in anonymous struct member array declaration" << flush;
			    continue;
			}
			int64_t n = parse_constant_integer_expression(pgm);
			if ( n < 0 )
			    pgm.Throw(tn) << "Fixed-size array dimension must be non-negative" << flush;
			cl = pgm.nextToken();
			if ( !cl || cl->id() != TokenID::tkClSqr )
			    pgm.Throw(cl ? cl : tn) << "Expected ']' in anonymous struct member array declaration" << flush;
			inner_dims.push_back((uint32_t)n);
			if ( n == 0 ) { inner_count = 0; break; }
			inner_count *= (size_t)n;
		    }

		    // Check for named bitfield: `int x : 4;`
		    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkColon )
		    {
			pgm.nextToken();
			if ( inner_count != 1 || inner_count_expr )
			    pgm.Throw(tn) << "Bit-field member cannot be an array" << flush;
			size_t bit_width = parse_bitfield_width(tn, inner_member_dd, true);
			inner->addBitField(inner_name, *inner_member_dd, bit_width);
		    }
		    else
		    {
			inner->addMember(inner_name, *inner_member_dd, inner_count,
			    inner_count_expr, inner_is_array_decl, &inner_dims);
		    }
		    tn = pgm.nextToken();
		    // Handle comma-separated members: `int f1, f2, f3;`
		    while ( tn && tn->id() == TokenID::tkComma )
		    {
			tn = pgm.nextToken();
			// pointer stars for this declarator
			DataDef *comma_dd = inner_member_dd;
			while ( tn && tn->id() == TokenID::tkMul )
			{
			    comma_dd = pgm.getPointerType(comma_dd);
			    tn = pgm.nextToken();
			}
			if ( !is_contextual_identifier_token(tn) )
			    pgm.Throw(tn ? tn : loc) << "Expecting member name after ',' in anonymous struct" << flush;
			std::string cname = contextual_identifier_name(tn);
			size_t ccount = 1;
			TokenBase *ccount_expr = NULL;
			while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpSqr )
			{
			    pgm.nextToken();
			    TokenBase *cl = pgm.nextToken();
			    if ( cl && cl->id() == TokenID::tkClSqr ) { ccount = 0; break; }
			    pgm.pushToken(cl);
			    if ( !ccount_expr && bracket_dim_needs_runtime_value(pgm) )
			    {
				ccount_expr = pgm.parseExpression(pgm.nextToken(), true);
				cl = pgm.nextToken();
				if ( !cl || cl->id() != TokenID::tkClSqr )
				    pgm.Throw(cl ? cl : tn) << "Expected ']'" << flush;
				continue;
			    }
			    int64_t n = parse_constant_integer_expression(pgm);
			    cl = pgm.nextToken();
			    if ( !cl || cl->id() != TokenID::tkClSqr )
				pgm.Throw(cl ? cl : tn) << "Expected ']'" << flush;
			    if ( n <= 0 ) { ccount = 0; break; }
			    ccount *= (size_t)n;
			}
			if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkColon )
			{
			    pgm.nextToken();
			    if ( ccount_expr )
				pgm.Throw(tn) << "Bit-field member cannot be an array" << flush;
			    size_t bw = parse_bitfield_width(tn, comma_dd, true);
			    inner->addBitField(cname, *comma_dd, bw);
			}
			else
			    inner->addMember(cname, *comma_dd, ccount,
				ccount_expr, ccount_expr != NULL || ccount != 1);
			tn = pgm.nextToken();
		    }
		    if ( !tn || tn->id() != TokenID::tkSemi )
			pgm.Throw(tn ? tn : loc) << "Expecting ';' after anonymous struct member" << flush;
		    } // end named member branch
		}
		if ( !tn || tn->id() != TokenID::tkClBrc )
		    pgm.Throw(tn ? tn : loc) << "Unexpected end of input in anonymous struct definition" << flush;
		pgm.nextToken(); // consume '}'
		inner->is_complete = true; // a `{ ... }` body was parsed
		inner->finalize();
	    };
	    if ( stag && stag->id() == TokenID::tkOpBrc )
	    {
		// Support anonymous nested struct members like:
		//   struct { int x; char y[8]; } member;
		pgm.nextToken(); // consume '{'
		DataDefSTRUCT *inner = new_anon_struct();
		inner->union_layout = nested_union_kw;
		if ( nested_packed )
		    inner->pack = 1;
		parse_nested_aggregate_body(inner, stag);
		mtype = new TokenDataType("anonymous", *inner);
	    }
	    else
	    {
		stag = pgm.nextToken();
		if ( stag->type() != TokenType::ttIdentifier )
		    pgm.Throw(stag) << "Expecting struct name" << flush;
		std::string sname = ((TokenIdent *)stag)->str;
		datadef_map_iter sdmi = pgm.struct_map.find(sname);
		if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrc )
		{
		    DataDefSTRUCT *inner = NULL;
		    if ( sdmi == pgm.struct_map.end() )
		    {
			inner = new DataDefSTRUCT(sname, 0);
			inner->union_layout = nested_union_kw;
			pgm.struct_map[sname] = inner;
		    }
		    else
		    {
			inner = static_cast<DataDefSTRUCT *>(sdmi->second);
			if ( inner->size != 0 || !inner->members.empty() )
			    pgm.Throw(stag) << "Struct '" << sname << "' already defined" << flush;
			inner->union_layout = nested_union_kw;
		    }
		    pgm.nextToken(); // consume '{'
		    parse_nested_aggregate_body(inner, stag);
		    // A named struct/union defined INLINE as a member type has C scope
		    // at the ENCLOSING level (`struct T` in `struct S { struct T {} t; }`
		    // is visible outside S). Record it as a standalone top-level def so
		    // the CIR builder emits its body before the enclosing struct uses it;
		    // otherwise the member's `struct T` tag stays incomplete. Pushed before
		    // the enclosing struct's own record_struct (runs after the full body),
		    // so it is emitted first.
		    record_struct(inner, stag);
		    mtype = new TokenDataType(sname.c_str(), *inner);
		}
		else
		{
		    if ( sdmi == pgm.struct_map.end() )
		    {
			DataDefSTRUCT *fwd = new DataDefSTRUCT(sname, 0);
			fwd->union_layout = nested_union_kw;
			pgm.struct_map[sname] = fwd;
			sdmi = pgm.struct_map.find(sname);
		    }
		    mtype = new TokenDataType(sname.c_str(), *sdmi->second);
		}
	    }
	}
	else if ( tn->id() == TokenID::tkENUM )
	{
	    pgm.nextToken();
	    if ( pgm.peekToken() && is_contextual_identifier_token(pgm.peekToken()) )
		pgm.nextToken();
	    mtype = new TokenDataType("enum", ddUINT32);
	}
	else
	    pgm.Throw(tn) << "Expecting type in struct definition" << flush;

		DataDef *base_member_dd = &mtype->definition;
		// Member declared via a user typedef alias (not a builtin or
		// "struct tag"): record it so CIR emits ID("alias") for this
		// member, matching c2m's node_t tree. Shared by every declarator
		// on this line (`sh_int a, b;`).
		std::string member_typedef_alias;
		if ( pgm.user_typedef_names.count(mtype->str) )
		    member_typedef_alias = mtype->str;
		// skip const/restrict qualifiers between type and pointer stars
		// e.g. `char const *p;`
		while ( pgm.peekToken() && (pgm.peekToken()->id() == TokenID::tkCONST
			|| pgm.peekToken()->id() == TokenID::tkVOLATILE
			|| pgm.peekToken()->id() == TokenID::tkRESTRICT) )
		    pgm.nextToken();
		// consume __attribute__((...)) after type — extract aligned(N)
		// for struct member alignment (e.g. `int __attribute__((aligned(8))) a;`)
		size_t member_align = 0;
		while ( is_attribute_identifier_token(pgm.peekToken()) )
		{
		    TokenBase *after = consume_gnu_attributes(pgm, pgm.nextToken(), NULL, NULL, &member_align);
		    if ( after )
			pgm.pushToken(after);
		}
		bool done_members = false;
		while ( !done_members )
		{
		    // Each declarator on the line can carry its own pointer stars:
		    // `int *a, b;` or `struct foo *next, *prev;`.
		    DataDef *member_dd = base_member_dd;
		    bool mem_fnptr_base = (dynamic_cast<DataDefFPTR *>(member_dd) != NULL);
		    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkMul )
		    {
			pgm.nextToken(); // consume '*'
			if ( !mem_fnptr_base )
			    member_dd = pgm.getPointerType(member_dd);
		    }

		    // skip trailing const/restrict qualifiers (e.g. `char const *p;`)
		    while ( pgm.peekToken() && (pgm.peekToken()->id() == TokenID::tkCONST
			    || pgm.peekToken()->id() == TokenID::tkVOLATILE
			    || pgm.peekToken()->id() == TokenID::tkRESTRICT) )
			pgm.nextToken();

		    // expect member name
		    tn = pgm.nextToken();
		    if ( tn && tn->id() == TokenID::tkSemi )
		    {
			if ( DataDefSTRUCT *anon = dynamic_cast<DataDefSTRUCT *>(member_dd) )
			{
			    dds->addAnonymousAggregate(*anon);
			    done_members = true;
			    continue;
			}
			pgm.Throw(tn) << "Expecting member name in struct definition" << flush;
		    }
		    if ( tn && tn->id() == TokenID::tkColon )
		    {
			size_t bit_width = parse_bitfield_width(tn, member_dd, false);
			dds->addUnnamedBitField(*member_dd, bit_width);
			tn = pgm.nextToken();
			if ( !tn )
			    pgm.Throw << "Unexpected end of input after unnamed bit-field" << flush;
			if ( tn->id() == TokenID::tkComma )
			    continue;
			if ( tn->id() != TokenID::tkSemi )
			    pgm.Throw(tn) << "Expecting ';' after unnamed bit-field" << flush;
			done_members = true;
			continue;
		    }
		    if ( tn && tn->id() == TokenID::tkOpBrk )
		    {
			TokenBase *inner = pgm.nextToken();
			if ( inner && inner->id() == TokenID::tkStar )
			{
			    // Typed function-pointer member, e.g. `void (*callback)(void *)`
			    // or `char *(*resolver)(int)`.
			    tn = pgm.nextToken();
			    if ( !is_contextual_identifier_token(tn) )
				pgm.Throw(tn) << "Expecting member name in function pointer struct declarator" << flush;
			    std::string mname = contextual_identifier_name(tn);
			    tn = pgm.nextToken();
			    if ( !tn || tn->id() != TokenID::tkClBrk )
				pgm.Throw(tn ? tn : inner) << "Expected ')' after function pointer member name" << flush;
			    tn = pgm.nextToken();
			    if ( !tn || tn->id() != TokenID::tkOpBrk )
				pgm.Throw(tn ? tn : inner) << "Expected '(' after function pointer member name" << flush;
			    FuncDef *func = pgm.parseFnPtrParams(*member_dd);
			    DataDefFPTR *fptr_type = new DataDefFPTR(func);
			    member_dd = fptr_type;

			    dds->addMember(mname, *member_dd, 1);
			    if ( !member_typedef_alias.empty() && !dds->members.empty() )
				dds->members.back().typedef_name = member_typedef_alias;
			    DBG(cout << "TokenSTRUCT::parse() added function pointer member " << mname
				<< " (size " << member_dd->size << ", total " << dds->size << ')' << endl);

			    tn = pgm.nextToken();
			    if ( !tn )
				pgm.Throw(inner) << "Unexpected end of input after function pointer struct member" << flush;
			    if ( tn->id() == TokenID::tkComma )
				continue;
			    if ( tn->id() != TokenID::tkSemi )
				pgm.Throw(tn) << "Expecting ';' after function pointer struct member" << flush;
			    done_members = true;
			    continue;
			}
			pgm.Throw(inner ? inner : tn) << "Unsupported parenthesized member declarator in struct definition" << flush;
		    }
		    if ( !is_contextual_identifier_token(tn) )
			pgm.Throw(tn) << "Expecting member name in struct definition" << flush;
		    std::string mname = contextual_identifier_name(tn);
		    TokenBase *member_name_tok = tn;  // CIR origin for this member

		    // Optional fixed-array dimensions: `char d_name[256];`, `int m[4][8];`.
		    // Multiply the dimensions into a single count so the member reserves
		    // N*sizeof(base) bytes inline. Access via `&obj.member` yields a pointer
		    // to the start of the inline buffer.
		    size_t member_count = 1;
		    bool member_is_array_decl = false;
		    TokenBase *member_count_expr = NULL;
		    std::vector<uint32_t> member_dims;
		    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpSqr )
		    {
			member_is_array_decl = true;
			pgm.nextToken(); // consume '['
			TokenBase *cl = pgm.nextToken();
			if ( cl && cl->id() == TokenID::tkClSqr )
			{
			    member_count = 0;
			    member_dims.push_back(0);
			    break;
			}
			pgm.pushToken(cl);
			if ( !member_count_expr && bracket_dim_needs_runtime_value(pgm) )
			{
			    member_count_expr = pgm.parseExpression(pgm.nextToken(), true);
			    cl = pgm.nextToken();
			    if ( !cl || cl->id() != TokenID::tkClSqr )
				pgm.Throw(cl ? cl : tn) << "Expected ']' in struct member array declaration" << flush;
			    continue;
			}
			int64_t n = parse_constant_integer_expression(pgm);
			if ( n < 0 )
			    pgm.Throw(tn) << "Fixed-size array dimension must be non-negative" << flush;
			cl = pgm.nextToken();
			if ( !cl || cl->id() != TokenID::tkClSqr )
			    pgm.Throw(cl ? cl : tn) << "Expected ']' in struct member array declaration" << flush;
			member_dims.push_back((uint32_t)n);
			if ( n == 0 ) { member_count = 0; break; }
			member_count *= (size_t)n;
		    }

		    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkColon )
		    {
			pgm.nextToken();
			if ( member_count != 1 || member_count_expr )
			    pgm.Throw(tn) << "Bit-field member cannot be an array" << flush;
			size_t bit_width = parse_bitfield_width(tn, member_dd, true);
			dds->addBitField(mname, *member_dd, bit_width);
			DBG(cout << "TokenSTRUCT::parse() added bit-field " << member_dd->name << ' ' << mname
			    << ':' << bit_width << " (storage offset " << dds->member_offsets.back()
			    << ", total " << dds->size << ')' << endl);
		    }
		    else
		    {
			dds->addMember(mname, *member_dd, member_count,
			    member_count_expr, member_is_array_decl, &member_dims);
			if ( !dds->members.empty() )
			{
			    if ( !member_typedef_alias.empty() )
				dds->members.back().typedef_name = member_typedef_alias;
			    dds->members.back().origin = member_name_tok;
			}
			if ( member_align > 0 )
			    dds->apply_member_alignment(member_align);
			DBG(cout << "TokenSTRUCT::parse() added member " << member_dd->name << ' ' << mname
			    << " (size " << member_dd->size << " x " << member_count
			    << ", total " << dds->size << ')' << endl);
		    }

		    tn = pgm.nextToken();
		    if ( !tn )
			pgm.Throw << "Unexpected end of input in struct definition" << flush;
		    // Skip __attribute__((...)) on struct members
		    if ( is_attribute_identifier_token(tn) )
		    {
			if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrk )
			{
			    int adepth = 0;
			    do {
				TokenBase *at = pgm.nextToken();
				if ( !at ) break;
				if ( at->id() == TokenID::tkOpBrk ) ++adepth;
				else if ( at->id() == TokenID::tkClBrk ) --adepth;
			    } while ( adepth > 0 );
			}
			tn = pgm.nextToken();
			if ( !tn )
			    pgm.Throw << "Unexpected end after __attribute__ in struct" << flush;
		    }
		    if ( tn->id() == TokenID::tkComma )
			continue;
		    if ( tn->id() != TokenID::tkSemi )
			pgm.Throw(tn) << "Expecting ';' after struct member" << flush;
		    done_members = true;
		}
	    }

    if ( !tn )
	pgm.Throw << "Unexpected end of input in struct definition" << flush;
    pgm.nextToken(); // consume '}'

    tn = pgm.peekToken();
    consume_attribute();
    if ( have_scalar_storage_order )
	dds->setReverseScalarStorage(reverse_scalar_storage);
    if ( is_packed )
	dds->pack = 1;

    bool has_bitfields = false;
    for ( size_t bi = 0; bi < dds->member_bitfields.size(); ++bi )
	if ( dds->member_bitfields[bi].is_bitfield )
	    has_bitfields = true;
    if ( is_packed && !has_bitfields )
    {
	dds->size = 0;
	dds->max_align = 1;
	for ( size_t mi = 0; mi < dds->members.size(); ++mi )
	{
	    DataDef *mdd = dds->members[mi].second;
	    size_t cnt = (mi < dds->member_counts.size()) ? dds->member_counts[mi] : 1;
	    TokenBase *count_expr =
		(mi < dds->member_count_exprs.size()) ? dds->member_count_exprs[mi] : NULL;
	    size_t fa = dds->field_align(*mdd);
	    if ( dds->union_layout )
	    {
		if ( fa > dds->max_align ) dds->max_align = fa;
		if ( mi < dds->member_offsets.size() )
		    dds->member_offsets[mi] = 0;
		size_t member_size = count_expr ? 0 : (mdd->size * cnt);
		if ( member_size > dds->size ) dds->size = member_size;
	    }
	    else
	    {
		dds->size = DataDefSTRUCT::align_up(dds->size, fa);
		if ( fa > dds->max_align ) dds->max_align = fa;
		if ( mi < dds->member_offsets.size() )
		    dds->member_offsets[mi] = dds->size;
		if ( !count_expr )
		    dds->size += mdd->size * cnt;
	    }
	}
    }
    if ( explicit_align > dds->max_align )
	dds->max_align = explicit_align;
    if ( explicit_align > dds->tag_explicit_align )
	dds->tag_explicit_align = explicit_align;	// __attribute__((aligned(N))) on the tag
    dds->is_complete = true; // a `{ ... }` body was parsed (even if it had no members)
    dds->finalize(); // round up size to struct alignment

    // A struct that contains an OBJECT member by value (a std::string or another
    // object) is — per C++ — a NON-TRIVIAL class: those members must be
    // constructed/destructed. The keyword does not decide struct-vs-class; the
    // CONTENTS do (a struct IS a class; it earns class-hood from an object
    // feature). Promote the parsed DataDefSTRUCT to a DataDefCLASS (which IS-A
    // DataDefSTRUCT) so it flows through the existing class machinery (member
    // ctors/dtors) exactly like a user class with such a member. A struct with NO
    // object members stays a plain DataDefSTRUCT — unchanged, zero cost.
    {
	bool has_object_member = false;
	for ( auto &m : dds->members )
	    if ( m.second && !m.second->is_pointer() && m.second->is_object() ) {
		has_object_member = true;	// std::string is an object too (is_object())
		break;
	    }
	if ( has_object_member && !dynamic_cast<DataDefCLASS *>(dds) ) {
	    DataDefCLASS *ddc = new DataDefCLASS(dds->name, dds->size, dds->rawtype());
	    static_cast<DataDefSTRUCT &>(*ddc) = *dds; // copy the parsed struct state
	    if ( was_pre_registered && tag )
		pgm.struct_map[tag_store_key] = ddc;   // repoint the self-ref pre-registration
	    // The old DataDefSTRUCT is left alive (not deleted): a self-reference
	    // member pointer (`struct s *next`) made during body-parsing points at it,
	    // and emission is by tag name, so both denote the same `struct s`. dds now
	    // refers to the class for the rest of this function (registration/typedef/return).
	    dds = ddc;
	}
    }

    // register the struct type
    if ( tag )
    {
	dmi = pgm.struct_map.find(tag_store_key);
	if ( dmi != pgm.struct_map.end() )
	{
	    DataDefSTRUCT *existing = static_cast<DataDefSTRUCT *>(dmi->second);
	    if ( was_pre_registered && existing == dds )
	    {
		// We registered `dds` ourselves before body-parsing to enable
		// self-reference; the entry is already correct, nothing to do.
		DBG(cout << "TokenSTRUCT::parse() finalized self-registered struct " << tag->str << " size=" << dds->size << endl);
	    }
	    else if ( existing->size == 0 && existing->members.empty() )
	    {
		// Complete a forward-declared struct in place.
		existing->members = dds->members;
		existing->member_counts = dds->member_counts;
		existing->member_count_exprs = dds->member_count_exprs;
		existing->member_array_flags = dds->member_array_flags;
		// Multi-dim shapes (`int m[49][81]`) and per-member access control
		// must carry over too — without member_dims the completed struct
		// keeps only the flattened member_count, so a 2D member renders as
		// `int m[3969]` and `m[i][j]` fails ("subscripted value is neither
		// array nor pointer").
		existing->member_dims = dds->member_dims;
		existing->member_access = dds->member_access;
		existing->member_explicit_align = dds->member_explicit_align;
		existing->member_offsets = dds->member_offsets;
		existing->member_bitfields = dds->member_bitfields;
		existing->size = dds->size;
		existing->runtime_size_expr = dds->runtime_size_expr;
		existing->pack = dds->pack;
		existing->max_align = dds->max_align;
		existing->tag_explicit_align = dds->tag_explicit_align;
		existing->union_layout = dds->union_layout;
		existing->is_complete = true;
		DBG(cout << "TokenSTRUCT::parse() completed forward-declared struct " << tag->str << " size=" << existing->size << endl);
		delete dds;
		dds = existing;
	    }
	    else
		pgm.Throw(tag) << "Struct '" << tag->str << "' already defined" << flush;
	}
	else
	{
	    pgm.struct_map[tag_store_key] = dds;
	    DBG(cout << "TokenSTRUCT::parse() registered struct " << tag_store_key << " size=" << dds->size << endl);
	}
    }

    // NOTE: the struct/union definition is recorded in top_decls only for
    // the *bare* `struct X { ... };` form (below). For `typedef struct X
    // {...} Y;` and `struct X {...} v;`, the body rides inline in the
    // typedef / variable declaration (one combined SPEC_DECL, matching c2m),
    // so we must NOT also record a standalone dkStruct here.

    // what follows the closing brace?
    tn = pgm.peekToken();

    // typedef struct [tag] { ... } alias;
    if ( do_typedef )
    {
	bool done_aliases = false;
	    while ( !done_aliases )
	    {
		DataDef *alias_dd = dds;
		while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkMul )
		{
		pgm.nextToken();
		alias_dd = pgm.getPointerType(alias_dd);
	    }
	    tn = pgm.nextToken();
		if ( !tn || tn->type() != TokenType::ttIdentifier )
		    pgm.Throw(tn) << "Expecting alias name in typedef" << flush;
		TokenIdent *alias = (TokenIdent *)tn;
		alias_dd = parse_typedef_array_suffix(pgm, alias_dd, alias->str, tn);
		bmi = pgm.datatype_map.find(alias->str);
	    if ( bmi != pgm.datatype_map.end() && pgm.compounds.empty() )
		pgm.Throw(tn) << "Identifier '" << alias->str << "' already defined" << flush;
	    tdt = new TokenDataType(alias->str.c_str(), *alias_dd);
	    pgm.datatype_map[alias->str] = tdt;
	    if ( alias_dd == dds )
		pgm.struct_map[alias->str] = dds;
	    // The non-pointer alias of a combined `typedef struct Tag {...} Tag;` is
	    // the tag's body-definition point: its SPEC_DECL carries the full struct
	    // body. Recording this lets the CIR backend treat an EARLIER typedef that
	    // merely references the tag (`typedef struct Tag *p;`, before the body) as
	    // a forward reference — STRUCT(tag, IGNORE) — so the body emits here,
	    // after any types its members name are defined.
	    record_typedef(alias->str, alias_dd, tdt, alias, alias_dd == dds);
	    DBG(cout << "TokenSTRUCT::parse() typedef alias " << alias->str << endl);
	    tn = pgm.nextToken();
	    if ( !tn )
		pgm.Throw(alias) << "Unexpected end of input in typedef" << flush;
	    if ( tn->id() == TokenID::tkComma )
		continue;
	    if ( tn->id() != TokenID::tkSemi )
		pgm.Throw(tn) << "Expecting ',' or ';' after typedef alias" << flush;
	    done_aliases = true;
	}
	return new TokenStructDef(dds, is_union);
    }

    // struct [tag] { ... } variable;
    // struct [tag] { ... } *first_whogr, *last_whogr;  (pointer decl)
    if ( tn && (tn->type() == TokenType::ttIdentifier
	     || tn->id() == TokenID::tkMul
	     || tn->id() == TokenID::tkSTATIC
	     || tn->id() == TokenID::tkEXTERN) )
    {
	string tname(aggregate_kw);
	tname.append(" ");
	tname.append(tag ? tag->str : "anonymous");
	tdt = new TokenDataType(tname.c_str(), *dds);
	// A tagged definition with body followed by variable declarators
	// (`struct node { ... } *first, *last;`) must still emit the struct
	// body as a standalone top-level definition; otherwise `struct node`
	// stays incomplete and the variables have unknown storage size. Only
	// named tags get a standalone def — anonymous `struct { ... } s;`
	// renders its body inline in the variable's own declaration.
	if ( tag )
	    record_struct(dds, tag);
	if ( tn->id() == TokenID::tkSTATIC )
	{
	    pgm.nextToken(); // consume static before declarator
	    return pgm.parseDeclaration(tdt, true);
	}
	if ( tn->id() == TokenID::tkEXTERN )
	{
	    pgm.nextToken(); // consume extern before declarator
	    pgm.parsing_extern_decl = true;
	    return pgm.parseDeclaration(tdt);
	}
	return pgm.parseDeclaration(tdt);
    }

    // struct tag { ... }; — bare type definition. This is the one form whose
    // body is emitted as a standalone struct SPEC_DECL, so record it here.
    if ( tn && tn->id() == TokenID::tkSemi )
    {
	record_struct(dds, tag);
	TokenBase *capture_stmt =
	    materialize_runtime_struct_size_captures(pgm,
		pgm.compounds.empty() ? NULL : pgm.compounds.top(), dds, tag ? (TokenBase *)tag : (TokenBase *)this);
	pgm.nextToken(); // consume ';'
	return capture_stmt;
    }

    pgm.Throw(tn) << "Expecting variable name or ';' after struct definition" << flush;
    return NULL;
}

// parse a class definition
// Bind a bodyless method / ctor / dtor / operator of a std:: class to its real
// libstdc++ Itanium symbol (set FuncDef::emit_symbol — madc emits no body, the
// linker resolves it against libstdc++). No-op unless: we are inside `namespace
// std`, the class's canonical template-id spelling is known (set on instantiation),
// and the method is a bodyless prototype (externally implemented). The symbol is
// GENERATED by the mangler from the parsed declaration — NEVER a hardcoded literal.
enum class StdSymKind { Method, Ctor, Dtor };
static void bind_std_libstdcpp_symbol(Program &pgm, DataDefCLASS *ddc, Variable *mvar,
				      StdSymKind kind, const std::string &mname,
				      bool is_operator)
{
    if ( !ddc || !mvar )
	return;
    FuncDef *fd = dynamic_cast<FuncDef *>(mvar->type);
    if ( !fd || !fd->declaration_only || ddc->canonical_cpp_spelling.empty() )
	return;
    if ( !fd->emit_symbol.empty() )
	return;
    // Bind only std:: library types, identified by the canonical spelling the
    // template instantiation derived from its DEFINING namespace (e.g.
    // "std::__cxx11::basic_string<...>") — NOT by the instantiation SITE's
    // namespace. So a std type binds wherever it is instantiated (a use-site
    // `std::string s;` at file scope, or a typedef inside `namespace std`),
    // while a user template `foo::Bar<...>` (spelling "foo::Bar<...>") never
    // does. Only bodyless methods reach here (declaration_only, checked above),
    // so body-templates like std::vector are unaffected.
    if ( ddc->canonical_cpp_spelling.compare(0, 5, "std::") != 0 )
	return;
    // Parameter spellings, captured at parse time, EXCLUDING the hidden __this
    // (slot 0). These carry pointee-const / `&` / template structure the DataDef
    // alone loses, so the symbol matches libstdc++ exactly (PKc, not Pc).
    std::vector<std::string> psp;
    for ( size_t i = 1; i < fd->param_cpp_spellings.size(); ++i )
	psp.push_back(fd->param_cpp_spellings[i]);
    const std::string &cls = ddc->canonical_cpp_spelling;
    switch ( kind )
    {
    case StdSymKind::Ctor: fd->emit_symbol = itanium_mangle_ctor_sub(cls, psp); break;
    case StdSymKind::Dtor: fd->emit_symbol = itanium_mangle_dtor_sub(cls);      break;
    case StdSymKind::Method:
	if ( is_operator )
	{
	    std::string op = (mname.compare(0, 8, "operator") == 0) ? mname.substr(8) : mname;
	    fd->emit_symbol = itanium_mangle_operator_sub(cls, op, psp, fd->is_const_method);
	}
	else
	    fd->emit_symbol = itanium_mangle_member_sub(cls, mname, psp, fd->is_const_method);
	break;
    }
    DBG(std::cout << "bind_std_libstdcpp_symbol(): " << cls << "::" << mname
	<< " -> " << fd->emit_symbol << std::endl);
}

// Make an overload-unique internal symbol for a member/ctor: the canonical
// `base` if it is still free, else base + "__oN" for the first free N >= 2.
// Member methods and constructors that share a name AND arity but differ only by
// parameter TYPE (`find(char)` vs `find(const char*)`, two ctors, `operator=(char)`
// vs `operator=(const char*)`) otherwise collide on `base` — the second overload
// would re-use the first's already-declared FuncDef, losing its own parameters and
// re-binding the first's emit_symbol. A unique key forces a fresh FuncDef so each
// overload mangles to its own symbol; the caller records the key in
// FuncDef::class_emit_name so CIR call sites reference the right body. (Call-site
// selection among same-arity overloads by argument TYPE is separate, later work.)
static std::string unique_overload_symbol(Program &pgm, std::string base)
{
    if ( !pgm.findVariable(base) )
	return base;
    for ( int n = 2; ; ++n )
    {
	std::string cand = base + "__o" + std::to_string(n);
	if ( !pgm.findVariable(cand) )
	    return cand;
    }
}

// forms:
// class Name { type member; rettype method() { ... } };
// class Name variable;
// typedef class Name alias;
TokenBase *TokenCLASS::parse(Program &pgm)
{
    TokenIdent *tag = NULL;
    TokenBase *tn;
    TokenDataType *tdt;
    bool do_typedef = pgm.parsing_typedef_decl
	|| (pgm.prevToken() ? pgm.prevToken()->id() == TokenID::tkTYPEDEF : false);
    datatype_map_iter bmi;
    datadef_map_iter dmi;

    DBG(std::cout << std::endl << "TokenCLASS::parse() top" << std::endl);
    if ( !(tn=pgm.peekToken()) )
	pgm.Throw << "Unexpected end of input" << flush;

    // class name is required (no anonymous classes)
    if ( tn->type() == TokenType::ttIdentifier )
    {
	tag = (TokenIdent *)pgm.nextToken();
	DBG(cout << "TokenCLASS::parse() got name " << tag->str << endl);
	tn = pgm.peekToken();
	if ( !tn )
	    pgm.Throw << "Unexpected end of input after class name" << flush;
    }

    // --- inheritance: class Derived : [virtual] [access] Base, ... { ... } ---
    // Multiple + virtual bases: collect a BaseSpec per base into base_specs (ddc
    // does not exist yet — assigned to ddc->bases at flatten time). inherit_base
    // keeps the first base for the legacy single-base code paths.
    DataDefCLASS *inherit_base = NULL;
    std::vector<BaseSpec> base_specs;
    if ( tn->id() == TokenID::tkColon )
    {
	pgm.nextToken(); // consume ':'
	do {
	    bool bvirtual = false;
	    uint32_t baccess = 0;
	    // `virtual` and the access specifier may appear in either order
	    for (;;)
	    {
		TokenBase *kw = pgm.peekToken();
		if ( kw && kw->type() == TokenType::ttIdentifier )
		{
		    std::string &s = ((TokenIdent *)kw)->str;
		    if ( s == "virtual" )   { bvirtual = true;       pgm.nextToken(); continue; }
		    if ( s == "public" )    { baccess = 0;           pgm.nextToken(); continue; }
		    if ( s == "protected" ) { baccess = vfPROTECTED; pgm.nextToken(); continue; }
		    if ( s == "private" )   { baccess = vfPRIVATE;   pgm.nextToken(); continue; }
		}
		break;
	    }
	    TokenBase *bn = pgm.nextToken();
	    if ( !bn || bn->type() != TokenType::ttIdentifier )
		pgm.Throw(bn ? bn : this) << "Expected base class name" << flush;
	    std::string base_name = ((TokenIdent *)bn)->str;
	    datadef_map_iter bdmi = pgm.struct_map.find(base_name);
	    DataDefCLASS *bcls = (bdmi != pgm.struct_map.end())
		? dynamic_cast<DataDefCLASS *>(bdmi->second) : NULL;
	    if ( !bcls )
		pgm.Throw(bn) << "Unknown base class '" << base_name << "'" << flush;
	    base_specs.push_back(BaseSpec{bcls, 0, bvirtual, baccess, false});
	    if ( !inherit_base ) inherit_base = bcls;
	    DBG(cout << "TokenCLASS::parse() inherits from " << base_name
		<< (bvirtual ? " (virtual)" : "") << endl);
	} while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkComma
		  && (pgm.nextToken(), true) );
	tn = pgm.peekToken();
    }

    // if no brace, class type must already be defined
    if ( tn->id() != TokenID::tkOpBrc )
    {
	if ( !tag )
	    pgm.Throw(tn) << "Expecting class name" << flush;
	if ( (dmi=pgm.struct_map.find(tag->str)) == pgm.struct_map.end() )
	    pgm.Throw(tn) << "Unknown class type '" << tag->str << "'" << flush;
	if ( do_typedef )
	{
	    tn = pgm.nextToken();
	    if ( tn->type() != TokenType::ttIdentifier )
		pgm.Throw(tn) << "Expecting identifier in typedef" << flush;
	    TokenIdent *alias = (TokenIdent *)tn;
	    if ( (bmi=pgm.datatype_map.find(alias->str)) != pgm.datatype_map.end() )
	    {
		// C allows identical typedef redeclarations
		if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
		    pgm.nextToken();
		return NULL;
	    }
	    tdt = new TokenDataType(alias->str.c_str(), *dmi->second);
	    pgm.datatype_map[alias->str] = tdt;
	    return NULL;
	}
	tdt = new TokenDataType(tag->str.c_str(), *dmi->second);
	return pgm.parseDeclaration(tdt);
    }

    // ---- defining a new class: class Name { ... } ----

    if ( !tag )
	pgm.Throw(tn) << "Class definition requires a name" << flush;

    pgm.nextToken(); // consume '{'

    // Redefinition guard: reject a second definition of an already-defined
    // class before we create anything (was checked at registration time, now
    // hoisted so the early self-registration below can't false-trip it).
    if ( pgm.struct_map.find(tag->str) != pgm.struct_map.end() )
	pgm.Throw(tag) << "Class '" << tag->str << "' already defined" << flush;

    DataDefCLASS *ddc = new DataDefCLASS(tag->str, 0, DataType::dtRESERVED);
    // If we are instantiating a std:: template (e.g. basic_ofstream<char,...>),
    // record its canonical C++ spelling so a bodyless method in this class can be
    // mangled to the real libstdc++ symbol with no hardcoded literal.
    if ( !pgm.instantiating_canonical_spelling.empty() )
	ddc->canonical_cpp_spelling = pgm.instantiating_canonical_spelling;
    DBG(cout << "TokenCLASS::parse() defining class " << tag->str << endl);

    // Register the class type EARLY — before the member/method body loop — so a
    // method or operator can name its OWN class as a parameter or return type
    // (e.g. `int operator+(const Counter& o)`, `V operator+(const V&)` returning
    // V, `C add(C o)`). TokenDataType wraps *ddc by REFERENCE, so as members are
    // appended the same object grows; method BODIES compile later (cir_builder),
    // by which point ddc is complete, so by-value self params/returns pick up the
    // final size. This is the standard incomplete-self-reference pattern, and is
    // consistent with the inherited-base members being copied in early below.
    pgm.struct_map[tag->str] = ddc;
    tdt = new TokenDataType(tag->str.c_str(), *ddc);
    pgm.datatype_map[tag->str] = tdt;
    if ( !pgm.current_namespace.empty() )
	pgm.namespace_datatype_map[pgm.current_namespace][tag->str] = tdt;

    // Flatten EACH base's data members into the derived class so method bodies can
    // resolve inherited members during the body loop. Tag each with its origin base
    // index; final offsets are assigned by compute_layout()+apply_member_layout()
    // after the body loop (member_offsets here hold the member's offset WITHIN its
    // base). Own members (added below via addMember) are origin -1 (normalized at
    // the compute_layout call site). size is reset to 0: own members start at 0,
    // and compute_layout assigns the real total.
    if ( !base_specs.empty() )
    {
	ddc->bases = base_specs;
	ddc->base_class = inherit_base; // first base, for legacy single-base paths
	for ( size_t bi = 0; bi < ddc->bases.size(); bi++ )
	{
	    DataDefCLASS *b = ddc->bases[bi].base;
	    for ( size_t i = 0; i < b->members.size(); ++i )
	    {
		ddc->members.push_back(b->members[i]);
		ddc->member_offsets.push_back(b->member_offsets[i]);
		ddc->member_counts.push_back(i < b->member_counts.size() ? b->member_counts[i] : 1);
		ddc->member_array_flags.push_back(i < b->member_array_flags.size() ? b->member_array_flags[i] : false);
		ddc->member_bitfields.push_back(i < b->member_bitfields.size() ? b->member_bitfields[i] : DataDefSTRUCT::BitFieldInfo());
		ddc->member_dims.push_back(i < b->member_dims.size() ? b->member_dims[i] : std::vector<uint32_t>());
		ddc->member_count_exprs.push_back(i < b->member_count_exprs.size() ? b->member_count_exprs[i] : NULL);
		ddc->member_access.push_back(i < b->member_access.size() ? b->member_access[i] : 0);
		ddc->member_origin.push_back((int)bi);
	    }
	    // Inherit methods (derived can override via method_map shadowing)
	    for ( auto &mp : b->method_map )
		if ( ddc->method_map.find(mp.first) == ddc->method_map.end() )
		    ddc->method_map[mp.first] = mp.second;
	    if ( b->has_user_dtor )
		ddc->has_user_dtor = true;
	    if ( b->has_vtable )
	    {
		ddc->has_vtable = true;
		for ( auto &vs : b->vtable_slots )
		    if ( ddc->vtable_slot(vs) < 0 ) ddc->vtable_slots.push_back(vs);
		for ( auto &vm : b->virtual_methods ) ddc->virtual_methods[vm.first] = true;
	    }
	    DBG(cout << "TokenCLASS::parse() flattened " << b->members.size()
		<< " members from base " << b->name << " (origin " << bi << ')' << endl);
	}
	ddc->size = 0; // own members start at 0; compute_layout assigns the real total
    }

    // C++ defaults: `class` members are PRIVATE until a public:/protected:
    // label, `struct` members are public. This is TokenCLASS::parse (the
    // `class` keyword only — `struct`/`union` go through TokenSTRUCT::parse,
    // which never marks members and so leaves them public), so the correct
    // default here is private. (P2.5b: corrected from the legacy
    // default-to-public artifact now that P2.5 enforces access.)
    uint32_t access_flags = vfPRIVATE; // 0=public, vfPRIVATE, vfPROTECTED

    while ( (tn=pgm.peekToken()) && tn->id() != TokenID::tkClBrc )
    {
	// --- access specifier labels: public: / private: / protected: ---
	if ( tn->type() == TokenType::ttIdentifier )
	{
	    std::string &label = ((TokenIdent *)tn)->str;
	    if ( label == "public" || label == "private" || label == "protected" )
	    {
		pgm.nextToken(); // consume the keyword
		TokenBase *colon = pgm.peekToken();
		if ( colon && colon->id() == TokenID::tkTerC )
		    pgm.nextToken(); // consume ':'
		if ( label == "private" )
		    access_flags = vfPRIVATE;
		else if ( label == "protected" )
		    access_flags = vfPROTECTED;
		else
		    access_flags = 0;
		continue;
	    }
	}

	// --- virtual keyword ---
	bool is_virtual = false;
	if ( tn->type() == TokenType::ttIdentifier
	  && ((TokenIdent *)tn)->str == "virtual" )
	{
	    pgm.nextToken(); // consume 'virtual'
	    is_virtual = true;
	    tn = pgm.peekToken();
	    if ( !tn )
		pgm.Throw(this) << "Unexpected end of input after 'virtual'" << flush;
	}

	// --- destructor: ~ClassName() { ... } ---
	if ( tn->id() == TokenID::tkBnot )
	{
	    pgm.nextToken(); // consume '~'
	    tn = pgm.nextToken();
	    if ( !tn || tn->type() != TokenType::ttIdentifier
	      || ((TokenIdent *)tn)->str != tag->str )
		pgm.Throw(tn) << "Expected ~" << tag->str << "() destructor" << flush;
	    tn = pgm.peekToken();
	    if ( !tn || tn->id() != TokenID::tkOpBrk )
		pgm.Throw(tn) << "Expected '(' after ~" << tag->str << flush;
	    pgm.nextToken(); // consume '('
	    std::string mangled = tag->str + "___dtor";
	    DBG(cout << "TokenCLASS::parse() parsing destructor " << mangled << endl);
	    pgm.parseFunction(ddVOID, mangled, ddc);
	    Variable *mvar;
	    if ( (mvar=pgm.tkProgram->findVariable(mangled)) )
	    {
		if ( access_flags )
		    mvar->flags |= access_flags;
		ddc->methods.push_back(mvar);
		ddc->method_map["~" + tag->str] = mvar;
		ddc->has_user_dtor = true;
		bind_std_libstdcpp_symbol(pgm, ddc, mvar, StdSymKind::Dtor, "~" + tag->str, false);
	    }
	    // Virtual destructor: register the Itanium D1/D0 slot pair at the
	    // destructor's declaration position. Markers are class-name-INDEPENDENT
	    // ("~" = D1 complete, "~$deleting" = D0 deleting) so base + overriding
	    // derived SHARE the slot (derived inherits them via the base-merge loop
	    // at ~11952; class_vtable_def re-resolves to the current class's symbols).
	    // The guard is also true (inherited virtuality) when the markers are
	    // already present from a virtual base dtor.
	    if ( is_virtual || ddc->vtable_slot("~") >= 0 )
	    {
		ddc->virtual_methods["~" + tag->str] = true;
		if ( ddc->vtable_slot("~") < 0 )
		    ddc->vtable_slots.push_back("~");
		if ( ddc->vtable_slot("~$deleting") < 0 )
		    ddc->vtable_slots.push_back("~$deleting");
		ddc->has_vtable = true;
	    }
	    continue;
	}

	// --- constructor: ClassName() { ... } (no return type) ---
	if ( tn->type() == TokenType::ttIdentifier
	  && ((TokenIdent *)tn)->str == tag->str )
	{
	    // peek ahead: if ClassName is followed by '(' it's a constructor
	    // (not a member of type ClassName)
	    TokenBase *name_tok = pgm.nextToken(); // consume class name
	    tn = pgm.peekToken();
	    if ( tn && tn->id() == TokenID::tkOpBrk )
	    {
		pgm.nextToken(); // consume '('
		std::string mangled = tag->str + "__" + tag->str;
		// A second (or later) constructor overload collides on Class__Class;
		// give it a unique key so it registers its own FuncDef + parameters.
		bool ctor_disambiguated = (pgm.findVariable(mangled) != NULL);
		if ( ctor_disambiguated )
		    mangled = unique_overload_symbol(pgm, mangled);
		DBG(cout << "TokenCLASS::parse() parsing constructor " << mangled << endl);
		pgm.parseFunction(ddVOID, mangled, ddc);
		Variable *mvar;
		if ( (mvar=pgm.tkProgram->findVariable(mangled)) )
		{
		    if ( ctor_disambiguated )
			if ( FuncDef *cfd = dynamic_cast<FuncDef *>(mvar->type) )
			    cfd->class_emit_name = mangled;
		    if ( access_flags )
			mvar->flags |= access_flags;
		    ddc->methods.push_back(mvar);
		    // Keep method_map[Class] pointing at the FIRST ctor (all ctors live
		    // in ddc->ctors); preserves existing single-ctor name resolution.
		    if ( ddc->method_map.find(tag->str) == ddc->method_map.end() )
			ddc->method_map[tag->str] = mvar;
		    ddc->ctors.push_back(mvar);
		    ddc->has_user_ctor = true;
		    bind_std_libstdcpp_symbol(pgm, ddc, mvar, StdSymKind::Ctor, tag->str, false);
		}
		continue;
	    }
	    // Not a constructor — push back and fall through to normal parsing.
	    // The class name is also a valid type for a member (self-referencing
	    // pointer etc.), so treat it as a type token.
	    pgm.pushToken(name_tok);
	}

	// --- normal member or method: type name ... ---
	// Optional leading cv-qualifiers on a data member / method return type:
	// `const char *m;`, `volatile int v;`. madc does not enforce member
	// const-correctness, so consume them (a const data member is an ordinary
	// member). const/volatile/restrict are dedicated token ids, not identifiers
	// (see the same idiom at parser.cpp:15942).
	while ( pgm.peekToken()
	     && ( pgm.peekToken()->id() == TokenID::tkCONST
	       || pgm.peekToken()->id() == TokenID::tkVOLATILE
	       || pgm.peekToken()->id() == TokenID::tkRESTRICT ) )
	    pgm.nextToken();

	// expect a data type token
	TokenDataType *mtype = resolve_declared_type_token(pgm, pgm.peekToken(), true, true);
	if ( !mtype )
	    pgm.Throw(pgm.peekToken()) << "Expecting type in class definition" << flush;
	pgm.nextToken();

	// check for pointer declarator(s): type * [*...] member_name
	DataDef *cmember_dd = &mtype->definition;
	while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkMul )
	{
	    pgm.nextToken(); // consume '*'
	    cmember_dd = pgm.getPointerType(cmember_dd);
	}

	// Reference return type on a method: `T& method()` / `T& operator[]()`.
	// Returned by address (a T*); recorded as returns_ref on the FuncDef below
	// so the call site is an lvalue. Only valid before a method, not a data
	// member (a `T&` data member is not supported — caught at member parse).
	bool ret_is_ref = false;
	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkBand )
	{
	    pgm.nextToken(); // consume '&'
	    ret_is_ref = true;
	}

	// expect member name — may be an identifier or 'operator' keyword
	tn = pgm.nextToken();
	std::string mname;
	bool is_operator_method = (tn->id() == TokenID::tkOPEROVER);
	if ( tn->id() == TokenID::tkOPEROVER )
	{
	    // operator overload: consume the operator symbol(s) to form the name
	    TokenBase *op_tok = pgm.nextToken();
	    if ( !op_tok )
		pgm.Throw(tn) << "Expected operator symbol after 'operator'" << flush;
	    // Multi-character operators: ==, !=, <=, >=, <<, >>
	    if ( dynamic_cast<TokenMultiOp *>(op_tok) )
		mname = "operator" + ((TokenMultiOp *)op_tok)->str;
	    else if ( op_tok->id() == TokenID::tkOpBrk )
	    {
		// operator()
		if ( !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkClBrk )
		    pgm.Throw(op_tok) << "Expected ')' in operator()" << flush;
		pgm.nextToken(); // consume ')'
		mname = "operator()";
	    }
	    else if ( op_tok->id() == TokenID::tkOpSqr )
	    {
		// operator[]
		if ( !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkClSqr )
		    pgm.Throw(op_tok) << "Expected ']' in operator[]" << flush;
		pgm.nextToken(); // consume ']'
		mname = "operator[]";
	    }
	    else
	    {
		// Single-character operators: +, -, *, /, %, <, >, =, etc.
		char sym = (char)op_tok->get();
		if ( !sym )
		    pgm.Throw(op_tok) << "Unrecognized operator symbol" << flush;
		mname = std::string("operator") + sym;
	    }
	}
	else if ( tn->type() != TokenType::ttIdentifier )
	    pgm.Throw(tn) << "Expecting member name in class definition" << flush;
	else
	    mname = ((TokenIdent *)tn)->str;

	// peek: is this a method (followed by '(') or a data member (followed by ';')?
	tn = pgm.peekToken();
	if ( tn && tn->id() == TokenID::tkOpBrk )
	{
	    pgm.nextToken(); // consume '('
	    // method declaration — parse as function, add to class methods
	    DBG(cout << "TokenCLASS::parse() parsing method " << mname << endl);
	    // mangle method name to avoid collisions: ClassName__methodName
	    std::string mangled = tag->str + "__" + mname;
	    // P2.1b gaps 3 & 4 — same-name operator overloads of DIFFERENT arity
	    // (unary `operator-()` + binary `operator-(const C&)`; prefix
	    // `operator++()` + postfix `operator++(int)`) share the default
	    // ClassName__operatorX name, which collides BOTH in funcdef_map (the
	    // second body fails to register its params) AND in the emitted C (two
	    // functions with one name). Detect the collision and give the NULLARY
	    // (unary / prefix) overload a distinct `..._un` symbol, recorded in
	    // FuncDef::class_emit_name so the CIR call sites reference the matching
	    // name. The CIR already selects the right FuncDef by arity. Only
	    // operators with a same-name peer collide; single overloads are
	    // untouched (class_emit_name stays empty -> default scheme).
	    // Nullary (unary / prefix) form: `(` immediately followed by `)`.
	    bool this_is_nullary = pgm.peekToken()
		&& pgm.peekToken()->id() == TokenID::tkClBrk;
	    Variable *peer = is_operator_method ? ddc->findMethod(mname) : NULL;
	    FuncDef *peer_fd = peer ? dynamic_cast<FuncDef *>(peer->type) : NULL;
	    bool name_disambiguated = false;
	    if ( peer_fd )
	    {
		// __this is param 0, so a nullary (unary/prefix) overload has
		// parameters.size() == 1; a parameterized one has > 1.
		bool peer_is_nullary = peer_fd->parameters.size() <= 1;
		if ( peer_is_nullary != this_is_nullary )
		{
		    // Give the nullary overload the `_un` suffix; the parameterized
		    // one keeps the canonical ClassName__operatorX name.
		    if ( this_is_nullary )
			mangled = tag->str + "__" + mname + "_un";
		    else
		    {
			// Existing peer was the nullary one — retag IT to `_un`
			// (rename its FuncDef body symbol + record class_emit_name).
			// Move its funcdef_map entry to the new key so the canonical
			// ClassName__operatorX name is FREE for this binary overload
			// (otherwise parseFunction sees it as already-declared and
			// skips this overload's parameter registration).
			std::string peer_name = tag->str + "__" + mname + "_un";
			std::string peer_old = tag->str + "__" + mname;
			peer_fd->class_emit_name = peer_name;
			peer->name = peer_name;
			pgm.funcdef_map[peer_name] = peer_fd;
			pgm.funcdef_map.erase(peer_old);
		    }
		    name_disambiguated = true;
		}
	    }
	    // Same-name, same-arity, different-TYPE overload (`find(char)` vs
	    // `find(const char*)`): not separable by the nullary `_un` rule above, so
	    // it still collides on Class__mname. Give it a unique key so it registers
	    // its own FuncDef + parameters and mangles to its own symbol.
	    bool type_overload_disambiguated = false;
	    if ( !name_disambiguated && pgm.findVariable(mangled) )
	    {
		mangled = unique_overload_symbol(pgm, mangled);
		type_overload_disambiguated = true;
	    }
	    pgm.parseFunction(*cmember_dd, mangled, ddc);
	    // find the variable that parseFunction created and add to class methods
	    Variable *mvar;
	    if ( (mvar=pgm.tkProgram->findVariable(mangled)) )
	    {
		if ( FuncDef *mfd = dynamic_cast<FuncDef *>(mvar->type) )
		{
		    mfd->returns_ref = ret_is_ref;
		    mfd->method_display_name = mname;
		    if ( mfd->declaration_only && !mvar->storage_alias_name.empty() )
			mfd->emit_symbol = mvar->storage_alias_name;
		    if ( (name_disambiguated && this_is_nullary) || type_overload_disambiguated )
			mfd->class_emit_name = mangled;
		}
		if ( access_flags )
		    mvar->flags |= access_flags;
		ddc->methods.push_back(mvar);
		// also register under the unmangled name for method lookup
		ddc->method_map[mname] = mvar;
		bind_std_libstdcpp_symbol(pgm, ddc, mvar, StdSymKind::Method, mname, is_operator_method);
		// Track virtual methods and assign vtable slots
		if ( is_virtual || ddc->is_virtual_method(mname) )
		{
		    ddc->virtual_methods[mname] = true;
		    if ( ddc->vtable_slot(mname) < 0 )
			ddc->vtable_slots.push_back(mname);
		    ddc->has_vtable = true;
		}
	    }
	}
	else
	{
	    // data member
	    if ( ret_is_ref )
		pgm.Throw(tn) << "Reference data members (T&) are not supported" << flush;
	    // Optional fixed-size array dimensions: `long _buf[64];`, `int m[4][8];`.
	    // (Constant dims only — a class data member is never a VLA.) Multiply the
	    // dims into one inline element count (mirrors TokenSTRUCT::parse's member loop).
	    size_t member_count = 1;
	    bool member_is_array = false;
	    std::vector<uint32_t> member_dims;
	    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpSqr )
	    {
		member_is_array = true;
		pgm.nextToken(); // consume '['
		if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkClSqr )
		{
		    pgm.nextToken(); // consume ']' (unsized [])
		    member_dims.push_back(0);
		    member_count = 0;
		    continue;
		}
		int64_t adim = parse_constant_integer_expression(pgm);
		if ( adim < 0 )
		    pgm.Throw(tn) << "Class member array dimension must be non-negative" << flush;
		TokenBase *acl = pgm.nextToken();
		if ( !acl || acl->id() != TokenID::tkClSqr )
		    pgm.Throw(acl ? acl : tn) << "Expected ']' in class member array declarator" << flush;
		member_dims.push_back((uint32_t)adim);
		member_count *= (size_t)adim;
	    }
	    ddc->addMember(mname, *cmember_dd, member_count, NULL, member_is_array,
		member_is_array ? &member_dims : NULL);
	    if ( access_flags && !ddc->member_access.empty() )
		ddc->member_access.back() = access_flags;
	    DBG(cout << "TokenCLASS::parse() added member " << cmember_dd->name << ' ' << mname
		<< " (count " << member_count << ", total " << ddc->size << ')' << endl);
	    tn = pgm.nextToken();
	    if ( tn->id() != TokenID::tkSemi )
		pgm.Throw(tn) << "Expecting ';' after class member" << flush;
	}
    }

    if ( !tn )
	pgm.Throw << "Unexpected end of input in class definition" << flush;
    pgm.nextToken(); // consume '}'

    // Finalize layout. compute_layout assigns base/vbase offsets, vptr placement,
    // nvsize/size and own_block_off; apply_member_layout rewrites every member's
    // final offset from its origin. This REPLACES the old +8 vptr fix-up and gives
    // correct non-zero offsets for multiple/virtual bases. For a base-less or
    // single-inheritance class it reproduces the previous layout byte-for-byte
    // (vptr@0, base@0, own members after).
    ddc->member_origin.resize(ddc->members.size(), -1); // own members (addMember) -> -1
    ddc->compute_layout();
    ddc->apply_member_layout();
    ddc->build_vtable_groups(); // grouped vtable (primary + secondary polymorphic bases)
    DBG(cout << "TokenCLASS::parse() finalized layout, size now " << ddc->size << endl);

    // Allocate vtable if needed
    if ( ddc->has_vtable )
    {
	size_t nslots = ddc->vtable_slots.size();
	ddc->vtable = (void **)calloc(nslots, sizeof(void *));
	DBG(cout << "TokenCLASS::parse() allocated vtable with " << nslots << " slots" << endl);
    }

    // The class type was registered early (before the body loop) so methods
    // could reference their own type; ddc is now complete with its final size.
    DBG(cout << "TokenCLASS::parse() registered class " << tag->str << " size=" << ddc->size << endl);

    // what follows?
    tn = pgm.peekToken();

    if ( do_typedef )
    {
	tn = pgm.nextToken();
	if ( !tn || tn->type() != TokenType::ttIdentifier )
	    pgm.Throw(tn) << "Expecting alias name in typedef" << flush;
	TokenIdent *alias = (TokenIdent *)tn;
	tdt = new TokenDataType(alias->str.c_str(), *ddc);
	pgm.datatype_map[alias->str] = tdt;
	pgm.struct_map[alias->str] = ddc;
	return NULL;
    }

    // class Name { ... } variable;
    if ( tn && tn->type() == TokenType::ttIdentifier )
    {
	tdt = new TokenDataType(tag->str.c_str(), *ddc);
	return pgm.parseDeclaration(tdt);
    }

    // class Name { ... }; — just a definition
    if ( tn && tn->id() == TokenID::tkSemi )
    {
	pgm.nextToken();
	return NULL;
    }

    pgm.Throw(tn) << "Expecting variable name or ';' after class definition" << flush;
    return NULL;
}

TokenBase *TokenGOTO::parse(Program &pgm)
{
    DBG(std::cout << std::endl << "TokenGOTO::parse()" << std::endl);
    TokenBase *tn = pgm.nextToken();
    if ( !tn )
	pgm.Throw((TokenBase *)this) << "expected label name or '*expr' after 'goto'" << flush;
    if ( tn->id() == TokenID::tkMul )
    {
	TokenBase *expr_tb = pgm.nextToken();
	if ( !expr_tb )
	    pgm.Throw(tn) << "expected expression after 'goto *'" << flush;
	indirect_target = pgm.parseExpression(expr_tb);
	TokenBase *semi = pgm.curToken();
	if ( !semi || semi->id() != TokenID::tkSemi )
	    pgm.Throw(semi ? semi : expr_tb) << "expected ';' after 'goto *expr'" << flush;
	return this;
    }
    if ( !is_contextual_identifier_token(tn) )
	pgm.Throw(tn) << "expected label name or '*expr' after 'goto'" << flush;
    target = contextual_identifier_name(tn);
    TokenBase *semi = pgm.nextToken();
    if ( !semi || semi->id() != TokenID::tkSemi )
	pgm.Throw(semi ? semi : tn) << "expected ';' after 'goto " << target << "'" << flush;
    return this;
}

TokenBase *TokenRETURN::parse(Program &pgm)
{
    TokenBase *tn;

    DBG(std::cout << std::endl << "TokenRETURN::parse()" << std::endl);
    tn = pgm.nextToken();

    // return with no value
    if ( tn->id() == TokenID::tkSemi )
	return this;

    // parse first return expression
    returns = pgm.parseExpression(tn);

    // Multi-return detection: parseExpression consumed either `,` or `;`
    // to stop the first return expression. The cleanest signal is the
    // consumed stop token itself (curToken()): only `,` indicates that
    // a second expression follows. Without that check, a plain
    // `return x;` followed by any identifier-starting statement (e.g.
    // `noop(ch);`) misfires as multi-return because the peek-based
    // heuristic can't distinguish a second return expression from the
    // start of the next statement.
    auto looks_like_second_return = [&]() -> bool {
	TokenBase *stop = pgm.curToken();
	if ( !stop || stop->id() != TokenID::tkComma )
	    return false;
	TokenBase *p = pgm.peekToken();
	if ( !p ) return false;
	if ( p->id() == TokenID::tkSemi ) return false;
	if ( p->id() == TokenID::tkClBrc ) return false;
	if ( p->type() == TokenType::ttSymbol ) return false;
	if ( p->type() == TokenType::ttKeyword ) return false;
	if ( p->type() == TokenType::ttDataType ) return false;
	return true;
    };
    if ( looks_like_second_return() )
    {
	return_exprs.push_back(returns);
	tn = pgm.nextToken();
	return_exprs.push_back(pgm.parseExpression(tn));
	while ( looks_like_second_return() )
	{
	    tn = pgm.nextToken();
	    return_exprs.push_back(pgm.parseExpression(tn));
	}
	returns = NULL; // multi-return uses return_exprs instead

	// mark the enclosing function as multi-return so it gets __retbuf at compile time
	TokenCpnd *code = pgm.compounds.empty() ? NULL : pgm.compounds.top();
	if ( code && code->method )
	{
	    FuncDef *fdef = (FuncDef *)code->method->returns.type;
	    if ( fdef && fdef->return_types.empty() )
	    {
		// populate return_types with int64 for each value (inferred)
		for ( size_t i = 0; i < return_exprs.size(); ++i )
		    fdef->return_types.push_back(&ddINT64);
	    }
	}
	DBG(std::cout << "TokenRETURN::parse() multi-return with " << return_exprs.size() << " values" << std::endl);
    }

    return this;
}

TokenBase *TokenIF::parse(Program &pgm)
{
    TokenBase *tn;

    DBG(std::cout << std::endl << "TokenIF::parse()" << std::endl);

    DBG(cout << "TokenIF::parse() calling parse_parenthesized_expression()" << endl);
    if ( !(condition = parse_parenthesized_expression(pgm, "if", true)) )
	pgm.Throw << "Failed to parse if expression" << flush;

    tn = pgm.nextToken();
    DBG(cout << "TokenIF::parse() calling statement=parseStatement(" << (char)tn->get() << ')' << endl);
    if ( !(statement=pgm.parseStatement(tn)) )
	pgm.Throw(tn) << "Failed to parse if statement" << flush;

    // Some statement parsers (TokenBREAK, TokenCONT, plain TokenRETURN
    // for void) leave the trailing ';' in the stream — they're handled
    // as no-op statements by parseCompound on the next iteration.  But
    // here, with `if (cond) break;` (or any bare-flow-statement body),
    // peeking for `else` finds the unconsumed ';' first and we miss
    // attaching the else to *this* if.  C semantics require the else
    // to bind to the nearest unmatched if (the dangling-else rule), so
    // skip past a trailing ';' before checking.
    while ( (tn = pgm.peekToken()) && tn->id() == TokenID::tkSemi )
	pgm.nextToken();

    if ( tn && tn->id() == TokenID::tkELSE )
    {
	tn = pgm.nextToken(); // get the else
	tn = pgm.nextToken(); // skip the else
	DBG(cout << "TokenIF::parse() calling elsestmt=parseStatement(" << (char)tn->get() << ')' << endl);
	elsestmt = pgm.parseStatement(tn);
	if ( !elsestmt )
	    pgm.Throw(tn) << "parse error on else" << flush;
    }
    else
    if ( tn )
	DBG(cout << "TokenIF::peekToken() type: " << (int)tn->type() << " id: " << (int)tn->id() << ')' << endl);

    return this;
}

TokenBase *TokenFOR::parse(Program &pgm)
{
    TokenBase *tn;

    DBG(std::cout << std::endl << "TokenFOR::parse() START" << std::endl);
    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkOpBrk )
    {
	DBG(cerr << "TokenFOR::parse() expecting (" << endl);
	pgm.Throw(tn) << "expecting ( after for" << flush;
    }

    tn = pgm.nextToken();

    // A leading `const` on the loop-var / init type: `for (const T& v : c)`
    // (const reference — reads). Consume it so the type resolves; for a range-
    // for reference this just marks the alias read-only (the alias machinery is
    // the same). Restored on the traditional-for fall-through.
    TokenBase *const_tok = NULL;
    if ( tn && tn->id() == TokenID::tkCONST )
    {
	const_tok = tn;
	tn = pgm.nextToken();
    }

    // detect range-based for: for (type var : container)
    bool typed_for_init = false;
    if ( TokenDataType *dt = resolve_declared_type_token(pgm, tn, true, true) )
    {
	// Optional `&` between the element type and the name: `for (T& v : c)`
	// is a REFERENCE loop var that aliases (and may mutate) the source
	// element. Consume the `&` here so the identifier follows; record it on
	// the TokenFOREACH so the CIR lowering binds `v` to the element ADDRESS
	// (vfREFERENCE pointer model) instead of copying the value.
	bool range_elem_ref = false;
	TokenBase *amp_tok = NULL;
	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkBand )
	{
	    amp_tok = pgm.nextToken(); // consume '&' (kept for push-back below)
	    range_elem_ref = true;
	}
	TokenBase *tn2 = pgm.nextToken();
	if ( tn2 && is_contextual_identifier_token(tn2) )
	{
	    TokenBase *tn3 = pgm.peekToken();
	    if ( tn3 && tn3->id() == TokenID::tkTerC )
	    {
		pgm.nextToken(); // consume the colon

		TokenFOREACH *fe = new TokenFOREACH();
		fe->file = this->file;
		fe->line = this->line;
		fe->column = this->column;
		fe->elemtype = &dt->definition;
		fe->elemname = contextual_identifier_name(tn2);
		fe->elem_is_ref = range_elem_ref;

		DBG(cout << "TokenFOR::parse() range-for detected: " << dt->definition.name << ' ' << fe->elemname
		    << (range_elem_ref ? " (reference)" : "") << endl);

		// add the loop variable to the current scope. A reference loop
		// var is a pointer-to-element with vfREFERENCE (auto-deref on
		// read; writes hit the aliased source) — the same model as a
		// numeric `T&` parameter.
		TokenCpnd *code = pgm.compounds.empty() ? NULL : pgm.compounds.top();
		if ( range_elem_ref )
		{
		    DataDef *ref_ptr = pgm.getPointerType(&dt->definition);
		    fe->elemvar = pgm.addVariable(code, *ref_ptr, fe->elemname, 1, NULL, false);
		    fe->elemvar->flags |= vfREFERENCE;
		}
		else
		    fe->elemvar = pgm.addVariable(code, dt->definition, fe->elemname, 1, NULL, false);

		// parse the container expression
		TokenBase *tn4 = pgm.nextToken();
		fe->container = pgm.parseExpression(tn4, true);
		if ( !fe->container )
		    pgm.Throw(tn4) << "Failed to parse container expression in range-for" << flush;

		tn4 = pgm.nextToken();
		if ( tn4->id() != TokenID::tkClBrk )
		    pgm.Throw(tn4) << "Expecting ) after range-for container" << flush;

		tn4 = pgm.nextToken();
		fe->statement = pgm.parseStatement(tn4);
		if ( !fe->statement )
		    pgm.Throw(tn4) << "Failed to parse range-for body" << flush;

		DBG(std::cout << "TokenFOR::parse() range-for END" << std::endl);
		return fe;
	    }
	}

	// not range-for — traditional for with type declaration. Restore tn2
	// and, if we speculatively consumed a `&`, restore it too (push in
	// reverse order so the deque front reads `& tn2 ...` again). A consumed
	// leading `const` is propagated to parseDeclaration via parsing_const_decl
	// (same channel TokenCONST::parse uses), not pushed back.
	DBG(cout << "TokenFOR::parse() traditional for with type declaration" << endl);
	pgm.pushToken(tn2);
	if ( amp_tok )
	    pgm.pushToken(amp_tok);
	if ( const_tok )
	    pgm.parsing_const_decl = true;
	initialize = pgm.parseDeclaration(dt);
	typed_for_init = true;
    }
    else
    {
	// `const` must qualify a type; `for (const <non-type> ...)` is malformed.
	if ( const_tok )
	    pgm.Throw(tn) << "expected a type after 'const' in for-init" << flush;
	if ( tn->id() == TokenID::tkSemi )
	{
	    initialize = NULL;
	}
	else
	{
	    DBG(cout << "TokenFOR::parse() initialize: calling parseExpression(" << (char)tn->get() << ')' << endl);
	    // conditional=true so parseExpression stops on `;` WITHOUT consuming;
	    // comma continuations read below get to see `,` left in the stream.
	    if ( !(initialize = pgm.parseExpression(tn, true)) )
		pgm.Throw(tn) << "Failed to parse initialize" << flush;
	}
    }

    // C comma-expression init: `for (a=0, b=1, ... ; ...)`. parseExpression
    // consumes `,`, so after a comma-terminated init the peek is already
    // the next expression starter; accept either case.
    while ( tn->id() != TokenID::tkSemi && pgm.peekToken() )
    {
	TokenBase *pk = pgm.peekToken();
	if ( pk->id() == TokenID::tkComma ) { pgm.nextToken(); pk = pgm.peekToken(); }
	if ( !pk || pk->id() == TokenID::tkSemi ) break;
	if ( typed_for_init && pk->type() == TokenType::ttDataType )
	{
	    tn = pgm.nextToken();
	    TokenBase *extra = pgm.parseStatement(tn);
	    if ( extra ) init_extras.push_back(extra);
	    continue;
	}
	// anything else must look like an expression starter
	if ( pk->type() == TokenType::ttSymbol || pk->type() == TokenType::ttKeyword
	  || pk->type() == TokenType::ttDataType ) break;
	tn = pgm.nextToken();
	TokenBase *extra = pgm.parseExpression(tn, true);
	if ( extra ) init_extras.push_back(extra);
    }

    if ( tn->id() != TokenID::tkSemi )
	tn = pgm.nextToken(); // consume `;` after init when init wasn't empty
    if ( tn->id() != TokenID::tkSemi )
	pgm.Throw(tn) << "Expecting ';' after for init" << flush;

    tn = pgm.nextToken();
    if ( tn->id() == TokenID::tkSemi )
	condition = new TokenInt(1);
    else
    {
	DBG(cout << "TokenFOR::parse() condition: calling parseExpression(" << (char)tn->get() << ')' << endl);
	if ( !(condition = pgm.parseExpression(tn, true, false, false, 0,
					       /*push_back_comma=*/true)) )
	    pgm.Throw(tn) << "Failed to parse expression" << flush;
	while ( condition && pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkComma )
	{
	    pgm.nextToken(); // consume ','
	    TokenBase *next_tb = pgm.nextToken();
	    if ( !next_tb )
		break;
	    TokenBase *right = pgm.parseExpression(next_tb, true, false, false, 0,
						  /*push_back_comma=*/true);
	    if ( !right )
		break;
	    TokenComma *seq = new TokenComma();
	    seq->left = condition;
	    seq->right = right;
	    seq->file = condition->file;
	    seq->line = condition->line;
	    seq->column = condition->column;
	    condition = seq;
	}
	tn = pgm.nextToken();  // consume ; separator between condition and increment
    }
    if ( tn->id() != TokenID::tkSemi )
	pgm.Throw(tn) << "Expecting ';' after for condition" << flush;
    tn = pgm.nextToken();  // first token of increment expression
    if ( tn->id() == TokenID::tkClBrk )
	increment = NULL;
    else
    {
	DBG(cout << "TokenFOR::parse() increment: calling parseExpression(" << (char)tn->get() << ')' << endl);
	if ( !(increment = pgm.parseExpression(tn, true)) )
	    pgm.Throw(tn) << "Failed to parse increment" << flush;
    }

    // C comma-expression increment: `for (...; ...; i++, j--, k++)`.
    while ( increment && pgm.peekToken() )
    {
	TokenBase *pk = pgm.peekToken();
	if ( pk->id() == TokenID::tkComma ) { pgm.nextToken(); pk = pgm.peekToken(); }
	if ( !pk || pk->id() == TokenID::tkClBrk ) break;
	if ( pk->type() == TokenType::ttSymbol || pk->type() == TokenType::ttKeyword
	  || pk->type() == TokenType::ttDataType ) break;
	tn = pgm.nextToken();
	TokenBase *extra = pgm.parseExpression(tn, true);
	if ( extra ) incr_extras.push_back(extra);
    }

    if ( tn->id() != TokenID::tkClBrk )
	tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkClBrk )
	pgm.Throw(tn) << "Expecting )" << flush;

	    tn = pgm.nextToken();
	    pgm.resetPrevToken();
	    DBG(cout << "TokenFOR::parse() statement(s): calling parseStatement(" << (char)tn->get() << ')' << endl);
	    if ( !(statement = pgm.parseStatement(tn)) )
		pgm.Throw(tn) << "Failed to parse statement" << flush;

    DBG(std::cout << "TokenFOR::parse() END" << std::endl);

    return this;
}

TokenBase *TokenWHILE::parse(Program &pgm)
{
    TokenBase *tn;

    DBG(std::cout << std::endl << "TokenWHILE::parse()" << std::endl);
    DBG(cout << "TokenWHILE::parse() calling parse_parenthesized_expression()" << endl);
    condition = parse_parenthesized_expression(pgm, "while", true);

    tn = pgm.nextToken();
    DBG(cout << "TokenWHILE::parse() calling parseStatement(" << (char)tn->get() << ')' << endl);
    statement = pgm.parseStatement(tn);

    return this;
}

TokenBase *TokenDO::parse(Program &pgm)
{
    TokenBase *tn;

    DBG(std::cout << std::endl << "TokenDO::parse()" << std::endl);
    tn = pgm.nextToken();
    DBG(cout << "TokenDO::parse() calling parseStatement(" << (char)tn->get() << ')' << endl);
    statement = pgm.parseStatement(tn);

    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkWHILE )
    {
	DBG(cerr << "TokenDO::parse() expecting while " << endl);
	pgm.Throw(tn) << "Expecting while after do" << flush;
    }
    DBG(cout << "TokenDO::parse() calling parse_parenthesized_expression()" << endl);
    condition = parse_parenthesized_expression(pgm, "do/while", true);

    return this;
}

// parse operator overload
TokenBase *TokenOPEROVER::parse(Program &pgm)
{
    TokenBase *tn;
    DBG(std::cout << std::endl << "TokenOPEROVER::parse()" << std::endl);
    tn = pgm.nextToken();
    // overload type conversion
    if ( tn->type() == TokenType::ttDataType )
    {
    }
    // overloading operator
    switch(tn->id())
    {
	// multi-token
	case TokenID::tkOpBrk:
	    if ( pgm.peekToken()->id() != TokenID::tkClBrk )
		pgm.Throw(pgm.peekToken()) << "Expecting )" << flush;
	    delete tn;
	    delete pgm.nextToken();
	    str = "()";
	    return this;
	case TokenID::tkOpSqr:
	    if ( pgm.peekToken()->id() != TokenID::tkClSqr )
		pgm.Throw(pgm.peekToken()) << "Expecting ]" << flush;
	    delete tn;
	    delete pgm.nextToken();
	    str = "[]";
	    return this;
	// MultiOp
	case TokenID::tkInc:
	case TokenID::tkDec:
	    str = ((TokenMultiOp *)tn)->str;
	    delete tn;
	    return this;

	// single character
	case TokenID::tkLT:
	case TokenID::tkGT:
	case TokenID::tkAdd:
	case TokenID::tkSub:
	case TokenID::tkMul:
	case TokenID::tkDiv:
	case TokenID::tkMod:
	case TokenID::tkBor:
	case TokenID::tkXor:
	case TokenID::tkBand:
	case TokenID::tkLnot:
	case TokenID::tkBnot:
	case TokenID::tkAssign:
	    str = tn->get();
	    delete tn;
	    return this;
	default:
	    pgm.Throw(tn) << "unexpected token type " << (int)tn->type() << flush;
    }
    return this;
}


TokenBase *TokenREGISTER::parse(Program &pgm)
{
    DBG(std::cout << "TokenREGISTER::parse()" << std::endl);
    TokenBase *tn = pgm.peekToken();
    if ( !tn )
        pgm.Throw << "Unexpected end of input after 'register'" << flush;

    // Accept `register struct ...`, `register TypedefName ...`, and
    // `register <primitive type> ...`. `register` is a C hint — we set
    // vfREGISTER for the primitive path where the Variable is numeric;
    // for struct/typedef paths the flag is dropped (the variable will
    // still live in a register when it's a pointer, which is typical).
    if ( tn->type() == TokenType::ttKeyword )
	return pgm.parseKeyword(static_cast<TokenKeyword *>(pgm.nextToken()));
    if ( tn->type() == TokenType::ttIdentifier )
    {
	std::string tname = ((TokenIdent *)tn)->str;
	if ( is_typeof_identifier(tname) )
	{
	    pgm.nextToken();
	    TokenBase *decl = pgm.parseDeclaration(parse_typeof_datatype(pgm, tn));
	    if ( decl && decl->type() == TokenType::ttDeclare )
		dynamic_cast<TokenDecl *>(decl)->var.flags |= vfREGISTER;
	    return decl;
	}
	datatype_map_iter tdmi = pgm.datatype_map.find(tname);
	if ( tdmi != pgm.datatype_map.end() )
	{
	    pgm.nextToken();
	    TokenBase *decl = pgm.parseDeclaration(tdmi->second);
	    if ( decl && decl->type() == TokenType::ttDeclare )
		dynamic_cast<TokenDecl *>(decl)->var.flags |= vfREGISTER;
	    return decl;
	}
    }
    if ( tn->type() != TokenType::ttDataType )
        pgm.Throw(tn) << "Expecting type after 'register'" << flush;
    tn = pgm.nextToken();
    TokenBase *decl = pgm.parseDeclaration(static_cast<TokenDataType *>(tn));
    if ( decl && decl->type() == TokenType::ttDeclare )
        dynamic_cast<TokenDecl *>(decl)->var.flags |= vfREGISTER;
    return decl;
}

static bool is_contextual_identifier_token(TokenBase *tb)
{
    if ( !tb )
	return false;
    if ( tb->type() == TokenType::ttIdentifier )
	return true;
    if ( tb->type() == TokenType::ttDataType )
    {
	TokenDataType *td = static_cast<TokenDataType *>(tb);
	if ( &td->definition == &ddARRAY )
	    return true;
    }
    if ( tb->type() != TokenType::ttKeyword )
	return false;
    switch ( tb->id() )
    {
	case TokenID::tkCLASS:
	case TokenID::tkNAMESPACE:
	case TokenID::tkUSING:
	case TokenID::tkPREFER:
	case TokenID::tkDEFER:
	// C++ keywords that are valid C identifiers — `int try;`, struct
	// member named `new`, `void *catch_block`, etc.
	case TokenID::tkTRY:
	case TokenID::tkCATCH:
	case TokenID::tkTHROW:
	case TokenID::tkNEW:
	case TokenID::tkDELETE:
	case TokenID::tkOPEROVER:
	    return true;
	default:
	    return false;
    }
}

static std::string contextual_identifier_name(TokenBase *tb)
{
    if ( !tb )
	return "";
    if ( tb->type() == TokenType::ttIdentifier )
	return ((TokenIdent *)tb)->str;
    if ( tb->type() == TokenType::ttDataType )
    {
	TokenDataType *td = static_cast<TokenDataType *>(tb);
	if ( &td->definition == &ddARRAY )
	    return td->str;
    }
    if ( tb->id() == TokenID::tkCLASS
	|| tb->id() == TokenID::tkNAMESPACE
	|| tb->id() == TokenID::tkUSING
	|| tb->id() == TokenID::tkPREFER
	|| tb->id() == TokenID::tkDEFER
	|| tb->id() == TokenID::tkTRY
	|| tb->id() == TokenID::tkCATCH
	|| tb->id() == TokenID::tkTHROW
	|| tb->id() == TokenID::tkNEW
	|| tb->id() == TokenID::tkDELETE
	|| tb->id() == TokenID::tkOPEROVER )
	return ((TokenKeyword *)tb)->str;
    return "";
}

// typedef TYPE alias; or typedef struct/class ... (struct/class detected via prevToken)
TokenBase *TokenTYPEDEF::parse(Program &pgm)
{
    DBG(std::cout << "TokenTYPEDEF::parse()" << std::endl);
    TokenBase *tn = pgm.peekToken();
    if ( !tn )
	pgm.Throw << "Unexpected end of input after 'typedef'" << flush;

    // Record a typedef in source order for the CIR backend and return an
    // AST node so the typedef keeps its position. The legacy JIT/MIR
    // paths ignore the node (compile() is a no-op; MIR re-parses tokens).
    auto record_typedef = [&](const std::string &alias, DataDef *dd,
			      TokenDataType *tdt, TokenBase *otok = nullptr) -> TokenBase * {
	pgm.user_typedef_names.insert(alias);
	if ( tdt && !pgm.current_namespace.empty() )
	    pgm.namespace_datatype_map[pgm.current_namespace][alias] = tdt;
	// A typedef inside a function body is block-scoped: the CIR builder emits
	// it in-place from the returned TokenTypedefDecl in the statement stream
	// (the only correct scope for a VLA typedef whose bound references locals).
	// Only file-scope typedefs are recorded for top-level emission.
	if ( pgm.compounds.empty() )
	{
	    Program::TopDecl td;
	    td.kind = Program::DeclKind::dkTypedef;
	    td.name = alias;
	    td.dd = dd;
	    td.tdt = tdt;
	    td.file = TokenBase::_parse_file;
	    td.line = TokenBase::_parse_line;
	    td.origin = otok;
	    pgm.top_decls.push_back(td);
	}
	return new TokenTypedefDecl(alias, dd);
    };

    // skip const/restrict qualifiers: `typedef const struct X *const_ptr;`
    while ( tn && (tn->id() == TokenID::tkCONST
		|| tn->id() == TokenID::tkRESTRICT
		|| tn->id() == TokenID::tkVOLATILE) )
    {
	pgm.nextToken();
	tn = pgm.peekToken();
    }

    // typedef struct/union/class ... — handled by struct/class parse via prevToken
    // Set parsing_typedef_decl so struct/class parser knows to register a typedef
    // even when const/restrict qualifiers separate typedef from struct keyword
    if ( tn->id() == TokenID::tkSTRUCT || tn->id() == TokenID::tkCLASS || tn->id() == TokenID::tkUNION )
    {
	pgm.parsing_typedef_decl = true;
	TokenBase *result = pgm.parseKeyword(static_cast<TokenKeyword *>(pgm.nextToken()));
	pgm.parsing_typedef_decl = false;
	return result;
    }
    if ( tn->id() == TokenID::tkENUM )
    {
	pgm.nextToken(); // consume enum

	// optional tag name: typedef enum Tag { ... } Alias;
	tn = pgm.peekToken();
	if ( tn && tn->type() == TokenType::ttIdentifier )
	    pgm.nextToken();

	// optional body: typedef enum { ... } Alias;
	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrc )
	{
	    TokenENUM tenum;
	    tenum.parse(pgm);
	}

	tn = pgm.nextToken();
	if ( !tn )
	    pgm.Throw << "Unexpected end of input in typedef enum" << flush;

	std::string alias;
	if ( tn->type() == TokenType::ttIdentifier )
	    alias = ((TokenIdent *)tn)->str;
	else if ( tn->type() == TokenType::ttDataType )
	    alias = ((TokenDataType *)tn)->str;
	else if ( tn->type() == TokenType::ttKeyword )
	    alias = ((TokenKeyword *)tn)->str;
	else
	    pgm.Throw(tn) << "Expecting alias name in typedef enum" << flush;

	DataDef *enum_alias_dd = new DataDefENUM(alias);
	TokenDataType *tdt = new TokenDataType(alias.c_str(), *enum_alias_dd);
	pgm.datatype_map[alias] = tdt;
	DBG(std::cout << "TokenTYPEDEF::parse() enum alias " << alias << " = int" << std::endl);

	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
	    pgm.nextToken();
	return record_typedef(alias, enum_alias_dd, tdt);
    }

    // typedef TYPE alias; — primitive type alias
    DataDef *base_dd = NULL;
    if ( tn->type() == TokenType::ttDataType )
    {
	base_dd = &((TokenDataType *)pgm.nextToken())->definition;
    }
    else if ( tn->type() == TokenType::ttIdentifier )
    {
	std::string tname = ((TokenIdent *)tn)->str;
	datatype_map_iter tdmi = pgm.datatype_map.find(tname);
	if ( tdmi != pgm.datatype_map.end() )
	{
	    pgm.nextToken();
	    base_dd = &tdmi->second->definition;
	}
	else
	{
	    // A qualified and/or templated base type — `typedef
	    // __cxx11::basic_string<char,...> string;`, `typedef ns::T<X> alias;`.
	    // `tn` here is a PEEK (still at the deque front, like the datatype-map
	    // branch above which consumes it explicitly), so consume it first, then
	    // let the unified resolver skip the namespace-qualifier chain and
	    // instantiate the template (consuming `<...>`). This lets a typedef
	    // alias any composed type, not just a bare datatype_map name.
	    pgm.nextToken(); // consume the first type token (tn), aligning the deque
	    if ( TokenDataType *rt = resolve_declared_type_token(pgm, tn, true, true) )
		base_dd = &rt->definition;
	}
    }
    if ( !base_dd )
	pgm.Throw(tn) << "Expecting type after 'typedef'" << flush;

    // handle pointer: typedef int *intptr;
    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkMul )
    {
	pgm.nextToken();
	base_dd = pgm.getPointerType(base_dd);
    }

    std::string gnu_mode_name;
    size_t gnu_vector_bytes = 0;
    if ( is_attribute_identifier_token(pgm.peekToken()) )
    {
	consume_typedef_gnu_attributes(pgm, &gnu_mode_name, &gnu_vector_bytes);
	base_dd = apply_gnu_mode_alias(base_dd, gnu_mode_name);
    }

    // Function-pointer typedef Form 2: typedef RET (*NAME)(params);
    TokenBase *peek = pgm.peekToken();
    if ( peek && peek->id() == TokenID::tkOpBrk )
    {
	pgm.nextToken(); // consume '('
	TokenBase *star = pgm.nextToken();
	if ( !star || star->id() != TokenID::tkMul )
	    pgm.Throw(star ? star : tn) << "Expecting '*' in function pointer typedef" << flush;
	TokenBase *name_tok = pgm.nextToken();
	if ( !name_tok || name_tok->type() != TokenType::ttIdentifier )
	    pgm.Throw(name_tok ? name_tok : tn) << "Expecting identifier in function pointer typedef" << flush;
	std::string alias = ((TokenIdent *)name_tok)->str;
	TokenBase *rbrk = pgm.nextToken();
	if ( !rbrk || rbrk->id() != TokenID::tkClBrk )
	    pgm.Throw(rbrk ? rbrk : tn) << "Expecting ')' after function pointer name" << flush;
	TokenBase *open = pgm.nextToken();
	if ( !open || open->id() != TokenID::tkOpBrk )
	    pgm.Throw(open ? open : tn) << "Expecting '(' for parameter list" << flush;
	FuncDef *func = pgm.parseFnPtrParams(*base_dd);
	DataDefFPTR *fptr = new DataDefFPTR(func);
	fptr->ptr_syntax = true;   // Form 2: explicit `(*NAME)` — pointer typedef
	TokenDataType *tdt = new TokenDataType(alias.c_str(), *fptr);
	pgm.datatype_map[alias] = tdt;
	DBG(std::cout << "TokenTYPEDEF::parse() fptr (form 2): " << alias << std::endl);
	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
	    pgm.nextToken();
	return record_typedef(alias, fptr, tdt);
    }

    // get alias name (may be an identifier or an existing type name being redefined)
    tn = pgm.nextToken();
    TokenBase *alias_tok = tn;   // per-occurrence alias token (CIR origin)
    std::string alias;
    if ( tn->type() == TokenType::ttIdentifier )
	alias = ((TokenIdent *)tn)->str;
    else if ( tn->type() == TokenType::ttDataType )
	alias = ((TokenDataType *)tn)->str;
    else if ( tn->type() == TokenType::ttKeyword )
	alias = ((TokenKeyword *)tn)->str;
    else
	pgm.Throw(tn) << "Expecting alias name in typedef" << flush;

    if ( is_attribute_identifier_token(pgm.peekToken()) )
	gnu_vector_bytes = parse_gnu_vector_size_attribute(pgm);

    // Function-pointer typedef Form 1: typedef RET NAME(params);
    TokenBase *post = pgm.peekToken();
    if ( post && post->id() == TokenID::tkOpBrk )
    {
	pgm.nextToken(); // consume '('
	FuncDef *func = pgm.parseFnPtrParams(*base_dd);
	DataDefFPTR *fptr = new DataDefFPTR(func);
	fptr->ptr_syntax = false;  // Form 1: `typedef RET NAME(params)` — function typedef
	TokenDataType *tdt = new TokenDataType(alias.c_str(), *fptr);
	pgm.datatype_map[alias] = tdt;
	DBG(std::cout << "TokenTYPEDEF::parse() fptr (form 1): " << alias << std::endl);
	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
	    pgm.nextToken();
	return record_typedef(alias, fptr, tdt);
    }

    // Preserve typedef'd array shape so sizeof(NAME) can reflect the real
    // array extent instead of collapsing immediately to a pointer.
    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpSqr )
    {
	size_t alias_count = 1;
	TokenBase *alias_count_expr = NULL;
	while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpSqr )
	{
	    pgm.nextToken(); // consume '['
	    TokenBase *cl = pgm.nextToken();
	    if ( cl && cl->id() == TokenID::tkClSqr )
	    {
		alias_count = 0;
		continue;
	    }
	    pgm.pushToken(cl);
	    if ( !alias_count_expr && bracket_dim_needs_runtime_value(pgm) )
	    {
		alias_count_expr = pgm.parseExpression(pgm.nextToken(), true);
		cl = pgm.nextToken();
		if ( !cl || cl->id() != TokenID::tkClSqr )
		    pgm.Throw(cl ? cl : tn) << "Expected ] in typedef array declaration" << flush;
		continue;
	    }
	    int64_t n = parse_constant_integer_expression(pgm);
	    if ( n < 0 )
		pgm.Throw(tn) << "Typedef array dimension must be non-negative" << flush;
	    cl = pgm.nextToken();
	    if ( !cl || cl->id() != TokenID::tkClSqr )
		pgm.Throw(cl ? cl : tn) << "Expected ] in typedef array declaration" << flush;
	    if ( n == 0 )
		alias_count = 0;
	    else
		alias_count *= (size_t)n;
	}
	base_dd = new DataDefCArray(*base_dd, alias, alias_count, alias_count_expr);
    }

    // register in datatype_map
    if ( gnu_vector_bytes > 0 )
	base_dd = new DataDefSIMD(base_dd, alias, gnu_vector_bytes);
    TokenDataType *tdt = new TokenDataType(alias.c_str(), *base_dd);
    pgm.datatype_map[alias] = tdt;
    DBG(std::cout << "TokenTYPEDEF::parse() " << alias << " = " << base_dd->name << std::endl);

    // consume semicolon
    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
	pgm.nextToken();

    return record_typedef(alias, base_dd, tdt, alias_tok);
}

// Parse a function-pointer parameter list. The opening '(' has already been
// consumed by the caller. Stops after consuming the closing ')'. Parameter
// names are optional and discarded — typedef signatures don't bind names.
FuncDef *Program::parseFnPtrParams(DataDef &returns)
{
    FuncDef *func = new FuncDef(returns);
    TokenBase *nt = nextToken();
    if ( !nt )
	Throw << "Unexpected end of input in function pointer typedef" << flush;

    // empty param list: ()
    if ( nt->id() == TokenID::tkClBrk )
	return func;

    // (void) as sole parameter = no parameters
    if ( nt->type() == TokenType::ttDataType
      && &((TokenDataType *)nt)->definition == &ddVOID
      && peekToken() && peekToken()->id() == TokenID::tkClBrk )
    {
	func->is_void_params = true;
	nextToken(); // consume ')'
	return func;
    }

    while ( nt )
    {
	std::string param_alias;
	while ( nt && nt->id() == TokenID::tkCONST )
	    nt = nextToken();

	// variadic
	if ( nt->id() == TokenID::tkDot )
	{
	    TokenBase *d2 = nextToken();
	    TokenBase *d3 = nextToken();
	    if ( !d2 || d2->id() != TokenID::tkDot || !d3 || d3->id() != TokenID::tkDot )
		Throw(nt) << "Expecting '...' for variadic parameter" << flush;
	    func->is_varargs = true;
	    func->parameters.push_back(&ddINT64);
	    func->param_typedef_names.push_back("");
	    nt = nextToken();
	    if ( !nt || nt->id() != TokenID::tkClBrk )
		Throw(nt) << "Expecting ')' after '...'" << flush;
	    return func;
	}

	// Resolve parameter type: plain type, struct Tag, or typedef alias
	DataDef *param_dd = NULL;
	if ( nt->id() == TokenID::tkSTRUCT )
	{
	    TokenBase *tag = nextToken();
	    if ( !tag || tag->type() != TokenType::ttIdentifier )
		Throw(tag ? tag : nt) << "Expecting struct name after 'struct'" << flush;
	    std::string sname = ((TokenIdent *)tag)->str;
	    datadef_map_iter sdmi = struct_map.find(sname);
	    if ( sdmi == struct_map.end() )
	    {
		DataDefSTRUCT *fwd = new DataDefSTRUCT(sname, 0);
		struct_map[sname] = fwd;
		sdmi = struct_map.find(sname);
	    }
	    param_dd = sdmi->second;
	}
	else if ( nt->type() == TokenType::ttDataType )
	{
	    param_dd = &((TokenDataType *)nt)->definition;
	    if ( user_typedef_names.count(((TokenDataType *)nt)->str) )
		param_alias = ((TokenDataType *)nt)->str;
	}
	else if ( nt->type() == TokenType::ttIdentifier )
	{
	    std::string tname = ((TokenIdent *)nt)->str;
	    datatype_map_iter tdmi = datatype_map.find(tname);
	    if ( tdmi == datatype_map.end() )
		Throw(nt) << "Unknown type '" << tname << "' in function pointer typedef" << flush;
	    param_dd = &tdmi->second->definition;
	    if ( user_typedef_names.count(tname) )
		param_alias = tname;
	}
	else
	{
	    Throw(nt) << "Expecting parameter type in function pointer typedef" << flush;
	}

	// Pointer decorators
	while ( peekToken() && peekToken()->id() == TokenID::tkMul )
	{
	    nextToken();
	    param_dd = getPointerType(param_dd);
	}

	while ( peekToken() && peekToken()->id() == TokenID::tkCONST )
	    nextToken();

	// Optional parameter name (discard)
	if ( peekToken() && is_contextual_identifier_token(peekToken()) )
	    nextToken();

	func->parameters.push_back(param_dd);
	func->param_typedef_names.push_back(param_alias);

	// Next: ',' or ')'
	nt = nextToken();
	if ( !nt )
	    Throw << "Unexpected end of input in function pointer typedef" << flush;
	if ( nt->id() == TokenID::tkClBrk )
	    return func;
	if ( nt->id() != TokenID::tkComma )
	    Throw(nt) << "Expecting ',' or ')' in function pointer typedef" << flush;
	nt = nextToken(); // next parameter
    }

    Throw << "Unexpected end of input in function pointer typedef" << flush;
    return NULL; // unreachable
}

// parse enum { NAME, NAME = val, ... } [;]
// registers each enumerator as a #define constant
TokenBase *TokenENUM::parse(Program &pgm)
{
    DBG(std::cout << "TokenENUM::parse()" << std::endl);
    TokenBase *tn = pgm.peekToken();

    // C++11 scoped enum: `enum class Tag { ... }` / `enum struct Tag { ... }`.
    // Gated to C++/madc modes (I4): in C mode `class` is not a keyword (it
    // lexes as an identifier), so this branch never fires for C and the plain
    // C enum behavior below is preserved unchanged. The scoped flag changes
    // ONLY where enumerators are registered (scoped, not bare) and that the
    // tag itself becomes a usable type name.
    bool scoped = false;
    if ( tn && !pgm.is_c_mode()
      && (tn->id() == TokenID::tkCLASS || tn->id() == TokenID::tkSTRUCT) )
    {
	scoped = true;
	pgm.nextToken(); // consume 'class' / 'struct'
	tn = pgm.peekToken();
    }

    // optional tag name: enum colors { ... }
    std::string enum_tag;
    if ( tn && tn->type() == TokenType::ttIdentifier )
    {
	enum_tag = ((TokenIdent *)tn)->str;
	pgm.nextToken(); // consume tag name
    }

    // optional underlying type: `enum class Tag : int { ... }`. We lower a
    // scoped enum to a plain C enum / int (I2), so the underlying type only
    // affects storage width; parse and discard the type tokens here.
    tn = pgm.peekToken();
    if ( tn && tn->id() == TokenID::tkTerC )
    {
	pgm.nextToken(); // consume ':'
	// skip the underlying-type tokens up to '{'
	while ( (tn = pgm.peekToken())
	     && tn->id() != TokenID::tkOpBrc
	     && tn->id() != TokenID::tkSemi )
	    pgm.nextToken();
    }

    tn = pgm.peekToken();
    if ( !tn || tn->id() != TokenID::tkOpBrc )
    {
	// No '{' — this is a forward reference like `enum X var;`
	// Treat enum as int and let the caller parse the variable declaration
	if ( !enum_tag.empty() )
	{
	    // Scoped enum used as a type name resolves to its registered
	    // DataDefENUM; a plain enum forward-reference is just int.
	    if ( scoped )
	    {
		datatype_map_iter dti = pgm.datatype_map.find(enum_tag);
		if ( dti != pgm.datatype_map.end() )
		{
		    pgm.pushToken(dti->second);
		    return NULL;
		}
	    }
	    pgm.pushToken(new TokenDataType("int", ddINT));
	    return NULL;
	}
	pgm.Throw(tn) << "Expecting '{' after enum" << flush;
    }
    pgm.nextToken(); // consume '{'

    // A scoped enum's tag is itself a usable type name (no typedef needed):
    // register `datatype_map[Tag]` as a DataDefENUM (int-backed, I2).
    if ( scoped && !enum_tag.empty() )
    {
	DataDef *enum_dd = new DataDefENUM(enum_tag);
	pgm.datatype_map[enum_tag] = new TokenDataType(enum_tag.c_str(), *enum_dd);
	DBG(std::cout << "TokenENUM::parse() scoped enum type " << enum_tag << std::endl);
    }

    // Scoped enumerators are registered under a pseudo-namespace keyed by the
    // tag (Tag::Value), reusing the existing namespace_map resolution path so
    // `Tag::Value` resolves and the bare name does NOT leak (the C++ scoped
    // semantic). I6: no parallel scoped-naming machinery.
    variable_map_t *scope_ns = scoped ? &pgm.namespace_map[enum_tag] : NULL;

    int64_t val = 0;
    while ( (tn = pgm.peekToken()) && tn->id() != TokenID::tkClBrc )
    {
	if ( tn->id() == TokenID::tkComma ) { pgm.nextToken(); continue; }
	if ( !is_contextual_identifier_token(tn) )
	    pgm.Throw(tn) << "Expecting identifier in enum" << flush;
	std::string name = contextual_identifier_name(pgm.nextToken());

	// check for = explicit value
	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkAssign )
	{
	    pgm.nextToken(); // consume '='
	    val = parse_constant_integer_expression(pgm);
	}

	if ( scoped )
	{
	    // Scoped: a standalone constant Variable kept ONLY in the tag's
	    // pseudo-namespace, so bare `name` does not resolve globally.
	    Variable *evar = new Variable(name, ddINT, 1, NULL, true);
	    evar->set((int)val);
	    evar->makeconstant();
	    (*scope_ns)[name] = evar;
	    DBG(std::cout << "TokenENUM::parse() " << enum_tag << "::" << name
		<< " = " << val << std::endl);
	}
	else
	{
	    // register as a global constant variable (plain C enum)
	    Variable *evar = pgm.addVariable(NULL, ddINT, name, 1, NULL, true);
	    evar->set((int)val);
	    evar->makeconstant();
	    DBG(std::cout << "TokenENUM::parse() " << name << " = " << val << std::endl);
	}
	val++;
    }

    if ( !tn )
	pgm.Throw << "Unterminated enum" << flush;
    pgm.nextToken(); // consume '}'

    // consume optional semicolon
    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
	pgm.nextToken();

    return NULL;
}

TokenBase *TokenSTATIC::parse(Program &pgm)
{
    DBG(std::cout << "TokenSTATIC::parse()" << std::endl);
    TokenBase *tn = pgm.peekToken();
    if ( !tn )
	pgm.Throw << "Unexpected end of input after 'static'" << flush;
    // static can appear before other modifiers: static const, static struct, etc.
    // Set parsing_static_decl so the inner parseDeclaration call (which may
    // be reached via parseKeyword → TokenSTRUCT::parse → parseDeclaration
    // for `static struct X x;` or via parseDeclaration directly for plain
    // primitives) knows to mark the variable static and allocate persistent
    // storage. is_static parameter on parseDeclaration only covers the
    // direct path; the keyword-routed path needs the flag.
    bool prev_static = pgm.parsing_static_decl;
    pgm.parsing_static_decl = true;
    TokenBase *result = nullptr;
    if ( tn->type() == TokenType::ttKeyword )
	result = pgm.parseKeyword(static_cast<TokenKeyword *>(pgm.nextToken()));
    else if ( tn->type() != TokenType::ttDataType )
    {
	// might be a typedef'd identifier
	if ( tn->type() == TokenType::ttIdentifier )
	{
	    std::string tname = ((TokenIdent *)tn)->str;
	    if ( is_typeof_identifier(tname) )
	    {
		pgm.nextToken();
		result = pgm.parseDeclaration(parse_typeof_datatype(pgm, tn), true);
	    }
	    else
	    {
	    datatype_map_iter tdmi = pgm.datatype_map.find(tname);
	    if ( tdmi != pgm.datatype_map.end() )
	    {
		pgm.nextToken();
		result = pgm.parseDeclaration(tdmi->second, true);
	    }
	    else
	    {
		// C89 implicit int: `static funcname(...)` — treat as int
		TokenBase *id_tok = pgm.nextToken();
		TokenBase *peek2 = pgm.peekToken();
		pgm.pushToken(id_tok);
		if ( peek2 && peek2->id() == TokenID::tkOpBrk )
		{
		    TokenDataType tdt("int", ddINT);
		    result = pgm.parseDeclaration(&tdt, true);
		}
		else
		    pgm.Throw(tn) << "Expecting type after 'static'" << flush;
	    }
	    }
	}
	else
	    pgm.Throw(tn) << "Expecting type after 'static'" << flush;
    }
    else
    {
	tn = pgm.nextToken();
	result = pgm.parseDeclaration(static_cast<TokenDataType *>(tn), true);
    }
    pgm.parsing_static_decl = prev_static;
    return result;
}

// const — consume and pass through to the type that follows
TokenBase *TokenCONST::parse(Program &pgm)
{
    DBG(std::cout << "TokenCONST::parse() — consuming const" << std::endl);
    pgm.parsing_const_decl = true;
    TokenBase *tn = pgm.peekToken();
    if ( !tn )
	pgm.Throw << "Unexpected end of input after 'const'" << flush;
    if ( tn->type() == TokenType::ttKeyword )
	return pgm.parseKeyword(static_cast<TokenKeyword *>(pgm.nextToken()));
    if ( tn->type() == TokenType::ttDataType )
    {
	tn = pgm.nextToken();
	return pgm.parseDeclaration(static_cast<TokenDataType *>(tn));
    }
    // might be typedef'd name
    if ( tn->type() == TokenType::ttIdentifier )
    {
	std::string tname = ((TokenIdent *)tn)->str;
	if ( is_typeof_identifier(tname) )
	{
	    pgm.nextToken();
	    return pgm.parseDeclaration(parse_typeof_datatype(pgm, tn));
	}
	datatype_map_iter tdmi = pgm.datatype_map.find(tname);
	if ( tdmi != pgm.datatype_map.end() )
	{
	    pgm.nextToken();
	    return pgm.parseDeclaration(tdmi->second);
	}
    }
    pgm.Throw(tn) << "Expecting type after 'const'" << flush;
    return NULL;
}

// extern — consume and pass through to the declaration that follows
TokenBase *TokenEXTERN::parse(Program &pgm)
{
    DBG(std::cout << "TokenEXTERN::parse() — consuming extern" << std::endl);
    TokenBase *tn = pgm.peekToken();
    if ( !tn )
	pgm.Throw << "Unexpected end of input after 'extern'" << flush;
    bool prev_extern = pgm.parsing_extern_decl;
    pgm.parsing_extern_decl = true;
    TokenBase *result = NULL;
    bool handled = false;
    try
    {
	if ( tn->type() == TokenType::ttString )
	{
	    TokenStr *linkage = static_cast<TokenStr *>(pgm.nextToken());
	    if ( linkage->str != "C" && linkage->str != "C++" )
		pgm.Throw(linkage) << "Unsupported extern linkage '" << linkage->str << "'" << flush;
	    TokenBase *after = pgm.nextToken();
	    if ( !after )
		pgm.Throw(linkage) << "Unexpected end of input after extern linkage" << flush;
	    handled = true;
	    if ( after->id() == TokenID::tkOpBrc )
	    {
		while ( (tn = pgm.nextToken()) )
		{
		    if ( tn->id() == TokenID::tkClBrc )
			break;
		    if ( tn->id() == TokenID::tkSemi )
			continue;
		    TokenBase *stmt = pgm.parseStatement(tn);
		    if ( stmt && pgm.tkProgram )
			pgm.tkProgram->statements.push_back((TokenStmt *)stmt);
		}
		if ( !tn )
		    pgm.Throw(linkage) << "Missing '}' after extern linkage block" << flush;
	    }
	    else
		result = pgm.parseStatement(after);
	}
	else if ( tn->type() == TokenType::ttKeyword )
	{
	    handled = true;
	    result = pgm.parseKeyword(static_cast<TokenKeyword *>(pgm.nextToken()));
	}
	else if ( tn->type() == TokenType::ttDataType )
	{
	    handled = true;
	    tn = pgm.nextToken();
	    result = pgm.parseDeclaration(static_cast<TokenDataType *>(tn));
	}
	else if ( tn->type() == TokenType::ttIdentifier )
	{
	    std::string tname = ((TokenIdent *)tn)->str;
	    if ( is_typeof_identifier(tname) )
	    {
		handled = true;
		pgm.nextToken();
		result = pgm.parseDeclaration(parse_typeof_datatype(pgm, tn));
	    }
	    else
	    {
	    datatype_map_iter tdmi = pgm.datatype_map.find(tname);
	    if ( tdmi != pgm.datatype_map.end() )
	    {
		handled = true;
		pgm.nextToken();
		result = pgm.parseDeclaration(tdmi->second);
	    }
	    }
	}
	if ( !handled )
	    pgm.Throw(tn) << "Expecting type after 'extern'" << flush;
    }
    catch ( ... )
    {
	pgm.parsing_extern_decl = prev_extern;
	throw;
    }
    pgm.parsing_extern_decl = prev_extern;
    return result;
}

TokenBase *TokenRESTRICT::parse(Program &pgm)
{
    DBG(std::cout << "TokenRESTRICT::parse() — consuming restrict" << std::endl);
    TokenBase *tn = pgm.peekToken();
    if ( !tn )
	pgm.Throw << "Unexpected end of input after 'restrict'" << flush;
    if ( tn->type() == TokenType::ttKeyword )
	return pgm.parseKeyword(static_cast<TokenKeyword *>(pgm.nextToken()));
    if ( tn->type() == TokenType::ttDataType )
    {
	tn = pgm.nextToken();
	return pgm.parseDeclaration(static_cast<TokenDataType *>(tn));
    }
    if ( tn->type() == TokenType::ttIdentifier )
    {
	std::string tname = ((TokenIdent *)tn)->str;
	if ( is_typeof_identifier(tname) )
	{
	    pgm.nextToken();
	    return pgm.parseDeclaration(parse_typeof_datatype(pgm, tn));
	}
	datatype_map_iter tdmi = pgm.datatype_map.find(tname);
	if ( tdmi != pgm.datatype_map.end() )
	{
	    pgm.nextToken();
	    return pgm.parseDeclaration(tdmi->second);
	}
    }
    pgm.Throw(tn) << "Expecting type after 'restrict'" << flush;
    return NULL;
}

TokenBase *TokenVOLATILE::parse(Program &pgm)
{
    DBG(std::cout << "TokenVOLATILE::parse() — consuming volatile" << std::endl);
    TokenBase *tn = pgm.peekToken();
    if ( !tn )
	pgm.Throw << "Unexpected end of input after 'volatile'" << flush;
    if ( tn->type() == TokenType::ttKeyword )
	return pgm.parseKeyword(static_cast<TokenKeyword *>(pgm.nextToken()));
    if ( tn->type() == TokenType::ttDataType )
    {
	tn = pgm.nextToken();
	return pgm.parseDeclaration(static_cast<TokenDataType *>(tn));
    }
    if ( tn->type() == TokenType::ttIdentifier )
    {
	std::string tname = ((TokenIdent *)tn)->str;
	if ( is_typeof_identifier(tname) )
	{
	    pgm.nextToken();
	    return pgm.parseDeclaration(parse_typeof_datatype(pgm, tn));
	}
	datatype_map_iter tdmi = pgm.datatype_map.find(tname);
	if ( tdmi != pgm.datatype_map.end() )
	{
	    pgm.nextToken();
	    return pgm.parseDeclaration(tdmi->second);
	}
    }
    pgm.Throw(tn) << "Expecting type after 'volatile'" << flush;
    return NULL;
}

TokenBase *TokenDEFER::parse(Program &pgm)
{
    DBG(std::cout << "TokenDEFER::parse()" << std::endl);

    TokenCpnd *code = pgm.compounds.empty() ? NULL : pgm.compounds.top();
    if ( !code )
	pgm.Throw(this) << "'defer' must be inside a function or block" << flush;

    // parse the deferred statement
    TokenBase *tn = pgm.nextToken();
    if ( !tn )
	pgm.Throw(this) << "Unexpected end of input after 'defer'" << flush;

    TokenBase *stmt = pgm.parseStatement(tn);
    if ( !stmt )
	pgm.Throw(tn) << "Failed to parse deferred statement" << flush;

    // store on the enclosing compound — compiled in reverse order at scope exit
    code->deferred.push_back(stmt);

    DBG(std::cout << "TokenDEFER::parse() deferred statement added (total: " << code->deferred.size() << ")" << std::endl);

    // return NULL — defer doesn't produce code at this point
    return NULL;
}

// try { ... } catch (type var) { ... }
TokenBase *TokenTRY::parse(Program &pgm)
{
    DBG(std::cout << "TokenTRY::parse()" << std::endl);

    // Parse try body: must be { ... }
    TokenBase *tn = pgm.peekToken();
    if ( !tn || tn->id() != TokenID::tkOpBrc )
	pgm.Throw(tn ? tn : this) << "Expected '{' after 'try'" << flush;
    pgm.nextToken(); // consume '{'
    pgm.pushCompound();
    try_body = pgm.parseCompound();

    // Parse one or more catch clauses
    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkCATCH )
    {
	pgm.nextToken(); // consume 'catch'
	tn = pgm.nextToken();
	if ( !tn || tn->id() != TokenID::tkOpBrk )
	    pgm.Throw(tn ? tn : this) << "Expected '(' after 'catch'" << flush;

	// Parse catch parameter: (type var) or (...)
	tn = pgm.peekToken();
	if ( tn && tn->id() == TokenID::tkDot )
	{
	    // catch(...) — three dot tokens
	    pgm.nextToken(); // consume first '.'
	    tn = pgm.nextToken();
	    if ( !tn || tn->id() != TokenID::tkDot )
		pgm.Throw(tn) << "Expected '...' in catch" << flush;
	    tn = pgm.nextToken();
	    if ( !tn || tn->id() != TokenID::tkDot )
		pgm.Throw(tn) << "Expected '...' in catch" << flush;
	    catch_types.push_back(99); // MADC_EXCEPT_ANY
	    catch_varnames.push_back("");
	}
	else
	{
	    // catch(type var)
	    TokenDataType *ctype = resolve_declared_type_token(pgm, tn, true, true);
	    if ( !ctype )
		pgm.Throw(tn) << "Expected type in catch parameter" << flush;
	    pgm.nextToken(); // consume type

	    // Determine exception type tag
	    int tag = 99; // default: any
	    DataType dt = ctype->definition.rawtype();
	    if ( dt == DataType::dtINT || dt == DataType::dtINT32
	      || dt == DataType::dtINT64 )
		tag = 1; // MADC_EXCEPT_INT
	    else if ( dt == DataType::dtDOUBLE || dt == DataType::dtFLOAT )
		tag = 2; // MADC_EXCEPT_DOUBLE
	    else if ( dt == DataType::dtCHAR )
		tag = 3; // MADC_EXCEPT_CSTR (char* conceptually)
	    catch_types.push_back(tag);

	    // Variable name
	    tn = pgm.nextToken();
	    if ( tn && tn->type() == TokenType::ttIdentifier )
		catch_varnames.push_back(((TokenIdent *)tn)->str);
	    else
	    {
		catch_varnames.push_back("");
		if ( tn ) pgm.pushToken(tn);
	    }
	}

	tn = pgm.nextToken();
	if ( !tn || tn->id() != TokenID::tkClBrk )
	    pgm.Throw(tn ? tn : this) << "Expected ')' after catch parameter" << flush;

	// Parse catch body — create catch variable in scope first
	tn = pgm.peekToken();
	if ( !tn || tn->id() != TokenID::tkOpBrc )
	    pgm.Throw(tn ? tn : this) << "Expected '{' after catch(...)" << flush;
	pgm.nextToken(); // consume '{'
	pgm.pushCompound();
	// Add catch variable to the catch body's scope
	if ( !catch_varnames.back().empty() )
	{
	    TokenCpnd *catch_scope = pgm.compounds.empty() ? NULL : pgm.compounds.top();
	    DataDef *cv_type = &ddINT64; // default
	    if ( catch_types.back() == 2 ) cv_type = &ddDOUBLE;
	    pgm.addVariable(catch_scope, *cv_type, catch_varnames.back(), 1, NULL, false);
	}
	catch_bodies.push_back(pgm.parseCompound());
    }

    if ( catch_bodies.empty() )
	pgm.Throw(this) << "try without catch" << flush;

    return this;
}

// throw expr — throws an exception
TokenBase *TokenTHROW::parse(Program &pgm)
{
    DBG(std::cout << "TokenTHROW::parse()" << std::endl);

    // Check for throw; (rethrow, no expression)
    TokenBase *tn = pgm.peekToken();
    if ( tn && tn->id() == TokenID::tkSemi )
    {
	throw_expr = NULL; // rethrow
	return this;
    }

    // Parse the throw expression
    tn = pgm.nextToken();
    throw_expr = pgm.parseExpression(tn, true);
    // Consume trailing semicolon
    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
	pgm.nextToken();
    return this;
}

// new ClassName(args) — heap allocation + constructor call
TokenBase *TokenNEW::parse(Program &pgm)
{
    DBG(std::cout << "TokenNEW::parse()" << std::endl);

    // Placement new: `new (addr) Type(args)` constructs at `addr` instead of
    // allocating. A leading '(' after `new` is the placement-argument list.
    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrk )
    {
	pgm.nextToken(); // consume '('
	placement = pgm.parseExpression(pgm.nextToken(), true);
	if ( !placement )
	    pgm.Throw(this) << "Failed to parse placement-new address expression" << flush;
	if ( !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkClBrk )
	    pgm.Throw(this) << "Expected ')' after placement-new address" << flush;
	pgm.nextToken(); // consume ')'
    }

    TokenBase *tn = pgm.nextToken();
    if ( !tn )
	pgm.Throw(this) << "Expected type after 'new'" << flush;

    // Resolve the constructed type. A class -> alloc_class (the real
    // DataDefCLASS pointer); any other type (string, scalar) -> alloc_type.
    // TokenDataType::definition is a reference, so &definition is the real
    // object and dynamic_cast recovers a class.
    TokenDataType *tdt = resolve_declared_type_token(pgm, tn, true, true);
    if ( tdt )
    {
	if ( DataDefCLASS *c = dynamic_cast<DataDefCLASS *>(&tdt->definition) )
	    alloc_class = c;
	else
	    alloc_type = &tdt->definition;
    }
    else
    {
	// Fall back to a direct class lookup (forward-referenced class names
	// not yet in datatype_map).
	if ( tn->type() != TokenType::ttIdentifier )
	    pgm.Throw(tn) << "Expected type after 'new'" << flush;
	std::string class_name = ((TokenIdent *)tn)->str;
	datadef_map_iter dmi = pgm.struct_map.find(class_name);
	if ( dmi == pgm.struct_map.end() )
	    pgm.Throw(tn) << "Unknown type '" << class_name << "' in new expression" << flush;
	alloc_class = dynamic_cast<DataDefCLASS *>(dmi->second);
	if ( !alloc_class )
	    pgm.Throw(tn) << "'" << class_name << "' is not a class type" << flush;
    }

    // Parse constructor arguments: (arg1, arg2, ...)
    tn = pgm.peekToken();
    if ( tn && tn->id() == TokenID::tkOpBrk )
    {
	pgm.nextToken(); // consume '('
	while ( pgm.peekToken() && pgm.peekToken()->id() != TokenID::tkClBrk )
	{
	    TokenBase *arg = pgm.parseExpression(pgm.nextToken(), true);
	    ctor_args.push_back(arg);
	    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkComma )
		pgm.nextToken(); // consume ','
	}
	if ( !pgm.peekToken() || pgm.peekToken()->id() != TokenID::tkClBrk )
	    pgm.Throw(this) << "Expected ')' after new-expression arguments" << flush;
	pgm.nextToken(); // consume ')'
    }

    return this;
}

// delete expr — destructor call + free
TokenBase *TokenDELETE::parse(Program &pgm)
{
    DBG(std::cout << "TokenDELETE::parse()" << std::endl);

    TokenBase *tn = pgm.nextToken();
    if ( !tn )
	pgm.Throw(this) << "Expected expression after 'delete'" << flush;

    expr = pgm.parseExpression(tn, true);
    if ( !expr )
	pgm.Throw(tn) << "Failed to parse expression after 'delete'" << flush;

    // Try to determine the class type from the expression's pointer type
    DataDef *dd = expr->datadef();
    if ( dd && dd->is_pointer() )
    {
	DataDefPTR *ptr = dynamic_cast<DataDefPTR *>(dd);
	if ( ptr )
	    del_class = dynamic_cast<DataDefCLASS *>(ptr->base_type);
    }

    return this;
}

// parse switch(expr) { case val: ...; break; default: ...; }
static TokenCASE *parse_switch_label(Program &pgm, TokenSWITCH *sw, TokenBase *tn)
{
    TokenCASE *target = NULL;

    if ( tn->id() == TokenID::tkCASE )
    {
	TokenCASE *tc = new TokenCASE();
	tc->file = tn->file;
	tc->line = tn->line;
	tc->column = tn->column;
	TokenBase *val_anchor = pgm.peekToken();
	int64_t case_val = parse_constant_integer_expression(pgm);
	TokenInt *val_tok = new TokenInt(case_val);
	// Widen to 64-bit type when the value exceeds 32-bit range.
	uint64_t uval = (uint64_t)case_val;
	if ( uval > 0x7FFFFFFF )
	    val_tok->setDataType(case_val < 0 ? (DataDef *)&ddINT64 : (DataDef *)&ddUINT64);
	if ( val_anchor )
	{
	    val_tok->file = val_anchor->file;
	    val_tok->line = val_anchor->line;
	    val_tok->column = val_anchor->column;
	}
	tc->value = val_tok;
	tn = pgm.nextToken();
	// GNU case range extension: case LOW ... HIGH:
	if ( tn->id() == TokenID::tkDot )
	{
	    TokenBase *d2 = pgm.nextToken();
	    TokenBase *d3 = pgm.nextToken();
	    if ( !d2 || d2->id() != TokenID::tkDot
	      || !d3 || d3->id() != TokenID::tkDot )
		pgm.Throw(tn) << "Expecting '...' in case range" << flush;
	    int64_t high_val = parse_constant_integer_expression(pgm);
	    tc->range_high = new TokenInt(high_val);
	    uint64_t uhigh = (uint64_t)high_val;
	    if ( uhigh > 0x7FFFFFFF )
		tc->range_high->setDataType(high_val < 0 ? (DataDef *)&ddINT64 : (DataDef *)&ddUINT64);
	    tn = pgm.nextToken();
	}
	if ( tn->id() != TokenID::tkTerC )
	    pgm.Throw(tn) << "Expecting : after case value" << flush;
	sw->cases.push_back(tc);
	target = tc;
    }
    else
    {
	tn = pgm.nextToken();
	if ( tn->id() != TokenID::tkTerC )
	    pgm.Throw(tn) << "Expecting : after default" << flush;
	if ( sw->defaultcase )
	    pgm.Throw(tn) << "duplicate default label in switch" << flush;
	sw->defaultcase = new TokenCASE();
	sw->defaultcase->value = NULL;
	sw->defaultcase->file = tn->file;
	sw->defaultcase->line = tn->line;
	sw->defaultcase->column = tn->column;
	sw->default_index = (int)sw->cases.size();
	target = sw->defaultcase;
    }

    while ( pgm.peekToken()
	 && pgm.peekToken()->id() != TokenID::tkCASE
	 && pgm.peekToken()->id() != TokenID::tkDEFAULT
	 && pgm.peekToken()->id() != TokenID::tkClBrc )
    {
	TokenBase *stmt = pgm.parseStatement(pgm.nextToken());
	if ( stmt )
	    target->statements.push_back(stmt);
    }

    if ( !pgm.switch_case_stack.empty() )
	pgm.switch_case_stack.back() = target;

    return target;
}

TokenBase *TokenSWITCH::parse(Program &pgm)
{
    DBG(std::cout << "TokenSWITCH::parse()" << std::endl);

    // expect (
    TokenBase *tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkOpBrk )
	pgm.Throw(tn) << "Expecting ( after switch" << flush;

    // parse expression
    expression = pgm.parseExpression(pgm.nextToken(), true);
    if ( !expression )
	pgm.Throw(tn) << "Failed to parse switch expression" << flush;

    // expect )
    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkClBrk )
	pgm.Throw(tn) << "Expecting ) after switch expression" << flush;

    // expect {
    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkOpBrc )
	pgm.Throw(tn) << "Expecting { after switch()" << flush;

    pgm.switch_stack.push_back(this);
    pgm.switch_case_stack.push_back(NULL);

    // Parse the switch body as one linear statement stream. Case/default
    // labels can legally appear inside nested compounds; those nested
    // parses hoist the label into this switch and update
    // switch_case_stack.back() so later statements attach to the newest
    // active label.
    while ( (tn = pgm.nextToken()) )
    {
	if ( tn->id() == TokenID::tkClBrc )
	    break;
	if ( tn->id() == TokenID::tkCASE )
	{
	    parse_switch_label(pgm, this, tn);
	    continue;
	}
	else if ( tn->id() == TokenID::tkDEFAULT )
	{
	    parse_switch_label(pgm, this, tn);
	    continue;
	}

	TokenCASE *active_case = pgm.switch_case_stack.back();
	if ( !active_case )
	{
	    if ( tn->type() == TokenType::ttDataType
	      || tn->type() == TokenType::ttIdentifier )
	    {
		// C allows variable declarations in a switch body before any
		// case label — they're unreachable (no case path enters
		// there) but valid as compile-time declarations. SMAUG's
		// `switch(SPELL_POWER(skill)) { OBJ_DATA *clone; default: ... }`
		// is a common form. Keep them so the CIR emits the declaration
		// (it carries the type for uses later in the switch); discarding
		// left the variable undeclared in the emitted C.
		DBG(std::cout << "TokenSWITCH::parse() keeping pre-case declaration" << std::endl);
		TokenBase *pre = pgm.parseStatement(tn);
		if ( pre )
		    pre_case_stmts.push_back(pre);
	    }
	    else if ( tn->id() == TokenID::tkSemi )
	    {
		continue;
	    }
	    else
		pgm.Throw(tn) << "Expecting case or default in switch body" << flush;
	}
	else
	{
	    TokenBase *stmt = pgm.parseStatement(tn);
	    if ( stmt )
		active_case->statements.push_back(stmt);
	}
    }

    pgm.switch_case_stack.pop_back();
    pgm.switch_stack.pop_back();

    DBG(std::cout << "TokenSWITCH::parse() " << cases.size() << " cases" << (defaultcase ? " + default" : "") << std::endl);
    return this;
}

// TokenCASE::parse() is not called directly — TokenSWITCH::parse() handles case parsing
TokenBase *TokenCASE::parse(Program &pgm)
{
    pgm.Throw(this) << "case outside of switch" << flush;
    return NULL;
}

// rust::match (expr) { p1 | p2 | _ => stmt; ... }
// v1: integer constant patterns and `_` wildcard, no fall-through, multi-
// pattern arms via `|`. Bodies are a single statement (use `{ ... }` for
// multiple). One wildcard arm allowed; its source-order position decides
// where in the dispatch chain the fall-through target lands.
TokenBase *TokenMatch::parse(Program &pgm)
{
    DBG(std::cout << "TokenMatch::parse()" << std::endl);

    // expect (
    TokenBase *tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkOpBrk )
	pgm.Throw(tn) << "Expecting ( after rust::match" << flush;

    expression = pgm.parseExpression(pgm.nextToken(), true);
    if ( !expression )
	pgm.Throw(tn) << "Failed to parse rust::match scrutinee expression" << flush;

    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkClBrk )
	pgm.Throw(tn) << "Expecting ) after rust::match expression" << flush;

    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkOpBrc )
	pgm.Throw(tn) << "Expecting { after rust::match()" << flush;

    while ( (tn = pgm.nextToken()) )
    {
	if ( tn->id() == TokenID::tkClBrc )
	    break;

	MatchArm *arm = new MatchArm();

	// Parse the pattern list — `_`, integer constants, or any
	// number of those joined with `|`.
	while ( true )
	{
	    if ( tn->type() == TokenType::ttIdentifier
	      && ((TokenIdent *)tn)->str == "_" )
	    {
		if ( arm->is_wildcard )
		    pgm.Throw(tn) << "duplicate _ in match arm pattern list" << flush;
		arm->is_wildcard = true;
	    }
	    else
	    {
		// push token back and parse a constant integer pattern.
		// Stop at `|` so the arm-separator survives — that's why
		// this calls parse_constant_bxor (one rung below bor)
		// rather than parse_constant_integer_expression. `&` and
		// `^` inside a pattern still work; only `|` is reserved.
		pgm.pushToken(tn);
		TokenBase *anchor = pgm.peekToken();
		int64_t pv = parse_constant_bxor(pgm);
		TokenInt *ti = new TokenInt(pv);
		if ( anchor )
		{
		    ti->file = anchor->file;
		    ti->line = anchor->line;
		    ti->column = anchor->column;
		}
		arm->patterns.push_back(ti);
	    }

	    TokenBase *sep = pgm.peekToken();
	    if ( !sep )
		pgm.Throw(tn) << "unexpected end of input inside rust::match arm" << flush;
	    if ( sep->id() == TokenID::tkBor )
	    {
		pgm.nextToken(); // consume |
		tn = pgm.nextToken();
		continue;
	    }
	    break;
	}

	tn = pgm.nextToken();
	if ( tn->id() != TokenID::tkFatArrow )
	    pgm.Throw(tn) << "Expecting => after rust::match arm pattern" << flush;

	// Parse a single statement body. `{ ... }` is a TokenCpnd from
	// parseStatement, so block bodies fall out for free.
	arm->body = pgm.parseStatement(pgm.nextToken());

	// Optional trailing `;` after a non-block body.
	TokenBase *trailing = pgm.peekToken();
	if ( trailing && trailing->id() == TokenID::tkSemi )
	    pgm.nextToken();

	if ( arm->is_wildcard )
	{
	    if ( wildcard_index >= 0 )
		pgm.Throw(tn) << "rust::match has more than one _ arm" << flush;
	    wildcard_index = (int)arms.size();
	}

	arms.push_back(arm);
    }

    DBG(std::cout << "TokenMatch::parse() " << arms.size() << " arms"
	    << (wildcard_index >= 0 ? " (with _)" : "") << std::endl);
    return this;
}


// template<typename T[, ...]> class Name { ... }
// Capture the definition for Borland-model instantiation (substitute the type
// parameter(s) into a clone of the body and re-parse it as a concrete class when
// `Name<ConcreteT>` is used). Does NOT parse the body here — T is unbound.
// See docs/plans/2026-05-30-template-instantiation.md.
TokenBase *TokenTEMPLATE::parse(Program &pgm)
{
    DBG(std::cout << "TokenTEMPLATE::parse()" << std::endl);

    TokenBase *tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkLT )
	pgm.Throw(tn) << "Expecting '<' after template" << flush;

    // <typename T [, typename U ...]> — 'typename' lexes as an identifier,
    // 'class' as the tkCLASS keyword; both are optional before each param name.
    std::vector<std::string> typeparams;
    for (;;)
    {
	tn = pgm.nextToken();
	if ( tn->id() == TokenID::tkCLASS
	  || (tn->type() == TokenType::ttIdentifier
	      && ((TokenIdent *)tn)->str == "typename") )
	    tn = pgm.nextToken();   // consume 'class'/'typename', take the name next
	if ( tn->type() != TokenType::ttIdentifier )
	    pgm.Throw(tn) << "Expecting type-parameter name in template<>" << flush;
	typeparams.push_back(((TokenIdent *)tn)->str);
	tn = pgm.nextToken();
	if ( tn->id() == TokenID::tkComma ) continue;
	if ( tn->id() == TokenID::tkGT ) break;
	pgm.Throw(tn) << "Expecting ',' or '>' in template parameter list" << flush;
    }

    // Expect `class|struct Name` then capture through the matching '}'.
    TokenBase *class_kw = pgm.nextToken();
    if ( class_kw->id() != TokenID::tkCLASS && class_kw->id() != TokenID::tkSTRUCT )
	pgm.Throw(class_kw) << "template must be followed by a class/struct definition" << flush;
    TokenBase *name_tb = pgm.nextToken();
    if ( name_tb->type() != TokenType::ttIdentifier )
	pgm.Throw(name_tb) << "Expecting template class name" << flush;
    std::string class_name = ((TokenIdent *)name_tb)->str;

    // Capture the full `class Name ... { ... }` token range (incl. any base list
    // before the brace). Brace-match on tkOpBrc/tkClBrc.
    // Store CLONES we own: the consumed originals may be freed/reused once this
    // parse pass moves on, so capturing raw pointers would dangle at instantiation.
    std::vector<TokenBase *> body;
    body.push_back(class_kw->clone());
    body.push_back(name_tb->clone());
    int depth = 0;
    bool seen_brace = false;
    for (;;)
    {
	tn = pgm.nextToken();
	body.push_back(tn->clone());
	if ( tn->id() == TokenID::tkOpBrc ) { depth++; seen_brace = true; }
	else if ( tn->id() == TokenID::tkClBrc )
	{
	    if ( --depth == 0 && seen_brace ) break;
	}
    }
    // Consume an optional trailing ';' after the class definition.
    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
	pgm.nextToken();

    Program::TemplateDef td;
    td.typeparams = typeparams;
    td.class_name = class_name;
    td.body = body;
    td.defining_namespace = pgm.current_namespace;  // e.g. "std" — for canonical_cpp_spelling
    pgm.template_map[class_name] = td;
    DBG(std::cout << "TokenTEMPLATE::parse() captured template '" << class_name
	<< "' (" << typeparams.size() << " param(s), " << body.size()
	<< " body tokens)" << std::endl);

    return NULL;   // a definition: produces no statement node
}

TokenBase *Program::parseKeyword(TokenKeyword *tk)
{
    TokenBase *tb = (TokenBase *)tk->parse(*this);
    return tb;
}

// real parsing happens here, code should not be null
TokenBase *Program::parseCompound()
{
    if ( compounds.empty() ) { throw "Internal error -- compound stack empty"; }
    TokenCpnd *code = compounds.top();
    TokenBase *tb = NULL;

    DBG(std::cout << "parseCompound() start" << std::endl);

    while ( (tb=nextToken()) )
    {
	if ( tb->id() == TokenID::tkClBrc )
	{
	    code->end_line = tb->line;
	    popCompound();
	    DBG(std::cout << "parseCompound() ends" << std::endl);
	    return code;
	}

	if ( !switch_stack.empty()
	  && (tb->id() == TokenID::tkCASE || tb->id() == TokenID::tkDEFAULT) )
	{
	    parse_switch_label(*this, switch_stack.back(), tb);
	    continue;
	}

	if ( (tb=parseStatement(tb)) )
	{
	    DBG(std::cout << "parseStatement() returns token" << std::endl);
	    code->statements.push_back((TokenStmt *)tb);
	}
    }

    DBG(std::cout << "parseCompound() end of input" << std::endl);

    return code;
}

static bool old_style_param_name_exists(const std::vector<std::string> &ids,
					const std::string &name)
{
    for ( std::vector<std::string>::const_iterator it = ids.begin();
	  it != ids.end(); ++it )
	if ( *it == name )
	    return true;
    return false;
}

static bool is_old_style_parameter_head(Program &pgm, TokenBase *tb)
{
    if ( !is_contextual_identifier_token(tb) )
	return false;

    std::string name = contextual_identifier_name(tb);
    if ( resolve_named_datadef(pgm, name) )
	return false;

    TokenBase *peek = pgm.peekToken();
    return peek && (peek->id() == TokenID::tkComma || peek->id() == TokenID::tkClBrk);
}

static bool parse_array_designator_initializer(Program &pgm, TokenBase *&next_init,
					       size_t &first_index, size_t &last_index)
{
    if ( !next_init || next_init->id() != TokenID::tkOpSqr )
	return false;

    TokenBase *first_tok = pgm.nextToken();
    if ( !first_tok )
	pgm.Throw(next_init) << "Unexpected end of input in array designator" << flush;
    pgm.pushToken(first_tok);

    int64_t first = parse_constant_integer_expression(pgm);
    if ( first < 0 )
	pgm.Throw(next_init) << "Array designator index must be non-negative" << flush;

    int64_t last = first;
    if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkDot )
    {
	TokenBase *dot1 = pgm.nextToken();
	TokenBase *dot2 = pgm.nextToken();
	TokenBase *dot3 = pgm.nextToken();
	if ( !dot1 || !dot2 || !dot3
	  || dot1->id() != TokenID::tkDot
	  || dot2->id() != TokenID::tkDot
	  || dot3->id() != TokenID::tkDot )
	    pgm.Throw(dot1 ? dot1 : next_init) << "Expecting '...' in array designator range" << flush;
	last = parse_constant_integer_expression(pgm);
	if ( last < first )
	    pgm.Throw(next_init) << "Array designator range end precedes start" << flush;
    }

    TokenBase *close = pgm.nextToken();
    if ( !close || close->id() != TokenID::tkClSqr )
	pgm.Throw(close ? close : next_init) << "Expecting ']' after array designator" << flush;

    TokenBase *eq = pgm.nextToken();
    if ( !eq || eq->id() != TokenID::tkAssign )
	pgm.Throw(eq ? eq : close) << "Expecting '=' after array designator" << flush;

    first_index = (size_t)first;
    last_index = (size_t)last;
    next_init = pgm.nextToken();
    if ( !next_init )
	pgm.Throw(eq) << "Expected value after array designator" << flush;

    return true;
}

static DataDef *parse_old_style_parameter_base(Program &pgm, TokenBase *&nt)
{
    while ( nt && (nt->id() == TokenID::tkCONST
	       || nt->id() == TokenID::tkREGISTER
	       || is_restrict_token(nt)) )
	nt = pgm.nextToken();

    if ( !nt )
	pgm.Throw << "Unexpected end of input in K&R parameter declaration" << flush;

    if ( nt->id() == TokenID::tkSTRUCT || nt->id() == TokenID::tkUNION )
    {
	TokenBase *tag = pgm.nextToken();
	if ( !tag || !is_contextual_identifier_token(tag) )
	    pgm.Throw(tag ? tag : nt) << "Expecting struct/union name in K&R parameter declaration" << flush;
	std::string sname = contextual_identifier_name(tag);
	datadef_map_iter sdmi = pgm.struct_map.find(sname);
	if ( sdmi == pgm.struct_map.end() )
	{
	    DataDefSTRUCT *fwd = new DataDefSTRUCT(sname, 0);
	    if ( nt->id() == TokenID::tkUNION )
		fwd->union_layout = true;
	    pgm.struct_map[sname] = fwd;
	    sdmi = pgm.struct_map.find(sname);
	}
	return sdmi->second;
    }

    if ( nt->id() == TokenID::tkENUM )
    {
	TokenBase *tag = pgm.peekToken();
	if ( tag && is_contextual_identifier_token(tag) )
	    pgm.nextToken();
	return &ddINT32;
    }

    if ( nt->type() == TokenType::ttDataType )
	return &((TokenDataType *)nt)->definition;

    if ( nt->type() == TokenType::ttIdentifier )
    {
	std::string tname = ((TokenIdent *)nt)->str;
	datatype_map_iter tdmi = pgm.datatype_map.find(tname);
	if ( tdmi != pgm.datatype_map.end() )
	    return &tdmi->second->definition;
    }

    pgm.Throw(nt) << "Expecting type in K&R parameter declaration" << flush;
    return NULL;
}

static void parse_old_style_parameter_declaration(Program &pgm,
						  TokenBase *nt,
						  const std::vector<std::string> &param_ids,
						  std::map<std::string, DataDef *> &param_types)
{
    DataDef *base_type = parse_old_style_parameter_base(pgm, nt);
    nt = pgm.nextToken();

    while ( nt )
    {
	DataDef *decl_type = base_type;
	if ( nt && nt->id() == TokenID::tkOpBrk )
	{
	    TokenBase *open = nt;
	    TokenBase *star = pgm.nextToken();
	    if ( star && star->id() == TokenID::tkMul )
	    {
		TokenBase *name_tok = pgm.nextToken();
		while ( name_tok && (is_post_pointer_qualifier_token(name_tok) || name_tok->id() == TokenID::tkCONST) )
		    name_tok = pgm.nextToken();
		if ( !name_tok || !is_contextual_identifier_token(name_tok) )
		    pgm.Throw(name_tok ? name_tok : open) << "Expecting parameter name in K&R parameter declaration" << flush;

		std::string name = contextual_identifier_name(name_tok);
		if ( !old_style_param_name_exists(param_ids, name) )
		    pgm.Throw(name_tok) << "K&R declaration for non-parameter '" << name << "'" << flush;
		if ( param_types.find(name) != param_types.end() )
		    pgm.Throw(name_tok) << "Duplicate K&R parameter declaration for '" << name << "'" << flush;

		TokenBase *close = pgm.nextToken();
		if ( !close || close->id() != TokenID::tkClBrk )
		    pgm.Throw(close ? close : open) << "Expected ')' after function pointer parameter name" << flush;
		TokenBase *param_open = pgm.nextToken();
		if ( !param_open || param_open->id() != TokenID::tkOpBrk )
		    pgm.Throw(param_open ? param_open : open) << "Expecting '(' for function pointer parameter list" << flush;

		FuncDef *func = pgm.parseFnPtrParams(*decl_type);
		param_types[name] = new DataDefFPTR(func);

		nt = pgm.nextToken();
		if ( !nt )
		    pgm.Throw << "Unexpected end of input in K&R parameter declaration" << flush;
		if ( nt->id() == TokenID::tkSemi )
		    return;
		if ( nt->id() != TokenID::tkComma )
		    pgm.Throw(nt) << "Expecting ',' or ';' in K&R parameter declaration" << flush;
		nt = pgm.nextToken();
		continue;
	    }
	    if ( star )
		pgm.pushToken(star);
	}
	while ( nt && (nt->id() == TokenID::tkMul || is_post_pointer_qualifier_token(nt)) )
	{
	    if ( nt->id() == TokenID::tkMul )
		decl_type = pgm.getPointerType(decl_type);
	    nt = pgm.nextToken();
	}

	if ( !nt || !is_contextual_identifier_token(nt) )
	    pgm.Throw(nt) << "Expecting parameter name in K&R parameter declaration" << flush;

	std::string name = contextual_identifier_name(nt);
	if ( !old_style_param_name_exists(param_ids, name) )
	    pgm.Throw(nt) << "K&R declaration for non-parameter '" << name << "'" << flush;
	if ( param_types.find(name) != param_types.end() )
	    pgm.Throw(nt) << "Duplicate K&R parameter declaration for '" << name << "'" << flush;

	nt = pgm.nextToken();
	while ( nt && nt->id() == TokenID::tkOpSqr )
	{
	    int depth = 1;
	    while ( depth > 0 )
	    {
		nt = pgm.nextToken();
		if ( !nt )
		    pgm.Throw << "Unexpected end of input in K&R array parameter declarator" << flush;
		if ( nt->id() == TokenID::tkOpSqr )
		    ++depth;
		else if ( nt->id() == TokenID::tkClSqr )
		    --depth;
	    }
	    decl_type = pgm.getPointerType(decl_type);
	    nt = pgm.nextToken();
	}

	param_types[name] = decl_type;

	if ( !nt )
	    pgm.Throw << "Unexpected end of input in K&R parameter declaration" << flush;
	if ( nt->id() == TokenID::tkSemi )
	    return;
	if ( nt->id() != TokenID::tkComma )
	    pgm.Throw(nt) << "Expecting ',' or ';' in K&R parameter declaration" << flush;
	nt = pgm.nextToken();
    }

    pgm.Throw << "Unexpected end of input in K&R parameter declaration" << flush;
}

static bool is_old_style_parameter_declaration_start(Program &pgm, TokenBase *tb)
{
    if ( !tb )
	return false;
    if ( tb->type() == TokenType::ttDataType )
	return true;
    if ( tb->id() == TokenID::tkSTRUCT || tb->id() == TokenID::tkUNION
      || tb->id() == TokenID::tkENUM || tb->id() == TokenID::tkCONST
      || tb->id() == TokenID::tkREGISTER || is_restrict_token(tb) )
	return true;
    if ( is_contextual_identifier_token(tb) )
	return resolve_named_datadef(pgm, contextual_identifier_name(tb)) != NULL;
    return false;
}

static bool scan_old_style_definition_suffix(Program &pgm,
					     std::vector<TokenBase *> &suffix)
{
    if ( !is_old_style_parameter_declaration_start(pgm, pgm.peekToken()) )
	return false;

    int paren_depth = 0;
    int square_depth = 0;
    for ( size_t guard = 0; guard < 512; ++guard )
    {
	TokenBase *t = pgm.nextToken();
	if ( !t )
	    return false;
	suffix.push_back(t);

	if ( t->id() == TokenID::tkOpBrk )
	    ++paren_depth;
	else if ( t->id() == TokenID::tkClBrk && paren_depth > 0 )
	    --paren_depth;
	else if ( t->id() == TokenID::tkOpSqr )
	    ++square_depth;
	else if ( t->id() == TokenID::tkClSqr && square_depth > 0 )
	    --square_depth;
	else if ( t->id() == TokenID::tkOpBrc && paren_depth == 0 && square_depth == 0 )
	    return true;
	else if ( t->id() == TokenID::tkClBrc && paren_depth == 0 && square_depth == 0 )
	    return false;
    }
    return false;
}

// parse a function definition, can be a forward declaration, or function definition
void Program::parseFunction(DataDef &dd, std::string &id, DataDefCLASS *owner_class,
			    std::vector<DataDef *> *multi_ret, bool return_ref,
			    std::string return_typedef_alias)
{
    variable_map_iter vmi;
    funcdef_map_iter fmi;
    datadef_vec_iter dvi;
    FuncDef *func;
    TokenBase *nt = NULL; // next token;
    Variable *var;

    vector<std::string> ids;  // vector of variable names
    // Per-parameter typedef alias, PARALLEL to `ids`. Empty when the parameter
    // type was not a user typedef. Propagated to the param Variable's
    // typedef_name so the CIR builder emits `Alias *p` (keeping the pointee
    // complete) instead of `struct anonymous *p` for a pointer-to-typedef-of-
    // anonymous-aggregate parameter ("struct has no member" otherwise).
    vector<std::string> param_aliases;
    std::string param_alias;  // alias for the parameter currently being parsed
    TokenDataType *pb;        // parameter basetype
    std::string pid;          // parameter id
    RefType rtype = RefType::rtNone;
    // Number of `*` levels seen for the parameter currently being parsed.
    // Reset per parameter (where rtype is reset to rtValue). Used together with
    // pb / param_has_const / rtype to build param_cpp_spellings (the canonical
    // C++ type spelling captured from the source tokens, where top-level
    // pointee-const survives — the DataDef loses it).
    int param_ptr_depth = 0;
    int anon_param_index = 0;
    bool old_style_params = false;
    std::vector<std::string> old_style_ids;
    std::map<std::string, TokenBase *> param_vla_side_effects;
    bool is_nested_function = !compounds.empty() && compounds.top() && compounds.top()->method;
    std::string nested_local_name = id;
    TokenCpnd *nested_owner_scope = is_nested_function ? compounds.top() : NULL;
    if ( is_nested_function )
	id = make_nested_function_name(nested_owner_scope, id);

    DBG(cout << "parseFunction(" << dd.name << ' ' << id << ") START" << endl);
    cur_func_name = id; // for __FUNCTION__/__func__ expansion in the lexer

    // may have already been declared (e.g. forward decl → definition)
    bool func_already_declared = false;
    if ( (fmi=funcdef_map.find(id)) != funcdef_map.end() )
    {
	func = fmi->second;
	func_already_declared = true;
	// C `f()` is an old-style declaration with unspecified parameters,
	// not a real zero-parameter prototype. When the later definition
	// provides the actual parameter list, rebuild the FuncDef from
	// scratch so the body binds those names normally.
	if ( !func->is_void_params && func->parameters.empty() )
	{
	    FuncDef *fresh = new FuncDef(dd);
	    fresh->return_types = func->return_types;
	    fresh->return_typedef_name = func->return_typedef_name;
	    fresh->param_typedef_names = func->param_typedef_names;
	    fresh->no_instrument_function = func->no_instrument_function;
	    fresh->explicit_alignment = func->explicit_alignment;
	    funcdef_map[id] = fresh;
	    func = fresh;
	    func_already_declared = false;
	}
	// Return type mismatch (e.g. forward decl `void f()` → definition
	// `bool f()`): FuncDef::returns is a C++ reference and cannot be
	// reseated, so replace the FuncDef with a fresh one carrying the
	// correct return type.
	if ( &func->returns != &dd )
	{
	    DBG(std::cout << "parseFunction() return type refresh: " << func->returns.name << " → " << dd.name << " for " << id << std::endl);
	    FuncDef *fresh = new FuncDef(dd);
	    fresh->parameters   = func->parameters;
	    fresh->is_varargs   = func->is_varargs;
	    fresh->is_void_params = func->is_void_params;
	    fresh->return_types = func->return_types;
	    fresh->return_typedef_name = func->return_typedef_name;
	    fresh->param_typedef_names = func->param_typedef_names;
	    fresh->no_instrument_function = func->no_instrument_function;
	    fresh->explicit_alignment = func->explicit_alignment;
	    funcdef_map[id] = fresh;
	    func = fresh;
	}
    }
    else
    {
	func = new FuncDef(dd);
	funcdef_map[id] = func;
	DBG(std::cout << "parseFunction() Added new function declaration type: " << dd.name << " size: " << dd.size << " name: " << id << std::endl);
    }
    func->returns_ref = return_ref;
    if ( !return_typedef_alias.empty() )
	func->return_typedef_name = return_typedef_alias;

    // for multi-return functions, inject hidden __retbuf parameter as first arg
    if ( multi_ret && multi_ret->size() > 1 )
    {
	func->return_types = *multi_ret;
	if ( !func_already_declared )
	{
	    func->parameters.push_back(&ddINT64); // void* as int64
	    func->param_cpp_spellings.push_back(""); // hidden — excluded from mangling
	    func->param_typedef_names.push_back("");
	    func->param_defaults.push_back(NULL);    // keep aligned with parameters
	}
	ids.push_back("__retbuf");
	param_aliases.push_back("");
	DBG(cout << "parseFunction() injected hidden __retbuf for multi-return (" << multi_ret->size() << " types)" << endl);
    }

    // for class methods, inject hidden __this parameter as first arg.
    // Type it as a pointer to the owning class (`ClassName *`) — not a bare
    // `void *` — so the CIR backend can resolve `__this->member` against the
    // class's lowered C struct (a void* would leave member access untyped).
    if ( owner_class )
    {
	if ( !func_already_declared )
	{
	    func->parameters.push_back(getPointerType(owner_class));
	    // Keep ref_params / const_params index-aligned with parameters:
	    // __this occupies slot 0, so the first USER parameter's reference /
	    // const flag lands at index 1 (matching every consumer — e.g. the
	    // std::string operator registrations use ref_params[1] for the RHS).
	    // Without this, a method with a `T&` param mis-flagged __this as the
	    // reference and left the real param without vfREFERENCE, breaking
	    // `a.member` member access on a class-reference param.
	    func->ref_params.push_back(false);
	    func->const_params.push_back(false);
	    func->param_cpp_spellings.push_back(""); // hidden __this — excluded from mangling
	    func->param_typedef_names.push_back("");
	    func->param_defaults.push_back(NULL);    // keep aligned with parameters
	}
	ids.push_back("__this");
	param_aliases.push_back("");
	DBG(cout << "parseFunction() injected hidden __this parameter for class method" << endl);
    }

    Variable temp_param_fn(id, *func, 1, NULL, false);
    Method temp_param_method(temp_param_fn);
    pushCompound();
    TokenCpnd *param_scope = compounds.empty() ? NULL : compounds.top();
    if ( param_scope )
	param_scope->method = &temp_param_method;

    // look for parameters
    while ( (nt=nextToken()) && nt->id() != TokenID::tkClBrk )
    {
	if ( is_old_style_parameter_head(*this, nt) )
	{
	    old_style_params = true;
	    while ( true )
	    {
		if ( !is_contextual_identifier_token(nt) )
		    Throw(nt) << "Expecting identifier in K&R parameter list" << flush;
		pid = contextual_identifier_name(nt);
		if ( old_style_param_name_exists(old_style_ids, pid) )
		    Throw(nt) << "Duplicate K&R parameter name" << flush;
		ids.push_back(pid);
		param_aliases.push_back("");
		old_style_ids.push_back(pid);

		nt = nextToken();
		if ( nt->id() == TokenID::tkClBrk )
		    break;
		if ( nt->id() != TokenID::tkComma )
		    Throw(nt) << "Expecting ',' or ')' in K&R parameter list" << flush;
		nt = nextToken();
	    }
	    break;
	}

	// tolerate C qualifiers/storage hints in parameter lists such as
	// `const char *s` and `register char *s`
	param_alias.clear();  // reset per-parameter typedef alias
	// Default ARGUMENT expression (`T x = expr`) for this parameter, or NULL.
	// Declared at loop top so the gotos into `paramdecl:` do not cross its
	// initialization; the `= expr` is parsed just before `paramdecl:` below.
	TokenBase *param_default = NULL;
	bool param_has_const = false;
	// Pointee/top-level const that PRECEDES the base type — `const char *` /
	// `const std::string &`. This is the const Itanium mangles (PKc, RK...),
	// as opposed to a trailing `char * const` (top-level, dropped by the ABI).
	// Tracked separately from param_has_const so the spelling stays accurate.
	bool param_leading_const = false;
	while ( nt && (nt->id() == TokenID::tkCONST
	            || nt->id() == TokenID::tkVOLATILE
	            || nt->id() == TokenID::tkREGISTER) )
	{
	    if ( nt->id() == TokenID::tkCONST )
	    {
		param_has_const = true;
		param_leading_const = true;
	    }
	    nt = nextToken();
	}
	std::vector<uint32_t> param_array_dims;
	std::vector<TokenBase *> param_array_dim_exprs;

	// C `void` as sole parameter means no parameters (e.g. `int f(void)`).
	if ( nt->type() == TokenType::ttDataType
	  && &((TokenDataType *)nt)->definition == &ddVOID
	  && peekToken() && peekToken()->id() == TokenID::tkClBrk )
	{
	    func->is_void_params = true;
	    nextToken(); // consume ')'
	    break;
	}

	// detect ... (variadic parameter)
	if ( nt->id() == TokenID::tkDot )
	{
	    TokenBase *d2 = nextToken();
	    TokenBase *d3 = nextToken();
	    if ( !d2 || d2->id() != TokenID::tkDot || !d3 || d3->id() != TokenID::tkDot )
		Throw(nt) << "Expecting '...' for variadic parameter" << flush;
	    func->is_varargs = true;
	    // A prior prototype already owns the hidden varargs slot.
	    if ( !func_already_declared )
	    {
		func->parameters.push_back(&ddINT64);
		func->param_cpp_spellings.push_back(""); // hidden — excluded
		func->param_typedef_names.push_back("");
		func->param_defaults.push_back(NULL);    // keep aligned with parameters
	    }
	    ids.push_back("__va_args");
	    param_aliases.push_back("");
	    DBG(cout << "parseFunction() detected varargs, injected __va_args" << endl);
	    // next token should be )
	    nt = nextToken();
	    if ( nt->id() != TokenID::tkClBrk )
		Throw(nt) << "Expecting ')' after '...'" << flush;
	    break;
	}

	// handle 'struct Tag' / 'union Tag' / 'enum Tag' as parameter type
	if ( nt->id() == TokenID::tkSTRUCT || nt->id() == TokenID::tkUNION )
	{
	    const char *kw = (nt->id() == TokenID::tkUNION) ? "union" : "struct";
	    TokenBase *tag_nt = nextToken();
	    if ( tag_nt->type() != TokenType::ttIdentifier )
		Throw(tag_nt) << "Expecting " << kw << " name after '" << kw << "' in parameters" << flush;
	    std::string sname = ((TokenIdent *)tag_nt)->str;
	    datadef_map_iter sdmi = struct_map.find(sname);
	    if ( sdmi == struct_map.end() )
	    {
		// C permits pointers/references to incomplete struct types in
		// parameter lists, so synthesize a forward declaration on demand.
		DataDefSTRUCT *fwd = new DataDefSTRUCT(sname, 0);
		if ( nt->id() == TokenID::tkUNION )
		    fwd->union_layout = true;
		struct_map[sname] = fwd;
		sdmi = struct_map.find(sname);
	    }
	    std::string tname(kw);
	    tname += " ";
	    tname.append(sname);
	    pb = new TokenDataType(tname.c_str(), *sdmi->second);
	}
	else
	if ( nt->id() == TokenID::tkENUM )
	{
	    // enum parameter — consume tag, treat as int
	    TokenBase *tag_nt = peekToken();
	    if ( tag_nt && tag_nt->type() == TokenType::ttIdentifier )
		nextToken(); // consume tag name
	    pb = new TokenDataType("int", ddINT);
	}
	else
	if ( nt->type() != TokenType::ttDataType )
	{
	    // also check datatype_map for typedef'd names (e.g. CHAR_DATA)
	    if ( nt->type() == TokenType::ttIdentifier )
	    {
		std::string tname = ((TokenIdent *)nt)->str;
		datatype_map_iter tdmi = datatype_map.find(tname);
		if ( tdmi != datatype_map.end() )
		{
		    pb = tdmi->second;
		    // Record the typedef alias for this parameter (matches the
		    // var/member paths' user_typedef_names check) so the CIR
		    // builder keeps `Alias` as the type spec.
		    if ( user_typedef_names.count(tname) )
			param_alias = tname;
		}
		else if ( TokenDataType *inst = instantiate_template_use(*this, tname, nt) )
		{
		    // A template-id parameter type, e.g. `const allocator<_CharT>&`
		    // after the enclosing template substitutes _CharT -> char.
		    // Mirrors the var/member type path (resolve_declared_type_token):
		    // instantiate (or reuse) the concrete class and use its type.
		    // Without this a bare template-id in a parameter list threw
		    // "Failed to find type".
		    pb = inst;
		}
		else if ( TokenDataType *ns_type = resolve_namespaced_type_token(*this, nt, true) )
		{
		    pb = ns_type;
		}
		else
		{
		    DBG(std::cerr << "parseFunction() params: failed to obtain basetype for '" << tname << "'" << std::endl);
		    Throw(nt) << "Failed to find type '" << tname << "' when parsing function parameters" << flush;
		}
	    }
	    else
	    {
		DBG(std::cerr << "parseFunction() params: failed to obtain basetype" << std::endl);
		Throw(nt) << "Failed to find type when parsing function parameters" << flush;
	    }
	}
	else
	    pb = (TokenDataType *)nt;
	rtype = RefType::rtValue;
	param_ptr_depth = 0;
	DataDef *param_dd = &pb->definition;
grabnt:
	// grab the next token
	if ( !peekToken() )
	    Throw(nt) << "Unexpected end of file parsing function parameters" << flush;

	nt = nextToken();

	if ( nt->id() == TokenID::tkBand )
	{
	    rtype = RefType::rtReference;
	    DBG(std::cout << "parseFunction() setting reference token " << std::endl);
//	    pb->definition.setRef(RefType::rtReference);
	    goto grabnt;
	}
	if ( nt->id() == TokenID::tkStar )
	{
	    rtype = RefType::rtPointer;
	    ++param_ptr_depth;
	    param_dd = getPointerType(param_dd);
	    DBG(std::cout << "parseFunction() pointer param: " << param_dd->name << std::endl);
	    goto grabnt;
	}
	if ( is_restrict_token(nt)
	  || nt->id() == TokenID::tkCONST
	  || nt->id() == TokenID::tkVOLATILE )
	{
	    if ( nt->id() == TokenID::tkCONST )
		param_has_const = true;
	    goto grabnt;
	}
	if ( nt->id() == TokenID::tkComma || nt->id() == TokenID::tkClBrk )
	{
	    pid = "__anon_param_" + std::to_string(anon_param_index++);
	    goto paramdecl;
	}
	if ( nt->id() == TokenID::tkAssign )
	{
	    // Unnamed parameter carrying a default value, e.g.
	    // `const allocator<_CharT>& = allocator<_CharT>()`. The named-param
	    // path parses the default just before paramdecl:; an anonymous param
	    // never reaches it, so parse the default here (same stop-token rule)
	    // then fall into paramdecl with nt at the ',' / ')'.
	    pid = "__anon_param_" + std::to_string(anon_param_index++);
	    param_default = parseExpression(nextToken(), true);
	    nt = nextToken();   // the ',' or ')' that ends this parameter
	    goto paramdecl;
	}
	if ( nt->id() == TokenID::tkOpBrk )
	{
	    TokenBase *inner = nextToken();
	    if ( inner && inner->id() == TokenID::tkStar )
	    {
		nt = nextToken();
		while ( nt && (is_restrict_token(nt)
		           || nt->id() == TokenID::tkCONST
		           || nt->id() == TokenID::tkVOLATILE) )
		    nt = nextToken();
		if ( !is_contextual_identifier_token(nt) )
		    Throw(nt) << "Expecting identifier in function pointer parameter" << flush;
		pid = contextual_identifier_name(nt);

		nt = nextToken();
		if ( !nt || nt->id() != TokenID::tkClBrk )
		    Throw(nt ? nt : inner) << "Expected ')' after function pointer parameter name" << flush;
		nt = nextToken();
		if ( nt && nt->id() == TokenID::tkOpSqr )
		{
		    // Pointer-to-array parameter: `int (*a)[2]`. The `[N]` binds to
		    // the parenthesized inner declarator, yielding a true
		    // pointer-to-array `DataDefPTR(DataDefCArray(elem, N))` — NOT a
		    // plain `int *a` (deref of which is scalar `int`, so `(*a)[i]`
		    // would not be subscriptable). Build the nested CArray dims
		    // (outermost first), then wrap in one pointer level.
		    std::vector<uint32_t> pa_dims;
		    while ( nt && nt->id() == TokenID::tkOpSqr )
		    {
			TokenBase *dim_peek = peekToken();
			if ( dim_peek && dim_peek->id() == TokenID::tkClSqr )
			{
			    nextToken(); // consume ']' for unsized leading dim
			    pa_dims.push_back(0);
			}
			else
			{
			    int64_t n = parse_constant_integer_expression(*this);
			    if ( n < 0 )
				Throw(inner) << "Pointer-to-array parameter dimension must be non-negative" << flush;
			    pa_dims.push_back((uint32_t)n);
			    TokenBase *cl = nextToken();
			    if ( !cl || cl->id() != TokenID::tkClSqr )
				Throw(cl ? cl : inner) << "Expected ']' in pointer-to-array parameter" << flush;
			}
			nt = nextToken();
		    }
		    DataDef *pa_elem = param_dd;
		    for ( size_t i = pa_dims.size(); i-- > 0; )
			pa_elem = new DataDefCArray(*pa_elem, pa_elem->name,
						    pa_dims[i], NULL);
		    param_dd = getPointerType(pa_elem);
		    rtype = RefType::rtPointer;
		    goto paramdecl;
		}
		if ( !nt || nt->id() != TokenID::tkOpBrk )
		    Throw(nt ? nt : inner) << "Expected '(' after function pointer parameter name" << flush;

		// Function-pointer parameter declarator, e.g.
		// `void (*markfn)(void *)`.
		FuncDef *param_func = parseFnPtrParams(*param_dd);
		param_dd = new DataDefFPTR(param_func);
		rtype = RefType::rtValue;

		nt = nextToken();
		goto paramdecl;
	    }
	    Throw(inner ? inner : nt) << "Unsupported parenthesized parameter declarator" << flush;
	}
	if ( !is_contextual_identifier_token(nt) )
	{
	    Throw(nt) << "Expecting identifier after type" << flush;
	}

	// grab identifier string
	pid = contextual_identifier_name(nt);
	if ( !peekToken() )
	    Throw(nt) << "Expecting token after identifier" << flush;

	nt = nextToken();

	// Array parameters decay only once in C: `T a[]` -> `T *`, and
	// `T a[][3][4]` -> `T (*)[3][4]`. Preserve the trailing extents as a
	// nested array type under a single pointer instead of promoting one
	// pointer level per `[]`.
	while ( nt && nt->id() == TokenID::tkOpSqr )
	{
	    TokenBase *peek_dim = peekToken();
	    if ( peek_dim && peek_dim->id() == TokenID::tkClSqr )
	    {
		nextToken(); // consume ']'
		param_array_dims.push_back(0);
		param_array_dim_exprs.push_back(NULL);
	    }
	    else
	    {
		TokenBase *dim_expr = NULL;
		std::set<std::string> param_runtime_names;
		param_runtime_names.insert(pid);
		for ( size_t ii = 0; ii < ids.size(); ++ii )
		    param_runtime_names.insert(ids[ii]);
		if ( bracket_dim_needs_runtime_value(*this, &param_runtime_names) )
		{
		    dim_expr = parseExpression(nextToken(), true);
		    param_array_dims.push_back(0);
		}
		else
		{
		    int64_t n = parse_constant_integer_expression(*this);
		    if ( n < 0 )
			Throw(nt) << "Parameter array dimension must be non-negative" << flush;
		    param_array_dims.push_back((uint32_t)n);
		}
		param_array_dim_exprs.push_back(dim_expr);
		TokenBase *cl = nextToken();
		if ( !cl || cl->id() != TokenID::tkClSqr )
		    Throw(cl ? cl : nt) << "Expected ']' in parameter array declarator" << flush;
	    }
	    nt = nextToken();
	}
	if ( !param_array_dims.empty() )
	{
	    TokenBase *param_vla_sidefx = NULL;
	    for ( size_t i = 0; i < param_array_dim_exprs.size(); ++i )
	    {
		TokenBase *dim_expr = param_array_dim_exprs[i];
		if ( !dim_expr )
		    continue;
		if ( !param_vla_sidefx )
		    param_vla_sidefx = dim_expr;
		else
		{
		    TokenComma *comma = new TokenComma();
		    comma->left = param_vla_sidefx;
		    comma->right = dim_expr;
		    param_vla_sidefx = comma;
		}
	    }
	    if ( param_vla_sidefx )
		param_vla_side_effects[pid] = param_vla_sidefx;
	    DataDef *array_elem = param_dd;
	    for ( size_t i = param_array_dims.size(); i-- > 1; )
		array_elem = new DataDefCArray(*array_elem, array_elem->name,
					       param_array_dims[i],
					       i < param_array_dim_exprs.size()
					       ? param_array_dim_exprs[i] : NULL);
	    param_dd = getPointerType(array_elem);
	    rtype = RefType::rtPointer;
	}

	// Default argument: `T name = expr`. Parse the default expression — it stops
	// at the parameter-list ',' or ')' — and record it (stored index-aligned with
	// parameters in the push block below). A call that omits this argument fills
	// it; arity matching uses FuncDef::required_param_count().
	if ( nt && nt->id() == TokenID::tkAssign )
	{
	    param_default = parseExpression(nextToken(), true);
	    nt = nextToken();   // the ',' or ')' that ends this parameter
	}

paramdecl:
	// parameter declaration
	if ( nt->id() == TokenID::tkComma || nt->id() == TokenID::tkClBrk )
	{
	    // If this is a definition following a forward declaration, the
	    // function already has its parameter DataDefs — don't re-push.
	    DataDef *scope_param_type = NULL;
	    if ( func_already_declared )
	    {
		ids.push_back(pid);
		param_aliases.push_back(param_alias);
		scope_param_type = param_dd;
	    }
	    else if ( !func->findParameter(pid) )
	    {
		ids.push_back(pid);
		param_aliases.push_back(param_alias);
		// Canonical C++ spelling of this parameter, captured from the
		// SOURCE TOKENS (leading const + base type + `*`s + trailing `&`),
		// index-aligned with func->parameters. The DataDef alone loses
		// top-level pointee-const, so a DataDef-derived spelling would
		// mangle `const char*` as Pc not PKc; building from tokens keeps
		// it correct. Fed to the Itanium mangler for std:: header methods.
		std::string param_spelling;
		if ( param_leading_const )
		    param_spelling = "const ";
		param_spelling += pb->definition.canonical_cpp_spelling.empty()
		    ? pb->definition.name
		    : pb->definition.canonical_cpp_spelling;
		for ( int sd = 0; sd < param_ptr_depth; ++sd )
		    param_spelling += "*";
		if ( rtype == RefType::rtReference )
		    param_spelling += "&";
		func->param_cpp_spellings.push_back(param_spelling);
		func->param_typedef_names.push_back(param_alias);
		if ( rtype == RefType::rtReference )
		{
		    DataDef *ref_ptr = getPointerType(&pb->definition);
		    func->parameters.push_back(ref_ptr);
		    func->ref_params.push_back(true);
		    func->const_params.push_back(param_has_const);
		    scope_param_type = ref_ptr;
		}
		else if ( rtype == RefType::rtPointer )
		{
		    func->parameters.push_back(param_dd);
		    func->ref_params.push_back(false);
		    func->const_params.push_back(false);
		    scope_param_type = param_dd;
		}
		else if ( dynamic_cast<DataDefFPTR *>(param_dd) != NULL )
		{
		    func->parameters.push_back(param_dd);
		    func->ref_params.push_back(false);
		    func->const_params.push_back(false);
		    scope_param_type = param_dd;
		}
		else
		{
		    func->parameters.push_back(&pb->definition);
		    func->ref_params.push_back(false);
		    func->const_params.push_back(false);
		    scope_param_type = &pb->definition;
		}
		// Record this parameter's default-argument expression (NULL = none),
		// index-aligned with the parameter just pushed above.
		func->param_defaults.push_back(param_default);
		DBG(std::cout << "Added new parameter declaration type: " << dd.name << " size: "
		    << dd.size << " name: " << pid << " ptr: " << &dd << std::endl);
	    }
	    else
	    {
		DBG(std::cerr << "parseFunction() params: duplicate parameter name " << pid << std::endl);
		Throw(nt) << "Duplicate parameter name" << flush;
	    }
	    if ( scope_param_type )
	    {
		Variable *scope_param = new Variable(pid, *scope_param_type, 1, NULL, false);
		scope_param->flags |= vfPARAM | vfLOCAL;
		if ( rtype == RefType::rtReference )
		    scope_param->flags |= vfREFERENCE;
		if ( param_has_const && rtype == RefType::rtReference )
		    scope_param->flags |= vfCONSTANT;
		temp_param_method.parameters.push_back(scope_param);
	    }
	    if ( nt->id() == TokenID::tkClBrk )
		break;
	}
    }

    if ( !nt )
    {
	DBG(std::cerr << "parseFunction() expecting more tokens, missing closing bracket" << std::endl);
	Throw << "Missing closing bracket" << flush;
    }

    nt = nextToken();

    if ( !compounds.empty() && compounds.top() == param_scope )
	popCompound();

    if ( old_style_params )
    {
	std::map<std::string, DataDef *> old_style_param_types;
	while ( nt && nt->id() != TokenID::tkOpBrc )
	{
	    parse_old_style_parameter_declaration(*this, nt, old_style_ids,
						  old_style_param_types);
	    nt = nextToken();
	}

	if ( !func_already_declared )
	{
	    for ( std::vector<std::string>::const_iterator it = old_style_ids.begin();
		  it != old_style_ids.end(); ++it )
	    {
		std::map<std::string, DataDef *>::iterator pti =
		    old_style_param_types.find(*it);
		DataDef *kr_dd = pti == old_style_param_types.end()
		    ? &ddINT32
		    : pti->second;
		func->parameters.push_back(kr_dd);
		// Keep param_cpp_spellings index-aligned (K&R is C-only — never a
		// std:: method, so this best-effort name is never mangled).
		func->param_cpp_spellings.push_back(kr_dd->name);
		func->param_typedef_names.push_back("");
	    }
	}
    }

    Method *method;

    if ( (var=tkProgram->findVariable(id)) )
    {
	var->type = func;
	method = (Method *)var->data;
    }
    else
    {
	var = addVariable(NULL, *func, id);
	method = NULL;
    }

    // Trailing method cv/ref/exception qualifiers after the parameter list:
    // `T m() const`, `... volatile`, `... noexcept`, `... override`, `... final`.
    // Record const — it drives Itanium mangling (a const method is _ZNK…;
    // FuncDef::is_const_method is consumed by the keystone mangler at
    // parser.cpp:11774/11777). The rest are consumed (not yet modelled).
    // Must run before the forward-decl checks (tkSemi/tkComma) and the brace
    // check so both `T m() const;` and `T m() const {}` parse.
    for (;;) {
	TokenBase *q = nt;
	if ( !q ) break;
	if ( q->id() == TokenID::tkCONST ) { func->is_const_method = true; nt = nextToken(); continue; }
	if ( q->id() == TokenID::tkVOLATILE || q->id() == TokenID::tkRESTRICT ) { nt = nextToken(); continue; }
	if ( q->type() == TokenType::ttIdentifier ) {
	    const std::string &qs = ((TokenIdent *)q)->str;
	    if ( qs == "noexcept" || qs == "override" || qs == "final" ) { nt = nextToken(); continue; }
	}
	break;
    }

    std::string func_alias_name;
    nt = consume_gnu_asm_label(*this, nt, &func_alias_name);

    // Semicolon ends a function declaration; comma continues another
    // function declarator with the same return type: `void a(), b();`.
    if ( nt->id() == TokenID::tkSemi || nt->id() == TokenID::tkComma )
    {
	if ( !method )
	{
	    method = new Method(*var);
	    var->data = (void *)method;
	}
	if ( !func_alias_name.empty() )
	    var->storage_alias_name = func_alias_name;
	if ( owner_class )
	    method->owner_class = owner_class;
	func->declaration_only = true;	// prototype, no body (see FuncDef::declaration_only)
	DBG(std::cout << "parseFunction() forward declaration of function " << id << std::endl);
	if ( nt->id() == TokenID::tkComma )
	{
	    DataDef *next_return = &dd;
	    TokenBase *next = nextToken();
	    while ( next && next->id() == TokenID::tkMul )
	    {
		next_return = getPointerType(next_return);
		next = nextToken();
	    }
	    if ( !next || !is_contextual_identifier_token(next) )
		Throw(next ? next : nt) << "Expecting function name after ',' in function declaration" << flush;
	    std::string next_id = contextual_identifier_name(next);
	    TokenBase *open = nextToken();
	    if ( !open || open->id() != TokenID::tkOpBrk )
	    {
		// Mixed declaration list: `float fx(), inita(), a, b;`
		// After a function declarator continuation, fall back to the
		// normal variable declarator parser when the next name is not
		// followed by `(`.
		pushToken(open);
		pushToken(next);
		TokenDataType tdt(next_return->name.c_str(), *next_return);
		parseDeclaration(&tdt);
		return;
	    }
	    parseFunction(*next_return, next_id, owner_class, multi_ret,
			  false, return_typedef_alias);
	}
	return;
    }

    std::set<std::string> func_attrs;
    size_t func_align = 0;
    nt = consume_gnu_attributes(*this, nt, &func_attrs, NULL, &func_align);
    if ( !func_alias_name.empty() )
	var->storage_alias_name = func_alias_name;
    if ( func_attrs.count("no_instrument_function")
      || func_attrs.count("__no_instrument_function__") )
	func->no_instrument_function = true;
    if ( func_align > 0 )
	func->explicit_alignment = func_align;
    // Check again for forward declaration after __attribute__
    if ( nt->id() == TokenID::tkSemi )
    {
	method = new Method(*var);
	var->data = (void *)method;
	if ( owner_class )
	    method->owner_class = owner_class;
	func->declaration_only = true;	// prototype, no body (see FuncDef::declaration_only)
	return;
    }

    // Definitions must own a fresh Method instance. Some prior declaration
    // paths (notably SMAUG macro expansions) leave a non-null var->data that
    // is not a valid Method object, so reusing it corrupts method->parameters.
    if ( is_nested_function )
    {
	configure_nested_function_captures(*this, func);
	// The nested fn is hoisted to the unique symbol `id`; every call site
	// (which resolves the in-scope source-named alias below) must emit this
	// hoisted name. The CIR builder reads it via FuncDef::nested_emit_name.
	func->nested_emit_name = id;
    }

    method = new Method(*var);
    var->data = (void *)method;
    if ( is_nested_function && nested_owner_scope )
    {
	Variable *local_alias =
	    addVariable(nested_owner_scope, *func, nested_local_name, 1, NULL, false);
	local_alias->data = (void *)method;
    }

    // need to see a brace to define a function
    if ( nt->id() != TokenID::tkOpBrc )
    {
	// throw error
	Throw(nt) << "Expecting brace after function declaration" << flush;
    }

    if ( owner_class )
	method->owner_class = owner_class;

    DataDef *d;
    Variable *v;
    int i = 0;
    int user_param_index = 0;

    // A GNU nested function captures the enclosing locals/params it references
    // BY REFERENCE. That lowering is done in the CIR builder (each used
    // enclosing variable becomes a hidden `T *name` pointer parameter appended
    // after the user params, and every call site forwards `&var`), so no env
    // placeholder is injected into method->parameters here.

    DBG(cout << "parseFunction() param loop: func->parameters.size()=" << func->parameters.size()
	<< " ids.size()=" << ids.size() << " method=" << (void*)method << endl);
    // If a forward declaration registered N parameters and the definition
    // produced a mismatching count, ids[i] dereffed past size() crashes
    // through the std::string copy ctor (NULL+8 read). Bound the loop by
    // the smaller of the two — when ids is short, fill remaining slots
    // with synthetic names. (Pre-existing param-parse bugs in dlsym/
    // typedef'd-pointer paths can produce ids.size() < parameters.size();
    // those need separate fixes but shouldn't crash the parser here.
    // ids.size() > parameters.size() is the reverse case — extra ids are
    // ignored.)
    size_t n = func->parameters.size();
    for ( dvi = func->parameters.begin(); dvi != func->parameters.end(); ++dvi, ++i )
    {
	d = *dvi;
	std::string pname = (size_t)user_param_index < ids.size()
	    ? ids[user_param_index]
	    : std::string("__synthetic_p") + std::to_string(user_param_index);
	DBG(cout << "parseFunction() adding parameter variable " << pname << endl);
	v = new Variable(pname, *d, 1, NULL, false);
	v->flags |= vfPARAM | vfLOCAL;
	// Carry the parameter's typedef alias (parallel to `ids`) so the CIR
	// builder emits `Alias *p` and the pointee resolves through the
	// typedef's complete definition.
	if ( (size_t)user_param_index < param_aliases.size() )
	    v->typedef_name = param_aliases[user_param_index];
	if ( (size_t)i < func->ref_params.size() && func->ref_params[i] )
	    v->flags |= vfREFERENCE;
	if ( (size_t)i < func->const_params.size() && func->const_params[i] )
	    v->flags |= vfCONSTANT;
	std::map<std::string, TokenBase *>::iterator pvsi = param_vla_side_effects.find(pname);
	if ( pvsi != param_vla_side_effects.end() )
	    v->param_vla_side_effect_expr = pvsi->second;
	method->parameters.push_back(v);
	DBG(cout << "parseFunction() pushed param, method->parameters.size()=" << method->parameters.size() << endl);
	++user_param_index;
    }
    (void)n;

    pushCompound();
    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
    if ( code )
    {
	DBG(cout << "parseFunction() setting code->method" << endl);
	code->method = method;
    }
    else
    {
	DBG(cout << "parseFunction() code = NULL" << endl);
    }

    TokenFunc *tf = new TokenFunc(*var);
    // Capture the function declaration's source position before
    // parseCompound overwrites it.  The previous token is the `{`
    // which sits on the declaration line (or the line after the
    // signature for multi-line declarations).
    if ( _prv_token && _prv_token->file && _prv_token->line > 0 )
    {
	tf->file = _prv_token->file;
	tf->line = _prv_token->line;
	tf->column = 0;
    }
    else if ( source.line() > 0 )
    {
	tf->file = source.fname();
	tf->line = source.line();
	tf->column = 0;
    }
    DBG(cout << "parseFunction() calling parseCompound()" << endl);
    TokenCpnd *tc = dynamic_cast<TokenCpnd *>(parseCompound());

    tf->method = method;
    tf->parent = tc->parent;
    tf->variables = tc->variables;
    tf->statements = tc->statements;
    tf->deferred = tc->deferred;
    tf->end_line = tc->end_line;
    // Preserve the declaration line — don't let the statement-walk
    // overwrite it. For error reporting on the body, use statement
    // positions; for function-level diagnostics and --emit-function,
    // keep the declaration line.
    if ( !tf->file || tf->line <= 0 )
    {
	for ( auto *st : tf->statements )
	{
	    if ( st && st->file && st->line > 0 )
	    {
		tf->file = st->file;
		tf->line = st->line;
		tf->column = st->column;
		break;
	    }
	}
    }

    DBG(cout << "parseFunction() calling ast.push" << endl);
    ast.push(tf);
    pending_funcs.push_back(tf);

    DBG(cout << "parseFunction(" << id << ") END" << endl);
}

// Deduce the static type of a parsed return expression for C++14 lambda
// return-type deduction. Arithmetic operator tokens default `_datatype` to
// `int` until compile-time numeric inference runs, so their parse-time
// `datadef()` is unreliable. Apply the usual arithmetic conversions over the
// operands instead (gcc is canon): if either side is floating, the result is
// `double`; otherwise fall back to the expression's own `datadef()`. Pointer
// and non-arithmetic expressions report their declared type directly.
static DataDef *deduce_expr_type(TokenBase *expr)
{
    if ( !expr )
	return NULL;

    if ( TokenOperator *op = dynamic_cast<TokenOperator *>(expr) )
    {
	// Comparison / logical operators yield bool regardless of operands;
	// their own datadef() already reflects that, so only fold the
	// value-producing arithmetic operators here.
	switch ( op->id() )
	{
	case TokenID::tkAdd: case TokenID::tkSub:
	case TokenID::tkMul: case TokenID::tkDiv:
	    {
		DataDef *l = deduce_expr_type(op->left);
		DataDef *r = deduce_expr_type(op->right);
		if ( (l && l->is_real()) || (r && r->is_real()) )
		    return &ddDOUBLE;
		if ( l && l->is_pointer() ) return l;
		if ( r && r->is_pointer() ) return r;
		break;
	    }
	default:
	    break;
	}
    }

    return expr->datadef();
}

// parse a lambda expression: [](type arg, ...) { body }
// Returns a TokenVar referencing the lambda's anonymous function variable.
// The lambda is pushed onto ast as a top-level TokenFunc so it compiles
// before the enclosing function (asmjit can't nest addFunc/endFunc).
//
// Find the first `return <expr>;` statement reachable from a parsed body and
// report its expression's deduced type — the C++14 lambda return type.
// Descends into the common nested-statement forms (compounds, if/else, loops).
// Returns NULL when the body has no value-bearing return (e.g. only `return;`
// or none), in which case the caller keeps void.
static DataDef *deduce_return_type_from_stmt(TokenBase *stmt)
{
    if ( !stmt )
	return NULL;

    switch ( stmt->id() )
    {
    case TokenID::tkRETURN:
	{
	    TokenRETURN *ret = dynamic_cast<TokenRETURN *>(stmt);
	    if ( ret && ret->returns )
		return deduce_expr_type(ret->returns);
	    return NULL;
	}
    case TokenID::tkIF:
	{
	    TokenIF *tif = dynamic_cast<TokenIF *>(stmt);
	    if ( !tif )
		return NULL;
	    if ( DataDef *d = deduce_return_type_from_stmt(tif->statement) )
		return d;
	    return deduce_return_type_from_stmt(tif->elsestmt);
	}
    case TokenID::tkWHILE:
	{
	    TokenWHILE *tw = dynamic_cast<TokenWHILE *>(stmt);
	    return tw ? deduce_return_type_from_stmt(tw->statement) : NULL;
	}
    case TokenID::tkFOR:
	{
	    TokenFOR *tf = dynamic_cast<TokenFOR *>(stmt);
	    return tf ? deduce_return_type_from_stmt(tf->statement) : NULL;
	}
    case TokenID::tkDO:
	{
	    TokenDO *td = dynamic_cast<TokenDO *>(stmt);
	    return td ? deduce_return_type_from_stmt(td->statement) : NULL;
	}
    default:
	break;
    }

    // bare compound block: { ... }
    if ( TokenCpnd *cpnd = dynamic_cast<TokenCpnd *>(stmt) )
	for ( TokenStmt *s : cpnd->statements )
	    if ( DataDef *d = deduce_return_type_from_stmt(s) )
		return d;

    return NULL;
}

TokenBase *Program::parseLambda()
{
    static int lambda_counter = 0;

    DBG(cout << "parseLambda() START" << endl);

    auto resolve_lambda_param_type = [&](TokenBase *type_tb) -> TokenDataType * {
	return resolve_declared_type_token(*this, type_tb, true, true);
    };

    // we already consumed '[', peek at next token
    // [](params) { body }       — pure lambda (no capture)
    // [int](params) { body }    — pure lambda with return type
    // [&](params) { body }      — capture all outer vars by reference
    TokenBase *tn = nextToken();
    DataDef *rettype = &ddVOID;
    bool explicit_rettype = false;
    bool is_capturing = false;

    // check for [&] capture syntax
    if ( tn->id() == TokenID::tkBand )
    {
	is_capturing = true;
	tn = nextToken(); // consume &, expect ]
    }
    else if ( TokenDataType *ret_type = resolve_lambda_param_type(tn) )
    {
	rettype = &ret_type->definition;
	explicit_rettype = true;
	DBG(cout << "parseLambda() return type: " << rettype->name << endl);
	tn = nextToken();
    }

    if ( tn->id() != TokenID::tkClSqr )
	Throw(tn) << "Expecting ] in lambda expression" << flush;

    // expect '('
    tn = nextToken();
    if ( tn->id() != TokenID::tkOpBrk )
	Throw(tn) << "Expecting ( after lambda [...]" << flush;

    // generate unique name
    std::string lambda_name = "__lambda_" + std::to_string(lambda_counter++);

    DBG(cout << "parseLambda() name: " << lambda_name << endl);

    // create FuncDef
    FuncDef *func = new FuncDef(*rettype);
    funcdef_map[lambda_name] = func;
    func->has_captures = is_capturing;

    if ( is_capturing )
    {
	// Collect all currently visible vars from the enclosing compound chain
	// These are "potential captures" — whichever ones the body actually uses
	TokenCpnd *outer = compounds.empty() ? NULL : compounds.top();
	while ( outer )
	{
	    for ( auto *v : outer->variables )
		func->potential_captures.push_back(v);
	    // also capture method parameters from the outer scope
	    if ( outer->method )
		for ( auto *p : outer->method->parameters )
		    func->potential_captures.push_back(p);
	    outer = outer->parent;
	}
	// Capture-by-reference is lowered in the CIR builder: each enclosing
	// variable the body actually uses becomes a hidden `T *name` pointer
	// parameter (FuncDef::captured_vars), appended AFTER the user params, and
	// every call site forwards `&var`. So func->parameters carries ONLY the
	// user-declared params here — no synthetic env placeholder.
	DBG(cout << "parseLambda() [&] capturing " << func->potential_captures.size() << " outer vars" << endl);
    }

    // parse parameters (same pattern as parseFunction)
    std::vector<std::string> param_ids;
    TokenDataType *pb;

    while ( (tn=nextToken()) && tn->id() != TokenID::tkClBrk )
    {
	pb = resolve_lambda_param_type(tn);
	if ( !pb )
	    Throw(tn) << "Expecting type in lambda parameter list" << flush;

	tn = nextToken();

	if ( !is_contextual_identifier_token(tn) )
	    Throw(tn) << "Expecting identifier in lambda parameter list" << flush;

	std::string pid = contextual_identifier_name(tn);
	param_ids.push_back(pid);
	func->parameters.push_back(&pb->definition);
	func->param_typedef_names.push_back("");

	DBG(cout << "parseLambda() param: " << pb->definition.name << ' ' << pid << endl);

	// peek for comma or closing paren
	tn = peekToken();
	if ( tn && tn->id() == TokenID::tkComma )
	    nextToken(); // consume comma
    }

    // create Variable and Method (same as parseFunction)
    Variable *var = addVariable(NULL, *func, lambda_name);
    Method *method = new Method(*var);
    var->data = (void *)method;

    // Capture params are synthesized in the CIR builder (see parseLambda's
    // capture comment above) — method->parameters holds only the user params.
    for ( size_t i = 0; i < param_ids.size(); ++i )
    {
	Variable *pv = new Variable(param_ids[i], *func->parameters[i], 1, NULL, false);
	pv->flags |= vfPARAM | vfLOCAL;
	method->parameters.push_back(pv);
    }

    // expect '{' for the body
    tn = nextToken();
    if ( tn->id() != TokenID::tkOpBrc )
	Throw(tn) << "Expecting { for lambda body" << flush;

    // push compound scope and parse the body
    pushCompound();
    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
    if ( code )
	code->method = method;

    TokenFunc *tf = new TokenFunc(*var);
    DBG(cout << "parseLambda() calling parseCompound()" << endl);
    TokenCpnd *tc = dynamic_cast<TokenCpnd *>(parseCompound());

    tf->method = method;
    tf->parent = tc->parent;
    tf->variables = tc->variables;
    tf->statements = tc->statements;
    tf->deferred = tc->deferred;
    tf->end_line = tc->end_line;

    // C++14 lambda return-type deduction: with no explicit `[T](...)` return
    // type, deduce it from the body's first value-bearing `return`. FuncDef
    // returns is a C++ reference and cannot be reseated, so replace the
    // FuncDef with a fresh one carrying the deduced type (same pattern as the
    // return-type refresh in parseFunction). The Variable's `type` pointer and
    // funcdef_map entry are rebound; Method holds a reference to the Variable
    // so it picks up the new type automatically.
    if ( !explicit_rettype )
    {
	DataDef *deduced = NULL;
	for ( TokenStmt *s : tf->statements )
	    if ( (deduced = deduce_return_type_from_stmt(s)) )
		break;
	if ( deduced && deduced != &func->returns )
	{
	    DBG(cout << "parseLambda() deduced return type: "
		    << func->returns.name << " -> " << deduced->name << endl);
	    FuncDef *fresh = new FuncDef(*deduced);
	    fresh->parameters		 = func->parameters;
	    fresh->has_captures		 = func->has_captures;
	    fresh->potential_captures	 = func->potential_captures;
	    fresh->captures		 = func->captures;
	    fresh->return_types		 = func->return_types;
	    fresh->ref_params		 = func->ref_params;
	    fresh->const_params		 = func->const_params;
	    fresh->is_varargs		 = func->is_varargs;
	    fresh->is_void_params	 = func->is_void_params;
	    fresh->no_instrument_function = func->no_instrument_function;
	    fresh->explicit_alignment	 = func->explicit_alignment;
	    funcdef_map[lambda_name] = fresh;
	    var->type = fresh;
	    func = fresh;
	}
    }

    // push the lambda as a top-level function in the AST
    // It will be compiled before the enclosing function since
    // the enclosing function's ast.push happens after parseCompound returns.
    DBG(cout << "parseLambda() pushing " << lambda_name << " onto ast" << endl);
    ast.push(tf);
    pending_funcs.push_back(tf);

    DBG(cout << "parseLambda() END — returning TokenVar for " << lambda_name << endl);

    // return a TokenVar that references the lambda function variable
    // When compiled, TokenVar::compile() emits the function's address
    return new TokenVar(*var);
}

static void configure_nested_function_captures(Program &pgm, FuncDef *func)
{
    if ( !func )
	return;

    func->has_captures = true;
    func->potential_captures.clear();

    TokenCpnd *outer = pgm.compounds.empty() ? NULL : pgm.compounds.top();
    while ( outer )
    {
	for ( auto *v : outer->variables )
	    func->potential_captures.push_back(v);
	if ( outer->method )
	    for ( auto *p : outer->method->parameters )
		func->potential_captures.push_back(p);
	outer = outer->parent;
    }
    // No synthetic env parameter: capture-by-reference params are synthesized
    // per used variable in the CIR builder (FuncDef::captured_vars).
}

static bool literal_integer_value(TokenBase *tb, int64_t &out)
{
    if ( !tb )
    {
	out = 0;
	return true;
    }
    if ( tb->type() == TokenType::ttInteger || tb->type() == TokenType::ttChar )
    {
	out = tb->ival();
	return true;
    }
    // Unary minus: -N
    if ( tb->id() == TokenID::tkNeg )
    {
	int64_t inner;
	TokenOperator *op = static_cast<TokenOperator *>(tb);
	if ( literal_integer_value(op->right, inner) )
	{
	    out = -inner;
	    return true;
	}
    }
    // Unary plus: +N (no-op)
    if ( tb->id() == TokenID::tkAdd && static_cast<TokenOperator *>(tb)->left == nullptr )
    {
	TokenOperator *op = static_cast<TokenOperator *>(tb);
	return literal_integer_value(op->right, out);
    }
    // Bitwise NOT: ~N
    if ( tb->id() == TokenID::tkBnot )
    {
	int64_t inner;
	TokenOperator *op = static_cast<TokenOperator *>(tb);
	if ( literal_integer_value(op->right, inner) )
	{
	    out = ~inner;
	    return true;
	}
    }
    // Cast expression: (type)N — truncate/sign-extend to the target width
    if ( TokenCast *tc = dynamic_cast<TokenCast *>(tb) )
    {
	int64_t inner;
	if ( tc->expr && literal_integer_value(tc->expr, inner) && tc->cast_type )
	{
	    size_t sz = tc->cast_type->size;
	    if ( sz >= 8 )
		out = inner;
	    else if ( tc->cast_type->is_unsigned() )
		out = inner & ((uint64_t(1) << (sz * 8)) - 1);
	    else
	    {
		// Sign-extend from sz bytes
		uint64_t mask = (uint64_t(1) << (sz * 8)) - 1;
		int64_t val = inner & (int64_t)mask;
		int64_t sign = int64_t(1) << (sz * 8 - 1);
		out = (val ^ sign) - sign;
	    }
	    return true;
	}
    }
    return false;
}

static bool store_static_integer_array_value(Variable *var, size_t index, int64_t value)
{
    if ( !var || !var->data || !var->type || !var->type->is_integer() )
	return false;
    char *dst = (char *)var->data + index * var->type->size;
	switch ( var->type->rawtype() )
	{
	case DataType::dtBOOL:
	case DataType::dtCHAR:
	    *((int8_t *)dst) = (int8_t)value;
	    return true;
	case DataType::dtUINT8:
	    *((uint8_t *)dst) = (uint8_t)value;
	    return true;
	case DataType::dtINT16:
	case DataType::dtINT24:
	    *((int16_t *)dst) = (int16_t)value;
	    return true;
	case DataType::dtUINT16:
	case DataType::dtUINT24:
	    *((uint16_t *)dst) = (uint16_t)value;
	    return true;
	case DataType::dtINT32:
	    *((int32_t *)dst) = (int32_t)value;
	    return true;
	case DataType::dtUINT32:
	    *((uint32_t *)dst) = (uint32_t)value;
	    return true;
	case DataType::dtINT64:
	    *((int64_t *)dst) = (int64_t)value;
	    return true;
	case DataType::dtUINT64:
	    *((uint64_t *)dst) = (uint64_t)value;
	    return true;
	default:
	    return false;
    }
}

static bool initialize_static_fixed_array_data(Variable *var,
					       const std::vector<TokenBase *> &init_list)
{
    if ( !var || !var->is_fixed_array() || !var->data || !var->type )
	return false;
    size_t total = var->total_elements();
    size_t n = init_list.size() < total ? init_list.size() : total;
    for ( size_t i = 0; i < n; ++i )
    {
	int64_t value = 0;
	if ( !literal_integer_value(init_list[i], value) )
	    return false;
	if ( !store_static_integer_array_value(var, i, value) )
	    return false;
    }
    return true;
}

static bool is_char_array_element_type(DataDef *dd)
{
    // A char/unsigned-char ELEMENT, not a pointer to one. rawtype() strips
    // pointer-ness (`char *` -> dtCHAR), so without the is_pointer() guard an
    // array of char pointers (`char *names[3][2]`) would be mistaken for a 2D
    // char array and have its string-literal initializers expanded to bytes
    // ("excess elements" — the pointer slots expect addresses, not chars).
    return dd && !dd->is_pointer()
	       && (dd->rawtype() == DataType::dtCHAR
	       || dd->rawtype() == DataType::dtUINT8);
}

static TokenStructLit *char_init_from_literal(TokenStr *strtok, bool include_null)
{
    TokenStructLit *slit = new TokenStructLit();
    const std::string &s = strtok->str;
    for ( char c : s )
	slit->inits.push_back(new TokenInt((int64_t)(unsigned char)c));
    if ( include_null )
	slit->inits.push_back(new TokenInt(0));
    return slit;
}

static size_t find_struct_member_index(DataDefSTRUCT *sdd, const std::string &field_name)
{
    if ( !sdd )
	return 0;
    for ( size_t mi = 0; mi < sdd->members.size(); ++mi )
	if ( sdd->members[mi].first == field_name )
	    return mi;
    return sdd->members.size();
}

static void append_string_literal_chars(TokenStructLit *slit, TokenStr *strtok)
{
    const std::string &s = strtok->str;
    for ( char c : s )
	slit->inits.push_back(new TokenInt((int64_t)(unsigned char)c));
}

static void assign_initializer_range(std::vector<TokenBase *> &inits,
				     size_t first_index,
				     size_t last_index,
				     TokenBase *value)
{
    if ( inits.size() <= last_index )
	inits.resize(last_index + 1, NULL);
    for ( size_t idx = first_index; idx <= last_index; ++idx )
	inits[idx] = (idx == first_index) ? value : (value ? value->clone() : NULL);
}

static void infer_flexible_array_member_counts(DataDefSTRUCT *sdd,
					       const std::vector<TokenBase *> &init_list)
{
    if ( !sdd )
	return;
    for ( size_t i = 0; i < sdd->member_counts.size(); ++i )
    {
	if ( sdd->member_counts[i] != 0 )
	    continue;
	size_t inferred = 0;
	if ( i < init_list.size() )
	{
	    if ( TokenStructLit *slit = dynamic_cast<TokenStructLit *>(init_list[i]) )
		inferred = slit->inits.size();
	}
	if ( inferred == 0 )
	    continue;
	sdd->member_counts[i] = inferred;
	if ( i < sdd->member_offsets.size() && i < sdd->members.size() )
	{
	    size_t end = sdd->member_offsets[i] + sdd->members[i].second->size * inferred;
	    if ( end > sdd->size )
		sdd->size = DataDefSTRUCT::align_up(end, sdd->max_align);
	}
    }
}

static DataDef *peel_carray_dimensions(DataDef *base_type,
				       std::vector<uint32_t> &arr_dims,
				       TokenBase *&vla_size_expr)
{
    DataDef *decl_type = base_type;
    while ( DataDefCArray *alias_array = dynamic_cast<DataDefCArray *>(decl_type) )
    {
	if ( alias_array->count_expr )
	{
	    if ( !vla_size_expr )
		vla_size_expr = alias_array->count_expr;
	    arr_dims.push_back(1);
	}
	else
	    arr_dims.push_back((uint32_t)alias_array->count);
	decl_type = alias_array->element_type ? alias_array->element_type : &ddINT;
    }
    return decl_type;
}

// A parenthesized group at the front of the token queue is a function
// PARAMETER list (not constructor arguments) when its matching `)` is
// immediately followed by `{` — i.e. this is a function DEFINITION, not a
// ctor-call variable declaration. Disambiguates `string greet(int n) { ... }`
// (a function returning a class) from `string s(args);` (ctor call) now that
// class types (std::string, user classes with ctors) reach the ctor-call
// branch in parseDeclaration. tokens.front() is expected to be the `(`; it is
// NOT consumed here. (A function PROTOTYPE `string f(int);` ends in `;`, which
// is the most-vexing-parse case C++ also resolves as a declaration — out of
// scope here; only the unambiguous `{`-body definition is detected.)
static bool paren_group_is_function_def(Program &pgm)
{
    auto it = pgm.tokens.begin();
    if ( it == pgm.tokens.end() || (*it)->id() != TokenID::tkOpBrk )
	return false;
    int depth = 0;
    for ( ; it != pgm.tokens.end(); ++it )
    {
	TokenID id = (*it)->id();
	if ( id == TokenID::tkOpBrk ) { ++depth; continue; }
	if ( id == TokenID::tkClBrk )
	{
	    if ( --depth == 0 )
	    {
		++it; // token after the matching ')'
		return it != pgm.tokens.end()
		    && (*it)->id() == TokenID::tkOpBrc;
	    }
	}
    }
    return false;
}

// parse either a variable declaration, or a function declaration
TokenBase *Program::parseDeclaration(TokenDataType *tb, bool is_static)
{
    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
    TokenBase *nt; // next token;
    Variable *var;
    string id;
    std::vector<uint32_t> arr_dims;
    std::vector<TokenBase *> arr_dim_exprs;
    TokenBase *vla_size_expr = NULL;
    bool have_decl_id = false;
    Variable *provisional_decl_var = NULL;
    bool gotstatic = is_static || parsing_static_decl;
    bool gotconst = parsing_const_decl;
    // The flags cover exactly this declaration. Clear so nested declarations
    // (e.g. locals inside a `static void f() { string s = ...; }` body)
    // don't inherit static storage or const-ness.
    parsing_static_decl = false;
    parsing_const_decl = false;

    DBG(std::cout << "parseDeclaration(" << tb->str << ") START" << std::endl);

    // check for pointer declarator(s): type * [*...] identifier.
    // base_type is the declared type without any `*`s — comma-continuations
    // later in this function start fresh from base_type because each var
    // in `char *p, *q;` has its own `*`s, not cumulative.
    DataDef *base_type = &tb->definition;
    DataDef *decl_type = base_type;
    bool saw_pointer_decl = false;
    bool saw_const_after_star = false; // `int * const p` — top-level const on a pointer
    bool ret_is_ref = false;
    // If this declaration names a user typedef alias (not a builtin, where
    // tb->str == definition.name, nor a "struct tag" type, which isn't in
    // datatype_map), record the alias so the CIR backend emits ID("alias")
    // at the usage site — matching c2m's node_t tree. Every declarator in a
    // comma list re-enters parseDeclaration with a clone of the same tb, so
    // setting this on each created Variable covers them all.
    std::string decl_typedef_alias;
    if ( user_typedef_names.count(tb->str) )
	decl_typedef_alias = tb->str;
    // Function-pointer typedefs (DataDefFPTR) already represent pointers;
    // `DO_FUN *cmd;` and `DO_FUN cmd;` both name a function-pointer variable.
    bool is_fnptr_base = (dynamic_cast<DataDefFPTR *>(base_type) != NULL);
    // C allows CV-qualifiers (`const`/`restrict`) interleaved with
    // pointer stars: `type const *p`, `int * const *xpp`, etc.
    // Loop consuming any qualifier-or-star sequence; qualifiers are
    // treated as no-ops, stars stack the pointer level.
    while ( peekToken()
	 && (peekToken()->id() == TokenID::tkMul
	  || is_post_pointer_qualifier_token(peekToken())) )
    {
	if ( peekToken()->id() == TokenID::tkMul )
	{
	    nextToken(); // consume '*'
	    saw_pointer_decl = true;
	    saw_const_after_star = false; // reset — any const before this star is low-level
	    if ( !is_fnptr_base )
		decl_type = getPointerType(decl_type);
	    DBG(std::cout << "parseDeclaration() pointer: " << decl_type->name << std::endl);
	}
	else
	{
	    if ( saw_pointer_decl && peekToken()->id() == TokenID::tkCONST )
		saw_const_after_star = true; // const after last * = top-level const pointer
	    nextToken(); // consume const/restrict
	}
    }

    if ( !saw_pointer_decl )
    {
	if ( DataDefCArray *alias_array = dynamic_cast<DataDefCArray *>(base_type) )
	{
	    (void)alias_array;
	    decl_type = peel_carray_dimensions(base_type, arr_dims, vla_size_expr);
	}
    }

    if ( peekToken() && peekToken()->id() == TokenID::tkBand )
    {
	nextToken();
	ret_is_ref = true;
    }

    if ( !peekToken() )
	Throw(tb) << "Unexpected end of data: Expecting identifier after type" << flush;

    // Function-pointer variable declaration:
    //   RET (*name)(params);
    if ( peekToken() && peekToken()->id() == TokenID::tkOpBrk )
    {
	TokenBase *open = nextToken(); // consume '('
	TokenBase *inner = nextToken();
	if ( inner && is_contextual_identifier_token(inner) )
	{
	    // Plain parenthesized declarator: `int *(p[25]);`
	    id = contextual_identifier_name(inner);
	    TokenBase *rbrk = peekToken();
	    while ( rbrk && rbrk->id() == TokenID::tkOpSqr )
	    {
		nextToken(); // consume '['
		TokenBase *peek = peekToken();
		if ( peek && peek->id() == TokenID::tkClSqr )
		{
		    nextToken(); // consume ']'
		    arr_dims.push_back(0);
		}
		else
		{
		    int64_t n = parse_constant_integer_expression(*this);
		    if ( n < 0 )
			Throw(tb) << "Fixed-size array dimension must be non-negative" << flush;
		    arr_dims.push_back((uint32_t)n);
		    TokenBase *cl = nextToken();
		    if ( !cl || cl->id() != TokenID::tkClSqr )
			Throw(cl ? cl : tb) << "Expected ] in array declaration" << flush;
		}
		rbrk = peekToken();
	    }
	    rbrk = nextToken();
	    if ( !rbrk || rbrk->id() != TokenID::tkClBrk )
		Throw(rbrk ? rbrk : open) << "Expecting ')' after parenthesized declarator" << flush;
	    have_decl_id = true;
	    nt = peekToken();
	}
	else if ( inner && inner->id() == TokenID::tkMul )
	{
	    TokenBase *name_tok = nextToken();
	    while ( name_tok && (is_restrict_token(name_tok)
	                      || name_tok->id() == TokenID::tkCONST
	                      || name_tok->id() == TokenID::tkVOLATILE) )
		name_tok = nextToken();
	    if ( !name_tok || !is_contextual_identifier_token(name_tok) )
		Throw(name_tok ? name_tok : open) << "Expecting identifier in function pointer declaration" << flush;
	    id = contextual_identifier_name(name_tok);
	    TokenBase *rbrk = peekToken();
	    while ( rbrk && rbrk->id() == TokenID::tkOpSqr )
	    {
		nextToken(); // consume '['
		TokenBase *peek = peekToken();
		if ( peek && peek->id() == TokenID::tkClSqr )
		{
		    nextToken(); // consume ']'
		    arr_dims.push_back(0);
		}
		else
		{
		    int64_t n = parse_constant_integer_expression(*this);
		    if ( n < 0 )
			Throw(tb) << "Fixed-size array dimension must be non-negative" << flush;
		    arr_dims.push_back((uint32_t)n);
		    TokenBase *cl = nextToken();
		    if ( !cl || cl->id() != TokenID::tkClSqr )
			Throw(cl ? cl : tb) << "Expected ] in array declaration" << flush;
		}
		rbrk = peekToken();
	    }
	    rbrk = nextToken();
	    if ( !rbrk || rbrk->id() != TokenID::tkClBrk )
		Throw(rbrk ? rbrk : open) << "Expecting ')' after function pointer name" << flush;
	    TokenBase *param_open = peekToken();
	    if ( param_open && param_open->id() == TokenID::tkOpSqr )
	    {
		// Pointer-to-array: `type (*name)[N]`
		std::vector<uint32_t> ptr_array_dims;
		while ( peekToken() && peekToken()->id() == TokenID::tkOpSqr )
		{
		    nextToken(); // consume '['
		    TokenBase *dim_peek = peekToken();
		    if ( dim_peek && dim_peek->id() == TokenID::tkClSqr )
		    {
			nextToken(); // consume ']' for unsized
			ptr_array_dims.push_back(0);
			continue;
		    }
		    int64_t dim = parse_constant_integer_expression(*this);
		    if ( dim < 0 )
			Throw(open) << "Pointer-to-array dimension must be non-negative" << flush;
		    ptr_array_dims.push_back((uint32_t)dim);
		    TokenBase *cl = nextToken();
		    if ( !cl || cl->id() != TokenID::tkClSqr )
			Throw(cl ? cl : open) << "Expected ] in pointer-to-array declaration" << flush;
		}
		DataDef *array_type = decl_type;
		for ( size_t di = ptr_array_dims.size(); di-- > 0; )
		    array_type = new DataDefCArray(*array_type, array_type->name, ptr_array_dims[di], NULL);
		decl_type = getPointerType(array_type);
		have_decl_id = true;
		nt = peekToken();
	    }
	    else
	    {
		nextToken(); // consume the peeked token
		if ( !param_open || param_open->id() != TokenID::tkOpBrk )
		    Throw(param_open ? param_open : open) << "Expecting '(' for function pointer parameter list" << flush;

		FuncDef *func = parseFnPtrParams(*decl_type);
		decl_type = new DataDefFPTR(func);
		have_decl_id = true;
		nt = peekToken();
	    }
	}
	else
	{
	    pushToken(inner);
	    pushToken(open);
	}
    }

    if ( !have_decl_id )
	nt = nextToken();

    if ( !have_decl_id && !is_contextual_identifier_token(nt) )
    {
	DBG(cerr << "parseDeclaration() nt->type()=" << (int)nt->type() << endl);
	Throw(nt) << "Expecting identifier after type" << flush;
    }
    // grab identifier string
    if ( !have_decl_id )
	id = contextual_identifier_name(nt);
    DBG(std::cout << "parseDeclaration() identifier: " << id << std::endl);

    if ( !(nt=peekToken()) )
	Throw << "expecting token after identifier" << flush;

    // auto type inference: auto fn = func_name; or auto fn = [](params) { body };
    if ( &tb->definition == &ddAUTO )
    {
	if ( nt->id() != TokenID::tkAssign )
	    Throw(tb) << "'auto' requires an initializer" << flush;

	// consume '='
	nextToken();
	TokenBase *rhs_tok = peekToken();

	Variable *rhs_var = NULL;
	TokenBase *rhs_node = NULL;

	// General `auto x = <expr>;` deduction is C++11+ and C23+; in C89..C17
	// `auto` is a STORAGE-CLASS specifier, not a deduced type. Gate per I4:
	// allow deduction in C++/madc always, and in C only at C23. The fn-name /
	// lambda fn-ptr forms below are unaffected (they are the original behaviour
	// and remain available wherever this block runs).
	bool auto_deduction_allowed = !is_c_mode() || language_std == STD_C23;

	// Decide whether the initializer is a function name or lambda (the original
	// fn-ptr path) or a general expression (P2.3 deduction). A bare function
	// NAME (`auto fn = foo;`) takes the fn-ptr path; a function CALL
	// (`auto r = foo(args);`) is a general expression whose deduced type is the
	// call's RETURN type — distinguished by the token following the identifier
	// (tokens[0] is the identifier itself, tokens[1] the next token).
	bool rhs_is_lambda = rhs_tok && rhs_tok->id() == TokenID::tkOpSqr;
	bool rhs_is_func_name = false;
	if ( !rhs_is_lambda && rhs_tok && rhs_tok->type() == TokenType::ttIdentifier )
	{
	    Variable *probe = findVariable(((TokenIdent *)rhs_tok)->str);
	    bool followed_by_call = tokens.size() > 1 && tokens[1]
				    && tokens[1]->id() == TokenID::tkOpBrk;
	    rhs_is_func_name = probe && probe->type && probe->type->is_function()
			       && !followed_by_call;
	}

	if ( rhs_is_lambda || rhs_is_func_name )
	{
	    if ( rhs_is_lambda )
	    {
		// lambda: auto fn = [](params) { body };
		nextToken(); // consume '['
		rhs_node = parseLambda();
		rhs_var = &(dynamic_cast<TokenVar *>(rhs_node)->var);
	    }
	    else
	    {
		// named function: auto fn = func_name;
		nextToken(); // consume identifier
		rhs_var = findVariable(((TokenIdent *)rhs_tok)->str);
		rhs_node = new TokenVar(*rhs_var);
	    }

	    // consume the semicolon
	    TokenBase *semi = peekToken();
	    if ( semi && semi->id() == TokenID::tkSemi )
		nextToken();

	    // create a DataDefFPTR wrapping the target function's FuncDef
	    FuncDef *target_func = (FuncDef *)rhs_var->type;
	    DataDefFPTR *fptr_type = new DataDefFPTR(target_func);

	    bool alloc = (!code || gotstatic) ? true : false;
	    var = addVariable(code, *fptr_type, id, 1, NULL, alloc);
	    if ( !decl_typedef_alias.empty() )
		var->typedef_name = decl_typedef_alias;
	    TokenDecl *td = new TokenDecl(*var);
	    td->file = tb->file;
	    td->line = tb->line;
	    td->column = tb->column;

	    // build the assignment AST
	    TokenAssign *assign = new TokenAssign();
	    assign->file = tb->file;
	    assign->line = tb->line;
	    assign->column = tb->column;
	    assign->left = new TokenVar(*var);
	    assign->right = rhs_node;
	    td->initialize = assign;

	    DBG(std::cout << "parseDeclaration() auto: " << id << " = " << rhs_var->name << std::endl);
	    return td;
	}

	// General-expression `auto x = <expr>;` (P2.3) — only when std-gated on.
	if ( !auto_deduction_allowed )
	    Throw(tb) << "'auto' type deduction requires a function name or lambda"
		      << " (general 'auto x = expr' is C++11+/C23+)" << flush;

	// Parse the initializer expression, then deduce x's concrete type from it
	// (I2 — a real type, never a bespoke "auto" type) via the same engine the
	// lambda return-type deduction uses (I6 — one deduction path, no duplicate).
	TokenBase *init_expr = parseExpression(nextToken(), true);
	if ( !init_expr )
	    Throw(tb) << "Failed to parse 'auto' initializer expression" << flush;

	DataDef *deduced = deduce_expr_type(init_expr);
	if ( !deduced || deduced == &ddVOID )
	    Throw(tb) << "Cannot deduce 'auto' type from this initializer" << flush;

	// consume the semicolon
	TokenBase *semi = peekToken();
	if ( semi && semi->id() == TokenID::tkSemi )
	    nextToken();

	bool alloc = (!code || gotstatic) ? true : false;
	var = addVariable(code, *deduced, id, 1, NULL, alloc);
	if ( !decl_typedef_alias.empty() )
	    var->typedef_name = decl_typedef_alias;
	TokenDecl *td = new TokenDecl(*var);
	td->file = tb->file;
	td->line = tb->line;
	td->column = tb->column;

	TokenAssign *assign = new TokenAssign();
	assign->file = tb->file;
	assign->line = tb->line;
	assign->column = tb->column;
	assign->left = new TokenVar(*var);
	assign->right = init_expr;
	td->initialize = assign;

	DBG(std::cout << "parseDeclaration() auto: " << id << " = <expr> deduced "
		<< deduced->name << std::endl);
	return td;
    }

    // Check for C fixed-size array declaration: type id[N][M]... or type id[] = {...}
    // Also handles C99 variable-length arrays: `T id[expr]` where `expr`
    // references a runtime value. The first dim's runtime expression is
    // captured on the Variable as `vla_size_expr`; the variable then acts
    // as a pointer to a heap buffer allocated at scope entry and freed at
    // scope exit (see TokenCpnd::voperand / TokenCpnd::cleanup).
    while ( nt && nt->id() == TokenID::tkOpSqr )
    {
	nextToken(); // consume [
	TokenBase *peek = peekToken();
	if ( peek && peek->id() == TokenID::tkClSqr )
	{
	    // [] — size to be inferred from initializer
	    nextToken(); // consume ]
	    arr_dims.push_back(0);
	    arr_dim_exprs.push_back(NULL);
	}
	else
	{
	    // Scan ahead to the matching `]` to detect any non-constant
	    // identifier — that makes the dim a runtime expression (VLA).
	    // Constants (enum values, vfCONSTANT vars, typedef'd integer
	    // constants) stay on the parse_constant_integer_expression path
	    // because resolve_integer_constant handles them.
	    bool is_vla = bracket_dim_needs_runtime_value(*this);
	    if ( is_vla && arr_dims.empty() && !vla_size_expr )
	    {
		// First-dim VLA: capture the runtime expression.
		vla_size_expr = parseExpression(nextToken(), true);
		TokenBase *cl = nextToken();
		if ( !cl || cl->id() != TokenID::tkClSqr )
		    Throw(cl ? cl : tb) << "Expected ] after VLA size expression" << flush;
		arr_dims.push_back(1); // sentinel; real count is runtime
		arr_dim_exprs.push_back(vla_size_expr);
	    }
	    else if ( is_vla )
	    {
		TokenBase *dim_expr = parseExpression(nextToken(), true);
		TokenBase *cl = nextToken();
		if ( !cl || cl->id() != TokenID::tkClSqr )
		    Throw(cl ? cl : tb) << "Expected ] after VLA size expression" << flush;
		if ( !vla_size_expr )
		{
		    if ( !arr_dims.empty() )
			vla_size_expr = new TokenInt((int64_t)arr_dims[0]);
		    else
			vla_size_expr = dim_expr;
		}
		arr_dims.push_back(1); // runtime count carried by arr_dim_exprs
		arr_dim_exprs.push_back(dim_expr);
	    }
	    else
	    {
		int64_t n = parse_constant_integer_expression(*this);
		if ( n < 0 )
		    Throw(tb) << "Fixed-size array dimension must be non-negative" << flush;
		// GCC: int arr[0] has sizeof 0; keep the zero dim.
		arr_dims.push_back((uint32_t)n);
		arr_dim_exprs.push_back(NULL);
		TokenBase *cl = nextToken();
		if ( !cl || cl->id() != TokenID::tkClSqr )
		    Throw(cl ? cl : tb) << "Expected ] in array declaration" << flush;
	    }
	}
	nt = peekToken();
	if ( !nt )
	    Throw(tb) << "Unexpected end of data in array declaration" << flush;
    }

    std::string storage_alias_name;
    if ( is_attribute_identifier_token(nt) )
    {
	TokenBase *attr = nextToken();
	nt = consume_gnu_attributes(*this, attr, NULL, &storage_alias_name);
	if ( nt )
	{
	    pushToken(nt);
	    nt = peekToken();
	}
    }

    // Preserve pointer semantics for `char *p = "literal";`.
    // Only real array declarators (`char buf[] = "literal";`) should take the
    // char-array string-initializer path below.

    // Constructor call syntax: ClassName var(arg1, arg2, ...);
    // Only for user-defined classes with a constructor.
    if ( !ret_is_ref
      && nt->id() == TokenID::tkOpBrk && arr_dims.empty()
      && decl_type->basetype() == BaseType::btClass
      && !paren_group_is_function_def(*this) )
    {
	DataDefCLASS *ddc = static_cast<DataDefCLASS *>(decl_type);
	if ( ddc->has_user_ctor )
	{
	    nextToken(); // consume '('
	    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
	    bool alloc = (!code || gotstatic) ? true : false;
	    var = addVariable(code, *decl_type, id, 1, NULL, alloc);
	    if ( !decl_typedef_alias.empty() )
		var->typedef_name = decl_typedef_alias;
	    TokenDecl *td = new TokenDecl(*var);
	    td->file = tb->file;
	    td->line = tb->line;
	    td->column = tb->column;
	    // Parse constructor arguments
	    while ( peekToken() && peekToken()->id() != TokenID::tkClBrk )
	    {
		TokenBase *arg = parseExpression(nextToken(), true);
		td->ctor_args.push_back(arg);
		if ( peekToken() && peekToken()->id() == TokenID::tkComma )
		    nextToken(); // consume ','
	    }
	    if ( !peekToken() || peekToken()->id() != TokenID::tkClBrk )
		Throw(tb) << "Expected ')' after constructor arguments" << flush;
	    nextToken(); // consume ')'
	    if ( peekToken() && peekToken()->id() == TokenID::tkSemi )
		nextToken(); // consume ';'
	    return td;
	}
    }

    // variable declaration
    if ( nt->id() == TokenID::tkSemi || nt->id() == TokenID::tkAssign
      || nt->id() == TokenID::tkComma )
    {
	if ( ret_is_ref )
	    Throw(tb) << "Reference variables are not supported" << flush;

	// parse brace-enclosed initializer list for fixed-size arrays and structs
	std::vector<TokenBase *> init_list;
	bool saw_brace_init = false;
	// Only real user-defined structs/classes accept brace init.
	// Built-in class types (std::string, ostream, etc.) use DataDefCLASS
	// but have a concrete DataType; user-defined structs/classes use
	// dtRESERVED. Discriminate on that.
    bool is_struct_init = arr_dims.empty()
	    && dynamic_cast<DataDefSTRUCT *>(decl_type) != NULL
	    && decl_type->type() == DataType::dtRESERVED;
	bool is_simd_init = arr_dims.empty() && decl_type && decl_type->is_simd();
	if ( nt->id() == TokenID::tkAssign && !provisional_decl_var )
	{
	    bool alloc = parsing_extern_decl ? false : ((!code || gotstatic) ? true : false);
	    uint32_t prov_count = 1;
	    for ( auto d : arr_dims ) prov_count *= d;
	    provisional_decl_var = addVariable(code, *decl_type, id, prov_count, NULL, alloc);
	    if ( gotstatic )
		provisional_decl_var->flags |= vfSTATIC;
	    if ( parsing_extern_decl )
		provisional_decl_var->flags |= vfEXTERN;
	    // Set dims early so self-referencing init expressions like
	    // &e[1] in `struct E e[2] = { { 0, &e[1] }, ... }` can
	    // resolve the array element size correctly.
	    if ( !arr_dims.empty() && !vla_size_expr )
	    {
		provisional_decl_var->dims = arr_dims;
		provisional_decl_var->flags |= vfFIXEDARRAY;
	    }
	}
	if ( nt->id() == TokenID::tkAssign && (!arr_dims.empty() || is_struct_init || is_simd_init) )
	{
	    // peek past '=' to see if we have { (brace list) or "..." (string lit for char arr)
	    nextToken(); // consume '='
	    TokenBase *peek0 = peekToken();
	    if ( !peek0 )
		Throw(nt) << "Expected initializer after '='" << flush;

	    auto looks_like_wide_string_payload = [](TokenBase *tok) -> bool {
		if ( !tok || tok->type() != TokenType::ttString )
		    return false;
		if ( static_cast<TokenStr *>(tok)->wide )
		    return true;
		const std::string &s = static_cast<TokenStr *>(tok)->str;
		if ( s.empty() || (s.size() % 4) != 0 )
		    return false;
		for ( size_t i = 0; i + 3 < s.size(); i += 4 )
		    if ( s[i + 3] != '\0' )
			return false;
		return true;
	    };
	    bool wide_string_array_init =
		!arr_dims.empty()
		&& decl_type->rawtype() == DataType::dtINT32
		&& arr_dims.size() == 1
		&& looks_like_wide_string_payload(peek0);
	    // String-literal array init:
	    // char / signed char / unsigned char buf[] = "hello";
	    // wchar_t w[] = L"hello";
	    if ( !arr_dims.empty()
	      && peek0->type() == TokenType::ttString
	      && ((decl_type->rawtype() == DataType::dtCHAR
	       || decl_type->rawtype() == DataType::dtUINT8)
	       || wide_string_array_init)
	      && arr_dims.size() == 1 )
	    {
		// C concatenates adjacent string literals, so consume all
		// immediately consecutive ttString tokens here.
		while ( peekToken() && peekToken()->type() == TokenType::ttString )
		{
		    TokenBase *strtok = nextToken();
		    const std::string &s = ((TokenStr *)strtok)->str;
		    if ( wide_string_array_init )
		    {
			for ( size_t i = 0; i + 3 < s.size(); i += 4 )
			{
			    uint32_t cp = (uint8_t)s[i]
				| ((uint32_t)(uint8_t)s[i + 1] << 8)
				| ((uint32_t)(uint8_t)s[i + 2] << 16)
				| ((uint32_t)(uint8_t)s[i + 3] << 24);
			    init_list.push_back(new TokenInt((int64_t)cp));
			}
		    }
		    else
		    {
			for ( char c : s )
			    init_list.push_back(new TokenInt((int64_t)(unsigned char)c));
		    }
		}
		// C89/C99: if the explicit array size exactly matches the
		// string length, the null terminator is omitted (e.g.
		// `char c[3] = "abc";` is valid). For inferred sizes and
		// any size larger than the literal, append '\0'.
		if ( arr_dims[0] == 0 || arr_dims[0] > init_list.size() )
		    init_list.push_back(new TokenInt(0)); // null terminator
	    }
	    else if ( is_struct_init && peek0->id() != TokenID::tkOpBrc )
	    {
		// struct-copy init: `struct S a = expr;` where `expr` is
		// another struct-typed lvalue. Push '=' back and fall
		// through to the normal `=` init path below; TokenAssign's
		// struct-to-struct branch emits a bytewise memcpy at
		// compile time.
		pushToken(nt);
	    }
	    else if ( is_simd_init && peek0->id() != TokenID::tkOpBrc )
	    {
		// SIMD expression init: `V2SF x = (V2SF) y;`
		// Brace-init stays in this branch; plain expression init
		// falls through to the normal assignment initializer path.
		pushToken(nt);
	    }
	    else
	    {
	    if ( peek0->id() != TokenID::tkOpBrc )
		Throw(nt) << "Expected '{' or string literal for initializer" << flush;
	    nextToken(); // consume '{'
	    saw_brace_init = true;
	    // parse comma-separated elements up to '}'. Each element may itself
	    // be a brace-list (for array-of-structs or nested struct members).
	    auto padded_char_string_literal = [&](TokenStr *strtok,
						  size_t target_count) -> TokenStructLit * {
		TokenStructLit *slit = char_init_from_literal(strtok, false);
		while ( peekToken() && peekToken()->type() == TokenType::ttString )
		    append_string_literal_chars(slit, (TokenStr *)nextToken());
		if ( target_count == 0 )
		    slit->inits.push_back(new TokenInt(0));
		else
		    while ( slit->inits.size() < target_count )
			slit->inits.push_back(new TokenInt(0));
		return slit;
	    };
	    auto zero_array_initializer = [&](size_t depth) -> TokenBase * {
		if ( depth + 1 >= arr_dims.size() )
		    return new TokenInt(0);
		TokenStructLit *slit = new TokenStructLit();
		size_t count = 1;
		for ( size_t i = depth + 1; i < arr_dims.size(); ++i )
		    count *= (size_t)arr_dims[i];
		for ( size_t i = 0; i < count; ++i )
		    slit->inits.push_back(new TokenInt(0));
		return slit;
	    };
	    std::function<TokenStructLit *(size_t)> read_struct_lit;
	    read_struct_lit = [&](size_t depth) -> TokenStructLit * {
		nextToken(); // consume '{'
		TokenStructLit *slit = new TokenStructLit();
		while ( true )
		{
		    TokenBase *iln = peekToken();
		    if ( !iln )
			Throw(tb) << "Unexpected end of data in nested initializer" << flush;
		    if ( iln->id() == TokenID::tkClBrc )
		    {
			nextToken(); // consume '}'
			break;
		    }
		    if ( iln->id() == TokenID::tkOpBrc )
			slit->inits.push_back(read_struct_lit(depth + 1));
		    else
		    {
			TokenBase *ni = nextToken();
			// Skip designator `.field =` in nested init
			size_t design_first = 0;
			size_t design_last = 0;
			bool array_designator = false;
			if ( ni->id() == TokenID::tkDot )
			{
			    nextToken(); // field name
			    nextToken(); // '='
			    ni = nextToken();
			}
			else
			    array_designator = parse_array_designator_initializer(*this, ni,
				design_first, design_last);
			if ( ni->id() == TokenID::tkOpBrc )
			{
			    pushToken(ni);
			    TokenBase *nested = read_struct_lit(depth + 1);
			    if ( array_designator )
				assign_initializer_range(slit->inits, design_first, design_last, nested);
			    else
				slit->inits.push_back(nested);
			}
			else
			{
			    bool handled_string_subarray = false;
			    if ( !arr_dims.empty()
			      && arr_dims.size() > 1
			      && ni->type() == TokenType::ttString
			      && is_char_array_element_type(decl_type) )
			    {
				size_t target_count = 0;
				if ( depth + 1 < arr_dims.size() )
				    target_count = arr_dims[depth + 1];
				else if ( depth < arr_dims.size() )
				    target_count = arr_dims[depth];
				TokenBase *nested = padded_char_string_literal((TokenStr *)ni,
				    target_count);
				if ( array_designator )
				    assign_initializer_range(slit->inits, design_first, design_last, nested);
				else if ( depth + 1 >= arr_dims.size() )
				{
				    TokenStructLit *nested_lit = (TokenStructLit *)nested;
				    for ( TokenBase *child : nested_lit->inits )
					slit->inits.push_back(child);
				}
				else
				    slit->inits.push_back(nested);
				handled_string_subarray = true;
			    }
			    if ( !handled_string_subarray )
			    {
				TokenBase *expr = parseExpression(ni);
				if ( array_designator )
				    assign_initializer_range(slit->inits, design_first, design_last, expr);
				else
				    slit->inits.push_back(expr);
			    }
			}
		    }
		    TokenBase *isep = peekToken();
		    if ( isep && isep->id() == TokenID::tkComma )
			nextToken();
		}
		if ( !arr_dims.empty() && depth < arr_dims.size() )
		    while ( slit->inits.size() < arr_dims[depth] )
			slit->inits.push_back(zero_array_initializer(depth));
		return slit;
	    };
	    while ( true )
	    {
		TokenBase *look = peekToken();
		if ( !look )
		    Throw(tb) << "Unexpected end of data in initializer" << flush;
		if ( look->id() == TokenID::tkClBrc )
		{
		    nextToken(); // consume '}'
		    break;
		}
		if ( look->id() == TokenID::tkOpBrc )
		{
		    init_list.push_back(read_struct_lit(1));
		}
		else
		{
		    // C99 designated initializer: `.field = value`
		    // GNU legacy designated initializer: `field: value`
		    // Skip the designator and use the value expression.
		    TokenBase *next_init = nextToken();
		    if ( next_init->id() == TokenID::tkDot
		      || (is_struct_init && is_contextual_identifier_token(next_init)
		       && peekToken() && peekToken()->id() == TokenID::tkTerC) )
		    {
			std::vector<std::string> field_path;
			TokenBase *field_tok = next_init;
			if ( next_init->id() == TokenID::tkDot )
			{
			    field_tok = nextToken(); // consume field name
			    if ( !is_contextual_identifier_token(field_tok) )
				Throw(field_tok) << "Expecting field name in designated initializer" << flush;
			    field_path.push_back(contextual_identifier_name(field_tok));
			    while ( peekToken() && peekToken()->id() == TokenID::tkDot )
			    {
				nextToken();
				TokenBase *nested_field = nextToken();
				if ( !is_contextual_identifier_token(nested_field) )
				    Throw(nested_field) << "Expecting field name in designated initializer" << flush;
				field_path.push_back(contextual_identifier_name(nested_field));
			    }
			    TokenBase *eq = nextToken(); // consume '='
			    if ( eq->id() != TokenID::tkAssign )
				Throw(eq) << "Expecting '=' after designated initializer" << flush;
			}
			else
			{
			    field_path.push_back(contextual_identifier_name(field_tok));
			    nextToken(); // consume ':'
			}
			next_init = nextToken();
			if ( is_struct_init )
			{
			    DataDefSTRUCT *target_sdd = dynamic_cast<DataDefSTRUCT *>(decl_type);
			    std::vector<TokenBase *> *target_inits = &init_list;
			    size_t field_index = 0;
			    for ( size_t pi = 0; pi < field_path.size(); ++pi )
			    {
				const std::string &field_name = field_path[pi];
				field_index = target_sdd ? target_sdd->members.size() : 0;
				if ( target_sdd )
				{
				    for ( size_t mi = 0; mi < target_sdd->members.size(); ++mi )
				    {
					if ( target_sdd->members[mi].first == field_name )
					{
					    field_index = mi;
					    break;
					}
				    }
				}
				if ( !target_sdd || field_index >= target_sdd->members.size() )
				    Throw(field_tok) << "Unknown field '" << field_name << "' in designated initializer" << flush;
				if ( target_inits->size() <= field_index )
				    target_inits->resize(field_index + 1, NULL);
				if ( pi + 1 == field_path.size() )
				    break;
				DataDefSTRUCT *nested_sdd = dynamic_cast<DataDefSTRUCT *>(target_sdd->members[field_index].second);
				if ( !nested_sdd )
				    Throw(field_tok) << "Field '" << field_name << "' is not a struct in designated initializer" << flush;
				TokenStructLit *nested_lit = dynamic_cast<TokenStructLit *>((*target_inits)[field_index]);
				if ( !nested_lit )
				{
				    nested_lit = new TokenStructLit();
				    (*target_inits)[field_index] = nested_lit;
				}
				target_inits = &nested_lit->inits;
				target_sdd = nested_sdd;
			    }
			    if ( next_init && next_init->id() == TokenID::tkOpBrc )
			    {
				pushToken(next_init);
				(*target_inits)[field_index] = read_struct_lit(1);
				TokenBase *sep = peekToken();
				if ( sep && sep->id() == TokenID::tkComma )
				    nextToken();
				continue;
			    }
			    (*target_inits)[field_index] = parseExpression(next_init);
			    TokenBase *sep = peekToken();
			    if ( sep && sep->id() == TokenID::tkComma )
				nextToken();
			    continue;
			}
		    }
		    // Array designator: [index] = value
		    else if ( next_init->id() == TokenID::tkOpSqr )
		    {
			size_t first_index = 0;
			size_t last_index = 0;
			parse_array_designator_initializer(*this, next_init,
			    first_index, last_index);
			TokenBase *design_value = NULL;
			if ( next_init->id() == TokenID::tkOpBrc )
			{
			    pushToken(next_init);
			    design_value = read_struct_lit(1);
			}
			else
			    design_value = parseExpression(next_init);
			assign_initializer_range(init_list, first_index, last_index, design_value);
			TokenBase *sep = peekToken();
			if ( sep && sep->id() == TokenID::tkComma )
			    nextToken();
			continue;
		    }
		    if ( is_struct_init && next_init->type() == TokenType::ttString )
		    {
			DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(decl_type);
			size_t field_index = init_list.size();
			if ( sdd && field_index < sdd->members.size()
			  && field_index < sdd->member_counts.size()
			  && sdd->member_counts[field_index] != 1
			  && is_char_array_element_type(sdd->members[field_index].second) )
			{
			    TokenStructLit *slit = char_init_from_literal((TokenStr *)next_init, false);
			    while ( peekToken() && peekToken()->type() == TokenType::ttString )
				append_string_literal_chars(slit, (TokenStr *)nextToken());
			    size_t member_count = sdd->member_counts[field_index];
			    if ( member_count == 0 )
				slit->inits.push_back(new TokenInt(0));
			    else
			    {
				if ( member_count > 0 && slit->inits.size() > member_count )
				    slit->inits.resize(member_count);
				while ( slit->inits.size() < member_count )
				    slit->inits.push_back(new TokenInt(0));
			    }
			    init_list.push_back(slit);
			    TokenBase *sep = peekToken();
			    if ( sep && sep->id() == TokenID::tkComma )
				nextToken();
			    continue;
			}
		    }
		    if ( !arr_dims.empty()
		      && arr_dims.size() > 1
		      && next_init->type() == TokenType::ttString
		      && is_char_array_element_type(decl_type) )
		    {
			TokenStructLit *slit = char_init_from_literal((TokenStr *)next_init, false);
			while ( peekToken() && peekToken()->type() == TokenType::ttString )
			    append_string_literal_chars(slit, (TokenStr *)nextToken());
			size_t inner_count = arr_dims[1];
			if ( inner_count == 0 )
			    slit->inits.push_back(new TokenInt(0));
			else
			{
			    // C89/C99: truncate excess characters when the
			    // string literal is longer than the array dimension.
			    if ( inner_count > 0 && slit->inits.size() > inner_count )
				slit->inits.resize(inner_count);
			    while ( slit->inits.size() < inner_count )
				slit->inits.push_back(new TokenInt(0));
			}
			init_list.push_back(slit);
			TokenBase *sep = peekToken();
			if ( sep && sep->id() == TokenID::tkComma )
			    nextToken();
			continue;
		    }
		    TokenBase *expr = parseExpression(next_init);
		    init_list.push_back(expr);
		}
		TokenBase *sep = peekToken();
		if ( sep && sep->id() == TokenID::tkComma )
		    nextToken(); // consume ','
	    }
	    }
	    // Infer size for arrays with dims[0] == 0; validate count
	    if ( !arr_dims.empty() )
	    {
		bool has_nested_init = false;
		for ( TokenBase *init : init_list )
		    if ( dynamic_cast<TokenStructLit *>(init) != NULL )
			has_nested_init = true;

		size_t tail_count = 1;
		for ( size_t di = 1; di < arr_dims.size(); ++di )
		    tail_count *= (size_t)arr_dims[di];

		if ( arr_dims[0] == 0 )
		{
		    if ( arr_dims.size() > 1 && !has_nested_init && tail_count > 0 )
			arr_dims[0] = (uint32_t)((init_list.size() + tail_count - 1) / tail_count);
		    else
			arr_dims[0] = (uint32_t)init_list.size();
		}

		size_t initializer_capacity = (size_t)arr_dims[0];
		if ( arr_dims.size() > 1 && !has_nested_init )
		    initializer_capacity *= tail_count;
		if ( init_list.size() > initializer_capacity )
		    Throw(tb) << "Too many initializers for array (expected " << initializer_capacity << ")" << flush;
	    }
	    if ( is_struct_init )
		infer_flexible_array_member_counts(dynamic_cast<DataDefSTRUCT *>(decl_type), init_list);
	}
	else if ( !arr_dims.empty() )
	{
	    // GCC allows zero-length arrays (sizeof 0, flexible member).
	    // Only error when a dim is 0 because the size was truly
	    // missing (empty brackets with no initializer) — but we
	    // can't distinguish that from explicit [0] at this point,
	    // so just allow it.
	}

	bool alloc = parsing_extern_decl ? false : ((!code || gotstatic) ? true : false);
	uint32_t elem_count = 1;
	for ( auto d : arr_dims ) elem_count *= d;
	if ( provisional_decl_var )
	{
	    var = provisional_decl_var;
	    var->type = decl_type;
	    var->count = elem_count;
	}
	else
	    var = addVariable(code, *decl_type, id, elem_count, NULL, alloc);
	if ( !decl_typedef_alias.empty() )
	    var->typedef_name = decl_typedef_alias;
	// Record file-scope variables in top_decls in source order for the CIR
	// backend (a struct defined inline here, `struct X {...} v;`, rides in
	// this declaration). Locals (inside a function compound) are excluded.
	// Capture the index so the TokenDecl built later (which carries the
	// initializer) can be linked back into this entry for CIR emission.
	ssize_t global_top_decl_index = -1;
	if ( var && (code == NULL || code == tkProgram) )
	{
	    Program::TopDecl gtd;
	    gtd.kind = Program::DeclKind::dkGlobalVar;
	    gtd.name = var->name;
	    gtd.var = var;
	    gtd.dd = var->type;
	    gtd.file = tb->file;
	    gtd.line = tb->line;
	    gtd.origin = tb;
	    global_top_decl_index = (ssize_t)top_decls.size();
	    top_decls.push_back(gtd);
	}
	bool shared_global_extern_ref =
	    is_shared_global_extern_reference(*this, code, var);
	if ( gotstatic )
	    var->flags |= vfSTATIC;
	// Mark a SCALAR `const`-declared variable so the CIR backend can enforce
	// read-only-ness (reject assignment to it — P2.4). The variable itself is
	// const only when const qualifies the VALUE (`const int x`) or is the
	// top-level const on a pointer (`int * const p`); `const int *p` makes the
	// pointee const but leaves `p` reassignable, so it is excluded — matching
	// td->is_const_decl. Aggregate consts (const arrays/structs) are NOT
	// marked: their per-element write is a different (out-of-scope) enforcement,
	// and marking them would mis-route an array read through the scalar
	// constant-fold path (`const char a[N]` reading as 0). (This sets the
	// marking that was previously absent; the const-fold read of a constant is
	// also guarded on var->data + a scalar type so only a compile-time-valued
	// scalar const folds, leaving runtime-init consts to read normally.)
	bool decl_is_const = (gotconst && !saw_pointer_decl) || saw_const_after_star;
	if ( decl_is_const && !(var->flags & vfFIXEDARRAY)
	  && !(var->type && var->type->is_struct()) )
	    var->flags |= vfCONSTANT | vfCONSTDECL;
	// The extern flag is set at variable-CREATION time (addVariable), so it
	// can only mark a freshly-created symbol and can never demote an
	// already-defined (existing non-extern) one — a global with both a
	// definition and a redundant `extern` in the same TU stays defined.
	// Here we handle the converse: a declaration that is NOT extern (or a
	// shared-global extern that resolves to real local storage) is a
	// DEFINITION — promote a prior extern by allocating storage and clear the
	// extern flag.
	if ( !(parsing_extern_decl && !shared_global_extern_ref) )
	{
	    // Promoting a previously-extern declaration to a real
	    // definition: addVariable returned the existing var without
	    // running the allocate-storage path, so var->data is still
	    // NULL and voperand can't take the absolute-address branch.
	    // Allocate the storage now so the global ends up backed by
	    // real memory shared across every reference site.
	    if ( (var->flags & vfSTACK) && alloc && !var->data
	      && arr_dims.empty()
	      && decl_type->size > 0 && elem_count == 1
	      && decl_type->basetype() != BaseType::btFunct )
	    {
		var->data = calloc(1, decl_type->size);
		var->flags |= vfALLOC;
		var->flags &= ~vfSTACK;
	    }
	    var->flags &= ~vfEXTERN;
	}
	if ( !storage_alias_name.empty() )
	{
	    var->storage_alias_name = storage_alias_name;
	    Variable *alias_target = resolve_global_storage_variable(var);
	    if ( alias_target && alias_target != var )
	    {
		var->data = alias_target->data;
		var->flags &= ~vfSTACK;
		if ( alias_target->has_aot_data() )
		{
		    var->aot_data_offset = alias_target->aot_data_offset;
		    var->aot_cstr_offset = alias_target->aot_cstr_offset;
		}
	    }
	}
	if ( !arr_dims.empty() )
	{
	    if ( shared_global_extern_ref )
	    {
		// Function-scope `extern T name[...]` is only a redeclaration
		// of an existing file-scope object. Do not overwrite the
		// canonical global's array shape or storage metadata with a
		// later incomplete declaration like `extern char buf[];`.
	    }
	    else
	    if ( vla_size_expr )
	    {
		// C99 VLA: the variable is really a pointer-to-element. Don't
		// set vfFIXEDARRAY — let pointer-subscript handling cover
		// `arr[i]` access paths. voperand emits the malloc at scope
		// entry; the parent TokenCpnd's cleanup emits the free.
		DataDef *vla_elem_type = decl_type;
		for ( size_t i = arr_dims.size(); i-- > 1; )
		    vla_elem_type = new DataDefCArray(*vla_elem_type,
						      vla_elem_type->name,
						      arr_dims[i],
						      i < arr_dim_exprs.size()
						      ? arr_dim_exprs[i] : NULL);
		var->type = getPointerType(vla_elem_type);
		var->vla_size_expr = vla_size_expr;
	    }
	    else
	    {
		var->dims = arr_dims;
		var->flags |= vfFIXEDARRAY;
		// Global (or static-local) fixed arrays can't live on the
		// JIT stack — allocate a heap buffer now. voperand detects
		// var->data and loads the absolute address as the base.
		if ( alloc && !var->data )
		{
		    size_t total = (size_t)decl_type->size * (size_t)elem_count;
		    if ( total == 0 ) total = 1;
		    var->data = calloc(1, total);
		    var->flags |= vfALLOC;
		}
	    }
	}
	TokenDecl *td = new TokenDecl(*var);
	td->has_brace_init = saw_brace_init;
	td->is_const_decl = (gotconst && !saw_pointer_decl) || saw_const_after_star;
	// A function-block-scope `extern T name;` referring to a file-scope global:
	// mark it so the CIR backend emits a real `extern T name;` inside the block.
	// c2mir then rebinds `name` to the file-scope object, so an enclosing local
	// of the same name does not shadow the global (C scope rules). Without this
	// the redeclaration was dropped and the inner reference bound to the local.
	if ( shared_global_extern_ref && code && code != tkProgram )
	    td->block_extern_redecl = true;

	td->file = tb->file;
	td->line = tb->line;
	td->column = tb->column;
	td->init_list = init_list;

	if ( gotstatic && code && !td->init_list.empty()
	  && initialize_static_fixed_array_data(var, td->init_list) )
	{
	    td->init_list.clear();
	    td->baked_static_init = true; // mark as the defining decl (vs extern)
	}

	// Global/static SIMD with constant init: write directly to heap storage.
	if ( !code && var && var->data && var->type && var->type->is_simd()
	  && !td->init_list.empty() )
	{
	    DataDefSIMD *vdd = static_cast<DataDefSIMD *>(var->type);
	    size_t lane_size = vdd->element_type ? vdd->element_type->size : 0;
	    bool all_constant = lane_size > 0;

	    // Case 1: array of SIMD vectors — each init_list entry is a
	    // TokenStructLit whose inits hold the per-lane integer values.
	    if ( !arr_dims.empty() && all_constant )
	    {
		for ( size_t ei = 0; all_constant && ei < td->init_list.size(); ++ei )
		{
		    TokenStructLit *slit = dynamic_cast<TokenStructLit *>(td->init_list[ei]);
		    if ( !slit ) { all_constant = false; break; }
		    for ( size_t li = 0; all_constant && li < slit->inits.size(); ++li )
		    {
			int64_t v = 0;
			if ( !literal_integer_value(slit->inits[li], v) )
			    all_constant = false;
		    }
		}
		if ( all_constant )
		{
		    char *base = (char *)var->data;
		    size_t simd_bytes = vdd->size;
		    for ( size_t ei = 0; ei < td->init_list.size(); ++ei )
		    {
			TokenStructLit *slit = static_cast<TokenStructLit *>(td->init_list[ei]);
			for ( size_t li = 0; li < vdd->lane_count && li < slit->inits.size(); ++li )
			{
			    int64_t v = 0;
			    literal_integer_value(slit->inits[li], v);
			    memcpy(base + ei * simd_bytes + li * lane_size, &v, lane_size);
			}
		    }
		    td->init_list.clear();
		}
	    }
	    // Case 2: single SIMD variable — init_list holds flat lane values.
	    else if ( arr_dims.empty() && all_constant )
	    {
		for ( size_t i = 0; all_constant && i < td->init_list.size(); ++i )
		{
		    int64_t v = 0;
		    if ( !literal_integer_value(td->init_list[i], v) )
			all_constant = false;
		}
		if ( all_constant )
		{
		    char *base = (char *)var->data;
		    for ( size_t i = 0; i < vdd->lane_count && i < td->init_list.size(); ++i )
		    {
			int64_t v = 0;
			literal_integer_value(td->init_list[i], v);
			memcpy(base + i * lane_size, &v, lane_size);
		    }
		    td->init_list.clear();
		}
	    }
	}

	if ( nt->id() == TokenID::tkAssign && arr_dims.empty() && init_list.empty() )
	{
	    DBG(std::cout << "parseDeclaration() calling td->initialize = parseExpression" << std::endl);
	    // conditional=true so `;` stops without being consumed, which lets
	    // the comma-continuation loop below distinguish "end of decl" (peek
	    // is `;`) from "more decls" (peek is `,` or the next identifier).
	    td->initialize = parseExpression(new TokenVar(*var), true);
	    TokenAssign *ta = dynamic_cast<TokenAssign *>(td->initialize);
	    TokenVar *lhs_var = ta ? dynamic_cast<TokenVar *>(ta->left) : NULL;
	    if ( !ta || !lhs_var || &lhs_var->var != var )
	    {
		TokenAssign *wrap = new TokenAssign();
		wrap->file = tb->file;
		wrap->line = tb->line;
		wrap->column = tb->column;
		wrap->left = new TokenVar(*var);
		wrap->right = td->initialize;
		td->initialize = wrap;
	    }
	    // Function-scope static: KEEP the initializer on the TokenDecl so the
	    // CIR backend emits it as the SPEC_DECL's constant initializer. c2mir
	    // (like gcc) initializes a `static` object ONCE at load time from that
	    // constant initializer — correct C semantics for `static int c = 5;
	    // c++;` (init once, increment across calls) with no re-init per entry.
	    //   The OLD asmjit-era path instead hoisted the initializer into
	    // tkProgram->statements and cleared td->initialize, relying on
	    // TokenDecl::compile()'s vfSTATIC skip. That backend is gone; in the
	    // CIR pipeline those hoisted file-scope statements are never emitted,
	    // so clearing the initializer silently dropped EVERY scalar static
	    // local init (`static int x=7` read 0; `static const char *p="x"`
	    // SIGSEGV'd on null). Leaving td->initialize set lets var_decl emit it
	    // inline, which is the deepest-layer correct fix (PR-style: pr53084).
	}
	update_pointer_object_size_hints(td->initialize);

	// Link this fully-populated TokenDecl (carrying initialize / init_list)
	// into the file-scope TopDecl recorded earlier, so the CIR backend can
	// emit the global's initializer in its SPEC_DECL. The legacy JIT path
	// keeps its own statement-stream handling and ignores this field.
	if ( global_top_decl_index >= 0
	  && (size_t)global_top_decl_index < top_decls.size() )
	    top_decls[global_top_decl_index].decl = td;

	// Comma-continuation: `int a, b = 1, c;` or
	// `char a[8], b[16];` — after the first declarator, if the next token is
	// `,` (or parseExpression already consumed one and left an identifier/`*`
	// next), inject a clone of the base type token back into the stream.
	// parseCompound's next iteration sees it as a fresh `type ...`
	// statement and calls parseDeclaration recursively, which handles
	// pointer decorators, array declarators, initializers, and further commas.
	{
	    TokenBase *peek = peekToken();
	    bool have_comma = peek && peek->id() == TokenID::tkComma;
	    if ( have_comma )
	    {
		nextToken(); // consume ','
		peek = peekToken();
	    }
	    // Either we just consumed ',' and expect another decl, or
	    // parseExpression already consumed ',' and peek is the next one.
	    bool looks_like_next_decl = peek
		&& (peek->id() == TokenID::tkMul
		 || peek->type() == TokenType::ttIdentifier
		 || is_contextual_identifier_token(peek));
	    if ( have_comma || (looks_like_next_decl
		&& nt->id() == TokenID::tkAssign) ) // only infer no-comma case when we had an init
	    {
		if ( !looks_like_next_decl )
		    Throw(peek ? peek : tb) << "Expecting identifier after ',' in declaration" << flush;
		// Push back a synthetic base-type token so the next parseStatement
		// sees it as the start of a new declaration.
		pushToken(tb->clone());
		if ( parsing_extern_decl )
		    pushToken(new TokenEXTERN());
		if ( gotstatic )
		    pushToken(new TokenSTATIC());
	    }
	}

	DBG(std::cout << "parseDeclaration() returning" << std::endl);

	return td;
    }

    nt = nextToken();
    if ( nt->id() != TokenID::tkOpBrk )
    {
	DBG(std::cerr << "parseDeclaration() throwing token " << (int)nt->id() << std::endl);
	Throw(nt) << "unexpected token type " << (int)nt->type() << flush;
    }

    DBG(std::cout << "parseDeclaration() returning" << std::endl);

    std::string source_id = id;
    std::string parse_id = id;
    bool namespace_function = !current_namespace.empty();
    if ( namespace_function )
	parse_id = namespace_function_symbol(current_namespace, source_id);

    parseFunction(*decl_type, parse_id, NULL, NULL, ret_is_ref,
		  decl_typedef_alias);

    if ( namespace_function )
    {
	Variable *ns_var = tkProgram ? tkProgram->findVariable(parse_id) : NULL;
	if ( ns_var )
	{
	    variable_map_t &ns = namespace_map[current_namespace];
	    ns[source_id] = ns_var;
	    ns.erase(parse_id);
	}
    }

    return NULL;
}

// parse a statement into the AST
// Parse a statement-context expression, chaining comma-separated
// expressions into a TokenComma right-leaning tree. parseExpression
// always stops at `,` (function-arg / for-init separators rely on it),
// so without this wrapper a brace-less `while (c) e1, e2;` body
// dropped `, e2;` — it became a sibling statement after the loop, with
// only `e1` running per iteration. Found via SMAUG mud_prog.c:2437
// `while ((*p = *i) != '\0') ++p, ++i;` segfaulting because `++i`
// never ran inside the loop.
TokenBase *Program::parseExprStmt(TokenBase *tb)
{
    // push_back_comma=true so we can detect the chain by peeking after
    // each parseExpression — without it, parseExpression consumes the
    // `,` itself and the chain looks like a single expression.
    TokenBase *expr = parseExpression(tb, /*conditional=*/false,
                                      /*ternary_branch=*/false,
                                      /*stop_on_closing_paren=*/false,
                                      /*initial_brackets=*/0,
                                      /*push_back_comma=*/true);
    while ( expr && peekToken() && peekToken()->id() == TokenID::tkComma )
    {
	nextToken(); // consume the ','
	TokenBase *next_tb = nextToken();
	if ( !next_tb )
	    break;
	TokenBase *right = parseExpression(next_tb, false, false, false, 0,
	                                   /*push_back_comma=*/true);
	if ( !right )
	    break;
	TokenComma *seq = new TokenComma();
	seq->left   = expr;
	seq->right  = right;
	seq->file   = expr->file;
	seq->line   = expr->line;
	seq->column = expr->column;
	expr = seq;
    }
    update_pointer_object_size_hints(expr);
    return expr;
}

TokenBase *Program::parseStatement(TokenBase *tb)
{
    DBG(cout << "parseStatement() start" << endl);
    // Skip C23 [[...]] attributes before declarations/definitions.
    if ( tb->id() == TokenID::tkOpSqr
      && peekToken() && peekToken()->id() == TokenID::tkOpSqr )
    {
	pushToken(tb);
	skip_c23_attributes(*this);
	tb = nextToken();
	if ( !tb )
	    return NULL;
    }
    size_t attr_vector_bytes = 0;
    if ( is_attribute_identifier_token(tb) )
    {
	tb = consume_gnu_attributes(*this, tb, NULL, NULL, NULL, &attr_vector_bytes);
	if ( !tb )
	    return NULL;
    }
    switch(tb->type())
    {
	// for now, just ignore whitespace and comments
	// this shouldn't occur though, as they should already be culled
	case TokenType::ttSpace:
	case TokenType::ttTab:
	case TokenType::ttEOL:
	case TokenType::ttComment:
	    break;

	// if we start with a type (i.e. int), then this could
	// either be a function or a variable declaration
	case TokenType::ttDataType:
	    // Contextual type names (array, string) that shadow a variable in
	    // scope: treat as expression when followed by an operator or [.
	    if ( is_contextual_identifier_token(tb) )
	    {
		std::string ctx = contextual_identifier_name(tb);
		Variable *ctx_var = findVariable(ctx);
		if ( ctx_var && peekToken()
		  && peekToken()->id() != TokenID::tkMul  // not a pointer decl
		  && peekToken()->type() != TokenType::ttIdentifier ) // not a decl
		{
		    resetPrevToken();
		    return parseExprStmt(tb);
		}
	    }
	    if ( attr_vector_bytes > 0 )
	    {
		TokenDataType *tdt = (TokenDataType *)tb;
		DataDef *simd = new DataDefSIMD(&tdt->definition, "", attr_vector_bytes);
		tdt = new TokenDataType("", *simd);
		DBG(std::cout << "parseStatement: wrapping type in SIMD(" << attr_vector_bytes << " bytes)" << std::endl);
		return parseDeclaration(tdt);
	    }
	    DBG(std::cout << "parseStatement(" << (int)tb->type() << ") calling parseDeclaration" << std::endl);
	    return parseDeclaration((TokenDataType *)tb);
//	    break;

	case TokenType::ttSymbol:
	    if ( tb->id() == TokenID::tkOpBrc )
	    {
		pushCompound();
		return parseCompound();
	    }
	    if ( tb->id() == TokenID::tkClBrc )
	    {
		popCompound();
		return tb;
	    }
	    if ( tb->id() == TokenID::tkSemi )
		return tb;

	    DBG(std::cerr << "parseStatement() throwing token " << (char)tb->get() << std::endl);
	    Throw(tb) << "unexpected token type " << (int)tb->type() << flush;

	// if we start with an operator or an identifier, then this could be an
	// assignment or a function call
	case TokenType::ttIdentifier:
	    DBG(std::cout << "parseStatement() got identifier " << ((TokenIdent *)tb)->str << std::endl);
	    // Label definition: `name:` at statement position. `:` alone is
	    // tkTerC (it shares the id with the ternary-`:`), while `::` is
	    // the separate tkNS token emitted by the lexer, so a peek of
	    // tkTerC here cannot be a namespace prefix. This has to run
	    // before the datatype-identifier / namespace / `:=` branches
	    // because `name:` at statement position is otherwise ambiguous.
	    if ( peekToken() && peekToken()->id() == TokenID::tkTerC )
	    {
		std::string lname = ((TokenIdent *)tb)->str;
		nextToken(); // consume ':'
		DBG(std::cout << "parseStatement() label definition: " << lname << std::endl);
		TokenLabel *lbl = new TokenLabel(lname);
		// C grammar: `label : statement`. A label names the statement
		// that follows it, so parse and carry that statement. This makes
		// the label self-contained, so it survives in EVERY statement
		// context (compound block, switch case body, if/while/for body)
		// — not only the compound-block path. A trailing `}` (label at
		// block end, a GNU/C23 extension) leaves `labeled` NULL.
		TokenBase *pk = peekToken();
		if ( pk && pk->id() != TokenID::tkClBrc )
		    lbl->labeled = parseStatement(nextToken());
		return lbl;
	    }
	    if ( is_static_assert_identifier(((TokenIdent *)tb)->str) )
		return parse_static_assert_statement(*this, tb);
	    if ( is_typeof_identifier(((TokenIdent *)tb)->str) )
		return parseDeclaration(parse_typeof_datatype(*this, tb));
	    if ( ((TokenIdent *)tb)->str == "__label__" )
	    {
		TokenBase *tn = nextToken();
		bool expect_ident = true;
		while ( tn && tn->id() != TokenID::tkSemi )
		{
		    if ( expect_ident )
		    {
			if ( !is_contextual_identifier_token(tn) )
			    Throw(tn) << "expected local label name after '__label__'" << flush;
		    }
		    else if ( tn->id() != TokenID::tkComma )
			Throw(tn) << "expected ',' or ';' after local label declaration" << flush;
		    expect_ident = !expect_ident;
		    tn = nextToken();
		}
		if ( !tn || tn->id() != TokenID::tkSemi )
		    Throw(tb) << "expected ';' after '__label__' declaration" << flush;
		return tn;
	    }
	    // asm / __asm__ / __asm: skip the entire statement.
	    // GCC testsuite uses asm("" : ...) as an optimizer barrier;
	    // madc has no inline assembly support, so consume and discard.
	    {
		std::string &id = ((TokenIdent *)tb)->str;
		if ( id == "asm" || id == "__asm__" || id == "__asm" )
		{
		    // optional volatile qualifier
		    if ( peekToken()
		      && peekToken()->type() == TokenType::ttKeyword
		      && peekToken()->id() == TokenID::tkVOLATILE )
			nextToken();
		    else if ( peekToken() && peekToken()->type() == TokenType::ttIdentifier )
		    {
			std::string &q = ((TokenIdent *)peekToken())->str;
			if ( q == "volatile" || q == "__volatile__" )
			    nextToken();
		    }
		    TokenBase *ob = nextToken();
		    if ( ob && ob->id() == TokenID::tkOpBrk )
		    {
			TokenBase *tmpl = nextToken();
			bool parsed_simple_copy = false;
			if ( tmpl && tmpl->type() == TokenType::ttString
			  && ((TokenStr *)tmpl)->str.empty()
			  && peekToken() && peekToken()->id() == TokenID::tkColon )
			{
			    nextToken(); // ':'
			    TokenBase *out_c = nextToken();
			    TokenBase *out_ob = nextToken();
			    if ( out_c && out_c->type() == TokenType::ttString
			      && out_ob && out_ob->id() == TokenID::tkOpBrk )
			    {
				std::string out_constraint = ((TokenStr *)out_c)->str;
				if ( out_constraint == "+m" )
				{
				    int out_depth = 1;
				    while ( out_depth > 0 )
				    {
					TokenBase *t = nextToken();
					if ( !t ) break;
					if ( t->id() == TokenID::tkOpBrk ) ++out_depth;
					else if ( t->id() == TokenID::tkClBrk ) --out_depth;
				    }
				    int asm_depth = 1;
				    while ( asm_depth > 0 )
				    {
					TokenBase *t = nextToken();
					if ( !t ) break;
					if ( t->id() == TokenID::tkOpBrk ) ++asm_depth;
					else if ( t->id() == TokenID::tkClBrk ) --asm_depth;
				    }
				    if ( peekToken() && peekToken()->id() == TokenID::tkSemi )
					return nextToken();
				    return tb;
				}
				TokenBase *out_tb = nextToken();
				TokenBase *out_expr = out_tb ? parseExpression(out_tb, true) : NULL;
				TokenBase *out_cb = nextToken();
				TokenBase *next_clause = nextToken();
				if ( out_expr
				  && out_cb && out_cb->id() == TokenID::tkClBrk
				  && next_clause && next_clause->id() == TokenID::tkClBrk )
				{
				    if ( peekToken() && peekToken()->id() == TokenID::tkSemi )
					nextToken();
				    if ( out_constraint == "+r" )
					return out_expr;
				    return tb;
				}
				if ( out_expr
				  && out_cb && out_cb->id() == TokenID::tkClBrk
				  && next_clause && next_clause->id() == TokenID::tkSemi )
				{
				    if ( out_constraint == "+r" )
					return out_expr;
				    return next_clause;
				}
				TokenBase *in_c = nextToken();
				TokenBase *in_ob = nextToken();
				if ( out_expr
				  && out_cb && out_cb->id() == TokenID::tkClBrk
				  && next_clause && next_clause->id() == TokenID::tkColon
				  && in_c && in_c->type() == TokenType::ttString
				  && in_ob && in_ob->id() == TokenID::tkOpBrk )
				{
				    TokenBase *in_tb = nextToken();
				    TokenBase *in_expr = in_tb ? parseExpression(in_tb, true) : NULL;
				    TokenBase *in_cb = nextToken();
				    TokenBase *close = nextToken();
				    std::string in_constraint = ((TokenStr *)in_c)->str;
				    // If close is ':' (clobber list), consume
				    // remaining tokens up to outer ')'.
				    if ( close && close->id() == TokenID::tkColon )
				    {
					int rem = 1;
					while ( rem > 0 )
					{
					    TokenBase *t = nextToken();
					    if ( !t ) break;
					    if ( t->id() == TokenID::tkOpBrk ) ++rem;
					    else if ( t->id() == TokenID::tkClBrk ) --rem;
					}
					close = new TokenClBrk();
				    }
				    if ( in_expr
				      && in_cb && in_cb->id() == TokenID::tkClBrk
				      && close && close->id() == TokenID::tkClBrk
				      && out_constraint == "=r"
				      && in_constraint == "0" )
				    {
					TokenAssign *assign = new TokenAssign();
					assign->file = tb->file;
					assign->line = tb->line;
					assign->column = tb->column;
					assign->left = out_expr;
					assign->right = in_expr;
					if ( peekToken() && peekToken()->id() == TokenID::tkSemi )
					    nextToken();
					return assign;
				    }
				    if ( in_expr
				      && in_cb && in_cb->id() == TokenID::tkClBrk
				      && close && close->id() == TokenID::tkClBrk
				      && out_constraint == "=m"
				      && in_constraint == "m" )
				    {
					if ( peekToken() && peekToken()->id() == TokenID::tkSemi )
					    return nextToken();
					return tb;
				    }
				    // Unrecognized two-operand asm pattern — consume
				    // any remaining tokens until the outer asm ')' is
				    // found, then return as no-op. We track paren
				    // depth starting from `close` (which may be `,`,
				    // `:`, or a paren itself).
				    {
					int rem = 1; // 1 for the outer '(' consumed at asm entry
					if ( close && close->id() == TokenID::tkOpBrk ) ++rem;
					else if ( close && close->id() == TokenID::tkClBrk ) --rem;
					while ( rem > 0 )
					{
					    TokenBase *t = nextToken();
					    if ( !t ) break;
					    if ( t->id() == TokenID::tkOpBrk ) ++rem;
					    else if ( t->id() == TokenID::tkClBrk ) --rem;
					}
					parsed_simple_copy = true;
					if ( peekToken() && peekToken()->id() == TokenID::tkSemi )
					    return nextToken();
					return tb;
				    }
				}
				// Unrecognized asm pattern — the output operand
				// was consumed but the remaining clauses don't match
				// a known shape. Consume any remaining tokens until
				// the outer asm ')' is balanced. The outer '(' was
				// consumed at asm entry; out_ob/out_cb cancel; so we
				// need 1 more ')' to close, adjusted by any parens
				// in in_c and in_ob.
				{
				    int rem = 1; // for the outer '('
				    if ( in_c && in_c->id() == TokenID::tkOpBrk ) ++rem;
				    else if ( in_c && in_c->id() == TokenID::tkClBrk ) --rem;
				    if ( in_ob && in_ob->id() == TokenID::tkOpBrk ) ++rem;
				    else if ( in_ob && in_ob->id() == TokenID::tkClBrk ) --rem;
				    while ( rem > 0 )
				    {
					TokenBase *t = nextToken();
					if ( !t ) break;
					if ( t->id() == TokenID::tkOpBrk ) ++rem;
					else if ( t->id() == TokenID::tkClBrk ) --rem;
				    }
				    parsed_simple_copy = true;
				    if ( peekToken() && peekToken()->id() == TokenID::tkSemi )
					return nextToken();
				    return tb;
				}
			    }
			}
			if ( !parsed_simple_copy )
			{
			    int depth = 1;
			    while ( depth > 0 )
			    {
				TokenBase *t = nextToken();
				if ( !t ) break;
				if ( t->id() == TokenID::tkOpBrk ) ++depth;
				else if ( t->id() == TokenID::tkClBrk ) --depth;
			    }
			}
		    }
		    // Return the semicolon as the statement (no-op).
		    // If the asm is the body of `if (...) asm(...);`, the
		    // caller needs a non-NULL return.
		    if ( peekToken() && peekToken()->id() == TokenID::tkSemi )
			return nextToken();
		    return tb;
		}
	    }
	    // check if identifier is a user-defined type (class/struct registered in datatype_map)
	    {
		std::string tname = ((TokenIdent *)tb)->str;
		datatype_map_iter dmi = datatype_map.find(tname);
		if ( dmi == datatype_map.end() )
		{
		    // `Name<ConcreteType>` where Name is a captured template:
		    // instantiate the concrete class and declare a variable of it.
		    if ( TokenDataType *inst = instantiate_template_use(*this, tname, tb) )
		    {
			DBG(std::cout << "parseStatement() template instantiation, calling parseDeclaration" << std::endl);
			return parseDeclaration(inst);
		    }
		    if ( TokenDataType *ns_type = resolve_namespaced_type_token(*this, tb, true) )
		    {
			DBG(std::cout << "parseStatement() namespaced identifier is a registered type, calling parseDeclaration" << std::endl);
			return parseDeclaration(ns_type);
		    }
		    // try lazy type registration from #include headers
		    DataDef *dd = lazy_resolve_type(tname);
		    if ( dd )
		    {
			// register as a datatype so future lookups find it
			// TODO: create a TokenDataType wrapper for the resolved type
		    }
		}
		else
		{
		    DBG(std::cout << "parseStatement() identifier is a registered type, calling parseDeclaration" << std::endl);
		    return parseDeclaration(use_site_type_token(dmi->second, tb));
		}
	    }
	    // namespace resolution: set current namespace and re-enter parseStatement
	    if ( peekToken() && peekToken()->id() == TokenID::tkNS )
	    {
		std::string ns_name = ((TokenIdent *)tb)->str;
		if ( ns_name == "rust"
		  && tokens.size() >= 2
		  && tokens[1]->type() == TokenType::ttIdentifier
		  && ((TokenIdent *)tokens[1])->str == "match" )
		{
		    nextToken(); // consume ::
		    nextToken(); // consume "match"
		    TokenMatch *tm = new TokenMatch();
		    tm->file = tb->file;
		    tm->line = tb->line;
		    tm->column = tb->column;
		    return tm->parse(*this);
		}
		namespace_map_t::iterator nsi = namespace_map.find(ns_name);
		if ( nsi != namespace_map.end() )
		{
		    nextToken(); // consume ::
		    current_namespace = ns_name;
		    TokenBase *result = parseStatement(nextToken());
		    current_namespace.clear();
		    return result;
		}
	    }
	    // := short declaration: identifier := expression;
	    // also handles multi-return: a, b := func();
	    if ( peekToken() && (peekToken()->id() == TokenID::tkColEq
		|| peekToken()->id() == TokenID::tkComma) )
	    {
		std::string first_id = ((TokenIdent *)tb)->str;

		// check if this is a multi-variable declaration: a, b := func()
		if ( peekToken()->id() == TokenID::tkComma )
		{
		    // collect identifiers: a, b, c, ... := expr
		    std::vector<std::string> ids;
		    ids.push_back(first_id);
		    while ( peekToken() && peekToken()->id() == TokenID::tkComma )
		    {
			nextToken(); // consume comma
			TokenBase *next_id = nextToken();
			if ( next_id->type() != TokenType::ttIdentifier )
			    Throw(next_id) << "Expecting identifier in multi-return declaration" << flush;
			ids.push_back(((TokenIdent *)next_id)->str);
		    }
		    // expect :=
		    if ( !peekToken() || peekToken()->id() != TokenID::tkColEq )
		    {
			// not a multi-return declaration — we consumed commas we shouldn't have
			// this shouldn't happen in practice since comma after identifiers
			// only makes sense before :=
			Throw(tb) << "Expecting := after identifier list" << flush;
		    }
		    nextToken(); // consume :=

		    // parse the RHS function call
		    TokenBase *rhs = parseExpression(nextToken());

		    // look up the function's return_types to infer variable types
		    FuncDef *func = NULL;
		    if ( rhs->type() == TokenType::ttCallFunc )
		    {
			TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(rhs);
			if ( tcf->var.type->basetype() == BaseType::btFunct )
			    func = (FuncDef *)tcf->var.type;
		    }

		    // create a TokenCpnd-like wrapper that declares all variables
		    // and generates the multi-return unpack
		    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();

		    // create variables with inferred types from return_types
		    std::vector<Variable *> vars;
		    for ( size_t i = 0; i < ids.size(); ++i )
		    {
			DataDef *vtype = &ddINT64; // default
			if ( func && i < func->return_types.size() )
			    vtype = func->return_types[i];
			else if ( func && i == 0 )
			    vtype = &func->returns;
			bool alloc = (!code) ? true : false;
			Variable *v = addVariable(code, *vtype, ids[i], 1, NULL, alloc);
			vars.push_back(v);
		    }

		    // build a TokenDecl for the first variable with the call as initializer
		    // The multi-return unpack will be handled at compile time
		    TokenDecl *td = new TokenDecl(*vars[0]);
		    td->file = tb->file;
		    td->line = tb->line;
		    td->column = tb->column;

		    // store extra info for multi-return compile
		    // We use a TokenMultiReturn node that wraps the call and target variables
		    TokenAssign *assign = new TokenAssign();
		    assign->file = tb->file;
		    assign->line = tb->line;
		    assign->column = tb->column;
		    assign->left = new TokenVar(*vars[0]);
		    assign->right = rhs;
		    assign->multi_vars = vars; // store all target variables
		    td->initialize = assign;

		    DBG(std::cout << "parseStatement() multi-return ':=' with " << ids.size() << " variables" << std::endl);
		    return td;
		}

		// single := declaration (existing behavior)
		nextToken(); // consume :=
		TokenBase *rhs = parseExpression(nextToken());
		DataDef *inferred = rhs->datadef();
		if ( !inferred || inferred == &ddVOID )
		    inferred = &ddINT64;
		TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
		bool alloc = (!code) ? true : false;
		Variable *var = addVariable(code, *inferred, first_id, 1, NULL, alloc);
		TokenDecl *td = new TokenDecl(*var);
		td->file = tb->file;
		td->line = tb->line;
		td->column = tb->column;
		TokenAssign *assign = new TokenAssign();
		assign->file = tb->file;
		assign->line = tb->line;
		assign->column = tb->column;
		assign->left  = new TokenVar(*var);
		assign->right = rhs;
		td->initialize = assign;
		DBG(std::cout << "parseStatement() ':=' declared '" << first_id << "' type=" << inferred->name << std::endl);
		return td;
	    }
	// C89 implicit-int function definition: `name(params) { body }`
	// at file scope with no return type defaults to int.
	if ( tb->type() == TokenType::ttIdentifier
	    && peekToken() && peekToken()->id() == TokenID::tkOpBrk )
	{
	    std::string fname = ((TokenIdent *)tb)->str;
	    if ( !datatype_map.count(fname) && !struct_map.count(fname) )
	    {
		std::vector<TokenBase *> saved;
		saved.push_back(nextToken()); // consume (
		int depth = 1;
		while ( depth > 0 )
		{
		    TokenBase *t = nextToken();
		    if ( !t ) break;
		    saved.push_back(t);
		    if ( t->id() == TokenID::tkOpBrk ) ++depth;
		    else if ( t->id() == TokenID::tkClBrk ) --depth;
		}
		bool found_brace = peekToken() && peekToken()->id() == TokenID::tkOpBrc;
		std::vector<TokenBase *> suffix;
		if ( !found_brace )
		    found_brace = scan_old_style_definition_suffix(*this, suffix);
		for ( auto it = suffix.rbegin(); it != suffix.rend(); ++it )
		    pushToken(*it);
		for ( auto it = saved.rbegin(); it != saved.rend(); ++it )
		    pushToken(*it);
		if ( found_brace )
		{
		    nextToken(); // re-consume (
		    parseFunction(ddINT32, fname, NULL);
		    return NULL;
		}
	    }
	}
	case TokenType::ttOperator:
	case TokenType::ttMultiOp:
	    // multi-return function declaration: (type, type) funcname(...)
	    if ( tb->id() == TokenID::tkOpBrk && peekToken()
		 && peekToken()->type() == TokenType::ttDataType )
	    {
		std::vector<DataDef *> rtypes;
		std::vector<TokenBase *> saved;
		bool saw_multi_return_comma = false;
		while ( true )
		{
		    TokenBase *rt = nextToken();
		    saved.push_back(rt);
		    if ( rt->type() != TokenType::ttDataType )
			Throw(rt) << "Expecting type in multi-return declaration" << flush;
		    rtypes.push_back(&((TokenDataType *)rt)->definition);
		    TokenBase *sep = nextToken();
		    saved.push_back(sep);
		    if ( sep->id() == TokenID::tkClBrk ) break;
		    if ( sep->id() != TokenID::tkComma )
			Throw(sep) << "Expecting , or ) in multi-return type list" << flush;
		    saw_multi_return_comma = true;
		}
		if ( !saw_multi_return_comma )
		{
		    for ( std::vector<TokenBase *>::reverse_iterator it = saved.rbegin();
			  it != saved.rend(); ++it )
			if ( *it )
			    pushToken(*it);
		    resetPrevToken();
		    return parseExprStmt(tb);
		}
		TokenBase *fname = nextToken();
		saved.push_back(fname);
		if ( !fname || fname->type() != TokenType::ttIdentifier
		  || !peekToken() || peekToken()->id() != TokenID::tkOpBrk )
		{
		    for ( std::vector<TokenBase *>::reverse_iterator it = saved.rbegin();
			  it != saved.rend(); ++it )
			if ( *it )
			    pushToken(*it);
		    resetPrevToken();
		    return parseExprStmt(tb);
		}
		std::string id = ((TokenIdent *)fname)->str;
		TokenBase *opbrk = nextToken();
		if ( opbrk->id() != TokenID::tkOpBrk )
		    Throw(opbrk) << "Expecting ( after function name" << flush;
		parseFunction(*rtypes[0], id, NULL, &rtypes);
		return NULL;
	    }
	    DBG(std::cout << "parseStatement(" << (int)tb->type() << ") calling parseExprStmt" << std::endl);
	    resetPrevToken();
	    return parseExprStmt(tb);
	    break;

	case TokenType::ttKeyword:
	    // `class` is also a madc keyword (OOP class declaration), but C
	    // codebases (notably SMAUG) use it as a plain identifier for
	    // struct members / locals (`ch->class`, `int class;`). Treat as
	    // an identifier here when it's not the start of an actual class
	    // declaration — i.e. when the next token is not an identifier
	    // (class name) or '{' (anonymous class body).
	    if ( tb->id() == TokenID::tkCLASS
	      && peekToken()
	      && peekToken()->type() != TokenType::ttIdentifier
	      && peekToken()->id() != TokenID::tkOpBrc )
	    {
		DBG(std::cout << "parseStatement() 'class' used as identifier" << std::endl);
		resetPrevToken();
		return parseExpression(tb);
	    }
	    // `try` / `catch` / `throw` are C++ keywords but valid C
	    // identifiers (SMAUG has `int try;` then `try = saving_throw()`).
	    // A real try-block is `try { ... }`; a real catch starts a
	    // `catch (...)` block; a real throw is `throw expr;`. In any
	    // other follower context, treat as an identifier and route
	    // through parseExpression.
	    // `try` is a keyword when followed by '{'. `throw` is a keyword
	    // when followed by an expression or ';'. `catch` is only valid
	    // after a try block (handled by TokenTRY::parse). In C identifier
	    // context (SMAUG: `int try; try = saving_throw()`), these route
	    // through parseExpression instead.
	    if ( tb->id() == TokenID::tkCATCH
	      && peekToken()
	      && peekToken()->id() != TokenID::tkOpBrk )
	    {
		DBG(std::cout << "parseStatement() 'catch' used as identifier" << std::endl);
		resetPrevToken();
		return parseExpression(tb);
	    }
	    if ( tb->id() == TokenID::tkTRY
	      && peekToken()
	      && peekToken()->id() != TokenID::tkOpBrc )
	    {
		DBG(std::cout << "parseStatement() 'try' used as identifier" << std::endl);
		resetPrevToken();
		return parseExpression(tb);
	    }
	    if ( tb->id() == TokenID::tkNEW
	      && (!peekToken() || peekToken()->type() != TokenType::ttIdentifier) )
	    {
		DBG(std::cout << "parseStatement() 'new' used as identifier" << std::endl);
		resetPrevToken();
		return parseExpression(tb);
	    }
	    DBG(std::cout << "parseKeyword(" << ((TokenKeyword *)tb)->str << ") calling parseKeyword" << std::endl);
	    return parseKeyword((TokenKeyword *)tb);

/* keep this here for tokentype reference
	case TokenType::ttBase:
	case TokenType::ttOperator:
	case TokenType::ttIdentifier:
	case TokenType::ttString:
	case TokenType::ttChar:
	case TokenType::ttInteger:
	case TokenType::ttReal:
	case TokenType::ttKeyword:
	case TokenType::ttDataType:
*/
	default:
	    Throw(tb) << "unexpected token type " << (int)tb->type() << flush;
    } // end switch
    DBG(cout << "parseStatement() returns NULL" << endl);

    return NULL;
}

// parse the token queue
bool Program::parse(TokenProgram *tp)
{
    TokenBase *tb, *ts;

    DBG(cout << endl << "Program::parse() START" << endl);
    clear_diagnostics();
    clear_error();

    if ( tokens.empty() )
    {
	set_error(DiagnosticPhase::parser, "Program::parse() token queue empty", tp ? tp->source.c_str() : NULL, 0, 0);
	cerr << "Program::parse() token queue empty" << endl;
	return false;
    }

    inject_pending_auto_includes();
    _parser_init();

    DBG(cout << endl << "Program::parse() calling ast.push for TokenProgram" << endl);
    ast.push(tp);

    try
    {
	while ( !tokens.empty() )
	{
	    tb = nextToken();
//	    printt(tb);
	    // Skip C23 [[...]] attributes at top level.
	    if ( tb && tb->id() == TokenID::tkOpSqr
	      && peekToken() && peekToken()->id() == TokenID::tkOpSqr )
	    {
		pushToken(tb);
		skip_c23_attributes(*this);
		tb = nextToken();
		if ( !tb || tokens.empty() )
		    break;
	    }
#if 1
	    ts = parseStatement(tb);
	    if ( ts )
	    {
		if ( ts->type() != TokenType::ttCompound )
		{
		    DBG(cout << "Program::parse() calling tp->statements.push_back" << endl);
		    tp->statements.push_back((TokenStmt *)ts);
		}
		else
		{
		    DBG(cout << "Program::parse() calling ast.push" << endl);
		    ast.push(ts);
		}
	    }
#endif
        }
    }
    catch(const char *err_msg)
    {
	set_error(DiagnosticPhase::parser, err_msg ? err_msg : "(null error message)", tp ? tp->source.c_str() : NULL, tb ? tb->line : 0, tb ? tb->column : 0);
	print_last_diagnostic(error());
	return false;
    }
    catch(TokenIdent *ti)
    {
	set_error(DiagnosticPhase::parser, std::string("use of undeclared identifier '") + ti->str + '\'', tp ? tp->source.c_str() : NULL, ti->line, ti->column);
	print_last_diagnostic(error());
	return false;
    }
    catch(TokenBase *tb)
    {
	set_error(DiagnosticPhase::parser, std::string("unexpected token type ") + std::to_string((int)tb->type()), tp ? tp->source.c_str() : NULL, tb->line, tb->column);
	print_last_diagnostic(error());
	if ( tb->type() == TokenType::ttReal )
	{
	    error() << "TokenReal value: " << ((TokenReal *)tb)->dval() << endl;
	    printf("%.14lf\n", ((TokenReal *)tb)->dval());
	}
	return false;
    }
    catch(std::exception &e)
    {
	// throwbuf::sync() already printed the formatted error to stderr before throwing
	if ( !last_error.has_error )
	{
	    TokenBase *err_tb = Throw.token();
	    set_error(DiagnosticPhase::parser, Throw.str().empty() ? e.what() : Throw.str(),
		tp ? tp->source.c_str() : NULL,
		err_tb ? err_tb->line : 0,
		err_tb ? err_tb->column : 0);
	}
	return false;
    }

    DBG(std::cout << "Program::parse() finished parsing" << std::endl);
    
    return true;
}

TokenBase *Program::parse_expression_unit(TokenProgram *tp)
{
    TokenBase *tb = NULL;
    TokenBase *expr = NULL;

    DBG(cout << endl << "Program::parse_expression_unit() START" << endl);
    clear_diagnostics();
    clear_error();

    if ( tokens.empty() )
    {
	set_error(DiagnosticPhase::parser, "Program::parse_expression_unit() token queue empty",
		  tp ? tp->source.c_str() : NULL, 0, 0);
	error() << "Program::parse_expression_unit() token queue empty" << endl;
	return NULL;
    }

    _parser_init();

    try
    {
	tb = nextToken();
	expr = parseExpression(tb);
	if ( !expr )
	    Throw(tb) << "Failed to parse expression" << flush;

	TokenBase *stop = curToken();
	if ( !stop || stop->id() != TokenID::tkSemi )
	    Throw(stop ? stop : tb) << "Expecting ';' after expression" << flush;
	if ( !tokens.empty() )
	{
	    TokenBase *extra = nextToken();
	    Throw(extra) << "unexpected token after expression" << flush;
	}
    }
    catch(const char *err_msg)
    {
	set_error(DiagnosticPhase::parser, err_msg ? err_msg : "(null error message)",
		  tp ? tp->source.c_str() : NULL,
		  tb ? tb->line : 0, tb ? tb->column : 0);
	print_last_diagnostic(error());
	return NULL;
    }
    catch(TokenIdent *ti)
    {
	set_error(DiagnosticPhase::parser,
		  std::string("use of undeclared identifier '") + ti->str + '\'',
		  tp ? tp->source.c_str() : NULL, ti->line, ti->column);
	print_last_diagnostic(error());
	return NULL;
    }
    catch(TokenBase *err_tb)
    {
	set_error(DiagnosticPhase::parser,
		  std::string("unexpected token type ") + std::to_string((int)err_tb->type()),
		  tp ? tp->source.c_str() : NULL, err_tb->line, err_tb->column);
	print_last_diagnostic(error());
	return NULL;
    }
    catch(std::exception &e)
    {
	if ( !last_error.has_error )
	{
	    TokenBase *err_tb = Throw.token();
	    set_error(DiagnosticPhase::parser, Throw.str().empty() ? e.what() : Throw.str(),
		      tp ? tp->source.c_str() : NULL,
		      err_tb ? err_tb->line : 0,
		      err_tb ? err_tb->column : 0);
	}
	return NULL;
    }

    return expr;
}
