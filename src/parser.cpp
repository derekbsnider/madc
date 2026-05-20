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
#include <asmjit/x86.h>
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

using namespace std;
using namespace asmjit;

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
    func->is_varargs = true;
    func->parameters.push_back(arg_type);
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
    switch ( in.type )
    {
	case DataType::dtINT64:
	    out = madc::value(static_cast<int64_t>(in.as_int()));
	    return true;
	case DataType::dtDOUBLE:
	    out = madc::value(in.as_double());
	    return true;
	case DataType::dtSTRING:
	    out = madc::value(in.as_string());
	    return true;
	case DataType::dtARRAY:
	    return value_from_madarray_context(in.as_array(), out, reason);
	default:
	    break;
    }

    reason = std::string("unsupported context value kind '")
	+ std::to_string((int)in.type) + "'";
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
        && (tb->id() == TokenID::tkRESTRICT || tb->id() == TokenID::tkCONST);
}

static bool is_type_qualifier_token(TokenBase *tb)
{
    return tb && tb->type() == TokenType::ttKeyword
	&& (tb->id() == TokenID::tkCONST
	 || tb->id() == TokenID::tkRESTRICT
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

static TokenBase *consume_gnu_attributes(Program &pgm, TokenBase *nt,
					 std::set<std::string> *attrs = NULL)
{
    while ( nt && is_attribute_identifier_token(nt) )
    {
	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpBrk )
	{
	    int adepth = 0;
	    do {
		TokenBase *at = pgm.nextToken();
		if ( !at ) break;
		if ( attrs && adepth >= 2 && at->type() == TokenType::ttIdentifier )
		    attrs->insert(((TokenIdent *)at)->str);
		if ( at->id() == TokenID::tkOpBrk ) ++adepth;
		else if ( at->id() == TokenID::tkClBrk ) --adepth;
	    } while ( adepth > 0 );
	}
	nt = pgm.nextToken();
    }
    return nt;
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
	if ( at->id() == TokenID::tkOpBrk ) ++depth;
	else if ( at->id() == TokenID::tkClBrk ) --depth;
	else if ( at->type() == TokenType::ttIdentifier
	       && ((TokenIdent *)at)->str == "vector_size" )
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
	DataDef *cdd = chain ? chain->datadef() : NULL;
	if ( !cdd )
	    pgm.Throw(id_tb) << op_name << ": cannot determine type of expression" << flush;
	size_t value = query_datadef_measure(cdd, want_alignof);
	if ( deref && cdd->is_pointer() )
	{
	    DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(cdd);
	    if ( pdd && pdd->base_type )
		value = query_datadef_measure(pdd->base_type, want_alignof);
	}
	else if ( !want_alignof )
	{
	    if ( TokenVar *tv = dynamic_cast<TokenVar *>(chain) )
		if ( tv->var.is_fixed_array() )
		    value = tv->var.type->size * tv->var.total_elements();
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
			value = tv->var.type->size * tv->var.total_elements();
		if ( !value )
		    value = query_datadef_measure(dd, want_alignof);
		have_value = true;
		dd = NULL; // have_value is set, skip the pointer/array loop below
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
    // here for the type-name path.
    if ( !have_value || (pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkClBrk) )
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
	result = new TokenInt((int64_t)query_datadef_measure(dd, want_alignof));
    else
	result = new TokenTypeQuery(dd, want_alignof);
    result->file = op_tb ? op_tb->file : NULL;
    result->line = op_tb ? op_tb->line : 0;
    result->column = op_tb ? op_tb->column : 0;
    return result;
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

static bool bracket_dim_uses_runtime_value(Program &pgm)
{
    int depth = 1;
    for ( auto it = pgm.tokens.begin(); it != pgm.tokens.end() && depth > 0; ++it )
    {
	TokenBase *t = *it;
	if ( t->id() == TokenID::tkOpSqr ) { ++depth; continue; }
	if ( t->id() == TokenID::tkClSqr ) { --depth; continue; }
	if ( t->id() == TokenID::tkSemi || t->id() == TokenID::tkOpBrc ) break;
	std::string name;
	if ( t->type() == TokenType::ttIdentifier )
	    name = ((TokenIdent *)t)->str;
	else
	    continue;
	Variable *v = pgm.findVariable(name);
	if ( !v || v->is_constant() )
	    continue;
	return true;
    }
    return false;
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
DataDefSTRING ddSTRING;
DataDefSTRINGref ddSTRINGref;
DataDefISTREAM ddISTREAM;
DataDefOSTREAM ddOSTREAM;
DataDefSSTREAM ddSSTREAM;
DataDefARRAY ddARRAY;
DataDefIFSTREAM ddIFSTREAM;
DataDefOFSTREAM ddOFSTREAM;
DataDefFSTREAM ddFSTREAM;
DataDefLPSTR ddLPSTR;
DataDefPTR ddVOIDptr(ddVOID), ddCHARptr(ddCHAR), ddINTptr(ddINT), ddINT32ptr(ddINT32);
DataDefAUTO ddAUTO;
DataDefTEST ddTESTSTRUCT;


const char *c_str2(std::string *str)
{
    std::cout << "c_str2() on " << *str << '[' << (uint64_t)str << ']' << std::endl;
    std::cout << "c_str2() returning " << (uint64_t)str->c_str() << std::endl;
    uint64_t ui64 = (uint64_t)str->c_str();
    uint32_t ui32 = ui64;
    std::cout << "c_str2() uint32 " << ui32 << std::endl;
    return str->c_str();
}


void printuint32(uint32_t &i)
{
    std::cout << "i: " << i << std::endl << std::flush;
}

void printuint32(uint32_t i)
{
    std::cout << "i: " << i << std::endl << std::flush;
}

template<typename T> void streamout_type(std::ostream &os, T t)
{
    os << t;
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
    if ( init ) { alloc = true; }
    if ( !alloc ) { flags |= vfSTACK; }
    switch(type->type())
    {
	case DataType::dtSTRING:
	    if ( init )
	    {
		data = new std::string((const char *)init);
		flags |= vfALLOC;
		DBG(std::cout << "Variable::Variable data = new string for " << n << " (" << *(std::string *)data << ')' << std::endl);
	    }
	    else
	    if ( alloc )
	    {
		data = new std::string;
		flags |= vfALLOC;
	    }
	    DBG(std::cout << "Variable " << n << " Data address: " << (uint64_t)data << std::endl);
	    break;
	case DataType::dtSSTREAM:
	    if ( init )
	    {
		data = new std::stringstream((const std::string &)init);
		flags |= vfALLOC;
		DBG(std::cout << "Variable::Variable data = new stringstream for " << n << " (" << *(std::string *)init << ')' << std::endl);
	    }
	    else
	    if ( alloc )
	    {
		data = new std::stringstream;
		flags |= vfALLOC;
		DBG(std::cout << "Variable::Variable data = new stringstream for " << n << std::endl);
	    }
	    DBG(std::cout << "Data address: " << (uint64_t)data << std::endl);
	    break;
	case DataType::dtISTREAM:
	    if ( init )
	    {
		data = new std::istream((streambuf *)init);
		flags |= vfALLOC;
		DBG(std::cout << "Variable::Variable data = new istream for " << n << std::endl);
		DBG(std::cout << "Data address: " << (uint64_t)data << std::endl);
	    }
	    break;
	case DataType::dtOSTREAM:
	    if ( init )
	    {
		data = new std::ostream((streambuf *)init);
		flags |= vfALLOC;
		DBG(std::cout << "Variable::Variable data = new ostream for " << n << std::endl);
		DBG(std::cout << "Data address: " << (uint64_t)data << std::endl);
	    }
	    break;
	default:
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
	    break;
    } // switch
}

Variable::~Variable()
{
    if ( !(flags & vfALLOC) )
	return;

    DBG(std::cout << "Variable::~Variable(" << name << ") freeing data" << std::endl);

    switch(type->type())
    {
	case DataType::dtSTRING:  delete (std::string *)data;		break;
	case DataType::dtSSTREAM: delete (std::stringstream *)data;	break;
	case DataType::dtOSTREAM: delete (std::ostream *)data;		break;
	default:		  free(data);				break;
    } // switch
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

    return NULL;
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

typedef const char * (*fnSTRINGcstr)(void *);		// string::c_str()
typedef string & (*fnSTRINGmethodSTR)(const string &);	// string::append(string &)
typedef string & (*fnSTRINGmethodCSTR)(const char *);	// string::append(const char *)

union string_member_cast {
    const char * (string::*c_str)(void);
    string & (string::*method_str)(const string &);
    string & (string::*method_cstr)(const char *);
    void * void_pointer[1];
};

typedef string (*fnSSTREAMstr)(void *);		// stringstream::str()
union sstream_member_cast {
    string (stringstream::*str)(void) const;
    void * void_pointer[1];
};




// forward decl — body defined below next to other madc_string_* helpers
int64_t madc_string_length(void *str);

// add methods to ddSTRING
void Program::add_string_methods()
{
    string_member_cast scmc;
    Variable *var;

    scmc.c_str = (const char *(string::*)(void))&string::c_str;
    var = addFunction("c_str", datatype_vec_t{rtPtr(DataType::dtCHAR), rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)(fnSTRINGcstr)scmc.void_pointer[0], true);
    ddSTRING.methods.push_back(var);

    var = addFunction("c_str2", datatype_vec_t{rtPtr(DataType::dtCHAR), rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)c_str2, true);
    ddSTRING.methods.push_back(var);

    scmc.method_str = (string &(string::*)(const string &))&string::assign;
    var = addFunction("assign", datatype_vec_t{rtPtr(DataType::dtSTRING), rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)(fnSTRINGmethodSTR)scmc.void_pointer[0], true);
    ddSTRING.methods.push_back(var);

    scmc.method_cstr = (string &(string::*)(const char *))&string::assign;
    var = addFunction("assign", datatype_vec_t{rtPtr(DataType::dtSTRING), rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)(fnSTRINGmethodCSTR)scmc.void_pointer[0], true);
    ddSTRING.methods.push_back(var);

    scmc.method_str = (string &(string::*)(const string &))&string::append;
    var = addFunction("append", datatype_vec_t{rtPtr(DataType::dtSTRING), rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)(fnSTRINGmethodSTR)scmc.void_pointer[0], true);
    ddSTRING.methods.push_back(var);

    scmc.method_cstr = (string &(string::*)(const char *))&string::append;
    var = addFunction("append", datatype_vec_t{rtPtr(DataType::dtSTRING), rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)(fnSTRINGmethodCSTR)scmc.void_pointer[0], true);
    ddSTRING.methods.push_back(var);

    // length() and size() — wrap std::string::length via a free helper.
    // Signature: (int64_t, string*) matches madc method calling convention
    // where the object pointer is the hidden first argument.
    var = addFunction("length", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)madc_string_length, true);
    ddSTRING.methods.push_back(var);
    var = addFunction("size",   datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)madc_string_length, true);
    ddSTRING.methods.push_back(var);

    DBG(std::cout << "add_string_methods() ddSTRING.methods.size() = " << ddSTRING.methods.size() << std::endl);
}

// add methods to ddSSTREAM
void Program::add_sstream_methods()
{
    sstream_member_cast ssmc;
    Variable *var;

    ssmc.str = (string (stringstream::*)(void) const)&stringstream::str;
    var = addFunction("str", datatype_vec_t{rtPtr(DataType::dtSTRING), rtPtr(DataType::dtSSTREAM)}, (fVOIDFUNC)(fnSSTREAMstr)ssmc.void_pointer[0], true);
    ddSSTREAM.methods.push_back(var);
}


// some debugging functions
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

void printstarred(std::string &s)
{
    std::cout << "*** " << s << " ***" << std::endl;
}

void printstring(std::string *str)
{
    if ( !str ) { std::cerr << "ERROR: printstr: NULL!" << std::endl; return; }
    DBG(std::cout << "printstr(" << (uint64_t)str << "): " << *str << std::endl);
    cout << *str << endl;
}

void printstream(std::stringstream *os)
{
    if ( !os ) { std::cerr << "ERROR: printstream: NULL!" << std::endl; return; }
    DBG(std::cout << "printstream: " << os->str() << std::endl);
    cout << os->str() << endl;
}

// forward declarations for functions defined in compiler.cpp
extern void ifstream_open(void *, void *);
extern void ifstream_close(void *);
extern void ofstream_open(void *, void *);
extern void ofstream_close(void *);
extern void fstream_open(void *, void *);
extern void fstream_close(void *);
extern int64_t ifstream_eof(void *);
extern int64_t ifstream_good(void *);
extern int64_t ifstream_is_open(void *);
extern int64_t ofstream_good(void *);
extern int64_t ofstream_is_open(void *);
extern int64_t fstream_eof(void *);
extern int64_t fstream_good(void *);
extern int64_t fstream_is_open(void *);

// forward declarations for STL container methods (defined in ns_stl.cpp)
extern void vector_int_push_back(void *, int64_t);
extern void vector_int_pop_back(void *);
extern int64_t vector_int_at(void *, int64_t);
extern int64_t vector_int_size(void *);
extern void vector_int_clear(void *);
extern int64_t vector_int_empty(void *);
extern void vector_int_set(void *, int64_t, int64_t);
extern void vector_str_push_back(void *, void *);
extern void vector_str_pop_back(void *);
extern void *vector_str_at(void *, void *, int64_t);
extern void vector_str_set(void *, int64_t, void *);
extern int64_t vector_str_size(void *);
extern void vector_str_clear(void *);
extern int64_t vector_str_empty(void *);
extern void map_str_int_set(void *, void *, int64_t);
extern int64_t map_str_int_get(void *, void *);
extern int64_t map_str_int_contains(void *, void *);
extern void map_str_int_erase(void *, void *);
extern int64_t map_str_int_size(void *);
extern void map_str_int_clear(void *);
extern void map_str_str_set(void *, void *, void *);
extern void *map_str_str_get(void *, void *, void *);
extern int64_t map_str_str_contains(void *, void *);
extern int64_t map_str_str_size(void *);
extern void set_str_insert(void *, void *);
extern int64_t set_str_contains(void *, void *);
extern void set_str_erase(void *, void *);
extern int64_t set_str_size(void *);
extern void set_str_clear(void *);
extern void set_int_insert(void *, int64_t);
extern int64_t set_int_contains(void *, int64_t);
extern int64_t set_int_size(void *);
extern void list_int_push_back(void *, int64_t);
extern void list_int_push_front(void *, int64_t);
extern int64_t list_int_size(void *);
extern void list_int_clear(void *);
extern void list_str_push_back(void *, void *);
extern void list_str_push_front(void *, void *);
extern int64_t list_str_size(void *);

// dlopen/dlsym wrappers that accept std::string* (madc strings)
int64_t madc_dlopen(void *filename)
{
    const char *fn = ((std::string *)filename)->c_str();
    void *handle = dlopen(fn, RTLD_LAZY);
    if ( !handle )
	std::cerr << "dlopen: " << dlerror() << std::endl;
    return (int64_t)handle;
}

int64_t madc_dlsym(int64_t handle, void *name)
{
    const char *n = ((std::string *)name)->c_str();
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

// type conversion wrappers
void madc_to_string(void *result, int64_t val)
{
    *(std::string *)result = std::to_string(val);
}
void madc_to_string_d(void *result, double val)
{
    *(std::string *)result = std::to_string(val);
}
int64_t madc_stoi(void *str)
{
    try { return (int64_t)std::stoll(((std::string *)str)->c_str()); }
    catch (...) { return 0; }
}
double madc_stod(void *str)
{
    try { return std::stod(((std::string *)str)->c_str()); }
    catch (...) { return 0.0; }
}
int64_t madc_string_length(void *str)
{
    return (int64_t)((std::string *)str)->length();
}

// C library wrappers that accept madc strings
int64_t madc_system(void *cmd)
{
    return (int64_t)system(((std::string *)cmd)->c_str());
}

int64_t madc_getenv(void *result, void *name)
{
    const char *val = getenv(((std::string *)name)->c_str());
    std::string &res = *(std::string *)result;
    res = val ? val : "";
    return val ? 1 : 0;
}

const char *madc_get_argv(int64_t argv_ptr, int64_t index)
{
    char **argv = (char **)argv_ptr;
    return argv[index];
}

void madc_setenv(void *name, void *value)
{
    setenv(((std::string *)name)->c_str(), ((std::string *)value)->c_str(), 1);
}

void madc_unsetenv(void *name)
{
    unsetenv(((std::string *)name)->c_str());
}

// needed to add getline
typedef istream& (*fnGETLINE)(istream&, string&);
// needed to add endl
typedef ostream& (*fnENDL)(ostream&);

static void register_std_namespace_spec(Program &pgm)
{
    pgm.add_namespaces();
}

static void register_madc_namespace_spec(Program &pgm)
{
    pgm.add_madc_namespace();
}

static void register_php_namespace_spec(Program &pgm)
{
    pgm.add_php_namespace();
}

static void register_perl_namespace_spec(Program &pgm)
{
    pgm.add_perl_namespace();
}

static void register_python_namespace_spec(Program &pgm)
{
    pgm.add_python_namespace();
}

static void register_ruby_namespace_spec(Program &pgm)
{
    pgm.add_ruby_namespace();
}

static void register_js_namespace_spec(Program &pgm)
{
    pgm.add_js_namespace();
}

static void register_rust_namespace_spec(Program &pgm)
{
    pgm.add_rust_namespace();
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
      colors(false),
      aot_tracking(false),
      instrument_functions(false),
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
      colors(false),
      aot_tracking(false),
      instrument_functions(false),
      root_fn(NULL)
{
    attach_engine(eng);
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

// add file stream methods
void Program::add_fstream_methods()
{
    Variable *var;

    // ifstream methods — must use typed wrappers (ios is virtual base, pointer offset differs)
    var = addFunction("open", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtIFSTREAM), DataType::dtSTRING}, (fVOIDFUNC)ifstream_open, true);
    ddIFSTREAM.methods.push_back(var);
    var = addFunction("close", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtIFSTREAM)}, (fVOIDFUNC)ifstream_close, true);
    ddIFSTREAM.methods.push_back(var);
    var = addFunction("eof", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtIFSTREAM)}, (fVOIDFUNC)ifstream_eof, true);
    ddIFSTREAM.methods.push_back(var);
    var = addFunction("good", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtIFSTREAM)}, (fVOIDFUNC)ifstream_good, true);
    ddIFSTREAM.methods.push_back(var);
    var = addFunction("is_open", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtIFSTREAM)}, (fVOIDFUNC)ifstream_is_open, true);
    ddIFSTREAM.methods.push_back(var);

    // ofstream methods
    var = addFunction("open", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtOFSTREAM), DataType::dtSTRING}, (fVOIDFUNC)ofstream_open, true);
    ddOFSTREAM.methods.push_back(var);
    var = addFunction("close", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtOFSTREAM)}, (fVOIDFUNC)ofstream_close, true);
    ddOFSTREAM.methods.push_back(var);
    var = addFunction("good", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtOFSTREAM)}, (fVOIDFUNC)ofstream_good, true);
    ddOFSTREAM.methods.push_back(var);

    // fstream methods
    var = addFunction("open", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtFSTREAM), DataType::dtSTRING}, (fVOIDFUNC)fstream_open, true);
    ddFSTREAM.methods.push_back(var);
    var = addFunction("close", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtFSTREAM)}, (fVOIDFUNC)fstream_close, true);
    ddFSTREAM.methods.push_back(var);
    var = addFunction("eof", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtFSTREAM)}, (fVOIDFUNC)fstream_eof, true);
    ddFSTREAM.methods.push_back(var);
    var = addFunction("good", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtFSTREAM)}, (fVOIDFUNC)fstream_good, true);
    ddFSTREAM.methods.push_back(var);
}

