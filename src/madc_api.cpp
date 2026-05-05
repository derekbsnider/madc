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

bool eval(const std::string &source, value *result, const std::string &virtual_filename)
{
    program pgm;
    return pgm.eval(source, result, virtual_filename);
}

bool eval_expression(const std::string &expression,
		     value *result,
		     const std::string &virtual_filename)
{
    program pgm;
    return pgm.eval_expression(expression, result, virtual_filename);
}

} // namespace madc
