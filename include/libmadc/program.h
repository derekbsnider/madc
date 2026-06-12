#ifndef __LIBMADC_PROGRAM_H
#define __LIBMADC_PROGRAM_H 1

#include "libmadc/error.h"
#include "libmadc/options.h"
#include "libmadc/value.h"

#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace madc {

class engine;

class program
{
public:
    enum class native_type
    {
	void_type,
	boolean,
	integer,
	real,
	c_string,
	text_object	// by-value text-carrier parameter / hidden-retbuf return
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
    explicit program(engine &eng);
    ~program();

    program(program &&other) noexcept;
    program &operator=(program &&other) noexcept;

    program(const program &) = delete;
    program &operator=(const program &) = delete;

    void set_aot_mode(bool enabled);
    bool compile_file(const std::string &path);
    bool compile_string(const std::string &source,
			const std::string &virtual_filename = std::string());
    bool is_compiled() const;
    bool save_object(const std::string &path);
    bool save_executable(const std::string &path);
    bool load_object(const std::string &path);
    bool exec();
    bool exec_file(const std::string &path);
    bool exec_string(const std::string &source,
		     const std::string &virtual_filename = std::string());
    // Full translation unit: caller supplies the complete source and
    // either defines __madc_eval(...) explicitly or relies on the
    // existing full-unit contract.
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
    // Function-body text: wraps into __madc_eval(...) when the caller
    // does not provide an explicit entry function.
    bool eval_body(const std::string &source,
		   value *result,
		   native_type return_type,
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
    // Single-expression lane with the dedicated expression policy.
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
    template <typename Ret, typename... Args>
    bool register_function(const std::string &name, Ret (*callback)(Args...));
    bool register_function(const std::string &name,
			   native_function callback,
			   const native_signature &signature);
    bool has_function(const std::string &name) const;
    bool call(const std::string &name,
	      const std::vector<value> &args,
	      value *result = NULL);
    bool get_global(const std::string &name, value *result) const;
    bool set_global(const std::string &name, const value &new_value);
    void set_compile_options(const compile_options &options);
    const compile_options &get_compile_options() const;
    void set_security_policy(const security_policy &policy);
    const security_policy &get_security_policy() const;
    void set_expression_policy(const expression_policy &policy);
    const expression_policy &get_expression_policy() const;
    void set_runtime_eval_policy(const runtime_eval_policy &policy);
    const runtime_eval_policy &get_runtime_eval_policy() const;
    void set_expression_bindings(const std::map<std::string, value> &bindings);
    const std::map<std::string, value> &get_expression_bindings() const;
    void clear_expression_bindings();
    void set_expression_context(const value &context);
    const value &get_expression_context() const;
    void clear_expression_context();
    void set_invoke_limits(const invoke_limits &limits);
    const invoke_limits &get_invoke_limits() const;

    const std::vector<error> &diagnostics() const;
    const error *last_error() const;
    bool has_error() const;
    void clear_diagnostics();

private:
    bool register_cpp_callback(const std::string &name,
			       void *callback_ptr,
			       const native_signature &signature,
			       native_function adapter_entry);
    struct impl;
    std::unique_ptr<impl> _impl;
};

namespace detail {

template <typename T>
struct callback_type_map;

template <>
struct callback_type_map<bool>
{
    typedef bool low_type;
    static program::native_type native() { return program::native_type::boolean; }
    static bool from_low(bool v) { return v; }
    static bool to_low(bool v) { return v; }
};

template <>
struct callback_type_map<int64_t>
{
    typedef int64_t low_type;
    static program::native_type native() { return program::native_type::integer; }
    static int64_t from_low(int64_t v) { return v; }
    static int64_t to_low(int64_t v) { return v; }
};

template <>
struct callback_type_map<double>
{
    typedef double low_type;
    static program::native_type native() { return program::native_type::real; }
    static double from_low(double v) { return v; }
    static double to_low(double v) { return v; }
};

template <>
struct callback_type_map<const char *>
{
    typedef const char *low_type;
    static program::native_type native() { return program::native_type::c_string; }
    static const char *from_low(const char *v) { return v; }
    static const char *to_low(const char *v) { return v; }
};

template <typename Ret, typename... Args>
struct callback_adapter
{
    typedef Ret (*callback_ptr)(Args...);
    typedef typename callback_type_map<Ret>::low_type low_ret_type;

    static low_ret_type entry(uintptr_t raw, typename callback_type_map<Args>::low_type... args)
    {
	callback_ptr fn = reinterpret_cast<callback_ptr>(raw);
	return callback_type_map<Ret>::to_low(fn(callback_type_map<Args>::from_low(args)...));
    }
};

template <typename... Args>
struct callback_adapter<void, Args...>
{
    typedef void (*callback_ptr)(Args...);

    static void entry(uintptr_t raw, typename callback_type_map<Args>::low_type... args)
    {
	callback_ptr fn = reinterpret_cast<callback_ptr>(raw);
	fn(callback_type_map<Args>::from_low(args)...);
    }
};

template <typename Ret, typename... Args>
program::native_signature make_callback_signature()
{
    program::native_signature sig(std::is_void<Ret>::value
				      ? program::native_type::void_type
				      : callback_type_map<Ret>::native());
    int unused[] = {0, (sig.parameters.push_back(callback_type_map<Args>::native()), 0)...};
    (void)unused;
    return sig;
}

} // namespace detail

template <typename Ret, typename... Args>
bool program::register_function(const std::string &name, Ret (*callback)(Args...))
{
    native_function adapter = reinterpret_cast<native_function>(&detail::callback_adapter<Ret, Args...>::entry);
    return register_cpp_callback(name,
				 reinterpret_cast<void *>(callback),
				 detail::make_callback_signature<Ret, Args...>(),
				 adapter);
}

} // namespace madc

#endif // __LIBMADC_PROGRAM_H
