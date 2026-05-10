#ifndef __LIBMADC_ENGINE_H
#define __LIBMADC_ENGINE_H 1

#include "libmadc/options.h"
#include "libmadc/program.h"

#include <memory>
#include <string>

namespace madc {

class engine
{
public:
    engine();
    ~engine();

    engine(engine &&other) noexcept;
    engine &operator=(engine &&other) noexcept;

    engine(const engine &) = delete;
    engine &operator=(const engine &) = delete;

    /// Create a program that shares this engine's registry, policy
    /// defaults, and logging configuration.
    program create_program();

    /// Enable verbose diagnostic output for this engine's programs.
    void set_verbose(bool v);
    bool get_verbose() const;

    /// Policy defaults applied to every program created from this engine.
    void set_compile_options(const compile_options &options);
    const compile_options &get_compile_options() const;
    void set_security_policy(const security_policy &policy);
    const security_policy &get_security_policy() const;
    void set_expression_policy(const expression_policy &policy);
    const expression_policy &get_expression_policy() const;
    void set_runtime_eval_policy(const runtime_eval_policy &policy);
    const runtime_eval_policy &get_runtime_eval_policy() const;
    void set_invoke_limits(const invoke_limits &limits);
    const invoke_limits &get_invoke_limits() const;

    /// Register a host callback that will be available to every program
    /// created from this engine after the registration call.
    template <typename Ret, typename... Args>
    bool register_function(const std::string &name, Ret (*callback)(Args...));
    bool register_function(const std::string &name,
			   program::native_function callback,
			   const program::native_signature &signature);

private:
    bool register_cpp_callback(const std::string &name,
			       void *callback_ptr,
			       const program::native_signature &signature,
			       program::native_function adapter_entry);
    friend class program;
    struct impl;
    std::unique_ptr<impl> _impl;
};

template <typename Ret, typename... Args>
bool engine::register_function(const std::string &name, Ret (*callback)(Args...))
{
    program::native_function adapter =
	reinterpret_cast<program::native_function>(
	    &detail::callback_adapter<Ret, Args...>::entry);
    return register_cpp_callback(name,
				 reinterpret_cast<void *>(callback),
				 detail::make_callback_signature<Ret, Args...>(),
				 adapter);
}

} // namespace madc

#endif // __LIBMADC_ENGINE_H
