// madc::program — see include/libmadc/program.h

#include "libmadc/program.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <queue>
#include <stdexcept>
#include <sstream>
#include <stack>
#include <string>
#include <utility>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

extern bool madc_verbose;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <asmjit/x86.h>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

namespace madc {

namespace {

error::phase phase_from_program(Program::DiagnosticPhase ph)
{
    switch ( ph )
    {
	case Program::DiagnosticPhase::lexer:    return error::phase::lexer;
	case Program::DiagnosticPhase::parser:   return error::phase::parser;
	case Program::DiagnosticPhase::compiler: return error::phase::compiler;
	case Program::DiagnosticPhase::runtime:  return error::phase::runtime;
	case Program::DiagnosticPhase::unknown:  return error::phase::unknown;
    }
    return error::phase::unknown;
}

std::string temp_source_template()
{
    return "/tmp/madc_program_XXXXXX";
}

const char *eval_entry_name()
{
    return "__madc_eval";
}

uint64_t timeval_to_microseconds(const timeval &tv)
{
    return static_cast<uint64_t>(tv.tv_sec) * UINT64_C(1000000)
	+ static_cast<uint64_t>(tv.tv_usec);
}

uint64_t current_cpu_microseconds()
{
    struct rusage usage;
    if ( getrusage(RUSAGE_SELF, &usage) != 0 )
	return 0;
    return timeval_to_microseconds(usage.ru_utime)
	+ timeval_to_microseconds(usage.ru_stime);
}

uint64_t current_resident_bytes()
{
    std::ifstream statm("/proc/self/statm");
    uint64_t pages_total = 0;
    uint64_t pages_resident = 0;
    statm >> pages_total >> pages_resident;
    if ( !statm )
	return 0;
    long page_size = sysconf(_SC_PAGESIZE);
    if ( page_size <= 0 )
	return 0;
    return pages_resident * static_cast<uint64_t>(page_size);
}

Program::RegistrationPolicy registration_policy_from_compile_options(const compile_options &options)
{
    Program::RegistrationPolicy policy;
    policy.enable_core_functions = options.enable_core_functions;
    policy.enable_process_functions = options.enable_process_functions;
    policy.enable_dlfcn_functions = options.enable_dlfcn_functions;
    policy.enable_std_namespace = options.enable_std_namespace;
    policy.enable_madc_namespace = options.enable_madc_namespace;
    policy.enable_php_namespace = options.enable_php_namespace;
    policy.enable_perl_namespace = options.enable_perl_namespace;
    policy.enable_python_namespace = options.enable_python_namespace;
    policy.enable_ruby_namespace = options.enable_ruby_namespace;
    policy.enable_js_namespace = options.enable_js_namespace;
    policy.enable_rust_namespace = options.enable_rust_namespace;
    return policy;
}

compile_options compile_options_from_security_policy(const security_policy &policy)
{
    compile_options options;
    options.enable_core_functions = policy.allow_core_functions;
    options.enable_process_functions = policy.allow_process_functions;
    options.enable_dlfcn_functions = policy.allow_dlfcn_functions;
    options.enable_std_namespace = policy.allow_std_namespace;
    options.enable_madc_namespace = policy.allow_madc_namespace;
    options.enable_php_namespace = policy.allow_php_namespace;
    options.enable_perl_namespace = policy.allow_perl_namespace;
    options.enable_python_namespace = policy.allow_python_namespace;
    options.enable_ruby_namespace = policy.allow_ruby_namespace;
    options.enable_js_namespace = policy.allow_js_namespace;
    options.enable_rust_namespace = policy.allow_rust_namespace;
    return options;
}

security_policy security_policy_from_compile_options(const compile_options &options,
						     authority_mode mode)
{
    security_policy policy;
    policy.mode = mode;
    policy.allow_core_functions = options.enable_core_functions;
    policy.allow_process_functions = options.enable_process_functions;
    policy.allow_dlfcn_functions = options.enable_dlfcn_functions;
    policy.allow_std_namespace = options.enable_std_namespace;
    policy.allow_madc_namespace = options.enable_madc_namespace;
    policy.allow_php_namespace = options.enable_php_namespace;
    policy.allow_perl_namespace = options.enable_perl_namespace;
    policy.allow_python_namespace = options.enable_python_namespace;
    policy.allow_ruby_namespace = options.enable_ruby_namespace;
    policy.allow_js_namespace = options.enable_js_namespace;
    policy.allow_rust_namespace = options.enable_rust_namespace;
    return policy;
}

DataType datatype_from_native_type(program::native_type type)
{
    switch ( type )
    {
	case program::native_type::void_type: return DataType::dtVOID;
	case program::native_type::boolean:   return DataType::dtBOOL;
	case program::native_type::integer:   return DataType::dtINT64;
	case program::native_type::real:      return DataType::dtDOUBLE;
	case program::native_type::c_string:  return rtPtr(DataType::dtCHAR);
    }
    return DataType::dtVOID;
}

bool native_type_from_datadef(DataDef *type, program::native_type &out)
{
    if ( !type )
	return false;

    switch ( type->type() )
    {
	case DataType::dtVOID:
	    out = program::native_type::void_type;
	    return true;
	case DataType::dtBOOL:
	    out = program::native_type::boolean;
	    return true;
	case DataType::dtCHARptr:
	    out = program::native_type::c_string;
	    return true;
	default:
	    break;
    }

    if ( type->is_integer() )
    {
	out = program::native_type::integer;
	return true;
    }
    if ( type->is_real() )
    {
	out = program::native_type::real;
	return true;
    }
    return false;
}

template <typename T>
T value_as(const value &v);

template <>
bool value_as<bool>(const value &v)
{
    return v.as_boolean();
}

template <>
int64_t value_as<int64_t>(const value &v)
{
    return v.as_integer();
}

template <>
double value_as<double>(const value &v)
{
    if ( v.is_real() )
	return v.as_real();
    if ( v.is_integer() )
	return static_cast<double>(v.as_integer());
    throw std::runtime_error("madc::program::call expected real-compatible argument");
}

template <>
const char *value_as<const char *>(const value &v)
{
    return v.as_string().c_str();
}

template <typename T>
value value_from(T v);

template <>
value value_from<bool>(bool v)
{
    return value(v);
}

template <>
value value_from<int64_t>(int64_t v)
{
    return value(v);
}

template <>
value value_from<double>(double v)
{
    return value(v);
}

template <>
value value_from<const char *>(const char *v)
{
    return value(v);
}

bool value_from_variable(Variable *var, value &out)
{
    if ( !var || !var->type || !var->data )
	return false;

    if ( var->count != 1 || var->is_fixed_array() || var->is_vla() )
	return false;

    DataDef *type = var->type;
    if ( type == &ddBOOL )
    {
	out = value(static_cast<bool>(*static_cast<bool *>(var->data)));
	return true;
    }
    if ( type->is_integer() || type->is_pointer() )
    {
	out = value(static_cast<int64_t>(var->get<int64_t>()));
	return true;
    }
    if ( type->is_real() )
    {
	out = value(static_cast<double>(var->get<double>()));
	return true;
    }
    if ( type->rawtype() == DataType::dtSTRING )
    {
	out = value(*static_cast<std::string *>(var->data));
	return true;
    }
    return false;
}

bool set_variable_from_value(Variable *var, const value &in)
{
    if ( !var || !var->type || !var->data )
	return false;

    if ( var->count != 1 || var->is_fixed_array() || var->is_vla() )
	return false;

    DataDef *type = var->type;
    if ( type == &ddBOOL )
    {
	*static_cast<bool *>(var->data) = in.as_boolean();
	var->modified();
	return true;
    }
    if ( type->is_integer() || type->is_pointer() )
    {
	if ( !in.is_integer() )
	    return false;
	int64_t v = in.as_integer();
	if ( type == &ddCHAR )
	    *static_cast<char *>(var->data) = static_cast<char>(v);
	else if ( type == &ddINT || type == &ddINT64 )
	    *static_cast<int64_t *>(var->data) = v;
	else if ( type == &ddINT8 )
	    *static_cast<int8_t *>(var->data) = static_cast<int8_t>(v);
	else if ( type == &ddINT16 || type == &ddINT24 )
	    *static_cast<int16_t *>(var->data) = static_cast<int16_t>(v);
	else if ( type == &ddINT32 )
	    *static_cast<int32_t *>(var->data) = static_cast<int32_t>(v);
	else if ( type == &ddUINT8 )
	    *static_cast<uint8_t *>(var->data) = static_cast<uint8_t>(v);
	else if ( type == &ddUINT16 || type == &ddUINT24 )
	    *static_cast<uint16_t *>(var->data) = static_cast<uint16_t>(v);
	else if ( type == &ddUINT32 )
	    *static_cast<uint32_t *>(var->data) = static_cast<uint32_t>(v);
	else if ( type == &ddUINT64 )
	    *static_cast<uint64_t *>(var->data) = static_cast<uint64_t>(v);
	else
	    return false;
	var->modified();
	return true;
    }
    if ( type->is_real() )
    {
	double d = in.is_real() ? in.as_real() : static_cast<double>(in.as_integer());
	if ( type == &ddFLOAT )
	    *static_cast<float *>(var->data) = static_cast<float>(d);
	else
	    *static_cast<double *>(var->data) = d;
	var->modified();
	return true;
    }
    if ( type->rawtype() == DataType::dtSTRING )
    {
	*static_cast<std::string *>(var->data) = in.as_string();
	var->modified();
	return true;
    }
    return false;
}

template <typename R>
bool call_target0(void *fn, value *result)
{
    typedef R (*fn_t)();
    R ret = reinterpret_cast<fn_t>(fn)();
    if ( result )
	*result = value_from<R>(ret);
    return true;
}

template <>
bool call_target0<void>(void *fn, value *result)
{
    typedef void (*fn_t)();
    reinterpret_cast<fn_t>(fn)();
    if ( result )
	*result = value();
    return true;
}

template <typename R, typename A0>
bool call_target1(void *fn, const value &a0, value *result)
{
    typedef R (*fn_t)(A0);
    R ret = reinterpret_cast<fn_t>(fn)(value_as<A0>(a0));
    if ( result )
	*result = value_from<R>(ret);
    return true;
}

template <typename A0>
bool call_target1_void(void *fn, const value &a0, value *result)
{
    typedef void (*fn_t)(A0);
    reinterpret_cast<fn_t>(fn)(value_as<A0>(a0));
    if ( result )
	*result = value();
    return true;
}

template <typename R, typename A0, typename A1>
bool call_target2(void *fn, const value &a0, const value &a1, value *result)
{
    typedef R (*fn_t)(A0, A1);
    R ret = reinterpret_cast<fn_t>(fn)(value_as<A0>(a0), value_as<A1>(a1));
    if ( result )
	*result = value_from<R>(ret);
    return true;
}

template <typename A0, typename A1>
bool call_target2_void(void *fn, const value &a0, const value &a1, value *result)
{
    typedef void (*fn_t)(A0, A1);
    reinterpret_cast<fn_t>(fn)(value_as<A0>(a0), value_as<A1>(a1));
    if ( result )
	*result = value();
    return true;
}

} // namespace

struct program::impl
{
    struct invoke_snapshot
    {
	uint64_t cpu_microseconds = 0;
	uint64_t resident_bytes = 0;
	uint64_t output_bytes = 0;
    };