void Program::populate_builtin_registry()
{
    if ( builtin_registry.defaults_loaded )
	return;

    builtin_registry.add_core_function("printstarred", datatype_vec_t{DataType::dtVOID, DataType::dtSTRING}, (fVOIDFUNC)printstarred);
    builtin_registry.add_core_function("printstr",     datatype_vec_t{DataType::dtVOID, DataType::dtSTRING}, (fVOIDFUNC)printstring);
    builtin_registry.add_core_function("printstream",  datatype_vec_t{DataType::dtVOID, DataType::dtSSTREAM}, (fVOIDFUNC)printstream);
    builtin_registry.add_core_function("puts",	 datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtCHAR)}, (fVOIDFUNC)puts);
    builtin_registry.add_core_function("puti",	 datatype_vec_t{DataType::dtVOID, DataType::dtINT}, (fVOIDFUNC)printinteger);
    builtin_registry.add_core_function("putu",	 datatype_vec_t{DataType::dtVOID, DataType::dtUINT64}, (fVOIDFUNC)printuinteger);
    builtin_registry.add_core_function("putd",	 datatype_vec_t{DataType::dtVOID, DataType::dtDOUBLE}, (fVOIDFUNC)printdouble);
    builtin_registry.add_core_function("putf",	 datatype_vec_t{DataType::dtVOID, DataType::dtFLOAT}, (fVOIDFUNC)printfloat);
    builtin_registry.add_core_function("putchar", datatype_vec_t{DataType::dtINT,  DataType::dtINT}, (fVOIDFUNC)putchar);
    builtin_registry.add_core_function("__builtin_memcpy", datatype_vec_t{rtPtr(DataType::dtVOID), rtPtr(DataType::dtVOID), rtPtr(DataType::dtVOID), DataType::dtUINT64}, (fVOIDFUNC)memcpy);
    // alloca() is a compiler intrinsic, not a real libc function.
    // Map to malloc for now (true stack alloca needs JIT intrinsic support).
    builtin_registry.add_core_function("alloca", datatype_vec_t{rtPtr(DataType::dtVOID), DataType::dtUINT64}, (fVOIDFUNC)malloc);
    builtin_registry.add_core_function("__builtin_memset", datatype_vec_t{rtPtr(DataType::dtVOID), rtPtr(DataType::dtVOID), DataType::dtINT, DataType::dtUINT64}, (fVOIDFUNC)memset);
    // istream `getline(istream&, string&)` lives in std:: — moved out of
    // the global symbol table so user code defining its own `getline`
    // (e.g. SMAUG IMC's `static const char *getline(char *buffer)`)
    // doesn't collide. `using namespace std;` exposes it unqualified;
    // bare `getline` falls back to the user-defined function or libc
    // dlsym. Registered under `__std_getline` and aliased into
    // namespace_map["std"]["getline"] in add_namespaces().
    builtin_registry.add_core_function("__std_getline", datatype_vec_t{rtPtr(DataType::dtISTREAM),rtPtr(DataType::dtISTREAM),rtPtr(DataType::dtSTRING)}, (fVOIDFUNC)(fnGETLINE)std::getline);
    builtin_registry.add_core_function("endl", datatype_vec_t{rtPtr(DataType::dtOSTREAM),rtPtr(DataType::dtOSTREAM)}, (fVOIDFUNC)(fnENDL)std::endl);
    builtin_registry.add_core_function("to_string", datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtINT64}, (fVOIDFUNC)madc_to_string);
    builtin_registry.add_core_function("to_string_d", datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtDOUBLE}, (fVOIDFUNC)madc_to_string_d);
    builtin_registry.add_core_function("stoi", datatype_vec_t{DataType::dtINT64, DataType::dtSTRING}, (fVOIDFUNC)madc_stoi);
    builtin_registry.add_core_function("stod", datatype_vec_t{DataType::dtDOUBLE, DataType::dtSTRING}, (fVOIDFUNC)madc_stod);
    // strlen is NOT pre-registered: it resolves via dlsym fallback to libc's
    // strlen(const char *). For std::string, use str.length() or str.size().

    builtin_registry.add_process_function("system", datatype_vec_t{DataType::dtINT64, DataType::dtSTRING}, (fVOIDFUNC)madc_system);
    builtin_registry.add_process_function("getenv", datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)madc_getenv);
    builtin_registry.add_process_function("get_argv", datatype_vec_t{DataType::dtCHARptr, DataType::dtINT64, DataType::dtINT64}, (fVOIDFUNC)madc_get_argv);
    builtin_registry.add_process_function("setenv", datatype_vec_t{DataType::dtVOID, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)madc_setenv);
    builtin_registry.add_process_function("unsetenv", datatype_vec_t{DataType::dtVOID, DataType::dtSTRING}, (fVOIDFUNC)madc_unsetenv);
    builtin_registry.add_process_function("__errno_location", datatype_vec_t{rtPtr(DataType::dtINT32)}, (fVOIDFUNC)__errno_location);

    builtin_registry.add_dlfcn_function("dlopen", datatype_vec_t{DataType::dtINT64, DataType::dtSTRING}, (fVOIDFUNC)madc_dlopen);
    builtin_registry.add_dlfcn_function("dlsym", datatype_vec_t{DataType::dtINT64, DataType::dtINT64, DataType::dtSTRING}, (fVOIDFUNC)madc_dlsym);
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
    namespace_registry.add_namespace("php", register_php_namespace_spec);
    namespace_registry.add_namespace("perl", register_perl_namespace_spec);
    namespace_registry.add_namespace("python", register_python_namespace_spec);
    namespace_registry.add_namespace("ruby", register_ruby_namespace_spec);
    namespace_registry.add_namespace("js", register_js_namespace_spec);
    namespace_registry.add_namespace("rust", register_rust_namespace_spec);
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
    register_function_specs(builtin_registry.process_functions);
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
    addGlobal(ddSTRING,  "version", 1, (void *)"v0.0.1");
}

enum { LAZY_IOSTREAM = 1, LAZY_STDIO = 2, LAZY_MATHH = 3 };

// populates lazy_map — symbols are registered on first use via lazy_resolve()
void Program::add_iostream()
{
    lazy_map["cout"] = {LAZY_IOSTREAM, Program::lkVariable};
    lazy_map["cin"]  = {LAZY_IOSTREAM, Program::lkVariable};
    lazy_map["cerr"] = {LAZY_IOSTREAM, Program::lkVariable};
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

    if ( header == LAZY_IOSTREAM )
    {
	if ( name == "cout" )
	    var = addGlobal(ddOSTREAM, "cout", 1, output().rdbuf());
	else if ( name == "cin" )
	    var = addGlobal(ddISTREAM, "cin", 1, input().rdbuf());
	else if ( name == "cerr" )
	    var = addGlobal(ddOSTREAM, "cerr", 1, error().rdbuf());

	if ( var )
	    namespace_map["std"][name] = var;
    }
    else if ( header == LAZY_STDIO )
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
    Variable *var;
    std::string id;

    // std:: namespace — map to existing global variables and functions
    variable_map_t &std_ns = namespace_map["std"];

    // cout/cin/cerr/endl are registered by add_iostream() when #include <iostream> is processed

    // std::for_each(array, func_ptr) — iterate array calling function per element
    extern void std_for_each(void *, int64_t);
    var = addFunction("__std_for_each",
	datatype_vec_t{DataType::dtVOID, DataType::dtARRAY, DataType::dtINT64},
	(fVOIDFUNC)std_for_each);
    if (var) std_ns["for_each"] = var;

    // std::getline(istream&, string&) — registered as __std_getline at
    // the top of _parser_init(); alias into the std namespace here.
    std::string gl_name = "__std_getline";
    var = findVariable(gl_name);
    if (var) std_ns["getline"] = var;

    std::string endl_name = "endl";
    var = findVariable(endl_name);
    if (var) std_ns["endl"] = var;

    DBG(std::cout << "add_namespaces() registered std:: with " << std_ns.size() << " members" << std::endl);
}

