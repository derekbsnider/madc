#ifndef __LIBMADC_API_H
#define __LIBMADC_API_H 1

#include "libmadc/program.h"

#include <string>

namespace madc {

bool exec_file(const std::string &path);
bool exec_string(const std::string &source,
		 const std::string &virtual_filename = std::string());
// Full translation unit convenience wrapper.
bool eval_unit(const std::string &source,
	       value *result = NULL,
	       const std::string &virtual_filename = std::string());
bool eval_unit(const std::string &source,
	       bool &result,
	       const std::string &virtual_filename = std::string());
bool eval_unit(const std::string &source,
	       int64_t &result,
	       const std::string &virtual_filename = std::string());
bool eval_unit(const std::string &source,
	       double &result,
	       const std::string &virtual_filename = std::string());
bool eval_unit(const std::string &source,
	       std::string &result,
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
// Function-body text convenience wrapper.
bool eval_body(const std::string &source,
	       value *result,
	       program::native_type return_type,
	       const std::string &virtual_filename = std::string());
bool eval_body(const std::string &source,
	       bool &result,
	       const std::string &virtual_filename = std::string());
bool eval_body(const std::string &source,
	       int64_t &result,
	       const std::string &virtual_filename = std::string());
bool eval_body(const std::string &source,
	       double &result,
	       const std::string &virtual_filename = std::string());
bool eval_body(const std::string &source,
	       std::string &result,
	       const std::string &virtual_filename = std::string());
// Single-expression convenience wrapper.
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
