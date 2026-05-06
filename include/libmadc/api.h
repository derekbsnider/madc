#ifndef __LIBMADC_API_H
#define __LIBMADC_API_H 1

#include "libmadc/program.h"

#include <string>

namespace madc {

bool exec_file(const std::string &path);
bool exec_string(const std::string &source,
		 const std::string &virtual_filename = std::string());
bool eval(const std::string &source,
	  value *result = NULL,
	  const std::string &virtual_filename = std::string());
bool eval(const std::string &source,
	  bool &result,
	  const std::string &virtual_filename = std::string());
bool eval(const std::string &source,
	  int64_t &result,
	  const std::string &virtual_filename = std::string());
bool eval(const std::string &source,
	  double &result,
	  const std::string &virtual_filename = std::string());
bool eval(const std::string &source,
	  std::string &result,
	  const std::string &virtual_filename = std::string());
bool eval_expression(const std::string &expression,
		     value *result = NULL,
		     const std::string &virtual_filename = std::string());
bool eval_expression(const std::string &expression,
		     bool &result,
		     const std::string &virtual_filename = std::string());
bool eval_expression(const std::string &expression,
		     int64_t &result,
		     const std::string &virtual_filename = std::string());
bool eval_expression(const std::string &expression,
		     double &result,
		     const std::string &virtual_filename = std::string());
bool eval_expression(const std::string &expression,
		     std::string &result,
		     const std::string &virtual_filename = std::string());

} // namespace madc

#endif // __LIBMADC_API_H