void Program::add_madc_namespace()
{
    variable_map_t &madc_ns = namespace_map["madc"];
    Variable *var;

    // register array type as madc::array
    std::string id = "__madc_array";
    var = new Variable(id, ddARRAY, 1, NULL, false);
    var->flags |= vfSTATIC;
    madc_ns["array"] = var;

    // regex functions
    extern int64_t madc_regex_match(void *, void *);
    extern int64_t madc_regex_search(void *, void *);
    extern void *madc_regex_replace(void *, void *, void *, void *);

    var = addFunction("__madc_regex_match",
	datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtSTRING},
	(fVOIDFUNC)madc_regex_match);
    if (var) madc_ns["regex_match"] = var;

    var = addFunction("__madc_regex_search",
	datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtSTRING},
	(fVOIDFUNC)madc_regex_search);
    if (var) madc_ns["regex_search"] = var;

    var = addFunction("__madc_regex_replace",
	datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING},
	(fVOIDFUNC)madc_regex_replace);
    if (var) madc_ns["regex_replace"] = var;

    Variable *scope_var = NULL;

    var = addFunction("__madc_eval_runtime",
	datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING},
	(fVOIDFUNC)madc_runtime_eval);
    scope_var = addFunction("__madc_eval_ctx_runtime",
	datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtARRAY},
	(fVOIDFUNC)madc_runtime_eval_ctx);
    if (var) madc_ns["eval"] =
	registration_policy.enable_runtime_eval_source_scope_access && scope_var ? scope_var : var;
    if (var) madc_ns["eval_unit"] =
	registration_policy.enable_runtime_eval_source_scope_access && scope_var ? scope_var : var;

    var = addFunction("__madc_eval_bool_runtime",
	datatype_vec_t{DataType::dtBOOL, DataType::dtSTRING},
	(fVOIDFUNC)madc_runtime_eval_bool);
    scope_var = addFunction("__madc_eval_bool_ctx_runtime",
	datatype_vec_t{DataType::dtBOOL, DataType::dtSTRING, DataType::dtARRAY},
	(fVOIDFUNC)madc_runtime_eval_bool_ctx);
    if (var) madc_ns["eval_bool"] =
	registration_policy.enable_runtime_eval_source_scope_access && scope_var ? scope_var : var;

    var = addFunction("__madc_eval_int_runtime",
	datatype_vec_t{DataType::dtINT64, DataType::dtSTRING},
	(fVOIDFUNC)madc_runtime_eval_int);
    scope_var = addFunction("__madc_eval_int_ctx_runtime",
	datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtARRAY},
	(fVOIDFUNC)madc_runtime_eval_int_ctx);
    if (var) madc_ns["eval_int"] =
	registration_policy.enable_runtime_eval_source_scope_access && scope_var ? scope_var : var;

    var = addFunction("__madc_eval_double_runtime",
	datatype_vec_t{DataType::dtDOUBLE, DataType::dtSTRING},
	(fVOIDFUNC)madc_runtime_eval_double);
    scope_var = addFunction("__madc_eval_double_ctx_runtime",
	datatype_vec_t{DataType::dtDOUBLE, DataType::dtSTRING, DataType::dtARRAY},
	(fVOIDFUNC)madc_runtime_eval_double_ctx);
    if (var) madc_ns["eval_double"] =
	registration_policy.enable_runtime_eval_source_scope_access && scope_var ? scope_var : var;

    var = addFunction("__madc_eval_string_runtime",
	datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING},
	(fVOIDFUNC)madc_runtime_eval_string);
    scope_var = addFunction("__madc_eval_string_ctx_runtime",
	datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtARRAY},
	(fVOIDFUNC)madc_runtime_eval_string_ctx);
    if (var) madc_ns["eval_string"] =
	registration_policy.enable_runtime_eval_source_scope_access && scope_var ? scope_var : var;

    var = addFunction("__madc_eval_expression_runtime",
	datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING},
	(fVOIDFUNC)madc_runtime_eval_expression);
    scope_var = addFunction("__madc_eval_expression_ctx_runtime",
	datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtARRAY},
	(fVOIDFUNC)madc_runtime_eval_expression_ctx);
    if (var) madc_ns["eval_expression"] =
	registration_policy.enable_runtime_eval_expression_scope_access && scope_var ? scope_var : var;

    if (scope_var) madc_ns["eval_expression_ctx"] = scope_var;

    var = addFunction("__madc_eval_expression_bool_runtime",
	datatype_vec_t{DataType::dtBOOL, DataType::dtSTRING},
	(fVOIDFUNC)madc_runtime_eval_expression_bool);
    scope_var = addFunction("__madc_eval_expression_bool_ctx_runtime",
	datatype_vec_t{DataType::dtBOOL, DataType::dtSTRING, DataType::dtARRAY},
	(fVOIDFUNC)madc_runtime_eval_expression_bool_ctx);
    if (var) madc_ns["eval_expression_bool"] =
	registration_policy.enable_runtime_eval_expression_scope_access && scope_var ? scope_var : var;
    if (scope_var) madc_ns["eval_expression_bool_ctx"] = scope_var;

    var = addFunction("__madc_eval_expression_int_runtime",
	datatype_vec_t{DataType::dtINT64, DataType::dtSTRING},
	(fVOIDFUNC)madc_runtime_eval_expression_int);
    scope_var = addFunction("__madc_eval_expression_int_ctx_runtime",
	datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtARRAY},
	(fVOIDFUNC)madc_runtime_eval_expression_int_ctx);
    if (var) madc_ns["eval_expression_int"] =
	registration_policy.enable_runtime_eval_expression_scope_access && scope_var ? scope_var : var;
    if (scope_var) madc_ns["eval_expression_int_ctx"] = scope_var;

    var = addFunction("__madc_eval_expression_double_runtime",
	datatype_vec_t{DataType::dtDOUBLE, DataType::dtSTRING},
	(fVOIDFUNC)madc_runtime_eval_expression_double);
    scope_var = addFunction("__madc_eval_expression_double_ctx_runtime",
	datatype_vec_t{DataType::dtDOUBLE, DataType::dtSTRING, DataType::dtARRAY},
	(fVOIDFUNC)madc_runtime_eval_expression_double_ctx);
    if (var) madc_ns["eval_expression_double"] =
	registration_policy.enable_runtime_eval_expression_scope_access && scope_var ? scope_var : var;
    if (scope_var) madc_ns["eval_expression_double_ctx"] = scope_var;

    var = addFunction("__madc_eval_expression_string_runtime",
	datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING},
	(fVOIDFUNC)madc_runtime_eval_expression_string);
    scope_var = addFunction("__madc_eval_expression_string_ctx_runtime",
	datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtARRAY},
	(fVOIDFUNC)madc_runtime_eval_expression_string_ctx);
    if (var) madc_ns["eval_expression_string"] =
	registration_policy.enable_runtime_eval_expression_scope_access && scope_var ? scope_var : var;
    if (scope_var) madc_ns["eval_expression_string_ctx"] = scope_var;

    var = addFunction("__madc_context_set_int_runtime",
	datatype_vec_t{DataType::dtVOID, DataType::dtARRAY, DataType::dtSTRING, DataType::dtINT64},
	(fVOIDFUNC)madc_context_set_int);
    if (var) madc_ns["context_set_int"] = var;

    var = addFunction("__madc_context_set_real_runtime",
	datatype_vec_t{DataType::dtVOID, DataType::dtARRAY, DataType::dtSTRING, DataType::dtDOUBLE},
	(fVOIDFUNC)madc_context_set_real);
    if (var) madc_ns["context_set_real"] = var;

    var = addFunction("__madc_context_set_string_runtime",
	datatype_vec_t{DataType::dtVOID, DataType::dtARRAY, DataType::dtSTRING, DataType::dtCHARptr},
	(fVOIDFUNC)madc_context_set_string);
    if (var) madc_ns["context_set_string"] = var;

    var = addFunction("__madc_context_set_array_runtime",
	datatype_vec_t{DataType::dtVOID, DataType::dtARRAY, DataType::dtSTRING, DataType::dtARRAY},
	(fVOIDFUNC)madc_context_set_array);
    if (var) madc_ns["context_set_array"] = var;

    DBG(std::cout << "add_madc_namespace() registered madc:: with " << madc_ns.size() << " members" << std::endl);
}

void Program::_parser_init()
{
    ensure_registration_config();
    add_functions();
    add_string_methods();
    add_sstream_methods();
    add_fstream_methods();
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
	|| raw == DataType::dtSTRING
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

    var = new Variable(id, ddSTRING, 1, NULL, true);
    var->makeconstant();
    std::string &str = *(std::string *)var->data;
    str = s;
    tkProgram->variables.push_back(var);

    return var;
}

