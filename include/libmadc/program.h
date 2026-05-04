#ifndef __LIBMADC_PROGRAM_H
#define __LIBMADC_PROGRAM_H 1

#include "libmadc/error.h"
#include "libmadc/options.h"
#include "libmadc/value.h"

#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace madc {

class program
{
public:
    enum class native_type
    {
	void_type,
	boolean,
	integer,
	real,
	c_string
    };

    struct native_signature
    {
	native_type returns;
	std::vector<native_type> parameters;

	native_signature(native_type ret = native_type::void_type)
	    : returns(ret)
	{
	}

	native_signature(native_type ret, std::initializer_list<native_type> params)
	    : returns(ret), parameters(params)
	{
	}
    };

    typedef void (*native_function)();

    program();
    ~program();

    program(program &&other) noexcept;
    program &operator=(program &&other) noexcept;

    program(const program &) = delete;
    program &operator=(const program &) = delete;

    bool compile_file(const std::string &path);
    bool exec_file(const std::string &path);
    bool exec_string(const std::string &source,
		     const std::string &virtual_filename = std::string());
    bool eval(const std::string &source,
	      value *result = NULL,
	      const std::string &virtual_filename = std::string());
    bool register_function(const std::string &name,
			   native_function callback,
			   const native_signature &signature);
    bool call(const std::string &name,
	      const std::vector<value> &args,
	      value *result = NULL);
    bool get_global(const std::string &name, value *result) const;
    bool set_global(const std::string &name, const value &new_value);
    void set_compile_options(const compile_options &options);
    const compile_options &get_compile_options() const;
    void set_security_policy(const security_policy &policy);
    const security_policy &get_security_policy() const;
    void set_invoke_limits(const invoke_limits &limits);
    const invoke_limits &get_invoke_limits() const;

    const std::vector<error> &diagnostics() const;
    const error *last_error() const;
    bool has_error() const;
    void clear_diagnostics();

private:
    struct impl;
    std::unique_ptr<impl> _impl;
};

} // namespace madc

#endif // __LIBMADC_PROGRAM_H
