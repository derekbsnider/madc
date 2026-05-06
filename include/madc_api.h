#ifndef __MADC_API_H
#define __MADC_API_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct madc_program_opaque madc_program;

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

madc_program *madc_program_create(void);
void madc_program_destroy(madc_program *program);

int madc_program_compile_file(madc_program *program, const char *path);
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

const char *madc_program_last_error(madc_program *program);
void madc_program_clear_diagnostics(madc_program *program);

void madc_value_init(madc_value *value);
void madc_value_clear(madc_value *value);
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