Variable *Program::addVariable(TokenCpnd *code, DataDef &dd, std::string &id, int c, void *init, bool alloc)
{
    Variable *var;

    if ( code )
    {
	if ( (var=code->findVariable(id)) )
	    return var;
	// Function-local `extern T name;` is not a fresh local — it's a
	// reference to the file-scope global with that name. Without
	// this, comm.c's `extern char *help_greeting;` inside
	// new_descriptor() created an uninitialized local pointer
	// distinct from db.c's global, and the deref crashed on the
	// first incoming connection.
	if ( parsing_extern_decl )
	{
	    if ( (var=tkProgram->findVariable(id)) )
		return var;
	}
	var = new Variable(id, dd, c, init, alloc);
	var->flags |= vfLOCAL;
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
	return var;
    }
    var = new Variable(id, dd, c, init, alloc);
    tkProgram->variables.push_back(var);

    DBG(std::cout << "Added new global variable type: " << dd.name << " size: "
		<< dd.size << " name: " << id << " ptr: " << var << " flags: " << var->flags << std::endl);
    DBG(std::cout << "Data address: " << (uint64_t)var->data << std::endl);

    return var;
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

    auto resolve_data_type = [this](DataType dt) -> DataDef *
    {
	if ( DataDef::rawtype(dt) != dt )
	{
	    if ( dt == rtPtr(DataType::dtCHAR) )
		return &ddLPSTR;

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
		case DataType::dtSTRING: base = &ddSTRING;	break;
		case DataType::dtARRAY:  base = &ddARRAY;	break;
		case DataType::dtOSTREAM: base = &ddOSTREAM; break;
		case DataType::dtISTREAM: base = &ddISTREAM; break;
		case DataType::dtSSTREAM: base = &ddSSTREAM; break;
		case DataType::dtIFSTREAM: base = &ddIFSTREAM; break;
		case DataType::dtOFSTREAM: base = &ddOFSTREAM; break;
		case DataType::dtFSTREAM:  base = &ddFSTREAM;  break;
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
	    case DataType::dtSTRING:  return &ddSTRING;
	    case DataType::dtARRAY:   return &ddARRAY;
	    case DataType::dtOSTREAM: return &ddOSTREAM;
	    case DataType::dtISTREAM: return &ddISTREAM;
	    case DataType::dtSSTREAM: return &ddSSTREAM;
	    case DataType::dtIFSTREAM:return &ddIFSTREAM;
	    case DataType::dtOFSTREAM:return &ddOFSTREAM;
	    case DataType::dtFSTREAM: return &ddFSTREAM;
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
    }
    DBG(std::cout << endl);

    func->funcnode = NULL;
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
	    size_t expected = fd->parameters.size()
		- (fd->has_captures ? 1 : 0)
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
		// don't count hidden env param for [&] lambdas
		size_t expected = fd->parameters.size() - (fd->has_captures ? 1 : 0);
		if ( tc->argc() != expected )
		    Throw(tc) << "Incorrect number of parameters: expected " << expected << " got " << tc->argc() << flush;
	    }
	}
	else
	{
	    FuncDef *fd = (FuncDef *)tc->var.type;
	    Method *md = (Method *)tc->var.data;
	    // In C, f() with no params accepts any number of arguments (K&R style).
	    // Only f(void) means exactly zero. Skip the check for empty-param functions.
	    if ( !(fd->parameters.empty() && !fd->is_void_params) )
	    {
		size_t expected = fd->parameters.size() - (fd->has_captures ? 1 : 0)
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
	    mvar->flags = var->flags;

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
	    bool fixed_array = false;
	    if ( TokenVar *tv = dynamic_cast<TokenVar *>(result) )
		fixed_array = tv->var.is_fixed_array();
	    if ( fixed_array )
		elem_type = base_type ? base_type : &ddINT64;
	    else if ( base_type && (base_type->is_pointer() || dynamic_cast<DataDefCArray *>(base_type) != NULL) )
		elem_type = unwrap_subscript_element_type(base_type);
	    result = new TokenSubscriptExpr(result, idx_expr, elem_type);
	    continue;
	}
	break;
    }
    return result;
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
	if ( !dynamic_cast<TokenStructLit *>(compound) )
	    Throw(ampersand) << "expected compound literal after '&'" << flush;
	return compound;
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
	    avar = addFunction(aname, datatype_vec_t{DataType::dtINT64}, (fVOIDFUNC)sym);
    }
    if ( !avar )
	Throw(addr_tb) << "undeclared identifier '" << aname << "'" << flush;
    if ( avar->type && avar->type->is_function() )
	return new TokenVar(*avar);
    avar->flags |= vfADDRTAKEN;
    DataDefPTR *aptr = getPointerType(avar->type);
    return new TokenAddrOf(*avar, aptr);
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

    DBG(std::cout << tb->line << ':' << tb->column << ":Program::parseExpression(" << tb->get() << " type: " << (int)tb->type() << ") start" << (conditional ? " conditional" : "") << std::endl);

    while ( !done && tb )
    {
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
		    // evaluate left, discard, continue with right side.
		    if ( brackets > 0 )
		    {
			DBG(cout << "parseExpression: comma operator (brackets=" << brackets << ")" << endl);
			// Flush pending operators above the last '(' so the
			// left-side expression is fully reduced, then discard it.
			while ( !opStack.empty() && opStack.top()->get() != '(' )
			    popOperator(opStack, exStack);
			if ( !exStack.empty() )
			    exStack.pop(); // discard left operand
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
			if ( !exStack.empty() )
			    exStack.pop();
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
			static int sub_tmp_counter = 0;
			TokenVar *tv = dynamic_cast<TokenVar *>(exStack.top());
			exStack.pop();
			// parse index expression (stops at ] via peek-stop below)
			TokenBase *idx = parseExpression(nextToken());
			TokenBase *clsqr = nextToken(); // consume ]
			if ( !clsqr || clsqr->id() != TokenID::tkClSqr )
			    Throw(tb) << "Expected ] in subscript expression" << flush;
			// for string-returning containers, allocate a temp variable for the result
			Variable *tmp = nullptr;
			if ( tv->var.type->type() == DataType::dtVECTOR ) {
			    DataDefVECTOR *vdd = static_cast<DataDefVECTOR *>(tv->var.type);
			    if ( vdd->element_type->is_string() ) {
				std::string tmpname = "__sub_tmp_" + std::to_string(sub_tmp_counter++);
				tmp = addVariable(code, ddSTRING, tmpname, 1, NULL, true);
			    }
			} else if ( tv->var.type->type() == DataType::dtMAP ) {
			    DataDefMAP *mdd = static_cast<DataDefMAP *>(tv->var.type);
			    if ( mdd->val_type->is_string() ) {
				std::string tmpname = "__sub_tmp_" + std::to_string(sub_tmp_counter++);
				tmp = addVariable(code, ddSTRING, tmpname, 1, NULL, true);
			    }
			}
			DBG(cout << "parseExpression: subscript on " << tv->var.name << endl);
			exStack.push(new TokenSubscript(tv->var, idx, tmp));
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
		    bool top_is_cast_pointer = false;
		    if ( !exStack.empty() )
		    {
			if ( TokenCast *tc = dynamic_cast<TokenCast *>(exStack.top()) )
			{
			    if ( tc->datadef() && tc->datadef()->is_pointer() )
				top_is_cast_pointer = true;
			}
		    }
		    if ( !exStack.empty()
		      && (exStack.top()->type() == TokenType::ttMember
		       || exStack.top()->type() == TokenType::ttSubscript
		       || dynamic_cast<TokenDerefExpr *>(exStack.top()) != NULL
		       || dynamic_cast<TokenDeref *>(exStack.top()) != NULL
		       || top_is_complex_ptr_expr
		       || top_is_cast_pointer) )
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
			if ( dd && (dd->is_pointer() || dd->is_string()
			  || dd->type() == DataType::dtCHARptr) )
			{
			    TokenBase *base_expr = exStack.top();
			    exStack.pop();
			    TokenBase *idx = parseExpression(nextToken());
			    TokenBase *clsqr = nextToken();
			    if ( !clsqr || clsqr->id() != TokenID::tkClSqr )
				Throw(tb) << "Expected ] in subscript expression" << flush;
			    DataDef *elem_type = dd;
			    if ( elem_type->is_pointer() )
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
		    TokenBase *cast_qualifier = NULL;
		    if ( peek1 )
		    {
			if ( peek1->id() == TokenID::tkCONST )
			{
			    cast_qualifier = nextToken();
			    peek1 = peekToken();
			}
			if ( peek1->type() == TokenType::ttDataType )
			    cast_dd = &((TokenDataType *)peek1)->definition;
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
				cast_dd = &tdmi->second->definition;
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
			      || peekToken()->id() == TokenID::tkRESTRICT) )
			{
			    TokenBase *pt = nextToken();
			    if ( pt->id() == TokenID::tkMul )
				cast_dd = getPointerType(cast_dd);
			    // const/restrict are skipped — no JIT effect
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
						    slit->inits.push_back(read_compound_struct_lit(NULL));
						}
						else
						{
						    TokenBase *elem = nextToken();
						    if ( elem->id() == TokenID::tkDot )
						    {
							std::vector<std::string> field_path;
							TokenBase *field_tok = nextToken();
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
					exStack.push(slit);
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
			    // - n` means `((long)q) - n`, not `(long)(q - n)`. When
			    // the body is a bare identifier (with an optional postfix
			    // chain of ->/./[] accesses), use parsePostfixChain which
			    // stops at the first non-postfix token. Function calls
			    // (`ident(args)`) and everything else (parenthesized
			    // body, unary-operator head) still go through
			    // parseExpression so the full call or complex expression
			    // parses correctly.
		    bool ident_no_call = cast_expr_tb
		      && cast_expr_tb->type() == TokenType::ttIdentifier
		      && !(peekToken() && peekToken()->id() == TokenID::tkOpBrk);
		    if ( ident_no_call )
		    {
			cast_expr = parsePostfixChain(cast_expr_tb);
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
				cast_expr = cast_expr_tb;
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
		      && dynamic_cast<DataDefFPTR *>(var_call_base->var.type) )
		    {
			TokenVar *tv = var_call_base;
			exStack.pop();
			TokenCallFunc *tc = new TokenCallFunc(tv->var);
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
			bool postfix_follows = next
			    && (next->id() == TokenID::tkDot
			     || next->id() == TokenID::tkDeRef
			     || next->id() == TokenID::tkOpSqr);
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
		    TokenBase *texpr = nextToken();
		    ternary->true_expr = parseExpression(texpr, true, true);
		    // after conditional parseExpression, expect : next
		    TokenBase *colon = nextToken();
		    if ( colon->id() != TokenID::tkTerC )
			Throw(colon) << "Expecting : in ternary expression" << flush;
		    // parse false expression
		    TokenBase *fexpr = nextToken();
		    ternary->false_expr = parseExpression(fexpr, conditional);
		    // Propagate branch datadef up to the ternary so downstream
		    // type-directed paths (e.g. TokenAssign's dtSTRING → char*
		    // coercion) can see a meaningful datadef(). Prefer the true
		    // branch's type; fall back to the false branch if that's
		    // richer (non-NULL / non-int).
		    //
		    // Mixed string-literal/pointer ternary: when one branch is
		    // a string literal (dtSTRING) and the other is a real
		    // char*-yielding expression (pointer type), prefer the
		    // pointer type. The merge needs both branches to land as
		    // raw char* in the merge slot — labeling the result
		    // dtSTRING would force downstream `string_cstr` over the
		    // function's char* return, dereferencing it as if it were
		    // a std::string and crashing. Closes SMAUG boards.c:1615
		    // `feof(fp) ? "End" : fread_word(fp)`.
		    DataDef *tdd = ternary->true_expr  ? ternary->true_expr->datadef()  : NULL;
		    DataDef *fdd = ternary->false_expr ? ternary->false_expr->datadef() : NULL;
		    DataDef *ternary_dd = tdd;
		    if ( (!ternary_dd || ternary_dd == &ddINT64) && fdd && fdd != &ddINT64 )
			ternary_dd = fdd;

		    // C ternary type unification for pointer-flavored
		    // branches. Each branch has an *effective* type:
		    //   - real pointer types stay as-is
		    //   - dtSTRING (string literals) → char* equivalent
		    //   - fixed-array variables / members → char* equivalent
		    //     (their datadef reports the element type, but
		    //     the value decays to pointer-to-element)
		    // When both branches are pointer-flavored and their
		    // raw datadefs disagree, unify on a real pointer if
		    // available, else on `char *` (ddLPSTR). Without this,
		    // the merge slot inherits a single-byte / dtSTRING /
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
			if ( dd->is_pointer() || dd->is_string() )
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
		if ( tb->id() == TokenID::tkAdd && isUnaryPosition()
		  && (exStack.empty()
		   || (_prv_token && (_prv_token->id() == TokenID::tkAssign
		     || _prv_token->id() == TokenID::tkComma
		     || _prv_token->id() == TokenID::tkOpBrk
		     || _prv_token->id() == TokenID::tkOpSqr
		     || _prv_token->id() == TokenID::tkSemi))) )
		    break;
		// & address-of in unary position
		if ( tb->id() == TokenID::tkBand && isUnaryPosition() )
		{
		    exStack.push(parseAddressOfExpression(tb));
		    break;
		}
		// GNU computed-goto label address: `&&label`
		if ( tb->id() == TokenID::tkLand && isUnaryPosition() )
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
			if ( tb->id() == TokenID::tkMul && isUnaryPosition() )
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
			DataDef *dtype = deref_expr->datadef();
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
				    if ( !dvar->type->is_pointer() && !dvar->is_fixed_array() )
					Throw(deref_tb) << "cannot dereference non-pointer type" << flush;
				    DataDef *base = dvar->type;
				    if ( dvar->type->is_pointer() )
				    {
					DataDefPTR *dptr = dynamic_cast<DataDefPTR *>(dvar->type);
					base = dptr ? dptr->base_type : &ddINT64;
				    }
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
				    if ( inner_tb->id() == TokenID::tkOpBrk )
				    {
					TokenBase *inner_expr_tb = nextToken();
					TokenBase *inner_expr = parseExpression(inner_expr_tb, true);
					TokenBase *close = nextToken();
					if ( !close || close->id() != TokenID::tkClBrk )
					    Throw(close ? close : inner_tb) << "expected ')' after *(expr)" << flush;
					DataDef *inner_dtype = inner_expr ? inner_expr->datadef() : NULL;
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
					if ( !inner_var->type->is_pointer() && !inner_var->is_fixed_array() )
					    Throw(inner_tb) << "cannot dereference non-pointer type" << flush;
					DataDef *inner_base = inner_var->type;
					if ( inner_var->type->is_pointer() )
					{
					    DataDefPTR *inner_dptr = dynamic_cast<DataDefPTR *>(inner_var->type);
					    inner_base = inner_dptr ? inner_dptr->base_type : &ddINT64;
					}
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
				    DataDef *inner_dtype = inner_expr->datadef();
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
				DataDef *dtype = deref_expr->datadef();
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
		// Arithmetic chain at same precedence: + - * / % are
		// left-associative, so `a - b + c` must build as
		// `(a - b) + c`. The general operator> below uses a
		// strict-greater comparison; when both ops have the
		// same precedence it returns false, leaving the stack
		// top in place — which then resolves newest-first
		// (i.e. as if right-associative). Stream `<<` chains
		// and other compile paths depended on that historical
		// shape, so we keep the global behavior and only
		// force a left-associative pop here for the four
		// arithmetic precedences (3 = * / %, 4 = + -).
		while ( !opStack.empty() && opStack.top()->id() != TokenID::tkOpBrk
		&&      (opStack.top()->type() == TokenType::ttCallFunc || opStack.top()->type() == TokenType::ttCallMethod
		||      (opStack.top()->is_operator() && (*((TokenOperator *)opStack.top()) > *to))
		||      (opStack.top()->is_operator()
			 && ((TokenOperator *)opStack.top())->precedence() == to->precedence()
			 && (to->precedence() == 3 || to->precedence() == 4))) )
		{
		    DBG(cout << "Operator(" << (char)opStack.top()->get() << ") has precedence over operator(" << (char)to->get() << ')' << endl);
		    popOperator(opStack, exStack);
		}
		DBG(cout << "Pushing " << (char)tb->get() << " onto opStack" << endl);
		opStack.push(to);
		break;
            case TokenType::ttDataType:
		bt = (TokenDataType *)tb;
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
		var = addLiteral(((TokenIdent *)tb)->str);
		DBG(cout << "Pushing new variable of literal: " << var->name << " onto exStack" << endl);
		exStack.push(new TokenVar(*var));
		break;
	    case TokenType::ttKeyword:
		// const/volatile/restrict qualifiers are compile-time
		// annotations — skip them in expression context (casts,
		// sizeof, etc.)
		if ( tb->id() == TokenID::tkCONST
		  || tb->id() == TokenID::tkRESTRICT )
		{
		    tb = nextToken();
		    if ( !tb ) { done = true; break; }
		    continue;
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
			ti->file = tb->file;
			ti->line = tb->line;
			ti->column = tb->column;
			exStack.push(ti);
		    }
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
		// va_arg(ap, type) — compiler intrinsic for reading variadic args
		if ( ident_tb->str == "va_arg" )
		{
		    if ( !peekToken() || peekToken()->id() != TokenID::tkOpBrk )
			Throw(tb) << "Expecting '(' after va_arg" << flush;
		    nextToken(); // consume (
		    // first arg: the va_list variable name
		    TokenBase *ap_tb = nextToken();
		    // Handle *ptr form: va_arg(*ap, type)
		    bool deref_ap = false;
		    if ( ap_tb->id() == TokenID::tkMul )
		    {
			deref_ap = true;
			ap_tb = nextToken();
			// consume optional () around function call: va_arg(*foo(), type)
			if ( ap_tb->type() == TokenType::ttIdentifier && peekToken()
			  && peekToken()->id() == TokenID::tkOpBrk )
			{
			    // function call — parse as expression and get the result
			    // For now, just consume the call and use the name
			    nextToken(); // consume (
			    if ( peekToken() && peekToken()->id() == TokenID::tkClBrk )
				nextToken(); // consume )
			}
		    }
		    if ( ap_tb->type() != TokenType::ttIdentifier )
			Throw(ap_tb) << "Expecting va_list variable name in va_arg" << flush;
		    std::string ap_name = ((TokenIdent *)ap_tb)->str;
		    TokenCpnd *scope = compounds.empty() ? NULL : compounds.top();
		    Variable *ap_var = scope ? scope->findVariable(ap_name) : NULL;
		    if ( !ap_var )
			ap_var = findVariable(ap_name);
		    if ( !ap_var )
			Throw(ap_tb) << "Unknown variable '" << ap_name << "' in va_arg" << flush;
		    // consume comma
		    TokenBase *comma_tb = nextToken();
		    if ( comma_tb->id() != TokenID::tkComma )
			Throw(comma_tb) << "Expecting ',' after va_list variable in va_arg" << flush;
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
		    exStack.push(new TokenVaArg(ap_var, target_dd));
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
		      && lhs_dot->type() != TokenType::ttCallFunc )
			Throw(tb) << "member reference is not a structure or union" << flush;
		    Variable *tv_var;
		    DataDef  *struct_type;
		    if ( lhs_dot->type() == TokenType::ttVariable )
		    {
			TokenVar *tv = dynamic_cast<TokenVar *>(lhs_dot);
			tv_var      = &tv->var;
			struct_type =  tv->var.type;
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
			else
			    Throw(tb) << "member reference '.' on unsupported subscript form" << flush;
		    }
		    if ( !struct_type->is_struct() && !struct_type->is_object() )
			Throw(tb) << "member reference is not a structure or union" << flush;
		    var = NULL;
		    string id = ident_tb->str;
		    if ( struct_type->is_object() && (var=((DataDefCLASS *)struct_type)->findMethod(id)) )
		    {
			if ( lhs_dot->type() != TokenType::ttVariable )
			    Throw(tb) << "chained method call not yet supported" << flush;
			TokenVar *tv = dynamic_cast<TokenVar *>(lhs_dot);
			// cout << "Found " << tv->var.name << "::" << var->name << endl;
			TokenCallMethod *tc = new TokenCallMethod(tv->var, *var);
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
		    DataDef *mtype = ((DataDefSTRUCT *)struct_type)->m_type(id);
		    // create new variable
		    var = new Variable(id, *mtype, 1, NULL, false);
		    var->flags = tv_var->flags;
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
		    if ( lhs->type() == TokenType::ttVariable )
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
		    else if ( lhs->datadef() && lhs->datadef()->is_pointer() )
		    {
			obj_type = lhs->datadef();
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

		    // get member offset and type
		    ssize_t ofs = ((DataDefSTRUCT *)base)->m_offset(id);
		    if ( ofs == -1 )
			Throw(tb) << "no member named '" << id << "'" << flush;
		    DataDef *mtype = ((DataDefSTRUCT *)base)->m_type(id);

		    // create variable for the member
		    var = new Variable(id, *mtype, 1, NULL, false);
		    var->flags = obj_var->flags;

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
				datatype_vec_t{DataType::dtINT64},
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
			var = addVariable(NULL, *implicit_func, fname, 1, NULL, false);
			Method *implicit_method = new Method(*var);
			var->data = (void *)implicit_method;
			DBG(cout << "parseExpression() created implicit function declaration for " << fname << endl);
		    }
		}
		if ( !var )
		{
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
	    {
		// If the original Variable's name doesn't match the
		// namespace key (e.g. std::getline keyed as "getline"
		// but the underlying Variable is named "__std_getline"),
		// register an alias Variable so unqualified `getline(...)`
		// lookups resolve. Otherwise the imported Variable's name
		// must match — push it through unchanged.
		Variable *src = vmi->second;
		if ( src->name != name )
		{
		    Variable *alias = new Variable();
		    alias->name = name;
		    alias->type = src->type;
		    alias->data = src->data;
		    alias->count = src->count;
		    alias->flags = src->flags;
		    pgm.tkProgram->variables.push_back(alias);
		}
		else
		    pgm.tkProgram->variables.push_back(src);
	    }
	    DBG(std::cout << "TokenUSING::parse() imported " << ns_name << "::" << name << std::endl);
	}
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
	if ( vmi == nsi->second.end() )
	    pgm.Throw(tn) << "'" << member_name << "' is not a member of namespace '" << ns_name << "'" << flush;
	// import into global scope
	std::string name = member_name;
	if ( !pgm.findVariable(name) )
	    pgm.tkProgram->variables.push_back(vmi->second);
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

    // check for __attribute__((packed)) before or after tag
    bool is_packed = false;
    size_t explicit_align = 0;
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
    if ( tn->type() == TokenType::ttIdentifier )
    {
	tag = (TokenIdent *)pgm.nextToken(); // consume tag
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
	    if ( dmi == pgm.struct_map.end() )
	    {
		DataDefSTRUCT *fwd = new DataDefSTRUCT(tag->str, 0);
		fwd->union_layout = is_union;
		pgm.struct_map[scoped_struct_tag(tag->str)] = fwd;
	    }
	    pgm.nextToken(); // consume ';'
	    DBG(cout << "TokenSTRUCT::parse() forward declaration of struct " << tag->str << endl);
	    return NULL;
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
	    if ( tn->type() != TokenType::ttIdentifier )
		pgm.Throw(tn) << "Expecting identifier after struct tag in typedef" << flush;
	    TokenIdent *alias = (TokenIdent *)tn;
	    if ( (bmi=pgm.datatype_map.find(alias->str)) != pgm.datatype_map.end() )
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
	    tdt = new TokenDataType(alias->str.c_str(), *alias_dd);
	    pgm.datatype_map[alias->str] = tdt;
	    // also register tag in struct_map so "struct tag" works
	    if ( !alias_dd->is_pointer() )
		pgm.struct_map[alias->str] = dmi->second;
	    return NULL;
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

    DataDefSTRUCT *dds = new DataDefSTRUCT(tag ? tag->str : "anonymous", 0);
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
    if ( explicit_align > dds->max_align )
	dds->max_align = explicit_align;
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
	while ( tn && (tn->id() == TokenID::tkCONST) )
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
	    TokenBase *stag = pgm.peekToken();
	    std::function<void(DataDefSTRUCT *, TokenBase *)> parse_nested_aggregate_body;
	    parse_nested_aggregate_body = [&](DataDefSTRUCT *inner, TokenBase *loc) -> void
	    {
		while ( (tn = pgm.peekToken()) && tn->id() != TokenID::tkClBrc )
		{
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
			TokenBase *inner_tag = pgm.peekToken();
			if ( inner_tag && inner_tag->id() == TokenID::tkOpBrc )
			{
			    pgm.nextToken();
			    DataDefSTRUCT *nested = new DataDefSTRUCT("anonymous", 0);
			    nested->union_layout = inner_union_kw;
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
				    pgm.struct_map[sname] = nested;
				}
				else
				{
				    nested = static_cast<DataDefSTRUCT *>(sdmi->second);
				    if ( nested->size != 0 || !nested->members.empty() )
					pgm.Throw(inner_tag) << "Struct '" << sname << "' already defined" << flush;
				}
				nested->union_layout = inner_union_kw;
				pgm.nextToken();
				parse_nested_aggregate_body(nested, inner_tag);
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
		    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpSqr )
		    {
			pgm.nextToken();
			TokenBase *cl = pgm.nextToken();
			if ( cl && cl->id() == TokenID::tkClSqr )
			{
			    inner_count = 0;
			    break;
			}
			pgm.pushToken(cl);
			if ( !inner_count_expr && bracket_dim_uses_runtime_value(pgm) )
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
			    inner_count_expr, inner_count_expr != NULL || inner_count != 1);
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
			    if ( !ccount_expr && bracket_dim_uses_runtime_value(pgm) )
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
		inner->finalize();
	    };
	    if ( stag && stag->id() == TokenID::tkOpBrc )
	    {
		// Support anonymous nested struct members like:
		//   struct { int x; char y[8]; } member;
		pgm.nextToken(); // consume '{'
		DataDefSTRUCT *inner = new DataDefSTRUCT("anonymous", 0);
		inner->union_layout = nested_union_kw;
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
		// skip const/restrict qualifiers between type and pointer stars
		// e.g. `char const *p;`
		while ( pgm.peekToken() && (pgm.peekToken()->id() == TokenID::tkCONST
			|| pgm.peekToken()->id() == TokenID::tkRESTRICT) )
		    pgm.nextToken();
		// skip __attribute__((...)) after struct/union/enum type before member name
		if ( is_attribute_identifier_token(pgm.peekToken()) )
		{
		    pgm.nextToken(); // consume __attribute__
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

		    // Optional fixed-array dimensions: `char d_name[256];`, `int m[4][8];`.
		    // Multiply the dimensions into a single count so the member reserves
		    // N*sizeof(base) bytes inline. Access via `&obj.member` yields a pointer
		    // to the start of the inline buffer.
		    size_t member_count = 1;
		    bool member_is_array_decl = false;
		    TokenBase *member_count_expr = NULL;
		    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkOpSqr )
		    {
			member_is_array_decl = true;
			pgm.nextToken(); // consume '['
			TokenBase *cl = pgm.nextToken();
			if ( cl && cl->id() == TokenID::tkClSqr )
			{
			    member_count = 0;
			    break;
			}
			pgm.pushToken(cl);
			if ( !member_count_expr && bracket_dim_uses_runtime_value(pgm) )
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
			    member_count_expr, member_is_array_decl);
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
    dds->finalize(); // round up size to struct alignment

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
		existing->member_offsets = dds->member_offsets;
		existing->member_bitfields = dds->member_bitfields;
		existing->size = dds->size;
		existing->runtime_size_expr = dds->runtime_size_expr;
		existing->pack = dds->pack;
		existing->max_align = dds->max_align;
		existing->union_layout = dds->union_layout;
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
	    bmi = pgm.datatype_map.find(alias->str);
	    if ( bmi != pgm.datatype_map.end() && pgm.compounds.empty() )
		pgm.Throw(tn) << "Identifier '" << alias->str << "' already defined" << flush;
	    tdt = new TokenDataType(alias->str.c_str(), *alias_dd);
	    pgm.datatype_map[alias->str] = tdt;
	    if ( alias_dd == dds )
		pgm.struct_map[alias->str] = dds;
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
	return NULL;
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

    // struct tag { ... }; — just a type definition, nothing to compile
    if ( tn && tn->id() == TokenID::tkSemi )
    {
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

    DataDefCLASS *ddc = new DataDefCLASS(tag->str, 0, DataType::dtRESERVED);
    DBG(cout << "TokenCLASS::parse() defining class " << tag->str << endl);

    while ( (tn=pgm.peekToken()) && tn->id() != TokenID::tkClBrc )
    {
	// expect a data type token
	if ( tn->type() != TokenType::ttDataType )
	    pgm.Throw(tn) << "Expecting type in class definition" << flush;
	TokenDataType *mtype = (TokenDataType *)pgm.nextToken();

	// check for pointer declarator(s): type * [*...] member_name
	DataDef *cmember_dd = &mtype->definition;
	while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkMul )
	{
	    pgm.nextToken(); // consume '*'
	    cmember_dd = pgm.getPointerType(cmember_dd);
	}

	// expect member name
	tn = pgm.nextToken();
	if ( tn->type() != TokenType::ttIdentifier )
	    pgm.Throw(tn) << "Expecting member name in class definition" << flush;
	std::string mname = ((TokenIdent *)tn)->str;

	// peek: is this a method (followed by '(') or a data member (followed by ';')?
	tn = pgm.peekToken();
	if ( tn && tn->id() == TokenID::tkOpBrk )
	{
	    pgm.nextToken(); // consume '('
	    // method declaration — parse as function, add to class methods
	    DBG(cout << "TokenCLASS::parse() parsing method " << mname << endl);
	    // mangle method name to avoid collisions: ClassName__methodName
	    std::string mangled = tag->str + "__" + mname;
	    pgm.parseFunction(*cmember_dd, mangled, ddc);
	    // find the variable that parseFunction created and add to class methods
	    Variable *mvar;
	    if ( (mvar=pgm.tkProgram->findVariable(mangled)) )
	    {
		ddc->methods.push_back(mvar);
		// also register under the unmangled name for method lookup
		ddc->method_map[mname] = mvar;
	    }
	}
	else
	{
	    // data member
	    ddc->addMember(mname, *cmember_dd, 1);
	    DBG(cout << "TokenCLASS::parse() added member " << cmember_dd->name << ' ' << mname
		<< " (size " << cmember_dd->size << ", total " << ddc->size << ')' << endl);
	    tn = pgm.nextToken();
	    if ( tn->id() != TokenID::tkSemi )
		pgm.Throw(tn) << "Expecting ';' after class member" << flush;
	}
    }

    if ( !tn )
	pgm.Throw << "Unexpected end of input in class definition" << flush;
    pgm.nextToken(); // consume '}'

    // register the class type
    if ( pgm.struct_map.find(tag->str) != pgm.struct_map.end() )
	pgm.Throw(tag) << "Class '" << tag->str << "' already defined" << flush;
    pgm.struct_map[tag->str] = ddc;
    // also register as a data type so "ClassName var;" works without "class" prefix
    tdt = new TokenDataType(tag->str.c_str(), *ddc);
    pgm.datatype_map[tag->str] = tdt;
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

    // detect range-based for: for (type var : container)
    bool typed_for_init = false;
    if ( tn->type() == TokenType::ttDataType )
    {
	TokenDataType *dt = (TokenDataType *)tn;
	TokenBase *tn2 = pgm.nextToken();
	if ( tn2->type() == TokenType::ttIdentifier )
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
		fe->elemname = ((TokenIdent *)tn2)->str;

		DBG(cout << "TokenFOR::parse() range-for detected: " << dt->definition.name << ' ' << fe->elemname << endl);

		// add the loop variable to the current scope
		TokenCpnd *code = pgm.compounds.empty() ? NULL : pgm.compounds.top();
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

	// not range-for — traditional for with type declaration
	DBG(cout << "TokenFOR::parse() traditional for with type declaration" << endl);
	pgm.pushToken(tn2);
	initialize = pgm.parseDeclaration(dt);
	typed_for_init = true;
    }
    else
    {
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
	if ( &td->definition == &ddSTRING
	  || &td->definition == &ddARRAY )
	    return true;
    }
    if ( tb->type() != TokenType::ttKeyword )
	return false;
    switch ( tb->id() )
    {
	case TokenID::tkCLASS:
	case TokenID::tkVECTOR:
	case TokenID::tkMAP:
	case TokenID::tkSET:
	case TokenID::tkLIST:
	// C++ keywords that are valid C identifiers — `int try;`, struct
	// member named `new`, `void *catch_block`, etc.
	case TokenID::tkTRY:
	case TokenID::tkCATCH:
	case TokenID::tkTHROW:
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
	if ( &td->definition == &ddSTRING )
	    return td->str;
    }
    if ( tb->id() == TokenID::tkCLASS
	|| tb->id() == TokenID::tkVECTOR
	|| tb->id() == TokenID::tkMAP
	|| tb->id() == TokenID::tkSET
	|| tb->id() == TokenID::tkLIST
	|| tb->id() == TokenID::tkTRY
	|| tb->id() == TokenID::tkCATCH
	|| tb->id() == TokenID::tkTHROW )
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

    // skip const/restrict qualifiers: `typedef const struct X *const_ptr;`
    while ( tn && (tn->id() == TokenID::tkCONST || tn->id() == TokenID::tkRESTRICT) )
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

	DataDef *enum_alias_dd = new DataDef(ddINT);
	enum_alias_dd->name = alias;
	TokenDataType *tdt = new TokenDataType(alias.c_str(), *enum_alias_dd);
	pgm.datatype_map[alias] = tdt;
	DBG(std::cout << "TokenTYPEDEF::parse() enum alias " << alias << " = int" << std::endl);

	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
	    pgm.nextToken();
	return NULL;
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
    }
    if ( !base_dd )
	pgm.Throw(tn) << "Expecting type after 'typedef'" << flush;

    // handle pointer: typedef int *intptr;
    while ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkMul )
    {
	pgm.nextToken();
	base_dd = pgm.getPointerType(base_dd);
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
	TokenDataType *tdt = new TokenDataType(alias.c_str(), *fptr);
	pgm.datatype_map[alias] = tdt;
	DBG(std::cout << "TokenTYPEDEF::parse() fptr (form 2): " << alias << std::endl);
	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
	    pgm.nextToken();
	return NULL;
    }

    // get alias name (may be an identifier or an existing type name being redefined)
    tn = pgm.nextToken();
    std::string alias;
    if ( tn->type() == TokenType::ttIdentifier )
	alias = ((TokenIdent *)tn)->str;
    else if ( tn->type() == TokenType::ttDataType )
	alias = ((TokenDataType *)tn)->str;
    else if ( tn->type() == TokenType::ttKeyword )
	alias = ((TokenKeyword *)tn)->str;
    else
	pgm.Throw(tn) << "Expecting alias name in typedef" << flush;

    size_t gnu_vector_bytes = 0;
    if ( is_attribute_identifier_token(pgm.peekToken()) )
	gnu_vector_bytes = parse_gnu_vector_size_attribute(pgm);

    // Function-pointer typedef Form 1: typedef RET NAME(params);
    TokenBase *post = pgm.peekToken();
    if ( post && post->id() == TokenID::tkOpBrk )
    {
	pgm.nextToken(); // consume '('
	FuncDef *func = pgm.parseFnPtrParams(*base_dd);
	DataDefFPTR *fptr = new DataDefFPTR(func);
	TokenDataType *tdt = new TokenDataType(alias.c_str(), *fptr);
	pgm.datatype_map[alias] = tdt;
	DBG(std::cout << "TokenTYPEDEF::parse() fptr (form 1): " << alias << std::endl);
	if ( pgm.peekToken() && pgm.peekToken()->id() == TokenID::tkSemi )
	    pgm.nextToken();
	return NULL;
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
	    if ( !alias_count_expr && bracket_dim_uses_runtime_value(pgm) )
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

    return NULL;
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
	}
	else if ( nt->type() == TokenType::ttIdentifier )
	{
	    std::string tname = ((TokenIdent *)nt)->str;
	    datatype_map_iter tdmi = datatype_map.find(tname);
	    if ( tdmi == datatype_map.end() )
		Throw(nt) << "Unknown type '" << tname << "' in function pointer typedef" << flush;
	    param_dd = &tdmi->second->definition;
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

    // optional tag name: enum colors { ... }
    std::string enum_tag;
    if ( tn && tn->type() == TokenType::ttIdentifier )
    {
	enum_tag = ((TokenIdent *)tn)->str;
	pgm.nextToken(); // consume tag name
    }

    tn = pgm.peekToken();
    if ( !tn || tn->id() != TokenID::tkOpBrc )
    {
	// No '{' — this is a forward reference like `enum X var;`
	// Treat enum as int and let the caller parse the variable declaration
	if ( !enum_tag.empty() )
	{
	    pgm.pushToken(new TokenDataType("int", ddINT));
	    return NULL;
	}
	pgm.Throw(tn) << "Expecting '{' after enum" << flush;
    }
    pgm.nextToken(); // consume '{'

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

	// register as a global constant variable
	Variable *evar = pgm.addVariable(NULL, ddINT, name, 1, NULL, true);
	evar->set((int)val);
	evar->makeconstant();
	DBG(std::cout << "TokenENUM::parse() " << name << " = " << val << std::endl);
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
	if ( tn->type() == TokenType::ttKeyword )
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

// parse switch(expr) { case val: ...; break; default: ...; }
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

    // parse case/default blocks until }
    while ( (tn = pgm.nextToken()) )
    {
	if ( tn->id() == TokenID::tkClBrc )
	    break;
	if ( tn->id() == TokenID::tkCASE )
	{
	    TokenCASE *tc = new TokenCASE();
	    tc->file = tn->file;
	    tc->line = tn->line;
	    tc->column = tn->column;
	    // Parse case value as a constant integer expression. This accepts
	    // plain literals (`case 42:`, `case 'a':`), enum constants, and
	    // parenthesized / negated forms such as `case EOF:` where EOF
	    // expands to `(-1)`. The evaluated int64 is wrapped in a TokenInt
	    // for downstream compile().
	    TokenBase *val_anchor = pgm.peekToken();
	    int64_t case_val = parse_constant_integer_expression(pgm);
	    TokenInt *val_tok = new TokenInt(case_val);
	    if ( val_anchor )
	    {
		val_tok->file = val_anchor->file;
		val_tok->line = val_anchor->line;
		val_tok->column = val_anchor->column;
	    }
	    tc->value = val_tok;
	    // expect : after case value
	    tn = pgm.nextToken();
	    if ( tn->id() != TokenID::tkTerC )
		pgm.Throw(tn) << "Expecting : after case value" << flush;
	    // parse statements until next case/default/}
	    while ( pgm.peekToken() && pgm.peekToken()->id() != TokenID::tkCASE
		    && pgm.peekToken()->id() != TokenID::tkDEFAULT
		    && pgm.peekToken()->id() != TokenID::tkClBrc )
	    {
		TokenBase *stmt = pgm.parseStatement(pgm.nextToken());
		if ( stmt )
		    tc->statements.push_back(stmt);
	    }
	    cases.push_back(tc);
	}
	else if ( tn->id() == TokenID::tkDEFAULT )
	{
	    // expect : after default
	    tn = pgm.nextToken();
	    if ( tn->id() != TokenID::tkTerC )
		pgm.Throw(tn) << "Expecting : after default" << flush;
	    defaultcase = new TokenCASE();
	    defaultcase->value = NULL;
	    defaultcase->file = tn->file;
	    defaultcase->line = tn->line;
	    defaultcase->column = tn->column;
	    default_index = (int)cases.size();
	    // parse statements until next case/}
	    while ( pgm.peekToken() && pgm.peekToken()->id() != TokenID::tkCASE
		    && pgm.peekToken()->id() != TokenID::tkDEFAULT
		    && pgm.peekToken()->id() != TokenID::tkClBrc )
	    {
		TokenBase *stmt = pgm.parseStatement(pgm.nextToken());
		if ( stmt )
		    defaultcase->statements.push_back(stmt);
	    }
	}
	else if ( tn->type() == TokenType::ttDataType
	       || tn->type() == TokenType::ttIdentifier )
	{
	    // C allows variable declarations in a switch body before any
	    // case label — they're unreachable (no case path enters
	    // there) but valid as compile-time declarations. SMAUG's
	    // `switch(SPELL_POWER(skill)) { OBJ_DATA *clone; default: ... }`
	    // is a common form. Parse and discard.
	    DBG(std::cout << "TokenSWITCH::parse() skipping pre-case declaration" << std::endl);
	    pgm.parseStatement(tn);
	}
	else if ( tn->id() == TokenID::tkSemi )
	{
	    // Stray `;` between pre-case declarations and the first case
	    // label — also an empty statement at switch body scope.
	    continue;
	}
	else
	    pgm.Throw(tn) << "Expecting case or default in switch body" << flush;
    }

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

// parse vector<type> — creates DataDefVECTOR and delegates to parseDeclaration
TokenBase *TokenVECTOR::parse(Program &pgm)
{
    DBG(std::cout << "TokenVECTOR::parse()" << std::endl);
    TokenBase *tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkLT )
	pgm.Throw(tn) << "Expecting < after vector" << flush;

    tn = pgm.nextToken();
    if ( tn->type() != TokenType::ttDataType )
	pgm.Throw(tn) << "Expecting type inside vector<>" << flush;

    DataDef *elem = &((TokenDataType *)tn)->definition;

    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkGT )
	pgm.Throw(tn) << "Expecting > after vector<type" << flush;

    // build composite name and look up or create
    std::string tname = "vector<" + elem->name + ">";
    datatype_map_iter dmi = pgm.datatype_map.find(tname);
    TokenDataType *tdt;
    if ( dmi != pgm.datatype_map.end() )
    {
	tdt = dmi->second;
    }
    else
    {
	// use sizeof of the underlying vector type — all std::vector are same size
	DataDefVECTOR *dd = new DataDefVECTOR(elem, tname, sizeof(std::vector<int64_t>));
	tdt = new TokenDataType(tname.c_str(), *dd);
	pgm.datatype_map[tname] = tdt;

	// register methods on this parameterization
	Variable *mv;
	if ( elem->is_string() )
	{
	    mv = pgm.addFunction("push_back", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtVECTOR), DataType::dtSTRING}, (fVOIDFUNC)vector_str_push_back, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("pop_back", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtVECTOR)}, (fVOIDFUNC)vector_str_pop_back, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("at", datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, rtPtr(DataType::dtVECTOR), DataType::dtINT64}, (fVOIDFUNC)vector_str_at, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("size", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtVECTOR)}, (fVOIDFUNC)vector_str_size, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("clear", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtVECTOR)}, (fVOIDFUNC)vector_str_clear, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("empty", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtVECTOR)}, (fVOIDFUNC)vector_str_empty, true);
	    dd->methods.push_back(mv);
	}
	else
	{
	    mv = pgm.addFunction("push_back", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtVECTOR), DataType::dtINT64}, (fVOIDFUNC)vector_int_push_back, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("pop_back", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtVECTOR)}, (fVOIDFUNC)vector_int_pop_back, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("at", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtVECTOR), DataType::dtINT64}, (fVOIDFUNC)vector_int_at, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("size", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtVECTOR)}, (fVOIDFUNC)vector_int_size, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("clear", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtVECTOR)}, (fVOIDFUNC)vector_int_clear, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("empty", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtVECTOR)}, (fVOIDFUNC)vector_int_empty, true);
	    dd->methods.push_back(mv);
	}
    }

    return pgm.parseDeclaration(tdt);
}