    MadcEngine engine;
    std::unique_ptr<Program> pgm;
    std::vector<error> public_diagnostics;
    error public_last_error;
    bool has_public_last_error = false;
    bool runtime_initialized = false;
    compile_options current_compile_options;
    security_policy current_security_policy;
    invoke_limits current_invoke_limits;

    impl()
    {
	engine.tee_output_to_buffer();
	engine.capture_error_to_buffer();
	reset_program();
    }

    void reset_program()
    {
	engine.registration_policy = registration_policy_from_compile_options(current_compile_options);
	pgm = engine.create_program();
	runtime_initialized = false;
	clear_public_errors();
    }

    void set_compile_options(const compile_options &options)
    {
	current_compile_options = options;
	current_security_policy = security_policy_from_compile_options(options,
								 current_security_policy.mode);
	reset_program();
    }

    void set_security_policy(const security_policy &policy)
    {
	current_security_policy = policy;
	current_compile_options = compile_options_from_security_policy(policy);
	reset_program();
    }

    void set_invoke_limits(const invoke_limits &limits)
    {
	current_invoke_limits = limits;
    }

    void clear_public_errors()
    {
	public_diagnostics.clear();
	public_last_error = error();
	has_public_last_error = false;
    }

    void sync_public_errors(const std::string &display_file = std::string(),
			    const std::string &actual_file = std::string())
    {
	clear_public_errors();
	public_diagnostics = make_errors_from_program_diagnostics(*pgm);
	if ( !display_file.empty() && !actual_file.empty() )
	{
	    for ( std::size_t i = 0; i < public_diagnostics.size(); ++i )
	    {
		if ( public_diagnostics[i].file == actual_file )
		    public_diagnostics[i].file = display_file;
	    }
	}

	if ( !pgm->last_error.has_error )
	    return;

	error::phase public_phase = error::phase::unknown;
	const Program::Diagnostic *diag = pgm->last_diagnostic();
	if ( diag )
	    public_phase = phase_from_program(diag->phase);

	std::string file = pgm->last_error.file;
	if ( !display_file.empty() && !actual_file.empty() && file == actual_file )
	    file = display_file;

	public_last_error = error(error::severity::error,
				  public_phase,
				  pgm->last_error.message,
				  file,
				  pgm->last_error.line,
				  pgm->last_error.column);
	has_public_last_error = true;
    }

