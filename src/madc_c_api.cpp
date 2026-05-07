#include "madc_api.h"

#include "libmadc/engine.h"
#include "libmadc/program.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

struct madc_program_opaque
{
    madc::program program;
    std::string last_error_text;

    madc_program_opaque() {}
    explicit madc_program_opaque(madc::engine &eng) : program(eng) {}
};

struct madc_engine_opaque
{
    madc::engine engine;
};

namespace {

void clear_c_value(madc_value *value)
{
    if ( value == NULL )
	return;
    if ( value->string_value != NULL )
	std::free(value->string_value);
    value->kind = MADC_VALUE_NULL;
    value->boolean_value = 0;
    value->integer_value = 0;
    value->real_value = 0.0;
    value->string_value = NULL;
    value->string_length = 0;
}

bool set_c_string(madc_value *value, const char *data, size_t length)
{
    if ( value == NULL )
	return false;
    clear_c_value(value);
    char *copy = static_cast<char *>(std::malloc(length + 1));
    if ( copy == NULL )
	return false;
    if ( length > 0 && data != NULL )
	std::memcpy(copy, data, length);
    copy[length] = '\0';
    value->kind = MADC_VALUE_STRING;
    value->string_value = copy;
    value->string_length = length;
    return true;
}

bool duplicate_c_string(char **dst, size_t *dst_len, const std::string &src)
{
    if ( dst == NULL || dst_len == NULL )
	return false;
    *dst = NULL;
    *dst_len = 0;
    char *copy = static_cast<char *>(std::malloc(src.size() + 1));
    if ( copy == NULL )
	return false;
    if ( !src.empty() )
	std::memcpy(copy, src.data(), src.size());
    copy[src.size()] = '\0';
    *dst = copy;
    *dst_len = src.size();
    return true;
}

bool from_cpp_value(const madc::value &src, madc_value *dst)
{
    if ( dst == NULL )
	return true;

    switch ( src.type() )
    {
	case madc::value::kind::null:
	    clear_c_value(dst);
	    return true;
	case madc::value::kind::boolean:
	    clear_c_value(dst);
	    dst->kind = MADC_VALUE_BOOLEAN;
	    dst->boolean_value = src.as_boolean() ? 1 : 0;
	    return true;
	case madc::value::kind::integer:
	    clear_c_value(dst);
	    dst->kind = MADC_VALUE_INTEGER;
	    dst->integer_value = src.as_integer();
	    return true;
	case madc::value::kind::real:
	    clear_c_value(dst);
	    dst->kind = MADC_VALUE_REAL;
	    dst->real_value = src.as_real();
	    return true;
	case madc::value::kind::string:
	{
	    const std::string &s = src.as_string();
	    return set_c_string(dst, s.data(), s.size());
	}
	default:
	    return false;
    }
}

bool to_cpp_value(const madc_value &src, madc::value &dst)
{
    switch ( src.kind )
    {
	case MADC_VALUE_NULL:
	    dst = madc::value();
	    return true;
	case MADC_VALUE_BOOLEAN:
	    dst = madc::value(src.boolean_value != 0);
	    return true;
	case MADC_VALUE_INTEGER:
	    dst = madc::value(src.integer_value);
	    return true;
	case MADC_VALUE_REAL:
	    dst = madc::value(src.real_value);
	    return true;
	case MADC_VALUE_STRING:
	{
	    const char *data = src.string_value ? src.string_value : "";
	    dst = madc::value(std::string(data, src.string_length));
	    return true;
	}
	default:
	    return false;
    }
}

void sync_last_error(madc_program *program, const std::string &fallback = std::string())
{
    if ( program == NULL )
	return;
    program->last_error_text.clear();
    if ( program->program.has_error() && program->program.last_error() != NULL )
	program->last_error_text = program->program.last_error()->to_string();
    else if ( !fallback.empty() )
	program->last_error_text = fallback;
}

int return_failure(madc_program *program, const std::string &fallback = std::string())
{
    sync_last_error(program, fallback);
    return MADC_ERROR;
}

madc_authority_mode to_c_authority_mode(madc::authority_mode mode)
{
    switch ( mode )
    {
	case madc::authority_mode::system_locked: return MADC_AUTHORITY_SYSTEM_LOCKED;
	case madc::authority_mode::user_controlled: return MADC_AUTHORITY_USER_CONTROLLED;
	case madc::authority_mode::host_authoritative: return MADC_AUTHORITY_HOST_AUTHORITATIVE;
    }
    return MADC_AUTHORITY_HOST_AUTHORITATIVE;
}

madc_execution_mode to_c_execution_mode(madc::execution_mode mode)
{
    switch ( mode )
    {
	case madc::execution_mode::in_process: return MADC_EXECUTION_IN_PROCESS;
	case madc::execution_mode::fork_per_invocation: return MADC_EXECUTION_FORK_PER_INVOCATION;
    }
    return MADC_EXECUTION_IN_PROCESS;
}

madc::authority_mode from_c_authority_mode(madc_authority_mode mode)
{
    switch ( mode )
    {
	case MADC_AUTHORITY_SYSTEM_LOCKED: return madc::authority_mode::system_locked;
	case MADC_AUTHORITY_USER_CONTROLLED: return madc::authority_mode::user_controlled;
	case MADC_AUTHORITY_HOST_AUTHORITATIVE: return madc::authority_mode::host_authoritative;
    }
    return madc::authority_mode::host_authoritative;
}

madc::execution_mode from_c_execution_mode(madc_execution_mode mode)
{
    switch ( mode )
    {
	case MADC_EXECUTION_IN_PROCESS: return madc::execution_mode::in_process;
	case MADC_EXECUTION_FORK_PER_INVOCATION: return madc::execution_mode::fork_per_invocation;
    }
    return madc::execution_mode::in_process;
}

void to_c_compile_options(const madc::compile_options &src, madc_compile_options *dst)
{
    if ( dst == NULL )
	return;
    dst->enable_core_functions = src.enable_core_functions ? 1 : 0;
    dst->enable_process_functions = src.enable_process_functions ? 1 : 0;
    dst->enable_dlfcn_functions = src.enable_dlfcn_functions ? 1 : 0;
    dst->enable_runtime_eval_source_scope_access = src.enable_runtime_eval_source_scope_access ? 1 : 0;
    dst->enable_runtime_eval_expression_scope_access = src.enable_runtime_eval_expression_scope_access ? 1 : 0;
    dst->enable_std_namespace = src.enable_std_namespace ? 1 : 0;
    dst->enable_madc_namespace = src.enable_madc_namespace ? 1 : 0;
    dst->enable_php_namespace = src.enable_php_namespace ? 1 : 0;
    dst->enable_perl_namespace = src.enable_perl_namespace ? 1 : 0;
    dst->enable_python_namespace = src.enable_python_namespace ? 1 : 0;
    dst->enable_ruby_namespace = src.enable_ruby_namespace ? 1 : 0;
    dst->enable_js_namespace = src.enable_js_namespace ? 1 : 0;
    dst->enable_rust_namespace = src.enable_rust_namespace ? 1 : 0;
}

void from_c_compile_options(const madc_compile_options &src, madc::compile_options &dst)
{
    dst.enable_core_functions = src.enable_core_functions != 0;
    dst.enable_process_functions = src.enable_process_functions != 0;
    dst.enable_dlfcn_functions = src.enable_dlfcn_functions != 0;
    dst.enable_runtime_eval_source_scope_access = src.enable_runtime_eval_source_scope_access != 0;
    dst.enable_runtime_eval_expression_scope_access = src.enable_runtime_eval_expression_scope_access != 0;
    dst.enable_std_namespace = src.enable_std_namespace != 0;
    dst.enable_madc_namespace = src.enable_madc_namespace != 0;
    dst.enable_php_namespace = src.enable_php_namespace != 0;
    dst.enable_perl_namespace = src.enable_perl_namespace != 0;
    dst.enable_python_namespace = src.enable_python_namespace != 0;
    dst.enable_ruby_namespace = src.enable_ruby_namespace != 0;
    dst.enable_js_namespace = src.enable_js_namespace != 0;
    dst.enable_rust_namespace = src.enable_rust_namespace != 0;
}

void to_c_security_policy(const madc::security_policy &src, madc_security_policy *dst)
{
    if ( dst == NULL )
	return;
    dst->mode = to_c_authority_mode(src.mode);
    dst->execution = to_c_execution_mode(src.execution);
    dst->allow_core_functions = src.allow_core_functions ? 1 : 0;
    dst->allow_process_functions = src.allow_process_functions ? 1 : 0;
    dst->allow_dlfcn_functions = src.allow_dlfcn_functions ? 1 : 0;
    dst->allow_runtime_eval_source_scope_access = src.allow_runtime_eval_source_scope_access ? 1 : 0;
    dst->allow_runtime_eval_expression_scope_access = src.allow_runtime_eval_expression_scope_access ? 1 : 0;
    dst->allow_std_namespace = src.allow_std_namespace ? 1 : 0;
    dst->allow_madc_namespace = src.allow_madc_namespace ? 1 : 0;
    dst->allow_php_namespace = src.allow_php_namespace ? 1 : 0;
    dst->allow_perl_namespace = src.allow_perl_namespace ? 1 : 0;
    dst->allow_python_namespace = src.allow_python_namespace ? 1 : 0;
    dst->allow_ruby_namespace = src.allow_ruby_namespace ? 1 : 0;
    dst->allow_js_namespace = src.allow_js_namespace ? 1 : 0;
    dst->allow_rust_namespace = src.allow_rust_namespace ? 1 : 0;
}

void from_c_security_policy(const madc_security_policy &src, madc::security_policy &dst)
{
    dst.mode = from_c_authority_mode(src.mode);
    dst.execution = from_c_execution_mode(src.execution);
    dst.allow_core_functions = src.allow_core_functions != 0;
    dst.allow_process_functions = src.allow_process_functions != 0;
    dst.allow_dlfcn_functions = src.allow_dlfcn_functions != 0;
    dst.allow_runtime_eval_source_scope_access = src.allow_runtime_eval_source_scope_access != 0;
    dst.allow_runtime_eval_expression_scope_access = src.allow_runtime_eval_expression_scope_access != 0;
    dst.allow_std_namespace = src.allow_std_namespace != 0;
    dst.allow_madc_namespace = src.allow_madc_namespace != 0;
    dst.allow_php_namespace = src.allow_php_namespace != 0;
    dst.allow_perl_namespace = src.allow_perl_namespace != 0;
    dst.allow_python_namespace = src.allow_python_namespace != 0;
    dst.allow_ruby_namespace = src.allow_ruby_namespace != 0;
    dst.allow_js_namespace = src.allow_js_namespace != 0;
    dst.allow_rust_namespace = src.allow_rust_namespace != 0;
}

void to_c_runtime_eval_policy(const madc::runtime_eval_policy &src, madc_runtime_eval_policy *dst)
{
    if ( dst == NULL )
	return;
    dst->allow_core_functions = src.allow_core_functions ? 1 : 0;
    dst->allow_process_functions = src.allow_process_functions ? 1 : 0;
    dst->allow_dlfcn_functions = src.allow_dlfcn_functions ? 1 : 0;
    dst->allow_std_namespace = src.allow_std_namespace ? 1 : 0;
    dst->allow_madc_namespace = src.allow_madc_namespace ? 1 : 0;
    dst->allow_php_namespace = src.allow_php_namespace ? 1 : 0;
    dst->allow_perl_namespace = src.allow_perl_namespace ? 1 : 0;
    dst->allow_python_namespace = src.allow_python_namespace ? 1 : 0;
    dst->allow_ruby_namespace = src.allow_ruby_namespace ? 1 : 0;
    dst->allow_js_namespace = src.allow_js_namespace ? 1 : 0;
    dst->allow_rust_namespace = src.allow_rust_namespace ? 1 : 0;
    dst->restrict_headers_to_allowlist = src.restrict_headers_to_allowlist ? 1 : 0;
    dst->restrict_dlfcn_symbols_to_allowlist = src.restrict_dlfcn_symbols_to_allowlist ? 1 : 0;
}

void from_c_runtime_eval_policy(const madc_runtime_eval_policy &src, madc::runtime_eval_policy &dst)
{
    dst.allow_core_functions = src.allow_core_functions != 0;
    dst.allow_process_functions = src.allow_process_functions != 0;
    dst.allow_dlfcn_functions = src.allow_dlfcn_functions != 0;
    dst.allow_std_namespace = src.allow_std_namespace != 0;
    dst.allow_madc_namespace = src.allow_madc_namespace != 0;
    dst.allow_php_namespace = src.allow_php_namespace != 0;
    dst.allow_perl_namespace = src.allow_perl_namespace != 0;
    dst.allow_python_namespace = src.allow_python_namespace != 0;
    dst.allow_ruby_namespace = src.allow_ruby_namespace != 0;
    dst.allow_js_namespace = src.allow_js_namespace != 0;
    dst.allow_rust_namespace = src.allow_rust_namespace != 0;
    dst.restrict_headers_to_allowlist = src.restrict_headers_to_allowlist != 0;
    dst.restrict_dlfcn_symbols_to_allowlist = src.restrict_dlfcn_symbols_to_allowlist != 0;
}

void to_c_invoke_limits(const madc::invoke_limits &src, madc_invoke_limits *dst)
{
    if ( dst == NULL )
	return;
    dst->cpu_ms = src.cpu_ms;
    dst->memory_bytes = src.memory_bytes;
    dst->output_bytes = src.output_bytes;
}

void from_c_invoke_limits(const madc_invoke_limits &src, madc::invoke_limits &dst)
{
    dst.cpu_ms = src.cpu_ms;
    dst.memory_bytes = src.memory_bytes;
    dst.output_bytes = src.output_bytes;
}

madc_error_severity to_c_error_severity(madc::error::severity sev)
{
    return sev == madc::error::severity::warning ? MADC_SEVERITY_WARNING : MADC_SEVERITY_ERROR;
}

madc_error_phase to_c_error_phase(madc::error::phase ph)
{
    switch ( ph )
    {
	case madc::error::phase::unknown: return MADC_PHASE_UNKNOWN;
	case madc::error::phase::lexer: return MADC_PHASE_LEXER;
	case madc::error::phase::parser: return MADC_PHASE_PARSER;
	case madc::error::phase::compiler: return MADC_PHASE_COMPILER;
	case madc::error::phase::runtime: return MADC_PHASE_RUNTIME;
    }
    return MADC_PHASE_UNKNOWN;
}

bool from_cpp_error(const madc::error &src, madc_error *dst)
{
    if ( dst == NULL )
	return false;
    madc_error_clear(dst);
    dst->severity = to_c_error_severity(src.level);
    dst->phase = to_c_error_phase(src.stage);
    dst->line = src.line;
    dst->column = src.column;
    if ( !duplicate_c_string(&dst->message, &dst->message_length, src.message) )
	return false;
    if ( !duplicate_c_string(&dst->file, &dst->file_length, src.file) )
    {
	std::free(dst->message);
	dst->message = NULL;
	dst->message_length = 0;
	return false;
    }
    return true;
}

template <typename Fn>
int run_program_call(madc_program *program, Fn fn)
{
    if ( program == NULL )
	return MADC_ERROR;

    try
    {
	bool ok = fn();
	if ( ok )
	{
	    sync_last_error(program);
	    return MADC_OK;
	}
	return return_failure(program);
    }
    catch ( const std::exception &e )
    {
	program->last_error_text = e.what();
	return MADC_EXCEPTION;
    }
    catch ( ... )
    {
	program->last_error_text = "unexpected exception";
	return MADC_EXCEPTION;
    }
}

} // namespace