// parse map<key_type, val_type>
TokenBase *TokenMAP::parse(Program &pgm)
{
    DBG(std::cout << "TokenMAP::parse()" << std::endl);
    TokenBase *tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkLT )
	pgm.Throw(tn) << "Expecting < after map" << flush;

    tn = pgm.nextToken();
    if ( tn->type() != TokenType::ttDataType )
	pgm.Throw(tn) << "Expecting key type inside map<>" << flush;
    DataDef *key = &((TokenDataType *)tn)->definition;

    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkComma )
	pgm.Throw(tn) << "Expecting , between key and value types in map<k, v>" << flush;

    tn = pgm.nextToken();
    if ( tn->type() != TokenType::ttDataType )
	pgm.Throw(tn) << "Expecting value type inside map<k, v>" << flush;
    DataDef *val = &((TokenDataType *)tn)->definition;

    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkGT )
	pgm.Throw(tn) << "Expecting > after map<k, v" << flush;

    std::string tname = "map<" + key->name + "," + val->name + ">";
    datatype_map_iter dmi = pgm.datatype_map.find(tname);
    TokenDataType *tdt;
    if ( dmi != pgm.datatype_map.end() )
    {
	tdt = dmi->second;
    }
    else
    {
	DataDefMAP *dd = new DataDefMAP(key, val, tname, sizeof(std::map<std::string, int64_t>));
	tdt = new TokenDataType(tname.c_str(), *dd);
	pgm.datatype_map[tname] = tdt;

	// register methods
	Variable *mv;
	if ( val->is_string() )
	{
	    mv = pgm.addFunction("put", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtMAP), DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)map_str_str_set, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("get", datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, rtPtr(DataType::dtMAP), DataType::dtSTRING}, (fVOIDFUNC)map_str_str_get, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("contains", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtMAP), DataType::dtSTRING}, (fVOIDFUNC)map_str_str_contains, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("size", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtMAP)}, (fVOIDFUNC)map_str_str_size, true);
	    dd->methods.push_back(mv);
	}
	else
	{
	    mv = pgm.addFunction("put", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtMAP), DataType::dtSTRING, DataType::dtINT64}, (fVOIDFUNC)map_str_int_set, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("get", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtMAP), DataType::dtSTRING}, (fVOIDFUNC)map_str_int_get, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("contains", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtMAP), DataType::dtSTRING}, (fVOIDFUNC)map_str_int_contains, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("erase", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtMAP), DataType::dtSTRING}, (fVOIDFUNC)map_str_int_erase, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("size", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtMAP)}, (fVOIDFUNC)map_str_int_size, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("clear", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtMAP)}, (fVOIDFUNC)map_str_int_clear, true);
	    dd->methods.push_back(mv);
	}
    }

    return pgm.parseDeclaration(tdt);
}

