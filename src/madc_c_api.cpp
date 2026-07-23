#include "madc_api.h"
#include "madc_value_cell.h"

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

// C <-> C++ value bridges. Scalars, text, bytes, and typed instances share
// the 32-byte struct, so the bridge is a raw struct copy (cell-retaining);
// the array/object container kinds have no C-ABI payload yet.
bool from_cpp_value(const madc::value &src, madc_value *dst)
{
    if ( dst == NULL )
	return true;
    if ( src.is_array() || src.is_object() )
	return false;
    return madc_value_copy(dst, &src.raw()) == MADC_OK;
}

bool to_cpp_value(const madc_value &src, madc::value &dst)
{
    switch ( src.type_id )
    {
	case MADC_TYPEID_INVALID:
	    dst = madc::value();
	    return true;
	case MADC_TYPEID_BOOL:
	    dst = madc::value(src.integer_value != 0);
	    return true;
	case MADC_TYPEID_INT64:
	    dst = madc::value(src.integer_value);
	    return true;
	case MADC_TYPEID_DOUBLE:
	    dst = madc::value(src.real_value);
	    return true;
	case MADC_TYPEID_TEXT:
	{
	    size_t len = 0;
	    const char *data = madc_value_text(&src, &len);
	    dst = madc::value(std::string(data ? data : "", len));
	    return true;
	}
	default:
	    return false;
    }
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

void to_c_expression_policy(const madc::expression_policy &src, madc_expression_policy *dst)
{
    if ( dst == NULL )
	return;
    dst->allow_function_calls = src.allow_function_calls ? 1 : 0;
    dst->allow_member_access = src.allow_member_access ? 1 : 0;
    dst->allow_subscript_access = src.allow_subscript_access ? 1 : 0;
    dst->allow_pointer_operations = src.allow_pointer_operations ? 1 : 0;
}

void from_c_expression_policy(const madc_expression_policy &src, madc::expression_policy &dst)
{
    dst.allow_function_calls = src.allow_function_calls != 0;
    dst.allow_member_access = src.allow_member_access != 0;
    dst.allow_subscript_access = src.allow_subscript_access != 0;
    dst.allow_pointer_operations = src.allow_pointer_operations != 0;
}

madc::program::native_type to_cpp_native_type(madc_native_type t)
{
    switch ( t )
    {
	case MADC_NATIVE_BOOLEAN:       return madc::program::native_type::boolean;
	case MADC_NATIVE_INTEGER:       return madc::program::native_type::integer;
	case MADC_NATIVE_REAL:          return madc::program::native_type::real;
	case MADC_NATIVE_C_STRING:      return madc::program::native_type::c_string;
	default:                        return madc::program::native_type::void_type;
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

int madc_engine_set_verbose(madc_engine *engine, int verbose)
{
    if ( !engine )
	return MADC_ERROR;
    try { engine->engine.set_verbose(verbose != 0); return MADC_OK; }
    catch ( ... ) { return MADC_EXCEPTION; }
}

int madc_engine_get_verbose(madc_engine *engine)
{
    if ( !engine )
	return 0;
    return engine->engine.get_verbose() ? 1 : 0;
}

int madc_engine_set_expression_policy(madc_engine *engine,
				      const madc_expression_policy *policy)
{
    if ( !engine || !policy )
	return MADC_ERROR;
    try
    {
	madc::expression_policy cpp;
	from_c_expression_policy(*policy, cpp);
	engine->engine.set_expression_policy(cpp);
	return MADC_OK;
    }
    catch ( ... ) { return MADC_EXCEPTION; }
}

int madc_engine_get_expression_policy(madc_engine *engine,
				      madc_expression_policy *policy)
{
    if ( !engine || !policy )
	return MADC_ERROR;
    try
    {
	to_c_expression_policy(engine->engine.get_expression_policy(), policy);
	return MADC_OK;
    }
    catch ( ... ) { return MADC_EXCEPTION; }
}

int madc_engine_set_runtime_eval_policy(madc_engine *engine,
					const madc_runtime_eval_policy *policy)
{
    if ( !engine || !policy )
	return MADC_ERROR;
    try
    {
	madc::runtime_eval_policy cpp;
	from_c_runtime_eval_policy(*policy, cpp);
	engine->engine.set_runtime_eval_policy(cpp);
	return MADC_OK;
    }
    catch ( ... ) { return MADC_EXCEPTION; }
}

int madc_engine_get_runtime_eval_policy(madc_engine *engine,
					madc_runtime_eval_policy *policy)
{
    if ( !engine || !policy )
	return MADC_ERROR;
    try
    {
	to_c_runtime_eval_policy(engine->engine.get_runtime_eval_policy(), policy);
	return MADC_OK;
    }
    catch ( ... ) { return MADC_EXCEPTION; }
}

int madc_engine_register_function(madc_engine *engine,
				  const char *name,
				  madc_native_function callback,
				  madc_native_type return_type,
				  const madc_native_type *param_types,
				  size_t param_count)
{
    if ( !engine || !name || !callback )
	return MADC_ERROR;
    try
    {
	madc::program::native_signature sig(to_cpp_native_type(return_type));
	for ( size_t i = 0; i < param_count; ++i )
	    sig.parameters.push_back(to_cpp_native_type(param_types[i]));
	madc::program::native_function fn =
	    reinterpret_cast<madc::program::native_function>(callback);
	if ( !engine->engine.register_function(name, fn, sig) )
	    return MADC_ERROR;
	return MADC_OK;
    }
    catch ( ... ) { return MADC_EXCEPTION; }
}

int madc_engine_add_allowed_header(madc_engine *engine, const char *header)
{
    if ( !engine || !header )
	return MADC_ERROR;
    try
    {
	madc::compile_options opts = engine->engine.get_compile_options();
	opts.allowed_headers.push_back(header);
	engine->engine.set_compile_options(opts);
	return MADC_OK;
    }
    catch ( ... ) { return MADC_EXCEPTION; }
}

int madc_engine_clear_allowed_headers(madc_engine *engine)
{
    if ( !engine )
	return MADC_ERROR;
    try
    {
	madc::compile_options opts = engine->engine.get_compile_options();
	opts.allowed_headers.clear();
	engine->engine.set_compile_options(opts);
	return MADC_OK;
    }
    catch ( ... ) { return MADC_EXCEPTION; }
}

int madc_engine_add_allowed_dlfcn_symbol(madc_engine *engine, const char *symbol)
{
    if ( !engine || !symbol )
	return MADC_ERROR;
    try
    {
	madc::compile_options opts = engine->engine.get_compile_options();
	opts.allowed_dlfcn_symbols.push_back(symbol);
	engine->engine.set_compile_options(opts);
	return MADC_OK;
    }
    catch ( ... ) { return MADC_EXCEPTION; }
}

int madc_engine_clear_allowed_dlfcn_symbols(madc_engine *engine)
{
    if ( !engine )
	return MADC_ERROR;
    try
    {
	madc::compile_options opts = engine->engine.get_compile_options();
	opts.allowed_dlfcn_symbols.clear();
	engine->engine.set_compile_options(opts);
	return MADC_OK;
    }
    catch ( ... ) { return MADC_EXCEPTION; }
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

void madc_program_set_aot_mode(madc_program *program, int enabled)
{
    if ( program )
	program->program.set_aot_mode(enabled != 0);
}

int madc_program_compile_file(madc_program *program, const char *path)
{
    return run_program_call(program, [=]() {
	return program->program.compile_file(path ? path : "");
    });
}

int madc_program_compile_string(madc_program *program,
				const char *source,
				const char *virtual_filename)
{
    return run_program_call(program, [=]() {
	return program->program.compile_string(
	    source ? source : "",
	    virtual_filename ? virtual_filename : "");
    });
}

int madc_program_is_compiled(madc_program *program)
{
    if ( !program )
	return 0;
    return program->program.is_compiled() ? 1 : 0;
}

int madc_program_save_object(madc_program *program, const char *path)
{
    if ( !program || !path )
	return MADC_ERROR;
    return run_program_call(program, [=]() {
	return program->program.save_object(path);
    });
}

int madc_program_save_executable(madc_program *program, const char *path)
{
    if ( !program || !path )
	return MADC_ERROR;
    return run_program_call(program, [=]() {
	return program->program.save_executable(path);
    });
}

int madc_program_load_object(madc_program *program, const char *path)
{
    if ( !program || !path )
	return MADC_ERROR;
    return run_program_call(program, [=]() {
	return program->program.load_object(path);
    });
}

int madc_program_exec(madc_program *program)
{
    return run_program_call(program, [=]() {
	return program->program.exec();
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

int madc_program_eval_body(madc_program *program,
			   const char *source,
			   madc_value *result,
			   madc_value_kind return_kind,
			   const char *virtual_filename)
{
    if ( !program || !source )
	return MADC_ERROR;
    madc::program::native_type ret = madc::program::native_type::void_type;
    switch ( return_kind )
    {
	case MADC_VALUE_BOOLEAN: ret = madc::program::native_type::boolean; break;
	case MADC_VALUE_INTEGER: ret = madc::program::native_type::integer; break;
	case MADC_VALUE_REAL:    ret = madc::program::native_type::real; break;
	case MADC_VALUE_STRING:  ret = madc::program::native_type::c_string; break;
	default: break;
    }
    return run_program_call(program, [=]() {
	madc::value cpp_result;
	bool ok = program->program.eval_body(source,
					     result ? &cpp_result : NULL,
					     ret,
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

int madc_program_register_function(madc_program *program,
				   const char *name,
				   madc_native_function callback,
				   madc_native_type return_type,
				   const madc_native_type *param_types,
				   size_t param_count)
{
    if ( !program || !name || !callback )
	return MADC_ERROR;
    try
    {
	madc::program::native_signature sig(to_cpp_native_type(return_type));
	for ( size_t i = 0; i < param_count; ++i )
	    sig.parameters.push_back(to_cpp_native_type(param_types[i]));
	madc::program::native_function fn =
	    reinterpret_cast<madc::program::native_function>(callback);
	if ( !program->program.register_function(name, fn, sig) )
	    return MADC_ERROR;
	return MADC_OK;
    }
    catch ( ... )
    {
	return MADC_EXCEPTION;
    }
}

int madc_program_get_global(madc_program *program,
			    const char *name,
			    madc_value *result)
{
    if ( !program || !name || !result )
	return MADC_ERROR;
    return run_program_call(program, [=]() {
	madc::value cpp_result;
	if ( !program->program.get_global(name, &cpp_result) )
	    return false;
	return from_cpp_value(cpp_result, result);
    });
}

int madc_program_set_global(madc_program *program,
			    const char *name,
			    const madc_value *new_value)
{
    if ( !program || !name || !new_value )
	return MADC_ERROR;
    madc::value cpp_val;
    if ( !to_cpp_value(*new_value, cpp_val) )
	return return_failure(program, "madc C API does not support that value kind for set_global");
    return run_program_call(program, [=, &cpp_val]() {
	return program->program.set_global(name, cpp_val);
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

int madc_program_has_error(madc_program *program)
{
    if ( !program )
	return 0;
    return program->program.has_error() ? 1 : 0;
}

void madc_program_clear_diagnostics(madc_program *program)
{
    if ( program == NULL )
	return;
    program->program.clear_diagnostics();
    program->last_error_text.clear();
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









void madc_expression_policy_init(madc_expression_policy *policy)
{
    if ( policy == NULL )
	return;
    to_c_expression_policy(madc::expression_policy(), policy);
}

int madc_program_set_expression_policy(madc_program *program,
				       const madc_expression_policy *policy)
{
    if ( !program || !policy )
	return MADC_ERROR;
    return run_program_call(program, [=]() {
	madc::expression_policy cpp;
	from_c_expression_policy(*policy, cpp);
	program->program.set_expression_policy(cpp);
	return true;
    });
}

int madc_program_get_expression_policy(madc_program *program,
				       madc_expression_policy *policy)
{
    if ( !program || !policy )
	return MADC_ERROR;
    try
    {
	to_c_expression_policy(program->program.get_expression_policy(), policy);
	return MADC_OK;
    }
    catch ( ... ) { return MADC_EXCEPTION; }
}

int madc_program_add_allowed_header(madc_program *program, const char *header)
{
    if ( !program || !header )
	return MADC_ERROR;
    return run_program_call(program, [=]() {
	madc::compile_options opts = program->program.get_compile_options();
	opts.allowed_headers.push_back(header);
	program->program.set_compile_options(opts);
	return true;
    });
}

int madc_program_clear_allowed_headers(madc_program *program)
{
    if ( !program )
	return MADC_ERROR;
    return run_program_call(program, [=]() {
	madc::compile_options opts = program->program.get_compile_options();
	opts.allowed_headers.clear();
	program->program.set_compile_options(opts);
	return true;
    });
}

int madc_program_add_allowed_dlfcn_symbol(madc_program *program, const char *symbol)
{
    if ( !program || !symbol )
	return MADC_ERROR;
    return run_program_call(program, [=]() {
	madc::compile_options opts = program->program.get_compile_options();
	opts.allowed_dlfcn_symbols.push_back(symbol);
	program->program.set_compile_options(opts);
	return true;
    });
}

int madc_program_clear_allowed_dlfcn_symbols(madc_program *program)
{
    if ( !program )
	return MADC_ERROR;
    return run_program_call(program, [=]() {
	madc::compile_options opts = program->program.get_compile_options();
	opts.allowed_dlfcn_symbols.clear();
	program->program.set_compile_options(opts);
	return true;
    });
}

int madc_program_add_allowed_expression_header(madc_program *program, const char *header)
{
    if ( !program || !header )
	return MADC_ERROR;
    return run_program_call(program, [=]() {
	madc::expression_policy ep = program->program.get_expression_policy();
	ep.allowed_headers.push_back(header);
	program->program.set_expression_policy(ep);
	return true;
    });
}

int madc_program_clear_allowed_expression_headers(madc_program *program)
{
    if ( !program )
	return MADC_ERROR;
    return run_program_call(program, [=]() {
	madc::expression_policy ep = program->program.get_expression_policy();
	ep.allowed_headers.clear();
	program->program.set_expression_policy(ep);
	return true;
    });
}

int madc_program_add_allowed_expression_function(madc_program *program, const char *function_name)
{
    if ( !program || !function_name )
	return MADC_ERROR;
    return run_program_call(program, [=]() {
	madc::expression_policy ep = program->program.get_expression_policy();
	ep.allowed_functions.push_back(function_name);
	program->program.set_expression_policy(ep);
	return true;
    });
}

int madc_program_clear_allowed_expression_functions(madc_program *program)
{
    if ( !program )
	return MADC_ERROR;
    return run_program_call(program, [=]() {
	madc::expression_policy ep = program->program.get_expression_policy();
	ep.allowed_functions.clear();
	program->program.set_expression_policy(ep);
	return true;
    });
}

int madc_program_set_expression_binding(madc_program *program,
					const char *name,
					const madc_value *value)
{
    if ( !program || !name || !value )
	return MADC_ERROR;
    return run_program_call(program, [=]() {
	madc::value cpp_val;
	if ( !to_cpp_value(*value, cpp_val) )
	    return false;
	std::map<std::string, madc::value> bindings = program->program.get_expression_bindings();
	bindings[name] = cpp_val;
	program->program.set_expression_bindings(bindings);
	return true;
    });
}

int madc_program_clear_expression_bindings(madc_program *program)
{
    if ( !program )
	return MADC_ERROR;
    try
    {
	program->program.clear_expression_bindings();
	return MADC_OK;
    }
    catch ( ... ) { return MADC_EXCEPTION; }
}

int madc_program_set_expression_context(madc_program *program,
					const madc_value *context)
{
    if ( !program || !context )
	return MADC_ERROR;
    return run_program_call(program, [=]() {
	madc::value cpp_val;
	if ( !to_cpp_value(*context, cpp_val) )
	    return false;
	program->program.set_expression_context(cpp_val);
	return true;
    });
}

int madc_program_clear_expression_context(madc_program *program)
{
    if ( !program )
	return MADC_ERROR;
    try
    {
	program->program.clear_expression_context();
	return MADC_OK;
    }
    catch ( ... ) { return MADC_EXCEPTION; }
}

const char *madc_value_kind_name(madc_value_kind kind)
{
    switch ( kind )
    {
	case MADC_VALUE_NULL:    return "null";
	case MADC_VALUE_BOOLEAN: return "boolean";
	case MADC_VALUE_INTEGER: return "integer";
	case MADC_VALUE_REAL:    return "real";
	case MADC_VALUE_STRING:  return "string";
    }
    return "unknown";
}

const char *madc_error_severity_name(madc_error_severity severity)
{
    switch ( severity )
    {
	case MADC_SEVERITY_WARNING: return "warning";
	case MADC_SEVERITY_ERROR:   return "error";
    }
    return "unknown";
}

const char *madc_error_phase_name(madc_error_phase phase)
{
    switch ( phase )
    {
	case MADC_PHASE_UNKNOWN:  return "unknown";
	case MADC_PHASE_LEXER:    return "lexer";
	case MADC_PHASE_PARSER:   return "parser";
	case MADC_PHASE_COMPILER: return "compiler";
	case MADC_PHASE_RUNTIME:  return "runtime";
    }
    return "unknown";
}

} // extern "C"
