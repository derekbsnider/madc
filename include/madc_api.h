#ifndef __MADC_API_H
#define __MADC_API_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct madc_program_opaque madc_program;
typedef struct madc_engine_opaque madc_engine;

typedef enum madc_result_code
{
    MADC_OK = 0,
    MADC_ERROR = -1,
    MADC_EXCEPTION = -2
} madc_result_code;

typedef enum madc_value_kind
{
    MADC_VALUE_NULL = 0,
    MADC_VALUE_BOOLEAN = 1,
    MADC_VALUE_INTEGER = 2,
    MADC_VALUE_REAL = 3,
    MADC_VALUE_STRING = 4
} madc_value_kind;

typedef struct madc_value
{
    madc_value_kind kind;
    int boolean_value;
    int64_t integer_value;
    double real_value;
    char *string_value;
    size_t string_length;
} madc_value;

typedef enum madc_authority_mode
{
    MADC_AUTHORITY_SYSTEM_LOCKED = 0,
    MADC_AUTHORITY_USER_CONTROLLED = 1,
    MADC_AUTHORITY_HOST_AUTHORITATIVE = 2
} madc_authority_mode;

typedef enum madc_execution_mode
{
    MADC_EXECUTION_IN_PROCESS = 0,
    MADC_EXECUTION_FORK_PER_INVOCATION = 1
} madc_execution_mode;

typedef enum madc_error_severity
{
    MADC_SEVERITY_WARNING = 0,
    MADC_SEVERITY_ERROR = 1
} madc_error_severity;

typedef enum madc_error_phase
{
    MADC_PHASE_UNKNOWN = 0,
    MADC_PHASE_LEXER = 1,
    MADC_PHASE_PARSER = 2,
    MADC_PHASE_COMPILER = 3,
    MADC_PHASE_RUNTIME = 4
} madc_error_phase;

typedef struct madc_compile_options
{
    int enable_core_functions;
    int enable_process_functions;
    int enable_dlfcn_functions;
    int enable_runtime_eval_source_scope_access;
    int enable_runtime_eval_expression_scope_access;
    int enable_std_namespace;
    int enable_madc_namespace;
    int enable_php_namespace;
    int enable_perl_namespace;
    int enable_python_namespace;
    int enable_ruby_namespace;
    int enable_js_namespace;
    int enable_rust_namespace;
} madc_compile_options;

typedef struct madc_security_policy
{
    madc_authority_mode mode;
    madc_execution_mode execution;
    int allow_core_functions;
    int allow_process_functions;
    int allow_dlfcn_functions;
    int allow_runtime_eval_source_scope_access;
    int allow_runtime_eval_expression_scope_access;
    int allow_std_namespace;
    int allow_madc_namespace;
    int allow_php_namespace;
    int allow_perl_namespace;
    int allow_python_namespace;
    int allow_ruby_namespace;
    int allow_js_namespace;
    int allow_rust_namespace;
} madc_security_policy;

typedef struct madc_runtime_eval_policy
{
    int allow_core_functions;
    int allow_process_functions;
    int allow_dlfcn_functions;
    int allow_std_namespace;
    int allow_madc_namespace;
    int allow_php_namespace;
    int allow_perl_namespace;
    int allow_python_namespace;
    int allow_ruby_namespace;
    int allow_js_namespace;
    int allow_rust_namespace;
    int restrict_headers_to_allowlist;
    int restrict_dlfcn_symbols_to_allowlist;
} madc_runtime_eval_policy;

typedef struct madc_invoke_limits
{
    uint64_t cpu_ms;
    uint64_t memory_bytes;
    uint64_t output_bytes;
} madc_invoke_limits;

typedef struct madc_error
{
    madc_error_severity severity;
    madc_error_phase phase;
    char *message;
    size_t message_length;
    char *file;
    size_t file_length;
    int line;
    int column;
} madc_error;

madc_engine *madc_engine_create(void);
void madc_engine_destroy(madc_engine *engine);
madc_program *madc_engine_create_program(madc_engine *engine);
int madc_engine_set_compile_options(madc_engine *engine,
				    const madc_compile_options *options);
int madc_engine_get_compile_options(madc_engine *engine,
				    madc_compile_options *options);
int madc_engine_set_security_policy(madc_engine *engine,
				    const madc_security_policy *policy);
int madc_engine_get_security_policy(madc_engine *engine,
				    madc_security_policy *policy);
int madc_engine_set_invoke_limits(madc_engine *engine,
				  const madc_invoke_limits *limits);
int madc_engine_get_invoke_limits(madc_engine *engine,
				  madc_invoke_limits *limits);

madc_program *madc_program_create(void);
void madc_program_destroy(madc_program *program);

int madc_program_compile_file(madc_program *program, const char *path);
int madc_program_has_function(madc_program *program, const char *name);
int madc_program_exec_file(madc_program *program, const char *path);
int madc_program_exec_string(madc_program *program,
			     const char *source,
			     const char *virtual_filename);
int madc_program_eval_unit(madc_program *program,
			   const char *source,
			   madc_value *result,
			   const char *virtual_filename);
int madc_program_eval_expression(madc_program *program,
				 const char *expression,
				 madc_value *result,
				 const char *virtual_filename);
int madc_program_call(madc_program *program,
		      const char *name,
		      const madc_value *args,
		      size_t nargs,
		      madc_value *result);

void madc_compile_options_init(madc_compile_options *options);
void madc_security_policy_init(madc_security_policy *policy);
void madc_runtime_eval_policy_init(madc_runtime_eval_policy *policy);
void madc_invoke_limits_init(madc_invoke_limits *limits);
int madc_program_set_compile_options(madc_program *program,
				     const madc_compile_options *options);
int madc_program_get_compile_options(madc_program *program,
				     madc_compile_options *options);
int madc_program_set_security_policy(madc_program *program,
				     const madc_security_policy *policy);
int madc_program_get_security_policy(madc_program *program,
				     madc_security_policy *policy);
int madc_program_set_runtime_eval_policy(madc_program *program,
					 const madc_runtime_eval_policy *policy);
int madc_program_get_runtime_eval_policy(madc_program *program,
					 madc_runtime_eval_policy *policy);
int madc_program_set_invoke_limits(madc_program *program,
				   const madc_invoke_limits *limits);
int madc_program_get_invoke_limits(madc_program *program,
				   madc_invoke_limits *limits);

const char *madc_program_last_error(madc_program *program);
size_t madc_program_diagnostic_count(madc_program *program);
int madc_program_get_diagnostic(madc_program *program,
				size_t index,
				madc_error *diagnostic);
void madc_program_clear_diagnostics(madc_program *program);

void madc_value_init(madc_value *value);
void madc_value_clear(madc_value *value);
void madc_error_init(madc_error *error);
void madc_error_clear(madc_error *error);
int madc_value_set_null(madc_value *value);
int madc_value_set_bool(madc_value *value, int boolean_value);
int madc_value_set_integer(madc_value *value, int64_t integer_value);
int madc_value_set_real(madc_value *value, double real_value);
int madc_value_set_string(madc_value *value, const char *string_value);
int madc_value_set_string_n(madc_value *value,
			    const char *string_value,
			    size_t string_length);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // __MADC_API_H