// parse set<type>
TokenBase *TokenSET::parse(Program &pgm)
{
    DBG(std::cout << "TokenSET::parse()" << std::endl);
    TokenBase *tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkLT )
	pgm.Throw(tn) << "Expecting < after set" << flush;

    tn = pgm.nextToken();
    if ( tn->type() != TokenType::ttDataType )
	pgm.Throw(tn) << "Expecting type inside set<>" << flush;
    DataDef *elem = &((TokenDataType *)tn)->definition;

    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkGT )
	pgm.Throw(tn) << "Expecting > after set<type" << flush;

    std::string tname = "set<" + elem->name + ">";
    datatype_map_iter dmi = pgm.datatype_map.find(tname);
    TokenDataType *tdt;
    if ( dmi != pgm.datatype_map.end() )
    {
	tdt = dmi->second;
    }
    else
    {
	DataDefSET *dd = new DataDefSET(elem, tname, sizeof(std::set<std::string>));
	tdt = new TokenDataType(tname.c_str(), *dd);
	pgm.datatype_map[tname] = tdt;

	Variable *mv;
	if ( elem->is_string() )
	{
	    mv = pgm.addFunction("insert", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtSET), DataType::dtSTRING}, (fVOIDFUNC)set_str_insert, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("contains", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtSET), DataType::dtSTRING}, (fVOIDFUNC)set_str_contains, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("erase", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtSET), DataType::dtSTRING}, (fVOIDFUNC)set_str_erase, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("size", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtSET)}, (fVOIDFUNC)set_str_size, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("clear", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtSET)}, (fVOIDFUNC)set_str_clear, true);
	    dd->methods.push_back(mv);
	}
	else
	{
	    mv = pgm.addFunction("insert", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtSET), DataType::dtINT64}, (fVOIDFUNC)set_int_insert, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("contains", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtSET), DataType::dtINT64}, (fVOIDFUNC)set_int_contains, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("size", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtSET)}, (fVOIDFUNC)set_int_size, true);
	    dd->methods.push_back(mv);
	}
    }

    return pgm.parseDeclaration(tdt);
}