    bool compile_loaded_file(const std::string &path)
    {
	TokenProgram *tp = pgm->tokenize(path.c_str());
	if ( !tp )
	{
	    sync_public_errors();
	    return false;
	}
	if ( !pgm->parse(tp) )
	{
	    sync_public_errors();
	    return false;
	}
	if ( !pgm->compile() )
	{
	    sync_public_errors();
	    return false;
	}
	sync_public_errors();
	return true;
    }

    bool with_temp_source(const std::string &source,
			  const std::string &virtual_filename,
			  bool (impl::*fn)(const std::string &, const std::string &))
    {
	std::string path = temp_source_template();
	std::vector<char> writable(path.begin(), path.end());
	writable.push_back('\0');
	int fd = mkstemp(&writable[0]);
	if ( fd < 0 )
	{
	    clear_public_errors();
	    public_last_error = error(error::severity::error,
				      error::phase::runtime,
				      "failed to create temporary source file");
	    has_public_last_error = true;
	    public_diagnostics.push_back(public_last_error);
	    return false;
	}

	path.assign(&writable[0]);
	close(fd);

	std::ofstream out(path.c_str(), std::ios::binary);
	if ( !out )
	{
	    unlink(path.c_str());
	    clear_public_errors();
	    public_last_error = error(error::severity::error,
				      error::phase::runtime,
				      "failed to open temporary source file for writing",
				      path);
	    has_public_last_error = true;
	    public_diagnostics.push_back(public_last_error);
	    return false;
	}
	out << source;
	out.close();

	bool ok = (this->*fn)(path, virtual_filename.empty() ? path : virtual_filename);
	unlink(path.c_str());
	return ok;
    }

