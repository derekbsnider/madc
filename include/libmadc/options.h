#ifndef __LIBMADC_OPTIONS_H
#define __LIBMADC_OPTIONS_H 1

#include <cstdint>
#include <string>
#include <vector>

namespace madc {

struct compile_options
{
    bool enable_core_functions = true;
    bool enable_process_functions = true;
    bool enable_dlfcn_functions = true;
    bool enable_std_namespace = true;
    bool enable_madc_namespace = true;
    bool enable_php_namespace = true;
    bool enable_perl_namespace = true;
    bool enable_python_namespace = true;
    bool enable_ruby_namespace = true;
    bool enable_js_namespace = true;
    bool enable_rust_namespace = true;
    std::vector<std::string> allowed_headers;
    std::vector<std::string> allowed_dlfcn_symbols;
};

enum class authority_mode
{
    system_locked,
    user_controlled,
    host_authoritative
};

enum class execution_mode
{
    in_process,
    fork_per_invocation
};

struct security_policy
{
    authority_mode mode = authority_mode::host_authoritative;
    execution_mode execution = execution_mode::in_process;
    bool allow_core_functions = true;
    bool allow_process_functions = true;
    bool allow_dlfcn_functions = true;
    bool allow_std_namespace = true;
    bool allow_madc_namespace = true;
    bool allow_php_namespace = true;
    bool allow_perl_namespace = true;
    bool allow_python_namespace = true;
    bool allow_ruby_namespace = true;
    bool allow_js_namespace = true;
    bool allow_rust_namespace = true;
    std::vector<std::string> allowed_headers;
    std::vector<std::string> allowed_dlfcn_symbols;
};

struct expression_policy
{
    bool allow_function_calls = false;
    bool allow_member_access = false;
    bool allow_subscript_access = false;
    bool allow_pointer_operations = false;
    std::vector<std::string> allowed_headers;
    std::vector<std::string> allowed_functions;
};

struct invoke_limits
{
    uint64_t cpu_ms = 0;
    uint64_t memory_bytes = 0;
    uint64_t output_bytes = 0;
};

} // namespace madc

#endif // __LIBMADC_OPTIONS_H