// parse list<type>
TokenBase *TokenLIST::parse(Program &pgm)
{
    DBG(std::cout << "TokenLIST::parse()" << std::endl);
    TokenBase *tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkLT )
	pgm.Throw(tn) << "Expecting < after list" << flush;

    tn = pgm.nextToken();
    if ( tn->type() != TokenType::ttDataType )
	pgm.Throw(tn) << "Expecting type inside list<>" << flush;
    DataDef *elem = &((TokenDataType *)tn)->definition;

    tn = pgm.nextToken();
    if ( tn->id() != TokenID::tkGT )
	pgm.Throw(tn) << "Expecting > after list<type" << flush;

    std::string tname = "list<" + elem->name + ">";
    datatype_map_iter dmi = pgm.datatype_map.find(tname);
    TokenDataType *tdt;
    if ( dmi != pgm.datatype_map.end() )
    {
	tdt = dmi->second;
    }
    else
    {
	DataDefLIST *dd = new DataDefLIST(elem, tname, sizeof(std::list<int64_t>));
	tdt = new TokenDataType(tname.c_str(), *dd);
	pgm.datatype_map[tname] = tdt;

	Variable *mv;
	if ( elem->is_string() )
	{
	    mv = pgm.addFunction("push_back", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtLIST), DataType::dtSTRING}, (fVOIDFUNC)list_str_push_back, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("push_front", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtLIST), DataType::dtSTRING}, (fVOIDFUNC)list_str_push_front, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("size", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtLIST)}, (fVOIDFUNC)list_str_size, true);
	    dd->methods.push_back(mv);
	}
	else
	{
	    mv = pgm.addFunction("push_back", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtLIST), DataType::dtINT64}, (fVOIDFUNC)list_int_push_back, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("push_front", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtLIST), DataType::dtINT64}, (fVOIDFUNC)list_int_push_front, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("size", datatype_vec_t{DataType::dtINT64, rtPtr(DataType::dtLIST)}, (fVOIDFUNC)list_int_size, true);
	    dd->methods.push_back(mv);
	    mv = pgm.addFunction("clear", datatype_vec_t{DataType::dtVOID, rtPtr(DataType::dtLIST)}, (fVOIDFUNC)list_int_clear, true);
	    dd->methods.push_back(mv);
	}
    }

    return pgm.parseDeclaration(tdt);
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
	    popCompound();
	    DBG(std::cout << "parseCompound() ends" << std::endl);
	    return code;
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
			    std::vector<DataDef *> *multi_ret)
{
    variable_map_iter vmi;
    funcdef_map_iter fmi;
    datadef_vec_iter dvi;
    FuncDef *func;
    TokenBase *nt = NULL; // next token;
    Variable *var;

    vector<std::string> ids;  // vector of variable names
    TokenDataType *pb;        // parameter basetype
    std::string pid;          // parameter id
    RefType rtype = RefType::rtNone;
    int anon_param_index = 0;
    bool old_style_params = false;
    std::vector<std::string> old_style_ids;
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
	    fresh->no_instrument_function = func->no_instrument_function;
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
	    fresh->no_instrument_function = func->no_instrument_function;
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

    // for multi-return functions, inject hidden __retbuf parameter as first arg
    if ( multi_ret && multi_ret->size() > 1 )
    {
	func->return_types = *multi_ret;
	if ( !func_already_declared )
	    func->parameters.push_back(&ddINT64); // void* as int64
	ids.push_back("__retbuf");
	DBG(cout << "parseFunction() injected hidden __retbuf for multi-return (" << multi_ret->size() << " types)" << endl);
    }

    // for class methods, inject hidden __this parameter as first arg
    if ( owner_class )
    {
	if ( !func_already_declared )
	    func->parameters.push_back(&ddINT64); // void* as int64
	ids.push_back("__this");
	DBG(cout << "parseFunction() injected hidden __this parameter for class method" << endl);
    }

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
	while ( nt && (nt->id() == TokenID::tkCONST || nt->id() == TokenID::tkREGISTER) )
	{
	    nt = nextToken();
	}
	std::vector<uint32_t> param_array_dims;

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
		func->parameters.push_back(&ddINT64);
	    ids.push_back("__va_args");
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
	    param_dd = getPointerType(param_dd);
	    DBG(std::cout << "parseFunction() pointer param: " << param_dd->name << std::endl);
	    goto grabnt;
	}
	if ( is_restrict_token(nt) || nt->id() == TokenID::tkCONST )
	    goto grabnt;
	if ( nt->id() == TokenID::tkComma || nt->id() == TokenID::tkClBrk )
	{
	    pid = "__anon_param_" + std::to_string(anon_param_index++);
	    goto paramdecl;
	}
	if ( nt->id() == TokenID::tkOpBrk )
	{
	    TokenBase *inner = nextToken();
	    if ( inner && inner->id() == TokenID::tkStar )
	    {
		nt = nextToken();
		while ( nt && (is_restrict_token(nt) || nt->id() == TokenID::tkCONST) )
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
		    // Pointer-to-array parameter: `int (*a)[2]`. Approximate it
		    // as `int *a` — the passed address is the same base address,
		    // and existing `(*a)[i]` parsing already lowers compatibly.
		    param_dd = getPointerType(param_dd);
		    rtype = RefType::rtPointer;
		    while ( nt && nt->id() == TokenID::tkOpSqr )
		    {
			int sq_depth = 1;
			while ( sq_depth > 0 )
			{
			    nt = nextToken();
			    if ( !nt )
				Throw(inner) << "Unexpected end of input in array-pointer parameter" << flush;
			    if ( nt->id() == TokenID::tkOpSqr )
				++sq_depth;
			    else if ( nt->id() == TokenID::tkClSqr )
				--sq_depth;
			}
			nt = nextToken();
		    }
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
	    }
	    else
	    {
		int64_t n = parse_constant_integer_expression(*this);
		if ( n < 0 )
		    Throw(nt) << "Parameter array dimension must be non-negative" << flush;
		param_array_dims.push_back((uint32_t)n);
		TokenBase *cl = nextToken();
		if ( !cl || cl->id() != TokenID::tkClSqr )
		    Throw(cl ? cl : nt) << "Expected ']' in parameter array declarator" << flush;
	    }
	    nt = nextToken();
	}
	if ( !param_array_dims.empty() )
	{
	    DataDef *array_elem = param_dd;
	    for ( size_t i = param_array_dims.size(); i-- > 1; )
		array_elem = new DataDefCArray(*array_elem, array_elem->name, param_array_dims[i], NULL);
	    param_dd = getPointerType(array_elem);
	    rtype = RefType::rtPointer;
	}

paramdecl:
	// parameter declaration
	if ( nt->id() == TokenID::tkComma || nt->id() == TokenID::tkClBrk )
	{
	    // If this is a definition following a forward declaration, the
	    // function already has its parameter DataDefs — don't re-push.
	    if ( func_already_declared )
	    {
		ids.push_back(pid);
	    }
	    else if ( !func->findParameter(pid) )
	    {
		ids.push_back(pid);
		if ( rtype == RefType::rtReference && pb->definition.rawtype() == DataType::dtSTRING )
		    func->parameters.push_back(&ddSTRINGref);
		else if ( rtype == RefType::rtPointer )
		    func->parameters.push_back(param_dd);
		else if ( dynamic_cast<DataDefFPTR *>(param_dd) != NULL )
		    func->parameters.push_back(param_dd);
		else
		    func->parameters.push_back(&pb->definition);
		DBG(std::cout << "Added new parameter declaration type: " << dd.name << " size: "
		    << dd.size << " name: " << pid << " ptr: " << &dd << std::endl);
	    }
	    else
	    {
		DBG(std::cerr << "parseFunction() params: duplicate parameter name " << pid << std::endl);
		Throw(nt) << "Duplicate parameter name" << flush;
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
		func->parameters.push_back(pti == old_style_param_types.end()
		    ? &ddINT32
		    : pti->second);
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

    // Semicolon ends a function declaration; comma continues another
    // function declarator with the same return type: `void a(), b();`.
    if ( nt->id() == TokenID::tkSemi || nt->id() == TokenID::tkComma )
    {
	if ( !method )
	{
	    method = new Method(*var);
	    var->data = (void *)method;
	}
	if ( owner_class )
	    method->owner_class = owner_class;
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
	    parseFunction(*next_return, next_id, owner_class, multi_ret);
	}
	return;
    }

    std::set<std::string> func_attrs;
    nt = consume_gnu_attributes(*this, nt, &func_attrs);
    if ( func_attrs.count("no_instrument_function")
      || func_attrs.count("__no_instrument_function__") )
	func->no_instrument_function = true;
    // Check again for forward declaration after __attribute__
    if ( nt->id() == TokenID::tkSemi )
    {
	method = new Method(*var);
	var->data = (void *)method;
	if ( owner_class )
	    method->owner_class = owner_class;
	return;
    }

    // Definitions must own a fresh Method instance. Some prior declaration
    // paths (notably SMAUG macro expansions) leave a non-null var->data that
    // is not a valid Method object, so reusing it corrupts method->parameters.
    if ( is_nested_function )
	configure_nested_function_captures(*this, func);

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

    if ( is_nested_function )
    {
	std::string env_name = "__env";
	Variable *env_pv = new Variable(env_name, ddINT64, 1, NULL, false);
	env_pv->flags |= vfPARAM | vfLOCAL;
	method->env_param = env_pv;
	method->parameters.push_back(env_pv);
    }

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
	if ( is_nested_function && i == 0 )
	    continue;
	std::string pname = (size_t)user_param_index < ids.size()
	    ? ids[user_param_index]
	    : std::string("__synthetic_p") + std::to_string(user_param_index);
	DBG(cout << "parseFunction() adding parameter variable " << pname << endl);
	v = new Variable(pname, *d, 1, NULL, false);
	v->flags |= vfPARAM | vfLOCAL;
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
    DBG(cout << "parseFunction() calling parseCompound()" << endl);
    TokenCpnd *tc = dynamic_cast<TokenCpnd *>(parseCompound());

    tf->method = method;
    tf->parent = tc->parent;
    tf->variables = tc->variables;
    tf->statements = tc->statements;
    tf->deferred = tc->deferred;
    // Walk statements to find one with real file/line info — the first
    // statement isn't always a body statement (parser sometimes hangs
    // initializer-shaped tokens off the front whose file pointer
    // belongs to the enclosing init context, not the function body).
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

    DBG(cout << "parseFunction() calling ast.push" << endl);
    ast.push(tf);
    pending_funcs.push_back(tf);

    DBG(cout << "parseFunction(" << id << ") END" << endl);
}