    bool exec_file_with_display(const std::string &path, const std::string &display_file)
    {
	reset_program();
	if ( !compile_loaded_file(path) )
	{
	    if ( display_file != path )
		sync_public_errors(display_file, path);
	    return false;
	}

	bool ok = invoke_with_limits("exec", [this]() -> bool {
	    pgm->execute();
	    runtime_initialized = !pgm->last_error.has_error;
	    sync_public_errors();
	    return !pgm->last_error.has_error;
	});
	if ( !ok && has_public_last_error && !pgm->last_error.has_error )
	    return false;
	if ( display_file != path )
	    sync_public_errors(display_file, path);
	else
	    sync_public_errors();
	return ok;
    }

    bool compile_file_with_display(const std::string &path, const std::string &display_file)
    {
	reset_program();
	bool ok = compile_loaded_file(path);
	if ( display_file != path )
	    sync_public_errors(display_file, path);
	return ok;
    }

    bool eval_file_with_display(const std::string &path,
				const std::string &display_file,
				value *result)
    {
	if ( !compile_file_with_display(path, display_file) )
	    return false;
	return call(eval_entry_name(), std::vector<value>(), result);
    }

    bool register_function(const std::string &name,
			   native_function callback,
			   const native_signature &signature)
    {
	if ( name.empty() )
	{
	    clear_public_errors();
	    public_last_error = error(error::severity::error,
				      error::phase::runtime,
				      "register_function requires a non-empty name");
	    has_public_last_error = true;
	    public_diagnostics.push_back(public_last_error);
	    return false;
	}
	if ( !callback )
	{
	    clear_public_errors();
	    public_last_error = error(error::severity::error,
				      error::phase::runtime,
				      "register_function requires a non-null callback");
	    has_public_last_error = true;
	    public_diagnostics.push_back(public_last_error);
	    return false;
	}

	engine.populate_default_registries();

	datatype_vec_t params;
	params.push_back(datatype_from_native_type(signature.returns));
	for ( std::size_t i = 0; i < signature.parameters.size(); ++i )
	    params.push_back(datatype_from_native_type(signature.parameters[i]));

	engine.builtin_registry.add_core_function(name,
						  params,
						  reinterpret_cast<fVOIDFUNC>(callback));
	reset_program();
	return true;
    }

