#ifndef __MADC_API_H
#define __MADC_API_H 1

#include <stddef.h>
#include <stdint.h>

#include "madc_typeid.h"

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

/* Value kinds are typeid slots (madc_typeid.h) — one vocabulary. The old
 * MADC_VALUE_* names are aliases kept for readability at call sites. */
typedef uint32_t madc_value_kind;
#define MADC_VALUE_NULL    ((madc_value_kind)MADC_TYPEID_INVALID)
#define MADC_VALUE_BOOLEAN ((madc_value_kind)MADC_TYPEID_BOOL)
#define MADC_VALUE_INTEGER ((madc_value_kind)MADC_TYPEID_INT64)
#define MADC_VALUE_REAL    ((madc_value_kind)MADC_TYPEID_DOUBLE)
#define MADC_VALUE_STRING  ((madc_value_kind)MADC_TYPEID_TEXT)

/* madc_value.flags bits. Storage discriminators + gradual typing
 * (docs/plans/2026-06-12-type-table-value-abi-design.md §4). All other
 * bits reserved-zero. */
enum
{
    MADC_VF_HEAP        = 1u << 0,  /* payload is a refcounted cell */
    MADC_VF_INLINE_TEXT = 1u << 1,  /* payload is SSO inline_text */
    MADC_VF_TYPE_LOCKED = 1u << 2,  /* re-tag is an error */
    MADC_VF_TYPE_COERCE = 1u << 3,  /* assignments convert to current type_id */
    MADC_VF_NULLABLE    = 1u << 4,  /* null ok even when LOCKED/COERCE */
    MADC_VF_CONST       = 1u << 5   /* value is read-only */
};

/* The 32-byte interchange value (design doc §3). type_id is the canonical
 * type identity; size is per-kind (text/bytes length, array count, struct
 * byte size, sizeof for scalars); the 16-byte payload inlines every madc
 * primitive. Text: <= 15 bytes lives in inline_text NUL-terminated
 * (MADC_VF_INLINE_TEXT); longer text lives in a NUL-terminated refcounted
 * cell (MADC_VF_HEAP). Copy with madc_value_copy (retains the cell),
 * release with madc_value_clear; read text uniformly via madc_value_text. */
