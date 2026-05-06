// madc convenience API — see include/libmadc/api.h

#include "libmadc/api.h"

namespace madc {

bool exec_file(const std::string &path)
{
    program pgm;
    return pgm.exec_file(path);
}

bool exec_string(const std::string &source, const std::string &virtual_filename)
{
    program pgm;
    return pgm.exec_string(source, virtual_filename);
}

bool eval_unit(const std::string &source, value *result, const std::string &virtual_filename)
{
    program pgm;
    return pgm.eval_unit(source, result, virtual_filename);
}

bool eval_unit(const std::string &source, bool &result, const std::string &virtual_filename)
{
    program pgm;
    return pgm.eval_unit(source, result, virtual_filename);
}

bool eval_unit(const std::string &source, int64_t &result, const std::string &virtual_filename)
{
    program pgm;
    return pgm.eval_unit(source, result, virtual_filename);
}

bool eval_unit(const std::string &source, double &result, const std::string &virtual_filename)
{
    program pgm;
    return pgm.eval_unit(source, result, virtual_filename);
}

bool eval_unit(const std::string &source, std::string &result, const std::string &virtual_filename)
{
    program pgm;
    return pgm.eval_unit(source, result, virtual_filename);
}

bool eval(const std::string &source, value *result, const std::string &virtual_filename)
{
    return eval_unit(source, result, virtual_filename);
}

bool eval(const std::string &source, bool &result, const std::string &virtual_filename)
{
    return eval_unit(source, result, virtual_filename);
}

bool eval(const std::string &source, int64_t &result, const std::string &virtual_filename)
{
    return eval_unit(source, result, virtual_filename);
}

bool eval(const std::string &source, double &result, const std::string &virtual_filename)
{
    return eval_unit(source, result, virtual_filename);
}

bool eval(const std::string &source, std::string &result, const std::string &virtual_filename)
{
    return eval_unit(source, result, virtual_filename);
}

bool eval_body(const std::string &source,
	       value *result,
	       program::native_type return_type,
	       const std::string &virtual_filename)
{
    program pgm;
    return pgm.eval_body(source, result, return_type, virtual_filename);
}

bool eval_body(const std::string &source, bool &result, const std::string &virtual_filename)
{
    program pgm;
    return pgm.eval_body(source, result, virtual_filename);
}

bool eval_body(const std::string &source, int64_t &result, const std::string &virtual_filename)
{
    program pgm;
    return pgm.eval_body(source, result, virtual_filename);
}

bool eval_body(const std::string &source, double &result, const std::string &virtual_filename)
{
    program pgm;
    return pgm.eval_body(source, result, virtual_filename);
}

bool eval_body(const std::string &source, std::string &result, const std::string &virtual_filename)
{
    program pgm;
    return pgm.eval_body(source, result, virtual_filename);
}

bool eval_expression(const std::string &expression,
		     value *result,
		     const std::string &virtual_filename)
{
    program pgm;
    return pgm.eval_expression(expression, result, virtual_filename);
}

bool eval_expression(const std::string &expression,
		     bool &result,
		     const std::string &virtual_filename)
{
    program pgm;
    return pgm.eval_expression(expression, result, virtual_filename);
}

bool eval_expression(const std::string &expression,
		     int64_t &result,
		     const std::string &virtual_filename)
{
    program pgm;
    return pgm.eval_expression(expression, result, virtual_filename);
}

bool eval_expression(const std::string &expression,
		     double &result,
		     const std::string &virtual_filename)
{
    program pgm;
    return pgm.eval_expression(expression, result, virtual_filename);
}

bool eval_expression(const std::string &expression,
		     std::string &result,
		     const std::string &virtual_filename)
{
    program pgm;
    return pgm.eval_expression(expression, result, virtual_filename);
}

} // namespace madc