    bool fail_runtime(const std::string &message)
    {
	clear_public_errors();
	public_last_error = error(error::severity::error,
				  error::phase::runtime,
				  message);
	has_public_last_error = true;
	public_diagnostics.push_back(public_last_error);
	return false;
    }

    invoke_snapshot capture_invoke_snapshot()
    {
	invoke_snapshot snap;
	snap.cpu_microseconds = current_cpu_microseconds();
	snap.resident_bytes = current_resident_bytes();
	snap.output_bytes = engine.output_buffer_str().size()
	    + engine.error_buffer_str().size();
	return snap;
    }

    bool enforce_invoke_limits(const std::string &op_name,
			       const invoke_snapshot &before)
    {
	if ( current_invoke_limits.cpu_ms > 0 )
	{
	    uint64_t after_cpu = current_cpu_microseconds();
	    uint64_t used_cpu = after_cpu >= before.cpu_microseconds
		? after_cpu - before.cpu_microseconds
		: 0;
	    uint64_t limit_cpu = current_invoke_limits.cpu_ms * UINT64_C(1000);
	    if ( used_cpu > limit_cpu )
	    {
		std::ostringstream os;
		os << "program::" << op_name
		   << " exceeded cpu_ms limit (" << current_invoke_limits.cpu_ms
		   << " ms, used " << (used_cpu / 1000) << " ms)";
		return fail_runtime(os.str());
	    }
	}

	if ( current_invoke_limits.memory_bytes > 0 )
	{
	    uint64_t after_resident = current_resident_bytes();
	    uint64_t used_resident = after_resident >= before.resident_bytes
		? after_resident - before.resident_bytes
		: 0;
	    if ( used_resident > current_invoke_limits.memory_bytes )
	    {
		std::ostringstream os;
		os << "program::" << op_name
		   << " exceeded memory_bytes limit ("
		   << current_invoke_limits.memory_bytes
		   << " bytes, grew " << used_resident << " bytes)";
		return fail_runtime(os.str());
	    }
	}

	if ( current_invoke_limits.output_bytes > 0 )
	{
	    uint64_t after_output = engine.output_buffer_str().size()
		+ engine.error_buffer_str().size();
	    uint64_t used_output = after_output >= before.output_bytes
		? after_output - before.output_bytes
		: 0;
	    if ( used_output > current_invoke_limits.output_bytes )
	    {
		std::ostringstream os;
		os << "program::" << op_name
		   << " exceeded output_bytes limit ("
		   << current_invoke_limits.output_bytes
		   << " bytes, produced " << used_output << " bytes)";
		return fail_runtime(os.str());
	    }
	}

	return true;
    }

    bool invoke_with_limits(const std::string &op_name,
			    const std::function<bool()> &fn)
    {
	engine.clear_output_buffer();
	engine.clear_error_buffer();
	invoke_snapshot before = capture_invoke_snapshot();
	if ( !fn() )
	    return false;
	return enforce_invoke_limits(op_name, before);
    }

