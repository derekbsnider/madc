#ifndef __LIBMADC_OPTIONS_H
#define __LIBMADC_OPTIONS_H 1

#include <cstdint>
#include <string>
#include <vector>

namespace madc {

/// What a program does when the frozen-forest discovery chain finds no
/// usable container (forest-carriers S3/S4). silent_fallback live-parses
/// quietly (the library default: an embedding host that never shipped a
/// container must not be nagged); loud_fallback live-parses after ONE
/// stderr notice; strict_require makes it a compile error — the choice for
/// a host that must never silently degrade to the slow path. A container
/// found but built for a DIFFERENT dialect (std / -D defines) is the
/// by-design multi-dialect fall-through: never a notice under
/// loud_fallback, still a hard error under strict_require.
enum class forest_policy
{
    silent_fallback,
    loud_fallback,
    strict_require
};

struct compile_options
{
    bool enable_core_functions = true;
    bool enable_process_functions = true;
    bool enable_dlfcn_functions = true;
    bool enable_runtime_eval_source_scope_access = true;
    bool enable_runtime_eval_expression_scope_access = true;
    bool enable_std_namespace = true;
    bool enable_madc_namespace = true;
    bool enable_php_namespace = true;
    bool enable_perl_namespace = true;
    bool enable_python_namespace = true;
    bool enable_ruby_namespace = true;
    bool enable_js_namespace = true;
    bool enable_rust_namespace = true;
    /// Bind frozen-forest state at all — the library twin of the CLI's
    /// --no-forest-bind. ON by default: a host linked against a packed
    /// libmadc gets grove-backed system headers (the precompiled-header
    /// model) for free. Turn it off to force live parsing.
    bool enable_forest_bind = true;
    /// Enable target-owned additive POSIX header compatibility.
    /// This is effective only for targets that need the layer (currently
    /// Win64); POSIX targets always use their native surface.
    bool enable_posix_compat = true;
    /// Frozen-forest discovery: what happens when no container is found.
    forest_policy forest_missing = forest_policy::silent_fallback;
    /// Allow the discovery arms that read frozen state from OUTSIDE the
    /// executable / libmadc images — the <exe>.forest and <lib>.forest
    /// sidecars and the MADC_FOREST environment variable. A sandboxed host
    /// turns this off so nothing external can redirect where the compiler
    /// loads frozen state from; the image arms keep working.
    bool enable_external_forest = true;
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
    bool allow_runtime_eval_source_scope_access = true;
    bool allow_runtime_eval_expression_scope_access = true;
    bool allow_std_namespace = true;
    bool allow_madc_namespace = true;
    bool allow_php_namespace = true;
    bool allow_perl_namespace = true;
    bool allow_python_namespace = true;
    bool allow_ruby_namespace = true;
    bool allow_js_namespace = true;
    bool allow_rust_namespace = true;
    /// Permission twin of compile_options::enable_external_forest — the
    /// sidecar / MADC_FOREST discovery arms are an external input, so a
    /// sandbox policy owns them. The forest_missing choice stays a
    /// compile_option: it is an operational contract, not a permission.
    bool allow_external_forest = true;
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

struct runtime_eval_policy
{
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
    bool restrict_headers_to_allowlist = false;
    bool restrict_dlfcn_symbols_to_allowlist = false;
    std::vector<std::string> allowed_headers;
    std::vector<std::string> allowed_dlfcn_symbols;
};

struct invoke_limits
{
    uint64_t cpu_ms = 0;
    uint64_t memory_bytes = 0;
    uint64_t output_bytes = 0;
};

} // namespace madc

#endif // __LIBMADC_OPTIONS_H