typedef struct __attribute__((aligned(16))) madc_value
{
    uint32_t type_id;
    uint32_t flags;
    uint64_t size;
    union
    {
	int64_t  integer_value;   /* bool folds in; type_id distinguishes */
	double   real_value;
	char    *text_value;      /* cell payload when MADC_VF_HEAP */
	void    *data_ptr;        /* array/object/oversize-struct cell */
	char     inline_text[16]; /* SSO when MADC_VF_INLINE_TEXT */
	uint64_t wide_value[2];   /* __int128/_Complex/v128 (P0) */
    };
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

typedef void (*madc_native_function)(void);

typedef enum madc_native_type
{
    MADC_NATIVE_VOID = 0,
    MADC_NATIVE_BOOLEAN = 1,
    MADC_NATIVE_INTEGER = 2,
    MADC_NATIVE_REAL = 3,
    MADC_NATIVE_C_STRING = 4
} madc_native_type;

/* ------------------------------------------------------------------ */
/* Structs                                                            */
/* ------------------------------------------------------------------ */

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

typedef struct madc_expression_policy
{
    int allow_function_calls;
    int allow_member_access;
    int allow_subscript_access;
    int allow_pointer_operations;
} madc_expression_policy;

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

/* ------------------------------------------------------------------ */
/* Engine lifecycle                                                    */
/* ------------------------------------------------------------------ */

madc_engine *madc_engine_create(void);
void madc_engine_destroy(madc_engine *engine);
madc_program *madc_engine_create_program(madc_engine *engine);

int madc_engine_set_verbose(madc_engine *engine, int verbose);
int madc_engine_get_verbose(madc_engine *engine);

int madc_engine_set_compile_options(madc_engine *engine,
				    const madc_compile_options *options);
int madc_engine_get_compile_options(madc_engine *engine,
				    madc_compile_options *options);
int madc_engine_set_security_policy(madc_engine *engine,
				    const madc_security_policy *policy);
int madc_engine_get_security_policy(madc_engine *engine,
				    madc_security_policy *policy);
int madc_engine_set_expression_policy(madc_engine *engine,
				      const madc_expression_policy *policy);
int madc_engine_get_expression_policy(madc_engine *engine,
				      madc_expression_policy *policy);
int madc_engine_set_runtime_eval_policy(madc_engine *engine,
					const madc_runtime_eval_policy *policy);
int madc_engine_get_runtime_eval_policy(madc_engine *engine,
					madc_runtime_eval_policy *policy);
int madc_engine_set_invoke_limits(madc_engine *engine,
				  const madc_invoke_limits *limits);
int madc_engine_get_invoke_limits(madc_engine *engine,
				  madc_invoke_limits *limits);
int madc_engine_register_function(madc_engine *engine,
				  const char *name,
				  madc_native_function callback,
				  madc_native_type return_type,
				  const madc_native_type *param_types,
				  size_t param_count);

/* ------------------------------------------------------------------ */
/* Allowlist management (engine level)                                */
/* ------------------------------------------------------------------ */

int madc_engine_add_allowed_header(madc_engine *engine, const char *header);
int madc_engine_clear_allowed_headers(madc_engine *engine);
int madc_engine_add_allowed_dlfcn_symbol(madc_engine *engine, const char *symbol);
int madc_engine_clear_allowed_dlfcn_symbols(madc_engine *engine);

/* ------------------------------------------------------------------ */
/* Program lifecycle                                                  */
/* ------------------------------------------------------------------ */

madc_program *madc_program_create(void);
void madc_program_destroy(madc_program *program);

/* ------------------------------------------------------------------ */
/* Compile / execute / eval / call                                    */
/* ------------------------------------------------------------------ */

void madc_program_set_aot_mode(madc_program *program, int enabled);
int madc_program_compile_file(madc_program *program, const char *path);
int madc_program_compile_string(madc_program *program,
				const char *source,
				const char *virtual_filename);
int madc_program_is_compiled(madc_program *program);
int madc_program_save_object(madc_program *program, const char *path);
int madc_program_save_executable(madc_program *program, const char *path);
int madc_program_load_object(madc_program *program, const char *path);
int madc_program_exec(madc_program *program);
int madc_program_has_function(madc_program *program, const char *name);
int madc_program_exec_file(madc_program *program, const char *path);
int madc_program_exec_string(madc_program *program,
			     const char *source,
			     const char *virtual_filename);
int madc_program_eval_unit(madc_program *program,
			   const char *source,
			   madc_value *result,
			   const char *virtual_filename);
int madc_program_eval_body(madc_program *program,
			   const char *source,
			   madc_value *result,
			   madc_value_kind return_kind,
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

int madc_program_register_function(madc_program *program,
				   const char *name,
				   madc_native_function callback,
				   madc_native_type return_type,
				   const madc_native_type *param_types,
				   size_t param_count);

int madc_program_get_global(madc_program *program,
			    const char *name,
			    madc_value *result);
int madc_program_set_global(madc_program *program,
			    const char *name,
			    const madc_value *new_value);

/* ------------------------------------------------------------------ */
/* Policy configuration (struct init + program get/set)               */
/* ------------------------------------------------------------------ */

void madc_compile_options_init(madc_compile_options *options);
void madc_security_policy_init(madc_security_policy *policy);
void madc_expression_policy_init(madc_expression_policy *policy);
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
int madc_program_set_expression_policy(madc_program *program,
				       const madc_expression_policy *policy);
int madc_program_get_expression_policy(madc_program *program,
				       madc_expression_policy *policy);
int madc_program_set_runtime_eval_policy(madc_program *program,
					 const madc_runtime_eval_policy *policy);
int madc_program_get_runtime_eval_policy(madc_program *program,
					 madc_runtime_eval_policy *policy);
int madc_program_set_invoke_limits(madc_program *program,
				   const madc_invoke_limits *limits);
int madc_program_get_invoke_limits(madc_program *program,
				   madc_invoke_limits *limits);

/* ------------------------------------------------------------------ */
/* Allowlist management (program level)                               */
/* ------------------------------------------------------------------ */

int madc_program_add_allowed_header(madc_program *program, const char *header);
int madc_program_clear_allowed_headers(madc_program *program);
int madc_program_add_allowed_dlfcn_symbol(madc_program *program, const char *symbol);
int madc_program_clear_allowed_dlfcn_symbols(madc_program *program);
int madc_program_add_allowed_expression_header(madc_program *program, const char *header);
int madc_program_clear_allowed_expression_headers(madc_program *program);
int madc_program_add_allowed_expression_function(madc_program *program, const char *function_name);
int madc_program_clear_allowed_expression_functions(madc_program *program);

/* ------------------------------------------------------------------ */
/* Expression bindings and context                                    */
/* ------------------------------------------------------------------ */

int madc_program_set_expression_binding(madc_program *program,
					const char *name,
					const madc_value *value);
int madc_program_clear_expression_bindings(madc_program *program);
int madc_program_set_expression_context(madc_program *program,
					const madc_value *context);
int madc_program_clear_expression_context(madc_program *program);

/* ------------------------------------------------------------------ */
/* Diagnostics                                                        */
/* ------------------------------------------------------------------ */

const char *madc_program_last_error(madc_program *program);
int madc_program_has_error(madc_program *program);
size_t madc_program_diagnostic_count(madc_program *program);
int madc_program_get_diagnostic(madc_program *program,
				size_t index,
				madc_error *diagnostic);
void madc_program_clear_diagnostics(madc_program *program);

/* ------------------------------------------------------------------ */
/* Value and error helpers                                            */
/* ------------------------------------------------------------------ */

void madc_value_init(madc_value *value);
void madc_value_clear(madc_value *value);
void madc_error_init(madc_error *error);
void madc_error_clear(madc_error *error);
int madc_value_set_null(madc_value *value);
int madc_value_set_bool(madc_value *value, int boolean_value);
int madc_value_set_integer(madc_value *value, int64_t integer_value);
int madc_value_set_real(madc_value *value, double real_value);
int madc_value_set_string(madc_value *value, const char *text_value);
int madc_value_set_string_n(madc_value *value,
			    const char *text_value,
			    size_t text_length);
/* Deep-copy semantics at struct level, shared cell underneath: clears dst,
 * struct-assigns, retains the payload cell when present. */
int madc_value_copy(madc_value *dst, const madc_value *src);
/* Uniform text accessor (SSO or cell; NUL-terminated either way). NULL when
 * the value holds no text. text_length out-param is optional. */
const char *madc_value_text(const madc_value *value, size_t *text_length);

const char *madc_value_kind_name(madc_value_kind kind);
const char *madc_error_severity_name(madc_error_severity severity);
const char *madc_error_phase_name(madc_error_phase phase);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __MADC_API_H */