    bool ensure_runtime_initialized()
    {
	if ( runtime_initialized )
	    return true;
	if ( !pgm || !pgm->root_fn )
	    return fail_runtime("program::call requires a successfully compiled program");

	pgm->clear_diagnostics();
	pgm->clear_error();
	pgm->root_fn();
	sync_public_errors();
	if ( pgm->last_error.has_error )
	    return false;
	runtime_initialized = true;
	return true;
    }

    bool dispatch_call0(void *fn, native_type ret_type, value *result)
    {
	switch ( ret_type )
	{
	    case native_type::void_type: return call_target0<void>(fn, result);
	    case native_type::boolean:   return call_target0<bool>(fn, result);
	    case native_type::integer:   return call_target0<int64_t>(fn, result);
	    case native_type::real:      return call_target0<double>(fn, result);
	    case native_type::c_string:  return call_target0<const char *>(fn, result);
	}
	return false;
    }

    template <typename A0>
    bool dispatch_call1_ret(void *fn, native_type ret_type, const value &arg0, value *result)
    {
	switch ( ret_type )
	{
	    case native_type::void_type: return call_target1_void<A0>(fn, arg0, result);
	    case native_type::boolean:   return call_target1<bool, A0>(fn, arg0, result);
	    case native_type::integer:   return call_target1<int64_t, A0>(fn, arg0, result);
	    case native_type::real:      return call_target1<double, A0>(fn, arg0, result);
	    case native_type::c_string:  return call_target1<const char *, A0>(fn, arg0, result);
	}
	return false;
    }

    bool dispatch_call1(void *fn, native_type ret_type, native_type arg0_type,
			const value &arg0, value *result)
    {
	switch ( arg0_type )
	{
	    case native_type::boolean: return dispatch_call1_ret<bool>(fn, ret_type, arg0, result);
	    case native_type::integer: return dispatch_call1_ret<int64_t>(fn, ret_type, arg0, result);
	    case native_type::real:    return dispatch_call1_ret<double>(fn, ret_type, arg0, result);
	    case native_type::c_string:return dispatch_call1_ret<const char *>(fn, ret_type, arg0, result);
	    case native_type::void_type: break;
	}
	return false;
    }

    template <typename A0, typename A1>
    bool dispatch_call2_ret(void *fn, native_type ret_type,
			    const value &arg0, const value &arg1, value *result)
    {
	switch ( ret_type )
	{
	    case native_type::void_type: return call_target2_void<A0, A1>(fn, arg0, arg1, result);
	    case native_type::boolean:   return call_target2<bool, A0, A1>(fn, arg0, arg1, result);
	    case native_type::integer:   return call_target2<int64_t, A0, A1>(fn, arg0, arg1, result);
	    case native_type::real:      return call_target2<double, A0, A1>(fn, arg0, arg1, result);
	    case native_type::c_string:  return call_target2<const char *, A0, A1>(fn, arg0, arg1, result);
	}
	return false;
    }

    template <typename A0>
    bool dispatch_call2_arg1(void *fn, native_type ret_type, native_type arg1_type,
			     const value &arg0, const value &arg1, value *result)
    {
	switch ( arg1_type )
	{
	    case native_type::boolean: return dispatch_call2_ret<A0, bool>(fn, ret_type, arg0, arg1, result);
	    case native_type::integer: return dispatch_call2_ret<A0, int64_t>(fn, ret_type, arg0, arg1, result);
	    case native_type::real:    return dispatch_call2_ret<A0, double>(fn, ret_type, arg0, arg1, result);
	    case native_type::c_string:return dispatch_call2_ret<A0, const char *>(fn, ret_type, arg0, arg1, result);
	    case native_type::void_type: break;
	}
	return false;
    }