// parse a lambda expression: [](type arg, ...) { body }
// Returns a TokenVar referencing the lambda's anonymous function variable.
// The lambda is pushed onto ast as a top-level TokenFunc so it compiles
// before the enclosing function (asmjit can't nest addFunc/endFunc).
TokenBase *Program::parseLambda()
{
    static int lambda_counter = 0;

    DBG(cout << "parseLambda() START" << endl);

    // we already consumed '[', peek at next token
    // [](params) { body }       — pure lambda (no capture)
    // [int](params) { body }    — pure lambda with return type
    // [&](params) { body }      — capture all outer vars by reference
    TokenBase *tn = nextToken();
    DataDef *rettype = &ddVOID;
    bool is_capturing = false;

    // check for [&] capture syntax
    if ( tn->id() == TokenID::tkBand )
    {
	is_capturing = true;
	tn = nextToken(); // consume &, expect ]
    }
    else if ( tn->type() == TokenType::ttDataType )
    {
	rettype = &((TokenDataType *)tn)->definition;
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
	// Pre-register env as first parameter in FuncDef (user params appended after)
	func->parameters.push_back(&ddINT64);
	DBG(cout << "parseLambda() [&] capturing " << func->potential_captures.size() << " outer vars" << endl);
    }

    // parse parameters (same pattern as parseFunction)
    std::vector<std::string> param_ids;
    TokenDataType *pb;

    while ( (tn=nextToken()) && tn->id() != TokenID::tkClBrk )
    {
	if ( tn->type() != TokenType::ttDataType )
	    Throw(tn) << "Expecting type in lambda parameter list" << flush;

	pb = (TokenDataType *)tn;
	tn = nextToken();

	if ( !is_contextual_identifier_token(tn) )
	    Throw(tn) << "Expecting identifier in lambda parameter list" << flush;

	std::string pid = contextual_identifier_name(tn);
	param_ids.push_back(pid);
	func->parameters.push_back(&pb->definition);

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

    // if capturing: create hidden env_param at position 0 in method->parameters
    if ( is_capturing )
    {
	std::string env_name = "__env";
	Variable *env_pv = new Variable(env_name, ddINT64, 1, NULL, false);
	env_pv->flags |= vfPARAM | vfLOCAL;
	method->env_param = env_pv;
	method->parameters.push_back(env_pv); // will be moved to front below
    }

    // add user parameters to method
    for ( size_t i = 0; i < param_ids.size(); ++i )
    {
	// user params start at index 1 in func->parameters when capturing (0 is env)
	size_t fi = is_capturing ? i + 1 : i;
	Variable *pv = new Variable(param_ids[i], *func->parameters[fi], 1, NULL, false);
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

    if ( func->parameters.empty() || func->parameters.front() != &ddINT64 )
	func->parameters.insert(func->parameters.begin(), &ddINT64);
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
    return dd && (dd->rawtype() == DataType::dtCHAR
	       || dd->rawtype() == DataType::dtUINT8);
}

static TokenStructLit *string_literal_to_char_init(TokenStr *strtok, bool include_null)
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

// parse either a variable declaration, or a function declaration
TokenBase *Program::parseDeclaration(TokenDataType *tb, bool is_static)
{
    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
    TokenBase *nt; // next token;
    Variable *var;
    string id;
    std::vector<uint32_t> arr_dims;
    TokenBase *vla_size_expr = NULL;
    bool have_decl_id = false;
    Variable *provisional_decl_var = NULL;
    bool gotstatic = is_static || parsing_static_decl;
    // The flag covers exactly this declaration. Clear so nested declarations
    // (e.g. locals inside a `static void f() { string s = ...; }` body)
    // don't inherit static storage.
    parsing_static_decl = false;

    DBG(std::cout << "parseDeclaration(" << tb->str << ") START" << std::endl);

    // check for pointer declarator(s): type * [*...] identifier.
    // base_type is the declared type without any `*`s — comma-continuations
    // later in this function start fresh from base_type because each var
    // in `char *p, *q;` has its own `*`s, not cumulative.
    DataDef *base_type = &tb->definition;
    DataDef *decl_type = base_type;
    bool saw_pointer_decl = false;
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
	    if ( !is_fnptr_base )
		decl_type = getPointerType(decl_type);
	    DBG(std::cout << "parseDeclaration() pointer: " << decl_type->name << std::endl);
	}
	else
	    nextToken(); // consume const/restrict
    }

    if ( !saw_pointer_decl )
    {
	if ( DataDefCArray *alias_array = dynamic_cast<DataDefCArray *>(base_type) )
	{
	    decl_type = alias_array->element_type ? alias_array->element_type : &ddINT;
	    if ( alias_array->count_expr )
		vla_size_expr = alias_array->count_expr;
	    else
		arr_dims.push_back((uint32_t)alias_array->count);
	}
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
	    while ( name_tok && (is_restrict_token(name_tok) || name_tok->id() == TokenID::tkCONST) )
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
	    TokenBase *param_open = nextToken();
	    if ( !param_open || param_open->id() != TokenID::tkOpBrk )
		Throw(param_open ? param_open : open) << "Expecting '(' for function pointer parameter list" << flush;

	    FuncDef *func = parseFnPtrParams(*decl_type);
	    decl_type = new DataDefFPTR(func);
	    have_decl_id = true;
	    nt = peekToken();
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

	if ( rhs_tok && rhs_tok->id() == TokenID::tkOpSqr )
	{
	    // lambda: auto fn = [](params) { body };
	    nextToken(); // consume '['
	    rhs_node = parseLambda();
	    rhs_var = &(dynamic_cast<TokenVar *>(rhs_node)->var);
	}
	else if ( rhs_tok && rhs_tok->type() == TokenType::ttIdentifier )
	{
	    // named function: auto fn = func_name;
	    nextToken(); // consume identifier
	    rhs_var = findVariable(((TokenIdent *)rhs_tok)->str);
	    if ( !rhs_var || !rhs_var->type->is_function() )
		Throw(tb) << "'auto' type deduction requires a function name or lambda" << flush;
	    rhs_node = new TokenVar(*rhs_var);
	}
	else
	{
	    Throw(tb) << "'auto' type deduction requires a function name or lambda" << flush;
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
	}
	else
	{
	    // Scan ahead to the matching `]` to detect any non-constant
	    // identifier — that makes the dim a runtime expression (VLA).
	    // Constants (enum values, vfCONSTANT vars, typedef'd integer
	    // constants) stay on the parse_constant_integer_expression path
	    // because resolve_integer_constant handles them.
	    bool is_vla = false;
	    int depth = 1;
	    for ( auto it = tokens.begin(); it != tokens.end() && depth > 0; ++it )
	    {
		TokenBase *t = *it;
		if ( t->id() == TokenID::tkOpSqr ) { ++depth; continue; }
		if ( t->id() == TokenID::tkClSqr ) { --depth; continue; }
		if ( t->id() == TokenID::tkSemi || t->id() == TokenID::tkOpBrc ) break;
		std::string name;
		if ( t->type() == TokenType::ttIdentifier )
		    name = ((TokenIdent *)t)->str;
		else
		    continue;
		Variable *v = findVariable(name);
		// No matching variable → assume it'll resolve via some
		// other route (typedef, enum, lookup retry). NOT a VLA marker.
		if ( !v )
		    continue;
		// Constant variable → still a fixed dim.
		if ( v->is_constant() )
		    continue;
		is_vla = true;
		break;
	    }
	    if ( is_vla && arr_dims.empty() && !vla_size_expr )
	    {
		// First-dim VLA: capture the runtime expression.
		vla_size_expr = parseExpression(nextToken(), true);
		TokenBase *cl = nextToken();
		if ( !cl || cl->id() != TokenID::tkClSqr )
		    Throw(cl ? cl : tb) << "Expected ] after VLA size expression" << flush;
		arr_dims.push_back(1); // sentinel; real count is runtime
	    }
	    else
	    {
		int64_t n = parse_constant_integer_expression(*this);
		if ( n < 0 )
		    Throw(tb) << "Fixed-size array dimension must be non-negative" << flush;
		// GCC: int arr[0] has sizeof 0; keep the zero dim.
		arr_dims.push_back((uint32_t)n);
		TokenBase *cl = nextToken();
		if ( !cl || cl->id() != TokenID::tkClSqr )
		    Throw(cl ? cl : tb) << "Expected ] in array declaration" << flush;
	    }
	}
	nt = peekToken();
	if ( !nt )
	    Throw(tb) << "Unexpected end of data in array declaration" << flush;
    }

    // Skip __attribute__((...)) after variable declarator (GCC extension)
    if ( is_attribute_identifier_token(nt) )
    {
	nextToken(); // consume __attribute__
	if ( peekToken() && peekToken()->id() == TokenID::tkOpBrk )
	{
	    int adepth = 0;
	    do {
		TokenBase *at = nextToken();
		if ( !at ) break;
		if ( at->id() == TokenID::tkOpBrk ) ++adepth;
		else if ( at->id() == TokenID::tkClBrk ) --adepth;
	    } while ( adepth > 0 );
	}
	nt = peekToken();
    }

    // Preserve pointer semantics for `char *p = "literal";`.
    // Only real array declarators (`char buf[] = "literal";`) should take the
    // char-array string-initializer path below.

    // variable declaration
    if ( nt->id() == TokenID::tkSemi || nt->id() == TokenID::tkAssign
      || nt->id() == TokenID::tkComma )
    {
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
	if ( nt->id() == TokenID::tkAssign && arr_dims.empty() && !provisional_decl_var )
	{
	    bool alloc = parsing_extern_decl ? false : ((!code || gotstatic) ? true : false);
	    provisional_decl_var = addVariable(code, *decl_type, id, 1, NULL, alloc);
	    if ( gotstatic )
		provisional_decl_var->flags |= vfSTATIC;
	    if ( parsing_extern_decl )
		provisional_decl_var->flags |= vfEXTERN;
	}
	if ( nt->id() == TokenID::tkAssign && (!arr_dims.empty() || is_struct_init || is_simd_init) )
	{
	    // peek past '=' to see if we have { (brace list) or "..." (string lit for char arr)
	    nextToken(); // consume '='
	    TokenBase *peek0 = peekToken();
	    if ( !peek0 )
		Throw(nt) << "Expected initializer after '='" << flush;

	    // String-literal byte-array init:
	    // char / signed char / unsigned char buf[] = "hello";
	    if ( !arr_dims.empty()
	      && peek0->type() == TokenType::ttString
	      && (decl_type->rawtype() == DataType::dtCHAR
	       || decl_type->rawtype() == DataType::dtUINT8)
	      && arr_dims.size() == 1 )
	    {
		// C concatenates adjacent string literals, so consume all
		// immediately consecutive ttString tokens here.
		while ( peekToken() && peekToken()->type() == TokenType::ttString )
		{
		    TokenBase *strtok = nextToken();
		    const std::string &s = ((TokenStr *)strtok)->str;
		    for ( char c : s )
			init_list.push_back(new TokenInt((int64_t)(unsigned char)c));
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
	    std::function<TokenStructLit *(void)> read_struct_lit;
	    read_struct_lit = [&]() -> TokenStructLit * {
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
			slit->inits.push_back(read_struct_lit());
		    else
		    {
			TokenBase *ni = nextToken();
			// Skip designator `.field =` in nested init
			if ( ni->id() == TokenID::tkDot )
			{
			    nextToken(); // field name
			    nextToken(); // '='
			    ni = nextToken();
			}
			if ( ni->id() == TokenID::tkOpBrc )
			{
			    pushToken(ni);
			    slit->inits.push_back(read_struct_lit());
			}
			else
			    slit->inits.push_back(parseExpression(ni));
		    }
		    TokenBase *isep = peekToken();
		    if ( isep && isep->id() == TokenID::tkComma )
			nextToken();
		}
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
		    init_list.push_back(read_struct_lit());
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
				(*target_inits)[field_index] = read_struct_lit();
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
			int sqd = 1;
			while ( sqd > 0 ) {
			    TokenBase *t = nextToken();
			    if ( !t ) break;
			    if ( t->id() == TokenID::tkOpSqr ) ++sqd;
			    else if ( t->id() == TokenID::tkClSqr ) --sqd;
			}
			nextToken(); // consume '='
			next_init = nextToken();
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
			    TokenStructLit *slit = string_literal_to_char_init((TokenStr *)next_init, false);
			    while ( peekToken() && peekToken()->type() == TokenType::ttString )
				append_string_literal_chars(slit, (TokenStr *)nextToken());
			    size_t member_count = sdd->member_counts[field_index];
			    if ( member_count == 0 || member_count > slit->inits.size() )
				slit->inits.push_back(new TokenInt(0));
			    init_list.push_back(slit);
			    continue;
			}
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
		if ( arr_dims[0] == 0 )
		    arr_dims[0] = (uint32_t)init_list.size();
		if ( init_list.size() > (size_t)arr_dims[0] )
		    Throw(tb) << "Too many initializers for array (expected " << arr_dims[0] << ")" << flush;
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
	if ( provisional_decl_var && arr_dims.empty() && elem_count == 1 )
	{
	    var = provisional_decl_var;
	    var->type = decl_type;
	    var->count = elem_count;
	}
	else
	    var = addVariable(code, *decl_type, id, elem_count, NULL, alloc);
	bool shared_global_extern_ref =
	    is_shared_global_extern_reference(*this, code, var);
	if ( gotstatic )
	    var->flags |= vfSTATIC;
	if ( parsing_extern_decl && !shared_global_extern_ref )
	    var->flags |= vfEXTERN;
	else
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
		var->type = getPointerType(decl_type);
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

	td->file = tb->file;
	td->line = tb->line;
	td->column = tb->column;
	td->init_list = init_list;

	if ( gotstatic && code && !td->init_list.empty()
	  && initialize_static_fixed_array_data(var, td->init_list) )
	    td->init_list.clear();

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
	    // Function-scope static: hoist the initializer to program-startup
	    // so it runs once, before main() — C semantics. Without this the
	    // existing skip in TokenDecl::compile (vfSTATIC + tkFunction !=
	    // tkProgram) drops the initializer entirely; emitting it inline
	    // in the function body instead would re-init on every call,
	    // breaking patterns like `static int counter = 5; counter++;`.
	    // The Variable's storage is already heap-allocated (vfALLOC set
	    // by addVariable for static), so the assign at program scope
	    // writes the constant directly into that storage.
	    if ( gotstatic && code && td->initialize && tkProgram )
	    {
		tkProgram->statements.push_back((TokenStmt *)td->initialize);
		td->initialize = NULL;
	    }
	}

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

    parseFunction(*decl_type, id);

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
    return expr;
}

TokenBase *Program::parseStatement(TokenBase *tb)
{
    DBG(cout << "parseStatement() start" << endl);
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
		return new TokenLabel(lname);
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
		    if ( peekToken() && peekToken()->type() == TokenType::ttIdentifier )
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
				TokenBase *out_tb = nextToken();
				TokenBase *out_expr = out_tb ? parseExpression(out_tb, true) : NULL;
				TokenBase *out_cb = nextToken();
				TokenBase *in_colon = nextToken();
				TokenBase *in_c = nextToken();
				TokenBase *in_ob = nextToken();
				if ( out_expr
				  && out_cb && out_cb->id() == TokenID::tkClBrk
				  && in_colon && in_colon->id() == TokenID::tkColon
				  && in_c && in_c->type() == TokenType::ttString
				  && in_ob && in_ob->id() == TokenID::tkOpBrk )
				{
				    TokenBase *in_tb = nextToken();
				    TokenBase *in_expr = in_tb ? parseExpression(in_tb, true) : NULL;
				    TokenBase *in_cb = nextToken();
				    TokenBase *close = nextToken();
				    std::string out_constraint = ((TokenStr *)out_c)->str;
				    std::string in_constraint = ((TokenStr *)in_c)->str;
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
		    return parseDeclaration(dmi->second);
		}
	    }
	    // namespace resolution: set current namespace and re-enter parseStatement
	    if ( peekToken() && peekToken()->id() == TokenID::tkNS )
	    {
		std::string ns_name = ((TokenIdent *)tb)->str;
		namespace_map_t::iterator nsi = namespace_map.find(ns_name);
		if ( nsi != namespace_map.end() )
		{
		    nextToken(); // consume ::
		    // rust::match — namespaced statement form, dispatched
		    // here because `match` must remain a usable identifier
		    // outside this context. The `::` has been consumed; if
		    // the next token isn't `match`, fall through to the
		    // normal namespace re-entry path below.
		    if ( ns_name == "rust" )
		    {
			TokenBase *peeked = peekToken();
			if ( peeked
			  && peeked->type() == TokenType::ttIdentifier
			  && ((TokenIdent *)peeked)->str == "match" )
			{
			    nextToken(); // consume "match"
			    TokenMatch *tm = new TokenMatch();
			    tm->file = tb->file;
			    tm->line = tb->line;
			    tm->column = tb->column;
			    return tm->parse(*this);
			}
		    }
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
	    // Container-type keywords (map, vector, set, list) are only types when
	    // followed by '<'. Otherwise the user is using them as an identifier —
	    // e.g. `MAP_DATA *map;` followed by `map->vnum = …;` — so route through
	    // parseExpression which already accepts them as contextual identifiers.
	    if ( (tb->id() == TokenID::tkMAP || tb->id() == TokenID::tkVECTOR
		|| tb->id() == TokenID::tkSET || tb->id() == TokenID::tkLIST)
		&& peekToken() && peekToken()->id() != TokenID::tkLT )
	    {
		DBG(std::cout << "parseStatement() container keyword used as identifier: "
		    << ((TokenKeyword *)tb)->str << std::endl);
		resetPrevToken();
		return parseExpression(tb);
	    }
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
	    if ( (tb->id() == TokenID::tkTRY
	       || tb->id() == TokenID::tkCATCH
	       || tb->id() == TokenID::tkTHROW)
	      && peekToken()
	      && peekToken()->id() != TokenID::tkOpBrc
	      && peekToken()->id() != TokenID::tkOpBrk )
	    {
		DBG(std::cout << "parseStatement() '"
		    << ((TokenKeyword *)tb)->str << "' used as identifier" << std::endl);
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

    _parser_init();

    DBG(cout << endl << "Program::parse() calling ast.push for TokenProgram" << endl);
    ast.push(tp);

    try
    {
	while ( !tokens.empty() )
	{
	    tb = nextToken();
//	    printt(tb);
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