extern "C" {

madc_engine *madc_engine_create(void)
{
    try
    {
	return new madc_engine;
    }
    catch ( ... )
    {
	return NULL;
    }
}

void madc_engine_destroy(madc_engine *engine)
{
    delete engine;
}

madc_program *madc_engine_create_program(madc_engine *engine)
{
    if ( !engine )
	return NULL;
    try
    {
	return new madc_program_opaque(engine->engine);
    }
    catch ( ... )
    {
	return NULL;
    }
}

int madc_engine_set_compile_options(madc_engine *engine,
				    const madc_compile_options *options)
{
    if ( !engine || !options )
	return MADC_ERROR;
    try
    {
	madc::compile_options cpp;
	from_c_compile_options(*options, cpp);
	engine->engine.set_compile_options(cpp);
	return MADC_OK;
    }
    catch ( ... )
    {
	return MADC_EXCEPTION;
    }
}

int madc_engine_get_compile_options(madc_engine *engine,
				    madc_compile_options *options)
{
    if ( !engine || !options )
	return MADC_ERROR;
    try
    {
	to_c_compile_options(engine->engine.get_compile_options(), options);
	return MADC_OK;
    }
    catch ( ... )
    {
	return MADC_EXCEPTION;
    }
}

int madc_engine_set_security_policy(madc_engine *engine,
				    const madc_security_policy *policy)
{
    if ( !engine || !policy )
	return MADC_ERROR;
    try
    {
	madc::security_policy cpp;
	from_c_security_policy(*policy, cpp);
	engine->engine.set_security_policy(cpp);
	return MADC_OK;
    }
    catch ( ... )
    {
	return MADC_EXCEPTION;
    }
}

int madc_engine_get_security_policy(madc_engine *engine,
				    madc_security_policy *policy)
{
    if ( !engine || !policy )
	return MADC_ERROR;
    try
    {
	to_c_security_policy(engine->engine.get_security_policy(), policy);
	return MADC_OK;
    }
    catch ( ... )
    {
	return MADC_EXCEPTION;
    }
}

int madc_engine_set_invoke_limits(madc_engine *engine,
				  const madc_invoke_limits *limits)
{
    if ( !engine || !limits )
	return MADC_ERROR;
    try
    {
	madc::invoke_limits cpp;
	from_c_invoke_limits(*limits, cpp);
	engine->engine.set_invoke_limits(cpp);
	return MADC_OK;
    }
    catch ( ... )
    {
	return MADC_EXCEPTION;
    }
}

int madc_engine_get_invoke_limits(madc_engine *engine,
				  madc_invoke_limits *limits)
{
    if ( !engine || !limits )
	return MADC_ERROR;
    try
    {
	to_c_invoke_limits(engine->engine.get_invoke_limits(), limits);
	return MADC_OK;
    }
    catch ( ... )
    {
	return MADC_EXCEPTION;
    }
}

madc_program *madc_program_create(void)
{
    try
    {
	return new madc_program;
    }
    catch ( ... )
    {
	return NULL;
    }
}

void madc_program_destroy(madc_program *program)
{
    delete program;
}

int madc_program_compile_file(madc_program *program, const char *path)
{
    return run_program_call(program, [=]() {
	return program->program.compile_file(path ? path : "");
    });
}

int madc_program_has_function(madc_program *program, const char *name)
{
    if ( !program || !name )
	return 0;
    try
    {
	return program->program.has_function(name) ? 1 : 0;
    }
    catch ( ... )
    {
	return 0;
    }
}

int madc_program_exec_file(madc_program *program, const char *path)
{
    return run_program_call(program, [=]() {
	return program->program.exec_file(path ? path : "");
    });
}

int madc_program_exec_string(madc_program *program,
			     const char *source,
			     const char *virtual_filename)
{
    return run_program_call(program, [=]() {
	return program->program.exec_string(source ? source : "",
					    virtual_filename ? virtual_filename : "");
    });
}

int madc_program_eval_unit(madc_program *program,
			   const char *source,
			   madc_value *result,
			   const char *virtual_filename)
{
    return run_program_call(program, [=]() {
	madc::value cpp_result;
	bool ok = program->program.eval_unit(source ? source : "",
					     result ? &cpp_result : NULL,
					     virtual_filename ? virtual_filename : "");
	if ( !ok || result == NULL )
	    return ok;
	return from_cpp_value(cpp_result, result);
    });
}

int madc_program_eval_expression(madc_program *program,
				 const char *expression,
				 madc_value *result,
				 const char *virtual_filename)
{
    return run_program_call(program, [=]() {
	madc::value cpp_result;
	bool ok = program->program.eval_expression(expression ? expression : "",
						   result ? &cpp_result : NULL,
						   virtual_filename ? virtual_filename : "");
	if ( !ok || result == NULL )
	    return ok;
	return from_cpp_value(cpp_result, result);
    });
}

int madc_program_call(madc_program *program,
		      const char *name,
		      const madc_value *args,
		      size_t nargs,
		      madc_value *result)
{
    if ( program == NULL )
	return MADC_ERROR;

    std::vector<madc::value> cpp_args;
    cpp_args.reserve(nargs);
    for ( size_t i = 0; i < nargs; ++i )
    {
	madc::value arg;
	if ( !to_cpp_value(args[i], arg) )
	    return return_failure(program, "madc C API does not support that argument kind");
	cpp_args.push_back(arg);
    }

    return run_program_call(program, [=, &cpp_args]() {
	madc::value cpp_result;
	bool ok = program->program.call(name ? name : "",
					cpp_args,
					result ? &cpp_result : NULL);
	if ( !ok || result == NULL )
	    return ok;
	return from_cpp_value(cpp_result, result);
    });
}

void madc_compile_options_init(madc_compile_options *options)
{
    if ( options == NULL )
	return;
    to_c_compile_options(madc::compile_options(), options);
}

void madc_security_policy_init(madc_security_policy *policy)
{
    if ( policy == NULL )
	return;
    to_c_security_policy(madc::security_policy(), policy);
}

void madc_runtime_eval_policy_init(madc_runtime_eval_policy *policy)
{
    if ( policy == NULL )
	return;
    to_c_runtime_eval_policy(madc::runtime_eval_policy(), policy);
}

void madc_invoke_limits_init(madc_invoke_limits *limits)
{
    if ( limits == NULL )
	return;
    to_c_invoke_limits(madc::invoke_limits(), limits);
}

int madc_program_set_compile_options(madc_program *program,
				     const madc_compile_options *options)
{
    if ( program == NULL || options == NULL )
	return MADC_ERROR;
    madc::compile_options cpp;
    from_c_compile_options(*options, cpp);
    program->program.set_compile_options(cpp);
    return MADC_OK;
}

int madc_program_get_compile_options(madc_program *program,
				     madc_compile_options *options)
{
    if ( program == NULL || options == NULL )
	return MADC_ERROR;
    to_c_compile_options(program->program.get_compile_options(), options);
    return MADC_OK;
}

int madc_program_set_security_policy(madc_program *program,
				     const madc_security_policy *policy)
{
    if ( program == NULL || policy == NULL )
	return MADC_ERROR;
    madc::security_policy cpp;
    from_c_security_policy(*policy, cpp);
    program->program.set_security_policy(cpp);
    return MADC_OK;
}

int madc_program_get_security_policy(madc_program *program,
				     madc_security_policy *policy)
{
    if ( program == NULL || policy == NULL )
	return MADC_ERROR;
    to_c_security_policy(program->program.get_security_policy(), policy);
    return MADC_OK;
}

int madc_program_set_runtime_eval_policy(madc_program *program,
					 const madc_runtime_eval_policy *policy)
{
    if ( program == NULL || policy == NULL )
	return MADC_ERROR;
    madc::runtime_eval_policy cpp;
    from_c_runtime_eval_policy(*policy, cpp);
    program->program.set_runtime_eval_policy(cpp);
    return MADC_OK;
}

int madc_program_get_runtime_eval_policy(madc_program *program,
					 madc_runtime_eval_policy *policy)
{
    if ( program == NULL || policy == NULL )
	return MADC_ERROR;
    to_c_runtime_eval_policy(program->program.get_runtime_eval_policy(), policy);
    return MADC_OK;
}

int madc_program_set_invoke_limits(madc_program *program,
				   const madc_invoke_limits *limits)
{
    if ( program == NULL || limits == NULL )
	return MADC_ERROR;
    madc::invoke_limits cpp;
    from_c_invoke_limits(*limits, cpp);
    program->program.set_invoke_limits(cpp);
    return MADC_OK;
}

int madc_program_get_invoke_limits(madc_program *program,
				   madc_invoke_limits *limits)
{
    if ( program == NULL || limits == NULL )
	return MADC_ERROR;
    to_c_invoke_limits(program->program.get_invoke_limits(), limits);
    return MADC_OK;
}

const char *madc_program_last_error(madc_program *program)
{
    if ( program == NULL )
	return NULL;
    sync_last_error(program);
    return program->last_error_text.empty() ? NULL : program->last_error_text.c_str();
}

size_t madc_program_diagnostic_count(madc_program *program)
{
    if ( program == NULL )
	return 0;
    return program->program.diagnostics().size();
}

int madc_program_get_diagnostic(madc_program *program,
				size_t index,
				madc_error *diagnostic)
{
    if ( program == NULL || diagnostic == NULL )
	return MADC_ERROR;
    const std::vector<madc::error> &errors = program->program.diagnostics();
    if ( index >= errors.size() )
	return MADC_ERROR;
    return from_cpp_error(errors[index], diagnostic) ? MADC_OK : MADC_ERROR;
}

void madc_program_clear_diagnostics(madc_program *program)
{
    if ( program == NULL )
	return;
    program->program.clear_diagnostics();
    program->last_error_text.clear();
}

void madc_value_init(madc_value *value)
{
    if ( value == NULL )
	return;
    value->kind = MADC_VALUE_NULL;
    value->boolean_value = 0;
    value->integer_value = 0;
    value->real_value = 0.0;
    value->string_value = NULL;
    value->string_length = 0;
}

void madc_value_clear(madc_value *value)
{
    clear_c_value(value);
}

void madc_error_init(madc_error *error)
{
    if ( error == NULL )
	return;
    error->severity = MADC_SEVERITY_ERROR;
    error->phase = MADC_PHASE_UNKNOWN;
    error->message = NULL;
    error->message_length = 0;
    error->file = NULL;
    error->file_length = 0;
    error->line = 0;
    error->column = 0;
}

void madc_error_clear(madc_error *error)
{
    if ( error == NULL )
	return;
    if ( error->message != NULL )
	std::free(error->message);
    if ( error->file != NULL )
	std::free(error->file);
    madc_error_init(error);
}

int madc_value_set_null(madc_value *value)
{
    if ( value == NULL )
	return MADC_ERROR;
    clear_c_value(value);
    return MADC_OK;
}

int madc_value_set_bool(madc_value *value, int boolean_value)
{
    if ( value == NULL )
	return MADC_ERROR;
    clear_c_value(value);
    value->kind = MADC_VALUE_BOOLEAN;
    value->boolean_value = boolean_value ? 1 : 0;
    return MADC_OK;
}

int madc_value_set_integer(madc_value *value, int64_t integer_value)
{
    if ( value == NULL )
	return MADC_ERROR;
    clear_c_value(value);
    value->kind = MADC_VALUE_INTEGER;
    value->integer_value = integer_value;
    return MADC_OK;
}

int madc_value_set_real(madc_value *value, double real_value)
{
    if ( value == NULL )
	return MADC_ERROR;
    clear_c_value(value);
    value->kind = MADC_VALUE_REAL;
    value->real_value = real_value;
    return MADC_OK;
}

int madc_value_set_string(madc_value *value, const char *string_value)
{
    if ( string_value == NULL )
	return madc_value_set_string_n(value, "", 0);
    return madc_value_set_string_n(value, string_value, std::strlen(string_value));
}

int madc_value_set_string_n(madc_value *value,
			    const char *string_value,
			    size_t string_length)
{
    if ( value == NULL )
	return MADC_ERROR;
    return set_c_string(value, string_value, string_length) ? MADC_OK : MADC_ERROR;
}

} // extern "C"