    bool dispatch_call2(void *fn, native_type ret_type,
			native_type arg0_type, native_type arg1_type,
			const value &arg0, const value &arg1, value *result)
    {
	switch ( arg0_type )
	{
	    case native_type::boolean: return dispatch_call2_arg1<bool>(fn, ret_type, arg1_type, arg0, arg1, result);
	    case native_type::integer: return dispatch_call2_arg1<int64_t>(fn, ret_type, arg1_type, arg0, arg1, result);
	    case native_type::real:    return dispatch_call2_arg1<double>(fn, ret_type, arg1_type, arg0, arg1, result);
	    case native_type::c_string:return dispatch_call2_arg1<const char *>(fn, ret_type, arg1_type, arg0, arg1, result);
	    case native_type::void_type: break;
	}
	return false;
    }

    bool call(const std::string &name, const std::vector<value> &args, value *result)
    {
	return invoke_with_limits("call", [this, &name, &args, result]() -> bool {
	    if ( !ensure_runtime_initialized() )
		return false;

	    std::string id = name;
	    Variable *var = pgm->findVariable(id);
	    if ( !var )
		return fail_runtime("program::call cannot find function '" + name + "'");
	    if ( !var->type || var->type->basetype() != BaseType::btFunct )
		return fail_runtime("program::call target '" + name + "' is not a function");

	    Method *method = static_cast<Method *>(var->data);
	    if ( !method || !method->x86code )
		return fail_runtime("program::call target '" + name + "' has no callable code");

	    FuncDef *func = static_cast<FuncDef *>(method->returns.type);
	    if ( !func )
		return fail_runtime("program::call target '" + name + "' has no function metadata");
	    if ( func->is_multi_return() )
		return fail_runtime("program::call does not support multi-return functions yet");
	    if ( func->is_varargs )
		return fail_runtime("program::call does not support variadic functions yet");
	    if ( args.size() != func->parameters.size() )
		return fail_runtime("program::call argument count mismatch for '" + name + "'");
	    if ( args.size() > 2 )
		return fail_runtime("program::call currently supports up to 2 arguments");

	    native_type ret_type;
	    if ( !native_type_from_datadef(&func->returns, ret_type) )
		return fail_runtime("program::call does not support this return type yet");

	    std::vector<native_type> arg_types;
	    arg_types.reserve(func->parameters.size());
	    for ( std::size_t i = 0; i < func->parameters.size(); ++i )
	    {
		native_type arg_type;
		if ( !native_type_from_datadef(func->parameters[i], arg_type) )
		    return fail_runtime("program::call does not support this parameter type yet");
		arg_types.push_back(arg_type);
	    }

	    try
	    {
		switch ( args.size() )
		{
		    case 0:
			return dispatch_call0(method->x86code, ret_type, result);
		    case 1:
			return dispatch_call1(method->x86code, ret_type, arg_types[0], args[0], result);
		    case 2:
			return dispatch_call2(method->x86code, ret_type, arg_types[0], arg_types[1],
					      args[0], args[1], result);
		    default:
			break;
		}
	    }
	    catch ( const std::exception &e )
	    {
		return fail_runtime(e.what());
	    }

	    return fail_runtime("program::call could not dispatch the requested signature");
	});
    }

    bool get_global(const std::string &name, value *result)
    {
	return invoke_with_limits("get_global", [this, &name, result]() -> bool {
	    if ( !result )
		return fail_runtime("program::get_global requires a result destination");
	    if ( !ensure_runtime_initialized() )
		return false;

	    std::string id = name;
	    Variable *var = pgm->findVariable(id);
	    if ( !var )
		return fail_runtime("program::get_global cannot find variable '" + name + "'");
	    if ( !var->is_global() )
		return fail_runtime("program::get_global target '" + name + "' is not a global");
	    if ( var->type && var->type->basetype() == BaseType::btFunct )
		return fail_runtime("program::get_global target '" + name + "' is a function");

	    value out;
	    if ( !value_from_variable(var, out) )
		return fail_runtime("program::get_global does not support this variable type yet");
	    *result = out;
	    return true;
	});
    }

