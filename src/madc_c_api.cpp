#include "madc_api.h"

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

const char *madc_program_last_error(madc_program *program)
{
    if ( program == NULL )
	return NULL;
    sync_last_error(program);
    return program->last_error_text.empty() ? NULL : program->last_error_text.c_str();
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