    bool set_global(const std::string &name, const value &new_value)
    {
	return invoke_with_limits("set_global", [this, &name, &new_value]() -> bool {
	    if ( !ensure_runtime_initialized() )
		return false;

	    std::string id = name;
	    Variable *var = pgm->findVariable(id);
	    if ( !var )
		return fail_runtime("program::set_global cannot find variable '" + name + "'");
	    if ( !var->is_global() )
		return fail_runtime("program::set_global target '" + name + "' is not a global");
	    if ( var->is_constant() )
		return fail_runtime("program::set_global target '" + name + "' is constant");
	    if ( var->type && var->type->basetype() == BaseType::btFunct )
		return fail_runtime("program::set_global target '" + name + "' is a function");

	    try
	    {
		if ( !set_variable_from_value(var, new_value) )
		    return fail_runtime("program::set_global does not support this variable type yet");
	    }
	    catch ( const std::exception &e )
	    {
		return fail_runtime(e.what());
	    }
	    return true;
	});
    }
};

program::program()
    : _impl(new impl())
{
}

program::~program()
{
}

program::program(program &&other) noexcept
    : _impl(std::move(other._impl))
{
}

program &program::operator=(program &&other) noexcept
{
    if ( this != &other )
	_impl = std::move(other._impl);
    return *this;
}

bool program::compile_file(const std::string &path)
{
    return _impl->compile_file_with_display(path, path);
}

bool program::exec_file(const std::string &path)
{
    return _impl->exec_file_with_display(path, path);
}

bool program::exec_string(const std::string &source, const std::string &virtual_filename)
{
    return _impl->with_temp_source(source,
				   virtual_filename,
				   &impl::exec_file_with_display);
}

bool program::eval(const std::string &source,
		   value *result,
		   const std::string &virtual_filename)
{
    std::string path = temp_source_template();
    std::vector<char> writable(path.begin(), path.end());
    writable.push_back('\0');
    int fd = mkstemp(&writable[0]);
    if ( fd < 0 )
    {
	_impl->clear_public_errors();
	_impl->public_last_error = error(error::severity::error,
					 error::phase::runtime,
					 "failed to create temporary source file");
	_impl->has_public_last_error = true;
	_impl->public_diagnostics.push_back(_impl->public_last_error);
	return false;
    }

    path.assign(&writable[0]);
    close(fd);

    std::ofstream out(path.c_str(), std::ios::binary);
    if ( !out )
    {
	unlink(path.c_str());
	_impl->clear_public_errors();
	_impl->public_last_error = error(error::severity::error,
					 error::phase::runtime,
					 "failed to open temporary source file for writing",
					 path);
	_impl->has_public_last_error = true;
	_impl->public_diagnostics.push_back(_impl->public_last_error);
	return false;
    }
    out << source;
    out.close();

    bool ok = _impl->eval_file_with_display(path,
					    virtual_filename.empty() ? path : virtual_filename,
					    result);
    unlink(path.c_str());
    return ok;
}

bool program::register_function(const std::string &name,
				native_function callback,
				const native_signature &signature)
{
    return _impl->register_function(name, callback, signature);
}

bool program::call(const std::string &name,
		   const std::vector<value> &args,
		   value *result)
{
    return _impl->call(name, args, result);
}

bool program::get_global(const std::string &name, value *result) const
{
    return _impl->get_global(name, result);
}

bool program::set_global(const std::string &name, const value &new_value)
{
    return _impl->set_global(name, new_value);
}

void program::set_compile_options(const compile_options &options)
{
    _impl->set_compile_options(options);
}

const compile_options &program::get_compile_options() const
{
    return _impl->current_compile_options;
}

void program::set_security_policy(const security_policy &policy)
{
    _impl->set_security_policy(policy);
}

const security_policy &program::get_security_policy() const
{
    return _impl->current_security_policy;
}

void program::set_invoke_limits(const invoke_limits &limits)
{
    _impl->set_invoke_limits(limits);
}

const invoke_limits &program::get_invoke_limits() const
{
    return _impl->current_invoke_limits;
}

const std::vector<error> &program::diagnostics() const
{
    return _impl->public_diagnostics;
}

const error *program::last_error() const
{
    if ( !_impl->has_public_last_error )
	return NULL;
    return &_impl->public_last_error;
}

bool program::has_error() const
{
    return _impl->has_public_last_error;
}

void program::clear_diagnostics()
{
    _impl->clear_public_errors();
}

} // namespace madc
