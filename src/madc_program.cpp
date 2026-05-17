// madc::program and madc::engine — see include/libmadc/program.h, engine.h

#include "libmadc/engine.h"
#include "libmadc/program.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dlfcn.h>
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
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern thread_local bool madc_verbose;
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

std::string ensure_trailing_newline(const std::string &source)
{
    if ( source.empty() || source[source.size() - 1] == '\n' )
	return source;
    return source + "\n";
}

const char *eval_entry_name()
{
    return "__madc_eval";
}

const char *expression_eval_name()
{
    return "__madc_eval_expression";
}

class temp_file
{
public:
    temp_file()
	: fd_(-1)
    {
    }

    ~temp_file()
    {
	close_and_unlink();
    }

    bool create(const char *pattern)
    {
	std::vector<char> writable;
	for ( const char *p = pattern; *p; ++p )
	    writable.push_back(*p);
	writable.push_back('\0');
	fd_ = mkstemp(&writable[0]);
	if ( fd_ < 0 )
	    return false;
	path_.assign(&writable[0]);
	return true;
    }

    int fd() const { return fd_; }
    const std::string &path() const { return path_; }

    void close_fd()
    {
	if ( fd_ >= 0 )
	{
	    close(fd_);
	    fd_ = -1;
	}
    }

    void close_and_unlink()
    {
	close_fd();
	if ( !path_.empty() )
	    unlink(path_.c_str());
	path_.clear();
    }

private:
    int fd_;
    std::string path_;
};

void write_u64(std::ostream &os, uint64_t value)
{
    os.write(reinterpret_cast<const char *>(&value), sizeof(value));
}

bool read_u64(std::istream &is, uint64_t &value)
{
    is.read(reinterpret_cast<char *>(&value), sizeof(value));
    return static_cast<bool>(is);
}

void write_i32(std::ostream &os, int32_t value)
{
    os.write(reinterpret_cast<const char *>(&value), sizeof(value));
}

bool read_i32(std::istream &is, int32_t &value)
{
    is.read(reinterpret_cast<char *>(&value), sizeof(value));
    return static_cast<bool>(is);
}

void write_string(std::ostream &os, const std::string &value)
{
    write_u64(os, static_cast<uint64_t>(value.size()));
    if ( !value.empty() )
	os.write(value.data(), static_cast<std::streamsize>(value.size()));
}

bool read_string(std::istream &is, std::string &value)
{
    uint64_t len = 0;
    if ( !read_u64(is, len) )
	return false;
    value.assign(static_cast<std::size_t>(len), '\0');
    if ( len > 0 )
	is.read(&value[0], static_cast<std::streamsize>(len));
    return static_cast<bool>(is);
}

void write_error_record(std::ostream &os, const error &err)
{
    write_i32(os, static_cast<int32_t>(err.level));
    write_i32(os, static_cast<int32_t>(err.stage));
    write_string(os, err.message);
    write_string(os, err.file);
    write_i32(os, err.line);
    write_i32(os, err.column);
}

bool read_error_record(std::istream &is, error &err)
{
    int32_t level = 0;
    int32_t stage = 0;
    int32_t line = 0;
    int32_t column = 0;
    if ( !read_i32(is, level) )
	return false;
    if ( !read_i32(is, stage) )
	return false;
    if ( !read_string(is, err.message) )
	return false;
    if ( !read_string(is, err.file) )
	return false;
    if ( !read_i32(is, line) )
	return false;
    if ( !read_i32(is, column) )
	return false;
    err.level = static_cast<error::severity>(level);
    err.stage = static_cast<error::phase>(stage);
    err.line = line;
    err.column = column;
    return true;
}

struct exec_child_report
{
    bool ok = false;
    bool has_last_error = false;
    bool has_result = false;
    error last_error;
    value result;
    std::vector<error> diagnostics;
};

void write_double(std::ostream &os, double value)
{
    os.write(reinterpret_cast<const char *>(&value), sizeof(value));
}

bool read_double(std::istream &is, double &value)
{
    is.read(reinterpret_cast<char *>(&value), sizeof(value));
    return static_cast<bool>(is);
}

bool write_value_record(std::ostream &os, const value &v)
{
    write_i32(os, static_cast<int32_t>(v.type()));
    switch ( v.type() )
    {
	case value::kind::null:
	    return static_cast<bool>(os);
	case value::kind::boolean:
	    write_i32(os, v.as_boolean() ? 1 : 0);
	    return static_cast<bool>(os);
	case value::kind::integer:
	    write_u64(os, static_cast<uint64_t>(v.as_integer()));
	    return static_cast<bool>(os);
	case value::kind::real:
	    write_double(os, v.as_real());
	    return static_cast<bool>(os);
	case value::kind::string:
	    write_string(os, v.as_string());
	    return static_cast<bool>(os);
	default:
	    break;
    }
    return false;
}

bool read_value_record(std::istream &is, value &v)
{
    int32_t kind = 0;
    if ( !read_i32(is, kind) )
	return false;

    switch ( static_cast<value::kind>(kind) )
    {
	case value::kind::null:
	    v = value();
	    return true;
	case value::kind::boolean:
	{
	    int32_t raw = 0;
	    if ( !read_i32(is, raw) )
		return false;
	    v = value(raw != 0);
	    return true;
	}
	case value::kind::integer:
	{
	    uint64_t raw = 0;
	    if ( !read_u64(is, raw) )
		return false;
	    v = value(static_cast<int64_t>(raw));
	    return true;
	}
	case value::kind::real:
	{
	    double raw = 0.0;
	    if ( !read_double(is, raw) )
		return false;
	    v = value(raw);
	    return true;
	}
	case value::kind::string:
	{
	    std::string raw;
	    if ( !read_string(is, raw) )
		return false;
	    v = value(raw);
	    return true;
	}
	default:
	    break;
    }
    return false;
}

bool write_exec_child_report(std::ostream &os, const exec_child_report &report)
{
    write_i32(os, report.ok ? 1 : 0);
    write_i32(os, report.has_last_error ? 1 : 0);
    write_i32(os, report.has_result ? 1 : 0);
    write_u64(os, static_cast<uint64_t>(report.diagnostics.size()));
    for ( std::size_t i = 0; i < report.diagnostics.size(); ++i )
	write_error_record(os, report.diagnostics[i]);
    if ( report.has_last_error )
	write_error_record(os, report.last_error);
    if ( report.has_result && !write_value_record(os, report.result) )
	return false;
    return static_cast<bool>(os);
}

bool read_exec_child_report(const std::string &path, exec_child_report &report)
{
    std::ifstream is(path.c_str(), std::ios::binary);
    if ( !is )
	return false;
    int32_t ok = 0;
    int32_t has_last_error = 0;
    int32_t has_result = 0;
    uint64_t diag_count = 0;
    if ( !read_i32(is, ok) )
	return false;
    if ( !read_i32(is, has_last_error) )
	return false;
    if ( !read_i32(is, has_result) )
	return false;
    if ( !read_u64(is, diag_count) )
	return false;
    report.ok = (ok != 0);
    report.has_last_error = (has_last_error != 0);
    report.has_result = (has_result != 0);
    report.diagnostics.clear();
    report.diagnostics.reserve(static_cast<std::size_t>(diag_count));
    for ( uint64_t i = 0; i < diag_count; ++i )
    {
	error diag;
	if ( !read_error_record(is, diag) )
	    return false;
	report.diagnostics.push_back(diag);
    }
    if ( report.has_last_error && !read_error_record(is, report.last_error) )
	return false;
    if ( report.has_result && !read_value_record(is, report.result) )
	return false;
    return true;
}

struct expression_function_spec
{
    const char *name;
    datatype_vec_t signature;
};
bool string_list_contains(const std::vector<std::string> &items, const std::string &value);
std::vector<expression_function_spec> expression_header_function_specs(const std::string &header);
std::vector<std::string> expression_allowed_function_names(const expression_policy &policy);
std::vector<std::string> collect_expression_token_calls(const std::deque<TokenBase *> &tokens);
void append_unique_strings(std::vector<std::string> &dst,
			   const std::vector<std::string> &src);
std::vector<std::string> expand_header_symbol_groups(const std::vector<std::string> &headers);
bool native_type_from_datadef(DataDef *type, program::native_type &out);
template <typename R>
bool call_target0(void *fn, value *result);

void copy_program_public_error(Program &dst, const Program &src)
{
    dst.diagnostics = src.diagnostics;
    dst.last_error = src.last_error;
}

bool fail_program_runtime(Program &dst,
			  const std::string &message,
			  const char *file = NULL,
			  int line = 0,
			  int column = 0)
{
    dst.clear_diagnostics();
    dst.clear_error();
    dst.set_error(Program::DiagnosticPhase::runtime, message, file, line, column);
    return false;
}

std::string build_expression_input_from_policy(const expression_policy &policy,
					       const std::string &expression)
{
    std::ostringstream source;
    for ( std::size_t i = 0; i < policy.allowed_headers.size(); ++i )
	source << "#include <" << policy.allowed_headers[i] << ">\n";
    source << expression << ";\n";
    return source.str();
}

DataDef *expression_result_datadef_internal(DataDef *expr_type, bool have_result)
{
    if ( !have_result )
	return &ddVOID;
    if ( !expr_type )
	return NULL;
    if ( expr_type->rawtype() == DataType::dtSTRING )
	return &ddCHARptr;
    program::native_type native;
    return native_type_from_datadef(expr_type, native) ? expr_type : NULL;
}

bool register_expression_header_functions(Program &pgm,
					  const expression_policy &policy,
					  const std::string &display_file)
{
    for ( std::size_t i = 0; i < policy.allowed_headers.size(); ++i )
    {
	std::vector<expression_function_spec> specs =
	    expression_header_function_specs(policy.allowed_headers[i]);
	for ( std::size_t j = 0; j < specs.size(); ++j )
	{
	    std::string name = specs[j].name;
	    if ( pgm.findVariable(name) )
		continue;
	    if ( !pgm.is_dynamic_symbol_allowed(name) )
		continue;
	    void *sym = dlsym(RTLD_DEFAULT, name.c_str());
	    if ( !sym )
		return fail_program_runtime(pgm,
					    std::string("program::eval_expression could not resolve symbol '")
					    + name + "' for header-group registration",
					    display_file.c_str());
	    pgm.addFunction(name, specs[j].signature, (fVOIDFUNC)sym);
	}
    }
    return true;
}

bool validate_expression_function_policy(Program &pgm,
					 const expression_policy &policy,
					 const std::deque<TokenBase *> &tokens,
					 const std::string &display_file)
{
    std::vector<std::string> calls = collect_expression_token_calls(tokens);
    if ( calls.empty() )
	return true;
    if ( !policy.allow_function_calls )
	return fail_program_runtime(pgm,
				    "program::eval_expression rejected input: function calls are not allowed",
				    display_file.c_str());

    std::vector<std::string> allowed = expression_allowed_function_names(policy);
    for ( std::size_t i = 0; i < calls.size(); ++i )
    {
	if ( string_list_contains(allowed, calls[i]) )
	    continue;
	return fail_program_runtime(pgm,
				    std::string("program::eval_expression rejected function call to '")
				    + calls[i] + "'",
				    display_file.c_str());
    }
    return true;
}

bool invoke_program_zero_arg_function(Program &pgm,
				      const std::string &name,
				      value &result)
{
    std::string id = name;
    Variable *var = pgm.findVariable(id);
    if ( !var )
	return fail_program_runtime(pgm,
				    "program::call cannot find function '" + name + "'");
    if ( !var->type || var->type->basetype() != BaseType::btFunct )
	return fail_program_runtime(pgm,
				    "program::call target '" + name + "' is not a function");

    Method *method = static_cast<Method *>(var->data);
    if ( !method || !method->x86code )
	return fail_program_runtime(pgm,
				    "program::call target '" + name + "' has no callable code");

    FuncDef *func = static_cast<FuncDef *>(method->returns.type);
    if ( !func )
	return fail_program_runtime(pgm,
				    "program::call target '" + name + "' has no function metadata");
    if ( func->is_multi_return() )
	return fail_program_runtime(pgm,
				    "program::call does not support multi-return functions yet");
    if ( func->is_varargs )
	return fail_program_runtime(pgm,
				    "program::call does not support variadic functions yet");
    if ( !func->parameters.empty() )
	return fail_program_runtime(pgm,
				    "program::call argument count mismatch for '" + name + "'");

    program::native_type ret_type;
    if ( !native_type_from_datadef(&func->returns, ret_type) )
	return fail_program_runtime(pgm,
				    "program::call does not support this return type yet");

    pgm.clear_diagnostics();
    pgm.clear_error();
    pgm.push_runtime_scope();
    pgm.root_fn();
    pgm.pop_runtime_scope();
    if ( pgm.last_error.has_error )
	return false;

    try
    {
	pgm.push_runtime_scope();
	bool ok = false;
	switch ( ret_type )
	{
	    case program::native_type::void_type:
	    {
		typedef void (*fn_t)();
		reinterpret_cast<fn_t>(method->x86code)();
		result = value();
		ok = true;
		break;
	    }
	    case program::native_type::boolean:   ok = call_target0<bool>(method->x86code, &result); break;
	    case program::native_type::integer:   ok = call_target0<int64_t>(method->x86code, &result); break;
	    case program::native_type::real:      ok = call_target0<double>(method->x86code, &result); break;
	    case program::native_type::c_string:  ok = call_target0<const char *>(method->x86code, &result); break;
	    case program::native_type::string_object: ok = call_target0<std::string *>(method->x86code, &result); break;
	}
	pgm.pop_runtime_scope();
	if ( ok )
	    return true;
    }
    catch ( const std::exception &e )
    {
	pgm.pop_runtime_scope();
	return fail_program_runtime(pgm, e.what());
    }

    return fail_program_runtime(pgm,
				"program::call could not dispatch the requested signature");
}

std::string read_text_file(const std::string &path)
{
    std::ifstream is(path.c_str(), std::ios::binary);
    std::ostringstream os;
    os << is.rdbuf();
    return os.str();
}

bool string_list_contains(const std::vector<std::string> &items, const std::string &value)
{
    for ( std::size_t i = 0; i < items.size(); ++i )
    {
	if ( items[i] == value )
	    return true;
    }
    return false;
}

void append_unique_strings(std::vector<std::string> &dst,
			   const std::vector<std::string> &src)
{
    for ( std::size_t i = 0; i < src.size(); ++i )
    {
	if ( !string_list_contains(dst, src[i]) )
	    dst.push_back(src[i]);
    }
}

std::vector<std::string> header_symbol_group(const std::string &header)
{
    if ( header == "math.h" )
    {
	return std::vector<std::string>{
	    "acos", "asin", "atan", "atan2", "ceil", "cos", "cosh",
	    "exp", "fabs", "floor", "fmod", "log", "log10", "pow",
	    "sin", "sinh", "sqrt", "tan", "tanh"
	};
    }
    return std::vector<std::string>();
}

std::vector<std::string> expand_header_symbol_groups(const std::vector<std::string> &headers)
{
    std::vector<std::string> expanded;
    for ( std::size_t i = 0; i < headers.size(); ++i )
	append_unique_strings(expanded, header_symbol_group(headers[i]));
    return expanded;
}

DataDef *expression_binding_datadef(const value &v);

DataDef *infer_expression_result_type(TokenBase *expr)
{
    if ( !expr )
	return NULL;

    DataDef *dt = expr->datadef();
    TokenOperator *op = dynamic_cast<TokenOperator *>(expr);
    if ( !op )
	return dt;

    DataDef *left = infer_expression_result_type(op->left);
    DataDef *right = infer_expression_result_type(op->right);
    if ( left && left->is_pointer() )
	return left;
    if ( right && right->is_pointer() )
	return right;
    if ( (left && left->is_real()) || (right && right->is_real()) )
	return &ddDOUBLE;
    if ( dt && dt->is_numeric() && dt != &ddINT )
	return dt;
    if ( left && left->is_integer() )
	return left;
    if ( right && right->is_integer() )
	return right;
    return dt;
}

std::vector<expression_function_spec> expression_header_function_specs(const std::string &header)
{
    if ( header == "math.h" )
    {
	return std::vector<expression_function_spec>{
	    {"acos",  datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"asin",  datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"atan",  datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"atan2", datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"ceil",  datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"cos",   datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"cosh",  datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"exp",   datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"fabs",  datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"floor", datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"fmod",  datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"log",   datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"log10", datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"pow",   datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"sin",   datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"sinh",  datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"sqrt",  datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"tan",   datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE}},
	    {"tanh",  datatype_vec_t{DataType::dtDOUBLE, DataType::dtDOUBLE}}
	};
    }
    return std::vector<expression_function_spec>();
}

bool is_identifier_start_char(char ch)
{
    return (ch >= 'a' && ch <= 'z')
	|| (ch >= 'A' && ch <= 'Z')
	|| ch == '_';
}

bool is_identifier_char(char ch)
{
    return is_identifier_start_char(ch)
	|| (ch >= '0' && ch <= '9');
}

bool is_expression_assignment_operator(const std::string &expression, std::size_t pos)
{
    if ( expression[pos] != '=' )
	return false;
    char prev = (pos > 0) ? expression[pos - 1] : '\0';
    char next = (pos + 1 < expression.size()) ? expression[pos + 1] : '\0';
    if ( next == '=' )
	return false;
    if ( prev == '!' || prev == '<' || prev == '>' || prev == '=' )
	return false;
    return true;
}

bool is_reserved_expression_identifier(const std::string &identifier)
{
    return identifier.compare(0, 7, "__madc_") == 0;
}

bool source_contains_explicit_eval_entry(const std::string &source)
{
    enum class scan_state
    {
	normal,
	single_quote,
	double_quote,
	line_comment,
	block_comment
    };

    auto skip_trivia = [&](std::size_t pos) -> std::size_t {
	while ( pos < source.size() )
	{
	    char sch = source[pos];
	    char snext = (pos + 1 < source.size()) ? source[pos + 1] : '\0';
	    if ( sch == ' ' || sch == '\t' || sch == '\r' || sch == '\n' )
	    {
		++pos;
		continue;
	    }
	    if ( sch == '/' && snext == '/' )
	    {
		pos += 2;
		while ( pos < source.size() && source[pos] != '\n' )
		    ++pos;
		continue;
	    }
	    if ( sch == '/' && snext == '*' )
	    {
		pos += 2;
		while ( pos + 1 < source.size()
		     && !(source[pos] == '*' && source[pos + 1] == '/') )
		    ++pos;
		if ( pos + 1 < source.size() )
		    pos += 2;
		continue;
	    }
	    break;
	}
	return pos;
    };

    scan_state state = scan_state::normal;
    for ( std::size_t i = 0; i < source.size(); ++i )
    {
	char ch = source[i];
	char next = (i + 1 < source.size()) ? source[i + 1] : '\0';
	switch ( state )
	{
	    case scan_state::normal:
		if ( ch == '"' )
		{
		    state = scan_state::double_quote;
		    continue;
		}
		if ( ch == '\'' )
		{
		    state = scan_state::single_quote;
		    continue;
		}
		if ( ch == '/' && next == '/' )
		{
		    state = scan_state::line_comment;
		    ++i;
		    continue;
		}
		if ( ch == '/' && next == '*' )
		{
		    state = scan_state::block_comment;
		    ++i;
		    continue;
		}
		if ( !is_identifier_start_char(ch) )
		    continue;
		{
		    std::size_t start = i;
		    while ( i + 1 < source.size() && is_identifier_char(source[i + 1]) )
			++i;
		    std::string identifier = source.substr(start, i - start + 1);
		    if ( identifier != eval_entry_name() )
			continue;
		    std::size_t probe = skip_trivia(i + 1);
		    if ( probe < source.size() && source[probe] == '(' )
			return true;
		}
		break;
	    case scan_state::single_quote:
		if ( ch == '\\' && next != '\0' )
		{
		    ++i;
		    continue;
		}
		if ( ch == '\'' )
		    state = scan_state::normal;
		break;
	    case scan_state::double_quote:
		if ( ch == '\\' && next != '\0' )
		{
		    ++i;
		    continue;
		}
		if ( ch == '"' )
		    state = scan_state::normal;
		break;
	    case scan_state::line_comment:
		if ( ch == '\n' )
		    state = scan_state::normal;
		break;
	    case scan_state::block_comment:
		if ( ch == '*' && next == '/' )
		{
		    state = scan_state::normal;
		    ++i;
		}
		break;
	}
    }
    return false;
}

std::string build_eval_body_wrapper_source(const std::string &source,
					   const char *return_type)
{
    std::ostringstream wrapped;
    wrapped << return_type << " " << eval_entry_name() << "() {\n"
	    << source;
    if ( source.empty() || source[source.size() - 1] != '\n' )
	wrapped << "\n";
    wrapped << "}\n";
    return wrapped.str();
}

const char *eval_body_wrapper_return_type(madc::program::native_type return_type)
{
    switch ( return_type )
    {
	case madc::program::native_type::boolean:
	    return "bool";
	case madc::program::native_type::integer:
	    return "int";
	case madc::program::native_type::real:
	    return "double";
	case madc::program::native_type::c_string:
	    return "char *";
	case madc::program::native_type::string_object:
	case madc::program::native_type::void_type:
	    break;
	    }
    return NULL;
}

bool is_valid_expression_binding_name(const std::string &identifier)
{
    if ( identifier.empty() )
	return false;
    if ( !is_identifier_start_char(identifier[0]) )
	return false;
    for ( std::size_t i = 1; i < identifier.size(); ++i )
    {
	if ( !is_identifier_char(identifier[i]) )
	    return false;
    }
    return !is_reserved_expression_identifier(identifier);
}

bool is_expression_keyword_identifier(const std::string &identifier)
{
    return identifier == "sizeof"
	|| identifier == "alignof"
	|| identifier == "typeof"
	|| identifier == "typeof_unqual";
}

enum class expression_scan_state
{
    normal,
    single_quote,
    double_quote,
    line_comment,
    block_comment
};

bool validate_expression_source(const std::string &expression, std::string &reason)
{
    expression_scan_state state = expression_scan_state::normal;
    int paren_depth = 0;
    // Track whether each paren nesting level was preceded by an
    // identifier (i.e. is a function call).  Commas are allowed
    // inside function-call parens (argument separators) but not
    // inside grouping parens (comma operator).
    std::vector<bool> paren_is_call;
    bool prev_was_identifier = false;
    for ( std::size_t i = 0; i < expression.size(); ++i )
    {
	char ch = expression[i];
	char next = (i + 1 < expression.size()) ? expression[i + 1] : '\0';
	switch ( state )
	{
	    case expression_scan_state::normal:
		if ( ch == '(' )
		{
		    ++paren_depth;
		    paren_is_call.push_back(prev_was_identifier);
		}
		else if ( ch == ')' && paren_depth > 0 )
		{
		    --paren_depth;
		    if ( !paren_is_call.empty() )
			paren_is_call.pop_back();
		}
		if ( ch == '"' )
		{
		    state = expression_scan_state::double_quote;
		    continue;
		}
		if ( ch == '\'' )
		{
		    state = expression_scan_state::single_quote;
		    continue;
		}
		if ( ch == '/' && next == '/' )
		{
		    state = expression_scan_state::line_comment;
		    ++i;
		    continue;
		}
		if ( ch == '/' && next == '*' )
		{
		    state = expression_scan_state::block_comment;
		    ++i;
		    continue;
		}
		if ( ch == ';' )
		{
		    reason = "statement separators are not allowed";
		    return false;
		}
		if ( ch == '{' || ch == '}' )
		{
		    reason = "statement blocks are not allowed";
		    return false;
		}
		if ( ch == '#' )
		{
		    reason = "preprocessor directives are not allowed";
		    return false;
		}
		if ( is_expression_assignment_operator(expression, i) )
		{
		    reason = "assignment operators are not allowed";
		    return false;
		}
		if ( (ch == '+' || ch == '-') && next == ch )
		{
		    reason = "increment and decrement operators are not allowed";
		    return false;
		}
		if ( ch == ',' )
		{
		    bool in_call = paren_depth > 0
			&& !paren_is_call.empty()
			&& paren_is_call.back();
		    if ( !in_call )
		    {
			reason = "comma sequencing is not allowed";
			return false;
		    }
		}
		if ( is_identifier_start_char(ch) )
		{
		    std::size_t start = i;
		    while ( i + 1 < expression.size() && is_identifier_char(expression[i + 1]) )
			++i;
		    std::string identifier = expression.substr(start, i - start + 1);
		    if ( is_reserved_expression_identifier(identifier) )
		    {
			reason = "reserved madc implementation identifiers are not allowed";
			return false;
		    }
		    prev_was_identifier = !is_expression_keyword_identifier(identifier);
		    break;
		}
		prev_was_identifier = false;
		break;
	    case expression_scan_state::single_quote:
		if ( ch == '\\' && next != '\0' )
		{
		    ++i;
		    continue;
		}
		if ( ch == '\'' )
		    state = expression_scan_state::normal;
		break;
	    case expression_scan_state::double_quote:
		if ( ch == '\\' && next != '\0' )
		{
		    ++i;
		    continue;
		}
		if ( ch == '"' )
		    state = expression_scan_state::normal;
		break;
	    case expression_scan_state::line_comment:
		if ( ch == '\n' )
		    state = expression_scan_state::normal;
		break;
	    case expression_scan_state::block_comment:
		if ( ch == '*' && next == '/' )
		{
		    state = expression_scan_state::normal;
		    ++i;
		}
		break;
	}
    }

    if ( state == expression_scan_state::single_quote || state == expression_scan_state::double_quote )
    {
	reason = "unterminated string or character literal";
	return false;
    }
    if ( state == expression_scan_state::block_comment )
    {
	reason = "unterminated block comment";
	return false;
    }
    return true;
}

bool is_allowed_expression_operator_id(TokenID id)
{
    switch ( id )
    {
	case TokenID::tkAssign:
	case TokenID::tkAddEq:
	case TokenID::tkSubEq:
	case TokenID::tkMulEq:
	case TokenID::tkDivEq:
	case TokenID::tkModEq:
	case TokenID::tkBandEq:
	case TokenID::tkBorEq:
	case TokenID::tkXorEq:
	case TokenID::tkBSLEq:
	case TokenID::tkBSREq:
	case TokenID::tkColEq:
	case TokenID::tkInc:
	case TokenID::tkDec:
	case TokenID::tkComma:
	    return false;
	default:
	    break;
    }
    return true;
}

bool set_expression_ast_rejection(std::string &reason, const std::string &detail)
{
    reason = std::string("unsupported AST node: ") + detail;
    return false;
}

std::vector<std::string> expression_allowed_function_names(const expression_policy &policy)
{
    std::vector<std::string> allowed = policy.allowed_functions;
    append_unique_strings(allowed, expand_header_symbol_groups(policy.allowed_headers));
    return allowed;
}

bool validate_expression_ast_call_target(const expression_policy &policy,
					 TokenCallFunc *call,
					 std::string &reason)
{
    if ( !policy.allow_function_calls )
	return set_expression_ast_rejection(reason, "function calls are not allowed");
    if ( !call )
	return set_expression_ast_rejection(reason, "null function call target");
    if ( call->src_node )
	return set_expression_ast_rejection(reason, "indirect function calls are disabled by expression policy");

    std::vector<std::string> allowed = expression_allowed_function_names(policy);
    if ( !string_list_contains(allowed, call->var.name) )
    {
	reason = std::string("rejected function call to '") + call->var.name + "'";
	return false;
    }
    return true;
}

bool validate_expression_ast(const expression_policy &policy,
			     TokenBase *expr,
			     std::string &reason);

bool validate_expression_ast_list(const std::vector<TokenBase *> &items,
				  const expression_policy &policy,
				  std::string &reason)
{
    for ( std::size_t i = 0; i < items.size(); ++i )
    {
	if ( !validate_expression_ast(policy, items[i], reason) )
	    return false;
    }
    return true;
}

bool validate_expression_ast_operator(const expression_policy &policy,
				      TokenOperator *op,
				      std::string &reason)
{
    if ( !op )
	return set_expression_ast_rejection(reason, "null operator");
    if ( !is_allowed_expression_operator_id(op->id()) )
	return set_expression_ast_rejection(reason, "mutation or sequencing operator");

    TokenTerQ *ter = dynamic_cast<TokenTerQ *>(op);
    if ( ter )
    {
	if ( !validate_expression_ast(policy, ter->condition, reason) )
	    return false;
	if ( !validate_expression_ast(policy, ter->true_expr, reason) )
	    return false;
	if ( !validate_expression_ast(policy, ter->false_expr, reason) )
	    return false;
	return true;
    }

    if ( op->left && !validate_expression_ast(policy, op->left, reason) )
	return false;
    if ( op->right && !validate_expression_ast(policy, op->right, reason) )
	return false;
    return true;
}

bool validate_expression_ast_call(const expression_policy &policy,
				  TokenCallFunc *call,
				  std::string &reason)
{
    if ( !call )
	return set_expression_ast_rejection(reason, "null function call");
    if ( !validate_expression_ast_call_target(policy, call, reason) )
	return false;
    if ( call->src_node && !validate_expression_ast(policy, call->src_node, reason) )
	return false;
    return validate_expression_ast_list(call->parameters, policy, reason);
}

std::vector<std::string> collect_expression_token_calls(const std::deque<TokenBase *> &tokens)
{
    std::vector<std::string> calls;
    for ( std::size_t i = 0; i < tokens.size(); ++i )
    {
	TokenBase *tb = tokens[i];
	if ( !tb || tb->type() != TokenType::ttIdentifier )
	    continue;
	if ( i + 1 >= tokens.size() || !tokens[i + 1] || tokens[i + 1]->id() != TokenID::tkOpBrk )
	    continue;
	std::string identifier = ((TokenIdent *)tb)->str;
	if ( is_expression_keyword_identifier(identifier) )
	    continue;
	if ( !string_list_contains(calls, identifier) )
	    calls.push_back(identifier);
    }
    return calls;
}

bool validate_expression_ast_member(const expression_policy &policy,
				    TokenMember *member,
				    std::string &reason)
{
    if ( !member )
	return set_expression_ast_rejection(reason, "null member expression");
    if ( !policy.allow_member_access )
	return set_expression_ast_rejection(reason, "member access is disabled by expression policy");
    if ( member->parent_expr && !validate_expression_ast(policy, member->parent_expr, reason) )
	return false;
    return validate_expression_ast_list(member->parameters, policy, reason);
}

bool validate_expression_ast_subscript(const expression_policy &policy,
				       TokenSubscript *sub,
				       std::string &reason)
{
    if ( !sub )
	return set_expression_ast_rejection(reason, "null subscript");
    if ( !policy.allow_subscript_access )
	return set_expression_ast_rejection(reason, "subscript access is disabled by expression policy");
    if ( !validate_expression_ast(policy, sub->index, reason) )
	return false;
    return validate_expression_ast_list(sub->extra_indices, policy, reason);
}

bool validate_expression_ast_subscript_expr(TokenSubscriptExpr *sub,
					    const expression_policy &policy,
					    std::string &reason)
{
    if ( !sub )
	return set_expression_ast_rejection(reason, "null subscript expression");
    if ( !policy.allow_subscript_access )
	return set_expression_ast_rejection(reason, "subscript access is disabled by expression policy");
    if ( !validate_expression_ast(policy, sub->base_expr, reason) )
	return false;
    return validate_expression_ast(policy, sub->index, reason);
}

bool validate_expression_ast(const expression_policy &policy,
			     TokenBase *expr,
			     std::string &reason)
{
    if ( !expr )
	return set_expression_ast_rejection(reason, "null expression");

    if ( dynamic_cast<TokenInt *>(expr)
      || dynamic_cast<TokenReal *>(expr)
      || dynamic_cast<TokenChar *>(expr)
      || dynamic_cast<TokenStr *>(expr)
      || dynamic_cast<TokenNullptr *>(expr)
      || dynamic_cast<TokenVar *>(expr) )
	return true;
    if ( TokenExprContextObject *ctx = dynamic_cast<TokenExprContextObject *>(expr) )
	return set_expression_ast_rejection(reason,
					    std::string("context path '") + ctx->path
					    + "' resolves to an object value");

    if ( TokenCallMethod *method = dynamic_cast<TokenCallMethod *>(expr) )
	return validate_expression_ast_member(policy, method, reason);
    if ( TokenMember *member = dynamic_cast<TokenMember *>(expr) )
	return validate_expression_ast_member(policy, member, reason);
    if ( TokenCallFunc *call = dynamic_cast<TokenCallFunc *>(expr) )
	return validate_expression_ast_call(policy, call, reason);
    if ( TokenSubscriptExpr *subexpr = dynamic_cast<TokenSubscriptExpr *>(expr) )
	return validate_expression_ast_subscript_expr(subexpr, policy, reason);
    if ( TokenSubscript *sub = dynamic_cast<TokenSubscript *>(expr) )
	return validate_expression_ast_subscript(policy, sub, reason);
    if ( TokenAddrExpr *addr = dynamic_cast<TokenAddrExpr *>(expr) )
    {
	if ( !policy.allow_pointer_operations )
	    return set_expression_ast_rejection(reason, "pointer operations are disabled by expression policy");
	return validate_expression_ast(policy, addr->expr, reason);
    }
    if ( dynamic_cast<TokenAddrOf *>(expr) )
	return policy.allow_pointer_operations
	    ? true
	    : set_expression_ast_rejection(reason, "pointer operations are disabled by expression policy");
    if ( TokenDerefExpr *deref = dynamic_cast<TokenDerefExpr *>(expr) )
    {
	if ( !policy.allow_pointer_operations )
	    return set_expression_ast_rejection(reason, "pointer operations are disabled by expression policy");
	return validate_expression_ast(policy, deref->expr, reason);
    }
    if ( dynamic_cast<TokenDeref *>(expr) )
	return policy.allow_pointer_operations
	    ? true
	    : set_expression_ast_rejection(reason, "pointer operations are disabled by expression policy");
    if ( dynamic_cast<TokenDerefStep *>(expr) )
	return set_expression_ast_rejection(reason, "pointer step dereference");
    if ( TokenCast *cast = dynamic_cast<TokenCast *>(expr) )
	return validate_expression_ast(policy, cast->expr, reason);
    if ( TokenOperator *op = dynamic_cast<TokenOperator *>(expr) )
	return validate_expression_ast_operator(policy, op, reason);

    return set_expression_ast_rejection(reason, "disallowed expression form");
}

std::size_t skip_expression_trivia(const std::string &expression, std::size_t pos)
{
    while ( pos < expression.size() )
    {
	char ch = expression[pos];
	char next = (pos + 1 < expression.size()) ? expression[pos + 1] : '\0';
	if ( ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' )
	{
	    ++pos;
	    continue;
	}
	if ( ch == '/' && next == '/' )
	{
	    pos += 2;
	    while ( pos < expression.size() && expression[pos] != '\n' )
		++pos;
	    continue;
	}
	if ( ch == '/' && next == '*' )
	{
	    pos += 2;
	    while ( pos + 1 < expression.size()
		 && !(expression[pos] == '*' && expression[pos + 1] == '/') )
		++pos;
	    if ( pos + 1 < expression.size() )
		pos += 2;
	    continue;
	}
	break;
    }
    return pos;
}

bool parse_expression_identifier_path(const std::string &expression,
				      std::size_t start,
				      std::string &canonical_path,
				      std::size_t &end)
{
    if ( start >= expression.size() || !is_identifier_start_char(expression[start]) )
	return false;

    std::size_t cursor = start;
    while ( cursor + 1 < expression.size() && is_identifier_char(expression[cursor + 1]) )
	++cursor;

    canonical_path.assign(expression, start, cursor - start + 1);
    end = cursor;

    while ( true )
    {
	std::size_t probe = skip_expression_trivia(expression, end + 1);
	if ( probe >= expression.size() || expression[probe] != '.' )
	    break;
	probe = skip_expression_trivia(expression, probe + 1);
	if ( probe >= expression.size() || !is_identifier_start_char(expression[probe]) )
	    break;

	std::size_t segment_end = probe;
	while ( segment_end + 1 < expression.size() && is_identifier_char(expression[segment_end + 1]) )
	    ++segment_end;
	canonical_path += ".";
	canonical_path.append(expression, probe, segment_end - probe + 1);
	end = segment_end;
    }
    return true;
}

std::vector<std::string> collect_expression_identifier_paths(const std::string &expression)
{
    std::vector<std::string> paths;
    expression_scan_state state = expression_scan_state::normal;
    for ( std::size_t i = 0; i < expression.size(); ++i )
    {
	char ch = expression[i];
	char next = (i + 1 < expression.size()) ? expression[i + 1] : '\0';
	switch ( state )
	{
	    case expression_scan_state::normal:
		if ( ch == '"' )
		{
		    state = expression_scan_state::double_quote;
		    continue;
		}
		if ( ch == '\'' )
		{
		    state = expression_scan_state::single_quote;
		    continue;
		}
		if ( ch == '/' && next == '/' )
		{
		    state = expression_scan_state::line_comment;
		    ++i;
		    continue;
		}
		if ( ch == '/' && next == '*' )
		{
		    state = expression_scan_state::block_comment;
		    ++i;
		    continue;
		}
		if ( !is_identifier_start_char(ch) )
		    continue;
		{
		    std::string path;
		    std::size_t end = i;
		    if ( !parse_expression_identifier_path(expression, i, path, end) )
			continue;
		    if ( is_expression_keyword_identifier(path) )
		    {
			i = end;
			continue;
		    }
		    if ( !string_list_contains(paths, path) )
			paths.push_back(path);
		    i = end;
		}
		break;
	    case expression_scan_state::single_quote:
		if ( ch == '\\' && next != '\0' )
		{
		    ++i;
		    continue;
		}
		if ( ch == '\'' )
		    state = expression_scan_state::normal;
		break;
	    case expression_scan_state::double_quote:
		if ( ch == '\\' && next != '\0' )
		{
		    ++i;
		    continue;
		}
		if ( ch == '"' )
		    state = expression_scan_state::normal;
		break;
	    case expression_scan_state::line_comment:
		if ( ch == '\n' )
		    state = expression_scan_state::normal;
		break;
	    case expression_scan_state::block_comment:
		if ( ch == '*' && next == '/' )
		{
		    state = expression_scan_state::normal;
		    ++i;
		}
		break;
	}
    }

    return paths;
}

bool is_expression_binding_value_supported(const value &v)
{
    return expression_binding_datadef(v) != NULL;
}

std::vector<std::string> split_expression_context_path(const std::string &path)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while ( start < path.size() )
    {
	std::size_t dot = path.find('.', start);
	if ( dot == std::string::npos )
	{
	    parts.push_back(path.substr(start));
	    break;
	}
	parts.push_back(path.substr(start, dot - start));
	start = dot + 1;
    }
    return parts;
}

bool validate_expression_context_path_terminal(const std::string &path,
					       const value &resolved,
					       std::string &reason)
{
    if ( resolved.is_object() )
    {
	reason = std::string("context path '") + path
	    + "' resolves to an object value; use a nested primitive field";
	return false;
    }
    if ( !is_expression_binding_value_supported(resolved) )
    {
	reason = std::string("context path '") + path
	    + "' resolves to unsupported value kind '"
	    + value::kind_name(resolved.type()) + "'";
	return false;
    }
    return true;
}

bool validate_expression_context_path(const value &context,
				      const std::string &path,
				      std::string &reason)
{
    std::vector<std::string> parts = split_expression_context_path(path);
    const value *current = &context;
    std::string resolved_path;
    for ( std::size_t i = 0; i < parts.size(); ++i )
    {
	if ( !current->is_object() )
	{
	    reason = std::string("context path '") + path
		+ "' cannot descend through non-object field '" + resolved_path + "'";
	    return false;
	}
	const std::map<std::string, value> &fields = current->as_object();
	std::map<std::string, value>::const_iterator it = fields.find(parts[i]);
	if ( it == fields.end() )
	{
	    reason = std::string("context path '") + path
		+ "' cannot find field '" + parts[i] + "'";
	    return false;
	}
	if ( !resolved_path.empty() )
	    resolved_path += ".";
	resolved_path += parts[i];
	current = &it->second;
    }
    return validate_expression_context_path_terminal(path, *current, reason);
}

bool validate_expression_context_paths(const value &context,
				       const std::string &expression,
				       std::string &reason)
{
    if ( context.is_null() )
	return true;

    const std::map<std::string, value> &root = context.as_object();
    std::vector<std::string> paths = collect_expression_identifier_paths(expression);
    for ( std::size_t i = 0; i < paths.size(); ++i )
    {
	std::vector<std::string> parts = split_expression_context_path(paths[i]);
	if ( parts.empty() )
	    continue;
	if ( root.find(parts[0]) == root.end() )
	    continue;
	if ( !validate_expression_context_path(context, paths[i], reason) )
	    return false;
    }
    return true;
}

bool flatten_expression_context_fields(const std::map<std::string, value> &fields,
				       const std::string &prefix,
				       const std::map<std::string, value> &base_bindings,
				       std::string &reason)
{
    for ( std::map<std::string, value>::const_iterator it = fields.begin();
	  it != fields.end(); ++it )
    {
	const std::string &field = it->first;
	const value &bound = it->second;
	if ( !is_valid_expression_binding_name(field) )
	{
	    reason = std::string("context field '") + field + "' is not a valid identifier";
	    return false;
	}
	if ( prefix.empty() && base_bindings.find(field) != base_bindings.end() )
	{
	    reason = std::string("context field '") + field
		+ "' collides with an explicit binding";
	    return false;
	}
	if ( bound.is_object() )
	{
	    if ( !flatten_expression_context_fields(bound.as_object(),
						    prefix.empty() ? field : prefix + "." + field,
						    base_bindings,
						    reason) )
		return false;
	}
    }
    return true;
}

bool build_expression_context_overlay(const value &context,
				      const std::map<std::string, value> &base_bindings,
				      std::map<std::string, value> &effective_bindings,
				      std::map<std::string, std::string> &rewrites,
				      std::string &reason)
{
    effective_bindings = base_bindings;
    rewrites.clear();
    if ( context.is_null() )
	return true;
    if ( !context.is_object() )
    {
	reason = "context must be an object value";
	return false;
    }
    return flatten_expression_context_fields(context.as_object(),
					    std::string(),
					    base_bindings,
					    reason);
}

std::string rewrite_expression_context_paths(const std::string &expression,
					     const std::map<std::string, std::string> &rewrites)
{
    if ( rewrites.empty() )
	return expression;

    std::string out;
    out.reserve(expression.size());
    expression_scan_state state = expression_scan_state::normal;
    for ( std::size_t i = 0; i < expression.size(); ++i )
    {
	char ch = expression[i];
	char next = (i + 1 < expression.size()) ? expression[i + 1] : '\0';
	switch ( state )
	{
	    case expression_scan_state::normal:
		if ( ch == '"' )
		{
		    state = expression_scan_state::double_quote;
		    out.push_back(ch);
		    continue;
		}
		if ( ch == '\'' )
		{
		    state = expression_scan_state::single_quote;
		    out.push_back(ch);
		    continue;
		}
		if ( ch == '/' && next == '/' )
		{
		    state = expression_scan_state::line_comment;
		    out.push_back(ch);
		    out.push_back(next);
		    ++i;
		    continue;
		}
		if ( ch == '/' && next == '*' )
		{
		    state = expression_scan_state::block_comment;
		    out.push_back(ch);
		    out.push_back(next);
		    ++i;
		    continue;
		}
		if ( !is_identifier_start_char(ch) )
		{
		    out.push_back(ch);
		    continue;
		}
		{
		    std::size_t start = i;
		    std::string path;
		    std::size_t end = i;
		    if ( !parse_expression_identifier_path(expression, start, path, end) )
		    {
			out.push_back(ch);
			continue;
		    }
		    std::map<std::string, std::string>::const_iterator found = rewrites.find(path);
		    if ( found != rewrites.end() )
		    {
			out.append(found->second);
			i = end;
			continue;
		    }
		    out.append(expression, start, end - start + 1);
		    i = end;
		}
		break;
	    case expression_scan_state::single_quote:
		out.push_back(ch);
		if ( ch == '\\' && next != '\0' )
		{
		    out.push_back(next);
		    ++i;
		    continue;
		}
		if ( ch == '\'' )
		    state = expression_scan_state::normal;
		break;
	    case expression_scan_state::double_quote:
		out.push_back(ch);
		if ( ch == '\\' && next != '\0' )
		{
		    out.push_back(next);
		    ++i;
		    continue;
		}
		if ( ch == '"' )
		    state = expression_scan_state::normal;
		break;
	    case expression_scan_state::line_comment:
		out.push_back(ch);
		if ( ch == '\n' )
		    state = expression_scan_state::normal;
		break;
	    case expression_scan_state::block_comment:
		out.push_back(ch);
		if ( ch == '*' && next == '/' )
		{
		    out.push_back(next);
		    state = expression_scan_state::normal;
		    ++i;
		}
		break;
	}
    }
    return out;
}

class fd_redirect_capture
{
public:
    fd_redirect_capture()
	: target_fd(-1), saved_fd(-1), temp_fd(-1)
    {
    }

    ~fd_redirect_capture()
    {
	restore();
    }

    bool begin(int fd)
    {
	target_fd = fd;
	saved_fd = dup(fd);
	if ( saved_fd < 0 )
	    return false;

	std::string tmpl = "/tmp/madc_fd_capture_XXXXXX";
	std::vector<char> writable(tmpl.begin(), tmpl.end());
	writable.push_back('\0');
	temp_fd = mkstemp(&writable[0]);
	if ( temp_fd < 0 )
	{
	    close(saved_fd);
	    saved_fd = -1;
	    return false;
	}

	path.assign(&writable[0]);
	if ( dup2(temp_fd, target_fd) < 0 )
	{
	    restore();
	    return false;
	}
	return true;
    }

    uint64_t captured_bytes() const
    {
	if ( temp_fd < 0 )
	    return 0;
	off_t end = lseek(temp_fd, 0, SEEK_END);
	return end >= 0 ? static_cast<uint64_t>(end) : 0;
    }

    void restore()
    {
	if ( saved_fd >= 0 && target_fd >= 0 )
	    dup2(saved_fd, target_fd);
	if ( saved_fd >= 0 )
	    close(saved_fd);
	if ( temp_fd >= 0 )
	    close(temp_fd);
	saved_fd = -1;
	temp_fd = -1;
	target_fd = -1;
	if ( !path.empty() )
	    unlink(path.c_str());
	path.clear();
    }

private:
    int target_fd;
    int saved_fd;
    int temp_fd;
    std::string path;
};

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

Program::RegistrationPolicy::RuntimeEvalChildPolicy
runtime_eval_child_policy_from_public(const runtime_eval_policy &policy)
{
    Program::RegistrationPolicy::RuntimeEvalChildPolicy out;
    out.enable_core_functions = policy.allow_core_functions;
    out.enable_process_functions = policy.allow_process_functions;
    out.enable_dlfcn_functions = policy.allow_dlfcn_functions;
    out.enable_std_namespace = policy.allow_std_namespace;
    out.enable_madc_namespace = policy.allow_madc_namespace;
    out.enable_php_namespace = policy.allow_php_namespace;
    out.enable_perl_namespace = policy.allow_perl_namespace;
    out.enable_python_namespace = policy.allow_python_namespace;
    out.enable_ruby_namespace = policy.allow_ruby_namespace;
    out.enable_js_namespace = policy.allow_js_namespace;
    out.enable_rust_namespace = policy.allow_rust_namespace;
    out.restrict_headers_to_allowlist = policy.restrict_headers_to_allowlist;
    out.restrict_dlfcn_symbols_to_allowlist = policy.restrict_dlfcn_symbols_to_allowlist;
    out.allowed_headers = policy.allowed_headers;
    out.allowed_dlfcn_symbols = policy.allowed_dlfcn_symbols;
    append_unique_strings(out.allowed_dlfcn_symbols,
			  expand_header_symbol_groups(out.allowed_headers));
    return out;
}

runtime_eval_policy clamp_runtime_eval_policy_for_authority_mode(const runtime_eval_policy &policy,
								 authority_mode mode)
{
    runtime_eval_policy clamped = policy;
    if ( mode == authority_mode::system_locked )
    {
	clamped.allow_process_functions = false;
	clamped.allow_dlfcn_functions = false;
    }
    return clamped;
}

Program::RegistrationPolicy
runtime_eval_registration_policy_for_source_child(const Program::RegistrationPolicy &parent)
{
    Program::RegistrationPolicy child;
    const Program::RegistrationPolicy::RuntimeEvalChildPolicy &source = parent.runtime_eval_source_policy;
    child.enable_core_functions = source.enable_core_functions;
    child.enable_process_functions = source.enable_process_functions;
    child.enable_dlfcn_functions = source.enable_dlfcn_functions;
    child.enable_runtime_eval_source_scope_access = parent.enable_runtime_eval_source_scope_access;
    child.enable_runtime_eval_expression_scope_access = parent.enable_runtime_eval_expression_scope_access;
    child.enable_std_namespace = source.enable_std_namespace;
    child.enable_madc_namespace = source.enable_madc_namespace;
    child.enable_php_namespace = source.enable_php_namespace;
    child.enable_perl_namespace = source.enable_perl_namespace;
    child.enable_python_namespace = source.enable_python_namespace;
    child.enable_ruby_namespace = source.enable_ruby_namespace;
    child.enable_js_namespace = source.enable_js_namespace;
    child.enable_rust_namespace = source.enable_rust_namespace;
    child.restrict_headers_to_allowlist = source.restrict_headers_to_allowlist;
    child.restrict_dlfcn_symbols_to_allowlist = source.restrict_dlfcn_symbols_to_allowlist;
    child.allowed_headers = source.allowed_headers;
    child.allowed_dlfcn_symbols = source.allowed_dlfcn_symbols;
    child.runtime_eval_source_policy = parent.runtime_eval_source_policy;
    return child;
}

Program::RegistrationPolicy registration_policy_from_compile_options(const compile_options &options)
{
    Program::RegistrationPolicy policy;
    policy.enable_core_functions = options.enable_core_functions;
    policy.enable_process_functions = options.enable_process_functions;
    policy.enable_dlfcn_functions = options.enable_dlfcn_functions;
    policy.enable_runtime_eval_source_scope_access = options.enable_runtime_eval_source_scope_access;
    policy.enable_runtime_eval_expression_scope_access = options.enable_runtime_eval_expression_scope_access;
    policy.enable_std_namespace = options.enable_std_namespace;
    policy.enable_madc_namespace = options.enable_madc_namespace;
    policy.enable_php_namespace = options.enable_php_namespace;
    policy.enable_perl_namespace = options.enable_perl_namespace;
    policy.enable_python_namespace = options.enable_python_namespace;
    policy.enable_ruby_namespace = options.enable_ruby_namespace;
    policy.enable_js_namespace = options.enable_js_namespace;
    policy.enable_rust_namespace = options.enable_rust_namespace;
    policy.restrict_headers_to_allowlist = false;
    policy.restrict_dlfcn_symbols_to_allowlist = false;
    policy.allowed_headers = options.allowed_headers;
    policy.allowed_dlfcn_symbols = options.allowed_dlfcn_symbols;
    append_unique_strings(policy.allowed_dlfcn_symbols,
			  expand_header_symbol_groups(policy.allowed_headers));
    policy.runtime_eval_source_policy = runtime_eval_child_policy_from_public(runtime_eval_policy());
    return policy;
}

compile_options compile_options_from_security_policy(const security_policy &policy)
{
    compile_options options;
    options.enable_core_functions = policy.allow_core_functions;
    options.enable_process_functions = policy.allow_process_functions;
    options.enable_dlfcn_functions = policy.allow_dlfcn_functions;
    options.enable_runtime_eval_source_scope_access = policy.allow_runtime_eval_source_scope_access;
    options.enable_runtime_eval_expression_scope_access = policy.allow_runtime_eval_expression_scope_access;
    options.enable_std_namespace = policy.allow_std_namespace;
    options.enable_madc_namespace = policy.allow_madc_namespace;
    options.enable_php_namespace = policy.allow_php_namespace;
    options.enable_perl_namespace = policy.allow_perl_namespace;
    options.enable_python_namespace = policy.allow_python_namespace;
    options.enable_ruby_namespace = policy.allow_ruby_namespace;
    options.enable_js_namespace = policy.allow_js_namespace;
    options.enable_rust_namespace = policy.allow_rust_namespace;
    options.allowed_headers = policy.allowed_headers;
    options.allowed_dlfcn_symbols = policy.allowed_dlfcn_symbols;
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
    policy.allow_runtime_eval_source_scope_access = options.enable_runtime_eval_source_scope_access;
    policy.allow_runtime_eval_expression_scope_access = options.enable_runtime_eval_expression_scope_access;
    policy.allow_std_namespace = options.enable_std_namespace;
    policy.allow_madc_namespace = options.enable_madc_namespace;
    policy.allow_php_namespace = options.enable_php_namespace;
    policy.allow_perl_namespace = options.enable_perl_namespace;
    policy.allow_python_namespace = options.enable_python_namespace;
    policy.allow_ruby_namespace = options.enable_ruby_namespace;
    policy.allow_js_namespace = options.enable_js_namespace;
    policy.allow_rust_namespace = options.enable_rust_namespace;
    policy.allowed_headers = options.allowed_headers;
    policy.allowed_dlfcn_symbols = options.allowed_dlfcn_symbols;
    return policy;
}

compile_options clamp_compile_options_for_authority_mode(const compile_options &options,
							 authority_mode mode)
{
    compile_options clamped = options;
    if ( mode == authority_mode::system_locked )
    {
	clamped.enable_process_functions = false;
	clamped.enable_dlfcn_functions = false;
	clamped.enable_runtime_eval_source_scope_access = false;
	clamped.enable_runtime_eval_expression_scope_access = false;
    }
    return clamped;
}

security_policy clamp_security_policy_for_authority_mode(const security_policy &policy)
{
    security_policy clamped = policy;
    if ( clamped.mode == authority_mode::system_locked )
    {
	clamped.execution = execution_mode::fork_per_invocation;
	clamped.allow_process_functions = false;
	clamped.allow_dlfcn_functions = false;
	clamped.allow_runtime_eval_source_scope_access = false;
	clamped.allow_runtime_eval_expression_scope_access = false;
    }
    return clamped;
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
	case program::native_type::string_object: return DataType::dtSTRING;
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
	case DataType::dtSTRING:
	    out = program::native_type::string_object;
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

template <>
std::string *value_as<std::string *>(const value &v)
{
    return const_cast<std::string *>(&v.as_string());
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

template <>
value value_from<std::string *>(std::string *v)
{
    return value(v ? *v : std::string());
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

DataDef *expression_binding_datadef(const value &v)
{
    switch ( v.type() )
    {
	case value::kind::boolean:
	    return &ddBOOL;
	case value::kind::integer:
	    return &ddINT64;
	case value::kind::real:
	    return &ddDOUBLE;
	case value::kind::string:
	    return &ddSTRING;
	default:
	    break;
    }
    return NULL;
}

bool install_runtime_eval_scope_globals(Program &pgm,
					const value *context,
					const std::string &display_file)
{
    if ( !context || !context->is_object() )
	return true;

    const std::map<std::string, value> &fields = context->as_object();
    for ( std::map<std::string, value>::const_iterator it = fields.begin();
	  it != fields.end(); ++it )
    {
	const std::string &name = it->first;
	const value &bound = it->second;
	DataDef *dt = expression_binding_datadef(bound);
	if ( !dt )
	    continue;
	if ( !is_valid_expression_binding_name(name) )
	    return fail_program_runtime(pgm,
					std::string("program::eval rejected scope binding name '")
					+ name + "'",
					display_file.c_str());
	std::string mutable_name = name;
	if ( pgm.findVariable(mutable_name) )
	    return fail_program_runtime(pgm,
					std::string("program::eval scope binding name '")
					+ name + "' collides with an existing symbol",
					display_file.c_str());
	Variable *var = pgm.addVariable(NULL, *dt, mutable_name, 1, NULL, true);
	if ( !var || !set_variable_from_value(var, bound) )
	    return fail_program_runtime(pgm,
					std::string("program::eval failed to install scope binding '")
					+ name + "'",
					display_file.c_str());
	var->makeconstant();
    }
    return true;
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

template <typename R, typename A0, typename A1, typename A2>
bool call_target3(void *fn, const value &a0, const value &a1, const value &a2, value *result)
{
    typedef R (*fn_t)(A0, A1, A2);
    R ret = reinterpret_cast<fn_t>(fn)(value_as<A0>(a0),
				       value_as<A1>(a1),
				       value_as<A2>(a2));
    if ( result )
	*result = value_from<R>(ret);
    return true;
}

template <typename A0, typename A1, typename A2>
bool call_target3_void(void *fn, const value &a0, const value &a1, const value &a2, value *result)
{
    typedef void (*fn_t)(A0, A1, A2);
    reinterpret_cast<fn_t>(fn)(value_as<A0>(a0),
			       value_as<A1>(a1),
			       value_as<A2>(a2));
    if ( result )
	*result = value();
    return true;
}

template <typename R, typename A0, typename A1, typename A2, typename A3>
bool call_target4(void *fn,
		  const value &a0,
		  const value &a1,
		  const value &a2,
		  const value &a3,
		  value *result)
{
    typedef R (*fn_t)(A0, A1, A2, A3);
    R ret = reinterpret_cast<fn_t>(fn)(value_as<A0>(a0),
				       value_as<A1>(a1),
				       value_as<A2>(a2),
				       value_as<A3>(a3));
    if ( result )
	*result = value_from<R>(ret);
    return true;
}

template <typename A0, typename A1, typename A2, typename A3>
bool call_target4_void(void *fn,
		       const value &a0,
		       const value &a1,
		       const value &a2,
		       const value &a3,
		       value *result)
{
    typedef void (*fn_t)(A0, A1, A2, A3);
    reinterpret_cast<fn_t>(fn)(value_as<A0>(a0),
			       value_as<A1>(a1),
			       value_as<A2>(a2),
			       value_as<A3>(a3));
    if ( result )
	*result = value();
    return true;
}

bool build_cpp_callback_trampoline(asmjit::JitRuntime &runtime,
				   void *callback_ptr,
				   program::native_function adapter_entry,
				   program::native_function &out)
{
    out = NULL;
    if ( !callback_ptr || !adapter_entry )
	return false;

    asmjit::CodeHolder code;
    code.init(runtime.environment());
    asmjit::x86::Assembler a(&code);

    // Insert the original callback pointer as a hidden first GP argument:
    // rdi <- callback, rsi <- old rdi, rdx <- old rsi, rcx <- old rdx, r8 <- old rcx.
    a.mov(asmjit::x86::r8, asmjit::x86::rcx);
    a.mov(asmjit::x86::rcx, asmjit::x86::rdx);
    a.mov(asmjit::x86::rdx, asmjit::x86::rsi);
    a.mov(asmjit::x86::rsi, asmjit::x86::rdi);
    a.mov(asmjit::x86::rdi, asmjit::imm(reinterpret_cast<uint64_t>(callback_ptr)));
    a.mov(asmjit::x86::rax, asmjit::imm(reinterpret_cast<uint64_t>(adapter_entry)));
    a.jmp(asmjit::x86::rax);

    void *fn = NULL;
    if ( runtime.add(&fn, &code) != asmjit::kErrorOk )
	return false;
    out = reinterpret_cast<program::native_function>(fn);
    return true;
}

} // namespace

struct program::impl
{
    struct invoke_snapshot
    {
	uint64_t cpu_microseconds = 0;
	uint64_t resident_bytes = 0;
    };

    struct verbose_scope
    {
	bool saved;
	verbose_scope(bool v) : saved(madc_verbose) { madc_verbose = v; }
	~verbose_scope() { madc_verbose = saved; }
    };

    MadcEngine owned_engine;
    MadcEngine *eng;
    std::unique_ptr<Program> pgm;
    std::vector<error> public_diagnostics;
    error public_last_error;
    bool has_public_last_error = false;
    bool runtime_initialized = false;
    bool aot_mode = false;
    compile_options current_compile_options;
    security_policy current_security_policy;
    expression_policy current_expression_policy;
    runtime_eval_policy current_runtime_eval_policy;
    std::map<std::string, value> current_expression_bindings;
    value current_expression_context;
    std::map<std::string, value> active_expression_bindings;
    invoke_limits current_invoke_limits;
    asmjit::JitRuntime callback_trampoline_runtime;
    std::vector<void *> callback_trampolines;

    impl()
	: eng(&owned_engine)
    {
	eng->capture_output_to_buffer();
	eng->capture_error_to_buffer();
	reset_program();
    }

    explicit impl(MadcEngine *external_engine,
		  const compile_options &opts,
		  const security_policy &sec,
		  const expression_policy &expr,
		  const runtime_eval_policy &rteval,
		  const invoke_limits &limits)
	: eng(external_engine),
	  current_compile_options(opts),
	  current_security_policy(sec),
	  current_expression_policy(expr),
	  current_runtime_eval_policy(rteval),
	  current_invoke_limits(limits)
    {
	reset_program();
    }

    ~impl()
    {
	for ( std::size_t i = 0; i < callback_trampolines.size(); ++i )
	    callback_trampoline_runtime.release(callback_trampolines[i]);
    }

    MadcEngine &engine() { return *eng; }

    void reset_program()
    {
	eng->registration_policy = registration_policy_from_compile_options(current_compile_options);
	eng->registration_policy.runtime_eval_source_policy =
	    runtime_eval_child_policy_from_public(current_runtime_eval_policy);
	pgm = eng->create_program();
	pgm->aot_tracking = aot_mode;
	runtime_initialized = false;
	clear_public_errors();
    }

    void set_compile_options(const compile_options &options)
    {
	current_compile_options = clamp_compile_options_for_authority_mode(options,
									 current_security_policy.mode);
	current_security_policy = clamp_security_policy_for_authority_mode(
	    security_policy_from_compile_options(current_compile_options,
						 current_security_policy.mode));
	current_runtime_eval_policy = clamp_runtime_eval_policy_for_authority_mode(
	    current_runtime_eval_policy,
	    current_security_policy.mode);
	reset_program();
    }

    void set_security_policy(const security_policy &policy)
    {
	current_security_policy = clamp_security_policy_for_authority_mode(policy);
	current_compile_options = clamp_compile_options_for_authority_mode(
	    compile_options_from_security_policy(current_security_policy),
	    current_security_policy.mode);
	current_runtime_eval_policy = clamp_runtime_eval_policy_for_authority_mode(
	    current_runtime_eval_policy,
	    current_security_policy.mode);
	reset_program();
    }

    void set_expression_policy(const expression_policy &policy)
    {
	current_expression_policy = policy;
    }

    void set_runtime_eval_policy(const runtime_eval_policy &policy)
    {
	current_runtime_eval_policy = clamp_runtime_eval_policy_for_authority_mode(
	    policy,
	    current_security_policy.mode);
	reset_program();
    }

    void set_expression_bindings(const std::map<std::string, value> &bindings)
    {
	current_expression_bindings = bindings;
    }

    void set_expression_context(const value &context)
    {
	current_expression_context = context;
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
	verbose_scope vs(eng->verbose);
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

    bool compile_loaded_source(const std::string &source,
			       const std::string &display_file)
    {
	verbose_scope vs(eng->verbose);
	if ( !pgm->load_buffer(ensure_trailing_newline(source), display_file) )
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

    bool with_temp_source(const std::string &source,
			  const std::string &virtual_filename,
			  bool (impl::*fn)(const std::string &, const std::string &, bool),
			  bool have_result)
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

	bool ok = (this->*fn)(path, virtual_filename.empty() ? path : virtual_filename, have_result);
	unlink(path.c_str());
	return ok;
    }

    bool exec_file_with_display(const std::string &path, const std::string &display_file)
    {
	verbose_scope vs(eng->verbose);
	reset_program();
	if ( !compile_loaded_file(path) )
	{
	    if ( display_file != path )
		sync_public_errors(display_file, path);
	    return false;
	}

	if ( current_security_policy.execution == execution_mode::fork_per_invocation )
	    return exec_compiled_in_child(path, display_file);

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

    bool exec_compiled_with_display(const std::string &actual_file,
				    const std::string &display_file)
    {
	if ( current_security_policy.execution == execution_mode::fork_per_invocation )
	    return exec_compiled_in_child(actual_file, display_file);

	bool ok = invoke_with_limits("exec", [this]() -> bool {
	    pgm->execute();
	    runtime_initialized = !pgm->last_error.has_error;
	    sync_public_errors();
	    return !pgm->last_error.has_error;
	});
	if ( !ok && has_public_last_error && !pgm->last_error.has_error )
	    return false;
	if ( display_file != actual_file )
	    sync_public_errors(display_file, actual_file);
	else
	    sync_public_errors();
	return ok;
    }

    bool exec_compiled_in_child(const std::string &path, const std::string &display_file)
    {
	temp_file child_stdout;
	temp_file child_stderr;
	temp_file child_report;
	if ( !child_stdout.create("/tmp/madc_exec_stdout_XXXXXX")
	  || !child_stderr.create("/tmp/madc_exec_stderr_XXXXXX")
	  || !child_report.create("/tmp/madc_exec_report_XXXXXX") )
	    return fail_runtime("program::exec could not create child capture files");

	pid_t pid = fork();
	if ( pid < 0 )
	    return fail_runtime("program::exec could not fork");

	if ( pid == 0 )
	{
	    int report_fd = dup(child_report.fd());
	    if ( dup2(child_stdout.fd(), STDOUT_FILENO) < 0
	      || dup2(child_stderr.fd(), STDERR_FILENO) < 0 )
		_exit(120);
	    child_stdout.close_fd();
	    child_stderr.close_fd();
	    child_report.close_fd();
	    if ( report_fd < 0 )
		_exit(121);

	    engine().reset_standard_streams();
	    pgm->execute();
	    runtime_initialized = !pgm->last_error.has_error;
	    if ( display_file != path )
		sync_public_errors(display_file, path);
	    else
		sync_public_errors();

	    exec_child_report report;
	    report.ok = !has_public_last_error;
	    report.has_last_error = has_public_last_error;
	    report.diagnostics = public_diagnostics;
	    if ( has_public_last_error )
		report.last_error = public_last_error;

	    bool wrote = false;
	    {
		std::ofstream os("/proc/self/fd/" + std::to_string(report_fd),
				 std::ios::binary | std::ios::trunc);
		if ( os )
		    wrote = write_exec_child_report(os, report);
	    }
	    close(report_fd);
	    std::cout.flush();
	    std::cerr.flush();
	    fflush(stdout);
	    fflush(stderr);
	    _exit(wrote ? (report.ok ? 0 : 1) : 122);
	}

	child_stdout.close_fd();
	child_stderr.close_fd();
	child_report.close_fd();

	int status = 0;
	struct rusage usage;
	std::memset(&usage, 0, sizeof(usage));
	if ( wait4(pid, &status, 0, &usage) < 0 )
	    return fail_runtime("program::exec could not wait for child");

	std::string stdout_text = read_text_file(child_stdout.path());
	std::string stderr_text = read_text_file(child_stderr.path());
	if ( !stdout_text.empty() )
	{
	    engine().output() << stdout_text;
	    engine().output().flush();
	}
	if ( !stderr_text.empty() )
	{
	    engine().error() << stderr_text;
	    engine().error().flush();
	}

	exec_child_report report;
	bool have_report = read_exec_child_report(child_report.path(), report);
	if ( have_report )
	{
	    public_diagnostics = report.diagnostics;
	    has_public_last_error = report.has_last_error;
	    public_last_error = report.has_last_error ? report.last_error : error();
	    runtime_initialized = false;
	}
	else
	{
	    clear_public_errors();
	    runtime_initialized = false;
	}

	uint64_t child_output_bytes = static_cast<uint64_t>(stdout_text.size())
	    + static_cast<uint64_t>(stderr_text.size());
	if ( current_invoke_limits.cpu_ms > 0 )
	{
	    uint64_t used_cpu = timeval_to_microseconds(usage.ru_utime)
		+ timeval_to_microseconds(usage.ru_stime);
	    uint64_t limit_cpu = current_invoke_limits.cpu_ms * UINT64_C(1000);
	    if ( used_cpu > limit_cpu )
	    {
		std::ostringstream os;
		os << "program::exec exceeded cpu_ms limit (" << current_invoke_limits.cpu_ms
		   << " ms, used " << (used_cpu / 1000) << " ms)";
		return fail_runtime(os.str());
	    }
	}
	if ( current_invoke_limits.memory_bytes > 0 )
	{
	    uint64_t used_resident = usage.ru_maxrss > 0
		? static_cast<uint64_t>(usage.ru_maxrss) * UINT64_C(1024)
		: 0;
	    if ( used_resident > current_invoke_limits.memory_bytes )
	    {
		std::ostringstream os;
		os << "program::exec exceeded memory_bytes limit ("
		   << current_invoke_limits.memory_bytes
		   << " bytes, peak " << used_resident << " bytes)";
		return fail_runtime(os.str());
	    }
	}
	if ( current_invoke_limits.output_bytes > 0
	  && child_output_bytes > current_invoke_limits.output_bytes )
	{
	    std::ostringstream os;
	    os << "program::exec exceeded output_bytes limit ("
	       << current_invoke_limits.output_bytes
	       << " bytes, produced " << child_output_bytes << " bytes)";
	    return fail_runtime(os.str());
	}

	if ( WIFSIGNALED(status) )
	{
	    std::ostringstream os;
	    os << "program::exec child terminated by signal " << WTERMSIG(status);
	    return fail_runtime(os.str());
	}
	if ( !have_report )
	    return fail_runtime("program::exec child did not return a report");
	return report.ok;
    }

    bool compile_file_with_display(const std::string &path, const std::string &display_file)
    {
	reset_program();
	bool ok = compile_loaded_file(path);
	if ( display_file != path )
	    sync_public_errors(display_file, path);
	return ok;
    }

    bool compile_source_with_display(const std::string &source,
				     const std::string &display_file)
    {
	reset_program();
	return compile_loaded_source(source, display_file);
    }

    bool eval_file_with_display(const std::string &path,
				const std::string &display_file,
				value *result)
    {
	if ( !compile_file_with_display(path, display_file) )
	    return false;
	return call(eval_entry_name(), std::vector<value>(), result);
    }

    bool eval_source_with_display(const std::string &source,
				  const std::string &display_file,
				  value *result)
    {
	verbose_scope vs(eng->verbose);
	if ( !compile_source_with_display(source, display_file) )
	    return false;
	return call(eval_entry_name(), std::vector<value>(), result);
    }

    bool eval_body_with_display(const std::string &source,
				const std::string &display_file,
				value *result,
				program::native_type return_type)
    {
	const char *wrapper_return_type = eval_body_wrapper_return_type(return_type);
	if ( !wrapper_return_type )
	    return fail_runtime("program::eval_body requires a non-void scalar or string return type");

	std::string effective_source = source;
	if ( !source_contains_explicit_eval_entry(source) )
	    effective_source = build_eval_body_wrapper_source(source, wrapper_return_type);
	return eval_source_with_display(effective_source, display_file, result);
    }

    bool exec_source_with_display(const std::string &source,
				  const std::string &display_file)
    {
	verbose_scope vs(eng->verbose);
	if ( !compile_source_with_display(source, display_file) )
	    return false;
	return exec_compiled_with_display(display_file, display_file);
    }

    void absorb_public_state(const program &other)
    {
	public_diagnostics = other.diagnostics();
	if ( other.has_error() && other.last_error() )
	{
	    has_public_last_error = true;
	    public_last_error = *other.last_error();
	}
	else
	{
	    has_public_last_error = false;
	    public_last_error = error();
	}
	runtime_initialized = false;
    }

    security_policy expression_security_policy() const
    {
	security_policy policy = current_security_policy;
	policy.allowed_headers = current_expression_policy.allowed_headers;
	policy.allowed_dlfcn_symbols = current_expression_policy.allowed_functions;
	policy.allow_core_functions = false;
	policy.allow_process_functions = false;
	policy.allow_std_namespace = false;
	policy.allow_madc_namespace = false;
	policy.allow_php_namespace = false;
	policy.allow_perl_namespace = false;
	policy.allow_python_namespace = false;
	policy.allow_ruby_namespace = false;
	policy.allow_js_namespace = false;
	policy.allow_rust_namespace = false;
	policy.allow_dlfcn_functions = current_expression_policy.allow_function_calls
	    && (!policy.allowed_headers.empty() || !policy.allowed_dlfcn_symbols.empty());
	return clamp_security_policy_for_authority_mode(policy);
    }

    std::string build_expression_input(const std::string &expression) const
    {
	std::ostringstream source;
	for ( std::size_t i = 0; i < current_expression_policy.allowed_headers.size(); ++i )
	    source << "#include <" << current_expression_policy.allowed_headers[i] << ">\n";
	source << expression << ";\n";
	return source.str();
    }

    DataDef *expression_result_datadef(DataDef *expr_type, bool have_result) const
    {
	if ( !have_result )
	    return &ddVOID;
	if ( !expr_type )
	    return NULL;
	if ( expr_type->rawtype() == DataType::dtSTRING )
	    return &ddCHARptr;
	program::native_type native;
	return native_type_from_datadef(expr_type, native) ? expr_type : NULL;
    }

    bool register_expression_header_functions(const std::string &display_file)
    {
	for ( std::size_t i = 0; i < current_expression_policy.allowed_headers.size(); ++i )
	{
	    std::vector<expression_function_spec> specs =
		expression_header_function_specs(current_expression_policy.allowed_headers[i]);
	    for ( std::size_t j = 0; j < specs.size(); ++j )
	    {
		std::string name = specs[j].name;
		if ( pgm->findVariable(name) )
		    continue;
		if ( !pgm->is_dynamic_symbol_allowed(name) )
		    continue;
		void *sym = dlsym(RTLD_DEFAULT, name.c_str());
		if ( !sym )
		{
		    clear_public_errors();
		    public_last_error = error(error::severity::error,
					      error::phase::runtime,
					      std::string("program::eval_expression could not resolve symbol '")
					      + name + "' for header-group registration",
					      display_file);
		    has_public_last_error = true;
		    public_diagnostics.push_back(public_last_error);
		    return false;
		}
		pgm->addFunction(name, specs[j].signature, (fVOIDFUNC)sym);
	    }
	}
	return true;
    }

    bool validate_expression_function_policy(const std::deque<TokenBase *> &tokens,
					    const std::string &display_file)
    {
	std::vector<std::string> calls = collect_expression_token_calls(tokens);
	if ( calls.empty() )
	    return true;
	if ( !current_expression_policy.allow_function_calls )
	{
	    clear_public_errors();
	    public_last_error = error(error::severity::error,
				      error::phase::runtime,
				      "program::eval_expression rejected input: function calls are not allowed",
				      display_file);
	    has_public_last_error = true;
	    public_diagnostics.push_back(public_last_error);
	    return false;
	}

	std::vector<std::string> allowed = expression_allowed_function_names(current_expression_policy);
	for ( std::size_t i = 0; i < calls.size(); ++i )
	{
	    if ( string_list_contains(allowed, calls[i]) )
		continue;
	    clear_public_errors();
	    public_last_error = error(error::severity::error,
				      error::phase::runtime,
				      std::string("program::eval_expression rejected function call to '")
				      + calls[i] + "'",
				      display_file);
	    has_public_last_error = true;
	    public_diagnostics.push_back(public_last_error);
	    return false;
	}
	return true;
    }

    bool register_expression_bindings(const std::map<std::string, value> &bindings,
				      const std::string &display_file)
    {
	for ( std::map<std::string, value>::const_iterator it = bindings.begin();
	      it != bindings.end(); ++it )
	{
	    const std::string &name = it->first;
	    const value &bound = it->second;
	    if ( !is_valid_expression_binding_name(name) )
	    {
		clear_public_errors();
		public_last_error = error(error::severity::error,
					  error::phase::runtime,
					  std::string("program::eval_expression rejected binding name '")
					  + name + "'",
					  display_file);
		has_public_last_error = true;
		public_diagnostics.push_back(public_last_error);
		return false;
	    }
	    std::string mutable_name = name;
	    if ( pgm->findVariable(mutable_name) )
	    {
		clear_public_errors();
		public_last_error = error(error::severity::error,
					  error::phase::runtime,
					  std::string("program::eval_expression binding name '")
					  + name + "' collides with an existing symbol",
					  display_file);
		has_public_last_error = true;
		public_diagnostics.push_back(public_last_error);
		return false;
	    }
	    DataDef *dt = expression_binding_datadef(bound);
	    if ( !dt )
	    {
		clear_public_errors();
		public_last_error = error(error::severity::error,
					  error::phase::runtime,
					  std::string("program::eval_expression cannot bind value kind '")
					  + value::kind_name(bound.type()) + "' for '" + name + "'",
					  display_file);
		has_public_last_error = true;
		public_diagnostics.push_back(public_last_error);
		return false;
	    }
	    Variable *var = pgm->addVariable(NULL, *dt, mutable_name, 1, NULL, true);
	    if ( !var || !set_variable_from_value(var, bound) )
	    {
		clear_public_errors();
		public_last_error = error(error::severity::error,
					  error::phase::runtime,
					  std::string("program::eval_expression failed to install binding '")
					  + name + "'",
					  display_file);
		has_public_last_error = true;
		public_diagnostics.push_back(public_last_error);
		return false;
	    }
	    var->makeconstant();
	}
	return true;
    }

    bool compile_expression_with_display(const std::string &path,
					 const std::string &display_file,
					 bool have_result)
    {
	reset_program();
	pgm->set_expression_context_root(current_expression_context.is_object()
					 ? &current_expression_context
					 : NULL);

	TokenProgram *tp = pgm->tokenize(path.c_str());
	if ( !tp )
	{
	    if ( display_file != path )
		sync_public_errors(display_file, path);
	    else
		sync_public_errors();
	    return false;
	}

	if ( !validate_expression_function_policy(pgm->tokens,
						 display_file.empty() ? path : display_file) )
	    return false;
	if ( !register_expression_header_functions(display_file.empty() ? path : display_file) )
	    return false;
	if ( !register_expression_bindings(active_expression_bindings,
					   display_file.empty() ? path : display_file) )
	    return false;

	TokenBase *expr = pgm->parse_expression_unit(tp);
	if ( !expr )
	{
	    if ( display_file != path )
		sync_public_errors(display_file, path);
	    else
		sync_public_errors();
	    return false;
	}

	std::string ast_validation_error;
	if ( !validate_expression_ast(current_expression_policy, expr, ast_validation_error) )
	{
	    clear_public_errors();
	    public_last_error = error(error::severity::error,
				      error::phase::runtime,
				      std::string("program::eval_expression rejected parsed expression: ")
				      + ast_validation_error,
				      display_file.empty() ? path : display_file,
				      expr->line, expr->column);
	    has_public_last_error = true;
	    public_diagnostics.push_back(public_last_error);
	    return false;
	}

	DataDef *return_type = expression_result_datadef(infer_expression_result_type(expr), have_result);
	if ( !return_type )
	{
	    clear_public_errors();
	    public_last_error = error(error::severity::error,
				      error::phase::runtime,
				      "program::eval_expression cannot marshal this result type",
				      display_file.empty() ? path : display_file,
				      expr->line, expr->column);
	    has_public_last_error = true;
	    public_diagnostics.push_back(public_last_error);
	    return false;
	}

	if ( !pgm->build_expression_function(tp,
					     expr,
					     return_type,
					     expression_eval_name(),
					     have_result) )
	{
	    clear_public_errors();
	    public_last_error = error(error::severity::error,
				      error::phase::runtime,
				      "program::eval_expression failed to build synthetic expression function",
				      display_file.empty() ? path : display_file,
				      expr->line, expr->column);
	    has_public_last_error = true;
	    public_diagnostics.push_back(public_last_error);
	    return false;
	}

	bool ok = pgm->compile();
	if ( display_file != path )
	    sync_public_errors(display_file, path);
	else
	    sync_public_errors();
	return ok;
    }

    bool eval_expression(const std::string &expression,
			 value *result,
			 const std::string &virtual_filename)
    {
	verbose_scope vs(eng->verbose);
	std::string validation_error;
	std::string effective_expression = expression;
	std::map<std::string, value> effective_bindings;
	std::map<std::string, std::string> rewrites;
	std::string overlay_error;
	if ( expression.empty() )
	    return fail_runtime("program::eval_expression requires a non-empty expression");
	if ( !validate_expression_source(expression, validation_error) )
	    return fail_runtime(std::string("program::eval_expression rejected input: ")
				+ validation_error);
	if ( !build_expression_context_overlay(current_expression_context,
					       current_expression_bindings,
					       effective_bindings,
					       rewrites,
					       overlay_error) )
	    return fail_runtime(std::string("program::eval_expression rejected context: ")
				+ overlay_error);
	if ( !validate_expression_context_paths(current_expression_context,
						expression,
						overlay_error) )
	    return fail_runtime(std::string("program::eval_expression rejected context: ")
				+ overlay_error);
	effective_expression = rewrite_expression_context_paths(expression, rewrites);

	program expr_program;
	expr_program.set_expression_policy(current_expression_policy);
	expr_program.set_security_policy(expression_security_policy());
	expr_program.set_invoke_limits(current_invoke_limits);
	expr_program._impl->current_expression_context = current_expression_context;
	expr_program._impl->active_expression_bindings = effective_bindings;
	bool ok = expr_program._impl->with_temp_source(build_expression_input(effective_expression),
						       virtual_filename,
						       &impl::compile_expression_with_display,
						       result != NULL);
	expr_program._impl->active_expression_bindings.clear();
	absorb_public_state(expr_program);
	if ( !ok )
	    return false;
	if ( result )
	{
	    bool call_ok = expr_program.call(expression_eval_name(), std::vector<value>(), result);
	    absorb_public_state(expr_program);
	    if ( !call_ok )
		return false;
	}
	absorb_public_state(expr_program);
	return true;
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
	engine().populate_default_registries();

	datatype_vec_t params;
	params.push_back(datatype_from_native_type(signature.returns));
	for ( std::size_t i = 0; i < signature.parameters.size(); ++i )
	    params.push_back(datatype_from_native_type(signature.parameters[i]));

	engine().builtin_registry.add_core_function(name,
						    params,
						    reinterpret_cast<fVOIDFUNC>(callback));
	reset_program();
	return true;
    }

    bool register_cpp_callback(const std::string &name,
			       void *callback_ptr,
			       const native_signature &signature,
			       native_function adapter_entry)
    {
	native_function trampoline = NULL;
	if ( !build_cpp_callback_trampoline(callback_trampoline_runtime,
					    callback_ptr,
					    adapter_entry,
					    trampoline) )
	    return fail_runtime("register_function could not build callback trampoline");
	callback_trampolines.push_back(reinterpret_cast<void *>(trampoline));
	return register_function(name, trampoline, signature);
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
	return snap;
    }

    bool enforce_invoke_limits(const std::string &op_name,
			       const invoke_snapshot &before,
			       uint64_t raw_output_bytes)
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
	    uint64_t used_output = engine().output_buffer_str().size()
		+ engine().error_buffer_str().size()
		+ raw_output_bytes;
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
	engine().clear_output_buffer();
	engine().clear_error_buffer();
	invoke_snapshot before = capture_invoke_snapshot();
	std::cout.flush();
	std::cerr.flush();
	fflush(stdout);
	fflush(stderr);
	fd_redirect_capture stdout_capture;
	fd_redirect_capture stderr_capture;
	if ( !stdout_capture.begin(STDOUT_FILENO) )
	    return fail_runtime("program::" + op_name + " could not capture stdout");
	if ( !stderr_capture.begin(STDERR_FILENO) )
	    return fail_runtime("program::" + op_name + " could not capture stderr");
	if ( !fn() )
	{
	    std::cout.flush();
	    std::cerr.flush();
	    fflush(stdout);
	    fflush(stderr);
	    return false;
	}
	std::cout.flush();
	std::cerr.flush();
	fflush(stdout);
	fflush(stderr);
	uint64_t raw_output_bytes = stdout_capture.captured_bytes()
	    + stderr_capture.captured_bytes();
	return enforce_invoke_limits(op_name, before, raw_output_bytes);
    }

    bool ensure_runtime_initialized()
    {
	if ( runtime_initialized )
	    return true;
	if ( !pgm || !pgm->root_fn )
	    return fail_runtime("program::call requires a successfully compiled program");

	pgm->clear_diagnostics();
	pgm->clear_error();
	pgm->push_runtime_scope();
	pgm->root_fn();
	pgm->pop_runtime_scope();
	sync_public_errors();
	if ( pgm->last_error.has_error )
	    return false;
	runtime_initialized = true;
	return true;
    }

    bool should_fork_invocation() const
    {
	return current_security_policy.execution == execution_mode::fork_per_invocation;
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
	    case native_type::string_object: return call_target0<std::string *>(fn, result);
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
	    case native_type::string_object: return call_target1<std::string *, A0>(fn, arg0, result);
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
	    case native_type::string_object:return dispatch_call1_ret<std::string *>(fn, ret_type, arg0, result);
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
	    case native_type::string_object: return call_target2<std::string *, A0, A1>(fn, arg0, arg1, result);
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
	    case native_type::string_object:return dispatch_call2_ret<A0, std::string *>(fn, ret_type, arg0, arg1, result);
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
	    case native_type::string_object:return dispatch_call2_arg1<std::string *>(fn, ret_type, arg1_type, arg0, arg1, result);
	    case native_type::void_type: break;
	}
	return false;
    }

    template <typename A0, typename A1, typename A2>
    bool dispatch_call3_ret(void *fn,
			    native_type ret_type,
			    const value &arg0,
			    const value &arg1,
			    const value &arg2,
			    value *result)
    {
	switch ( ret_type )
	{
	    case native_type::void_type: return call_target3_void<A0, A1, A2>(fn, arg0, arg1, arg2, result);
	    case native_type::boolean:   return call_target3<bool, A0, A1, A2>(fn, arg0, arg1, arg2, result);
	    case native_type::integer:   return call_target3<int64_t, A0, A1, A2>(fn, arg0, arg1, arg2, result);
	    case native_type::real:      return call_target3<double, A0, A1, A2>(fn, arg0, arg1, arg2, result);
	    case native_type::c_string:  return call_target3<const char *, A0, A1, A2>(fn, arg0, arg1, arg2, result);
	    case native_type::string_object: return call_target3<std::string *, A0, A1, A2>(fn, arg0, arg1, arg2, result);
	}
	return false;
    }

    template <typename A0, typename A1>
    bool dispatch_call3_arg2(void *fn,
			     native_type ret_type,
			     native_type arg2_type,
			     const value &arg0,
			     const value &arg1,
			     const value &arg2,
			     value *result)
    {
	switch ( arg2_type )
	{
	    case native_type::boolean: return dispatch_call3_ret<A0, A1, bool>(fn, ret_type, arg0, arg1, arg2, result);
	    case native_type::integer: return dispatch_call3_ret<A0, A1, int64_t>(fn, ret_type, arg0, arg1, arg2, result);
	    case native_type::real:    return dispatch_call3_ret<A0, A1, double>(fn, ret_type, arg0, arg1, arg2, result);
	    case native_type::c_string:return dispatch_call3_ret<A0, A1, const char *>(fn, ret_type, arg0, arg1, arg2, result);
	    case native_type::string_object:return dispatch_call3_ret<A0, A1, std::string *>(fn, ret_type, arg0, arg1, arg2, result);
	    case native_type::void_type: break;
	}
	return false;
    }

    template <typename A0>
    bool dispatch_call3_arg1(void *fn,
			     native_type ret_type,
			     native_type arg1_type,
			     native_type arg2_type,
			     const value &arg0,
			     const value &arg1,
			     const value &arg2,
			     value *result)
    {
	switch ( arg1_type )
	{
	    case native_type::boolean: return dispatch_call3_arg2<A0, bool>(fn, ret_type, arg2_type, arg0, arg1, arg2, result);
	    case native_type::integer: return dispatch_call3_arg2<A0, int64_t>(fn, ret_type, arg2_type, arg0, arg1, arg2, result);
	    case native_type::real:    return dispatch_call3_arg2<A0, double>(fn, ret_type, arg2_type, arg0, arg1, arg2, result);
	    case native_type::c_string:return dispatch_call3_arg2<A0, const char *>(fn, ret_type, arg2_type, arg0, arg1, arg2, result);
	    case native_type::string_object:return dispatch_call3_arg2<A0, std::string *>(fn, ret_type, arg2_type, arg0, arg1, arg2, result);
	    case native_type::void_type: break;
	}
	return false;
    }

    bool dispatch_call3(void *fn,
			native_type ret_type,
			native_type arg0_type,
			native_type arg1_type,
			native_type arg2_type,
			const value &arg0,
			const value &arg1,
			const value &arg2,
			value *result)
    {
	switch ( arg0_type )
	{
	    case native_type::boolean: return dispatch_call3_arg1<bool>(fn, ret_type, arg1_type, arg2_type, arg0, arg1, arg2, result);
	    case native_type::integer: return dispatch_call3_arg1<int64_t>(fn, ret_type, arg1_type, arg2_type, arg0, arg1, arg2, result);
	    case native_type::real:    return dispatch_call3_arg1<double>(fn, ret_type, arg1_type, arg2_type, arg0, arg1, arg2, result);
	    case native_type::c_string:return dispatch_call3_arg1<const char *>(fn, ret_type, arg1_type, arg2_type, arg0, arg1, arg2, result);
	    case native_type::string_object:return dispatch_call3_arg1<std::string *>(fn, ret_type, arg1_type, arg2_type, arg0, arg1, arg2, result);
	    case native_type::void_type: break;
	}
	return false;
    }

    template <typename A0, typename A1, typename A2, typename A3>
    bool dispatch_call4_ret(void *fn,
			    native_type ret_type,
			    const value &arg0,
			    const value &arg1,
			    const value &arg2,
			    const value &arg3,
			    value *result)
    {
	switch ( ret_type )
	{
	    case native_type::void_type: return call_target4_void<A0, A1, A2, A3>(fn, arg0, arg1, arg2, arg3, result);
	    case native_type::boolean:   return call_target4<bool, A0, A1, A2, A3>(fn, arg0, arg1, arg2, arg3, result);
	    case native_type::integer:   return call_target4<int64_t, A0, A1, A2, A3>(fn, arg0, arg1, arg2, arg3, result);
	    case native_type::real:      return call_target4<double, A0, A1, A2, A3>(fn, arg0, arg1, arg2, arg3, result);
	    case native_type::c_string:  return call_target4<const char *, A0, A1, A2, A3>(fn, arg0, arg1, arg2, arg3, result);
	    case native_type::string_object: return call_target4<std::string *, A0, A1, A2, A3>(fn, arg0, arg1, arg2, arg3, result);
	}
	return false;
    }

    template <typename A0, typename A1, typename A2>
    bool dispatch_call4_arg3(void *fn,
			     native_type ret_type,
			     native_type arg3_type,
			     const value &arg0,
			     const value &arg1,
			     const value &arg2,
			     const value &arg3,
			     value *result)
    {
	switch ( arg3_type )
	{
	    case native_type::boolean: return dispatch_call4_ret<A0, A1, A2, bool>(fn, ret_type, arg0, arg1, arg2, arg3, result);
	    case native_type::integer: return dispatch_call4_ret<A0, A1, A2, int64_t>(fn, ret_type, arg0, arg1, arg2, arg3, result);
	    case native_type::real:    return dispatch_call4_ret<A0, A1, A2, double>(fn, ret_type, arg0, arg1, arg2, arg3, result);
	    case native_type::c_string:return dispatch_call4_ret<A0, A1, A2, const char *>(fn, ret_type, arg0, arg1, arg2, arg3, result);
	    case native_type::string_object:return dispatch_call4_ret<A0, A1, A2, std::string *>(fn, ret_type, arg0, arg1, arg2, arg3, result);
	    case native_type::void_type: break;
	}
	return false;
    }

    template <typename A0, typename A1>
    bool dispatch_call4_arg2(void *fn,
			     native_type ret_type,
			     native_type arg2_type,
			     native_type arg3_type,
			     const value &arg0,
			     const value &arg1,
			     const value &arg2,
			     const value &arg3,
			     value *result)
    {
	switch ( arg2_type )
	{
	    case native_type::boolean: return dispatch_call4_arg3<A0, A1, bool>(fn, ret_type, arg3_type, arg0, arg1, arg2, arg3, result);
	    case native_type::integer: return dispatch_call4_arg3<A0, A1, int64_t>(fn, ret_type, arg3_type, arg0, arg1, arg2, arg3, result);
	    case native_type::real:    return dispatch_call4_arg3<A0, A1, double>(fn, ret_type, arg3_type, arg0, arg1, arg2, arg3, result);
	    case native_type::c_string:return dispatch_call4_arg3<A0, A1, const char *>(fn, ret_type, arg3_type, arg0, arg1, arg2, arg3, result);
	    case native_type::string_object:return dispatch_call4_arg3<A0, A1, std::string *>(fn, ret_type, arg3_type, arg0, arg1, arg2, arg3, result);
	    case native_type::void_type: break;
	}
	return false;
    }

    template <typename A0>
    bool dispatch_call4_arg1(void *fn,
			     native_type ret_type,
			     native_type arg1_type,
			     native_type arg2_type,
			     native_type arg3_type,
			     const value &arg0,
			     const value &arg1,
			     const value &arg2,
			     const value &arg3,
			     value *result)
    {
	switch ( arg1_type )
	{
	    case native_type::boolean: return dispatch_call4_arg2<A0, bool>(fn, ret_type, arg2_type, arg3_type, arg0, arg1, arg2, arg3, result);
	    case native_type::integer: return dispatch_call4_arg2<A0, int64_t>(fn, ret_type, arg2_type, arg3_type, arg0, arg1, arg2, arg3, result);
	    case native_type::real:    return dispatch_call4_arg2<A0, double>(fn, ret_type, arg2_type, arg3_type, arg0, arg1, arg2, arg3, result);
	    case native_type::c_string:return dispatch_call4_arg2<A0, const char *>(fn, ret_type, arg2_type, arg3_type, arg0, arg1, arg2, arg3, result);
	    case native_type::string_object:return dispatch_call4_arg2<A0, std::string *>(fn, ret_type, arg2_type, arg3_type, arg0, arg1, arg2, arg3, result);
	    case native_type::void_type: break;
	}
	return false;
    }

    bool dispatch_call4(void *fn,
			native_type ret_type,
			native_type arg0_type,
			native_type arg1_type,
			native_type arg2_type,
			native_type arg3_type,
			const value &arg0,
			const value &arg1,
			const value &arg2,
			const value &arg3,
			value *result)
    {
	switch ( arg0_type )
	{
	    case native_type::boolean: return dispatch_call4_arg1<bool>(fn, ret_type, arg1_type, arg2_type, arg3_type, arg0, arg1, arg2, arg3, result);
	    case native_type::integer: return dispatch_call4_arg1<int64_t>(fn, ret_type, arg1_type, arg2_type, arg3_type, arg0, arg1, arg2, arg3, result);
	    case native_type::real:    return dispatch_call4_arg1<double>(fn, ret_type, arg1_type, arg2_type, arg3_type, arg0, arg1, arg2, arg3, result);
	    case native_type::c_string:return dispatch_call4_arg1<const char *>(fn, ret_type, arg1_type, arg2_type, arg3_type, arg0, arg1, arg2, arg3, result);
	    case native_type::string_object:return dispatch_call4_arg1<std::string *>(fn, ret_type, arg1_type, arg2_type, arg3_type, arg0, arg1, arg2, arg3, result);
	    case native_type::void_type: break;
	}
	return false;
    }

    bool has_function(const std::string &name) const
    {
	if ( pgm && pgm->has_loaded_function(name) )
	    return true;
	if ( !pgm || !pgm->tkProgram )
	    return false;
	std::string id = name;
	Variable *var = pgm->findVariable(id);
	if ( !var || !var->type || var->type->basetype() != BaseType::btFunct )
	    return false;
	Method *method = static_cast<Method *>(var->data);
	return method && method->x86code;
    }

    bool load_object(const std::string &path)
    {
	verbose_scope vs(eng->verbose);
	if ( !pgm )
	    return fail_runtime("program::load_object has no program");
	if ( !pgm->load_object(path) )
	    return fail_runtime("program::load_object failed to load " + path);
	return true;
    }

    bool call_loaded_function(const std::string &name,
			      const std::vector<value> &args,
			      value *result)
    {
	void *fn = pgm->loaded_function_ptr(name);
	if ( !fn )
	    return fail_runtime("program::call cannot find loaded function '" + name + "'");
	if ( args.size() > 4 )
	    return fail_runtime("program::call currently supports up to 4 arguments");

	// All loaded functions use the SysV x86-64 integer ABI.
	// Convert value args to int64_t for dispatch.
	int64_t iargs[4] = {0, 0, 0, 0};
	for ( std::size_t i = 0; i < args.size(); ++i )
	{
	    switch ( args[i].type() )
	    {
		case value::kind::boolean:
		    iargs[i] = args[i].as_boolean() ? 1 : 0;
		    break;
		case value::kind::integer:
		    iargs[i] = args[i].as_integer();
		    break;
		case value::kind::real:
		    // reinterpret double bits as int64 for register passing
		    { double d = args[i].as_real();
		      std::memcpy(&iargs[i], &d, sizeof(d)); }
		    break;
		default:
		    return fail_runtime("program::call loaded function: unsupported argument type");
	    }
	}

	typedef int64_t (*fn0_t)();
	typedef int64_t (*fn1_t)(int64_t);
	typedef int64_t (*fn2_t)(int64_t, int64_t);
	typedef int64_t (*fn3_t)(int64_t, int64_t, int64_t);
	typedef int64_t (*fn4_t)(int64_t, int64_t, int64_t, int64_t);

	int64_t rv = 0;
	switch ( args.size() )
	{
	    case 0: rv = reinterpret_cast<fn0_t>(fn)(); break;
	    case 1: rv = reinterpret_cast<fn1_t>(fn)(iargs[0]); break;
	    case 2: rv = reinterpret_cast<fn2_t>(fn)(iargs[0], iargs[1]); break;
	    case 3: rv = reinterpret_cast<fn3_t>(fn)(iargs[0], iargs[1], iargs[2]); break;
	    case 4: rv = reinterpret_cast<fn4_t>(fn)(iargs[0], iargs[1], iargs[2], iargs[3]); break;
	}

	if ( result )
	    *result = value(rv);
	return true;
    }

    bool perform_call(const std::string &name, const std::vector<value> &args, value *result)
    {
	// Check loaded-object functions first.
	if ( pgm && pgm->has_loaded_function(name) )
	    return call_loaded_function(name, args, result);

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
	if ( args.size() > 4 )
	    return fail_runtime("program::call currently supports up to 4 arguments");

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
	    bool ok = false;
	    std::vector<value> arg_storage(args.begin(), args.end());
	    pgm->push_runtime_scope();
	    switch ( args.size() )
	    {
		case 0:
		    ok = dispatch_call0(method->x86code, ret_type, result);
		    break;
		case 1:
		    ok = dispatch_call1(method->x86code, ret_type, arg_types[0], arg_storage[0], result);
		    break;
		case 2:
		    ok = dispatch_call2(method->x86code, ret_type, arg_types[0], arg_types[1],
					arg_storage[0], arg_storage[1], result);
		    break;
		case 3:
		    ok = dispatch_call3(method->x86code, ret_type, arg_types[0], arg_types[1], arg_types[2],
					arg_storage[0], arg_storage[1], arg_storage[2], result);
		    break;
		case 4:
		    ok = dispatch_call4(method->x86code, ret_type, arg_types[0], arg_types[1],
					arg_types[2], arg_types[3],
					arg_storage[0], arg_storage[1], arg_storage[2], arg_storage[3], result);
		    break;
		default:
		    break;
	    }
	    pgm->pop_runtime_scope();
	    if ( ok )
		return true;
	}
	catch ( const std::exception &e )
	{
	    pgm->pop_runtime_scope();
	    return fail_runtime(e.what());
	}

	return fail_runtime("program::call could not dispatch the requested signature");
    }

    bool call_in_child(const std::string &name, const std::vector<value> &args, value *result)
    {
	temp_file child_stdout;
	temp_file child_stderr;
	temp_file child_report;
	if ( !child_stdout.create("/tmp/madc_call_stdout_XXXXXX")
	  || !child_stderr.create("/tmp/madc_call_stderr_XXXXXX")
	  || !child_report.create("/tmp/madc_call_report_XXXXXX") )
	    return fail_runtime("program::call could not create child capture files");

	pid_t pid = fork();
	if ( pid < 0 )
	    return fail_runtime("program::call could not fork");

	if ( pid == 0 )
	{
	    int report_fd = dup(child_report.fd());
	    if ( dup2(child_stdout.fd(), STDOUT_FILENO) < 0
	      || dup2(child_stderr.fd(), STDERR_FILENO) < 0 )
		_exit(120);
	    child_stdout.close_fd();
	    child_stderr.close_fd();
	    child_report.close_fd();
	    if ( report_fd < 0 )
		_exit(121);

	    engine().reset_standard_streams();
	    value child_result;
	    bool ok = perform_call(name, args, result ? &child_result : NULL);

	    exec_child_report report;
	    report.ok = ok;
	    report.has_last_error = has_public_last_error;
	    report.diagnostics = public_diagnostics;
	    if ( has_public_last_error )
		report.last_error = public_last_error;
	    if ( result && ok )
	    {
		report.has_result = true;
		report.result = child_result;
	    }

	    bool wrote = false;
	    {
		std::ofstream os("/proc/self/fd/" + std::to_string(report_fd),
				 std::ios::binary | std::ios::trunc);
		if ( os )
		    wrote = write_exec_child_report(os, report);
	    }
	    close(report_fd);
	    std::cout.flush();
	    std::cerr.flush();
	    fflush(stdout);
	    fflush(stderr);
	    _exit(wrote ? (ok ? 0 : 1) : 122);
	}

	child_stdout.close_fd();
	child_stderr.close_fd();
	child_report.close_fd();

	int status = 0;
	struct rusage usage;
	std::memset(&usage, 0, sizeof(usage));
	if ( wait4(pid, &status, 0, &usage) < 0 )
	    return fail_runtime("program::call could not wait for child");

	std::string stdout_text = read_text_file(child_stdout.path());
	std::string stderr_text = read_text_file(child_stderr.path());
	if ( !stdout_text.empty() )
	{
	    engine().output() << stdout_text;
	    engine().output().flush();
	}
	if ( !stderr_text.empty() )
	{
	    engine().error() << stderr_text;
	    engine().error().flush();
	}

	exec_child_report report;
	bool have_report = read_exec_child_report(child_report.path(), report);
	if ( have_report )
	{
	    public_diagnostics = report.diagnostics;
	    has_public_last_error = report.has_last_error;
	    public_last_error = report.has_last_error ? report.last_error : error();
	    runtime_initialized = false;
	    if ( result && report.has_result )
		*result = report.result;
	}
	else
	{
	    clear_public_errors();
	    runtime_initialized = false;
	}

	uint64_t child_output_bytes = static_cast<uint64_t>(stdout_text.size())
	    + static_cast<uint64_t>(stderr_text.size());
	if ( current_invoke_limits.cpu_ms > 0 )
	{
	    uint64_t used_cpu = timeval_to_microseconds(usage.ru_utime)
		+ timeval_to_microseconds(usage.ru_stime);
	    uint64_t limit_cpu = current_invoke_limits.cpu_ms * UINT64_C(1000);
	    if ( used_cpu > limit_cpu )
	    {
		std::ostringstream os;
		os << "program::call exceeded cpu_ms limit (" << current_invoke_limits.cpu_ms
		   << " ms, used " << (used_cpu / 1000) << " ms)";
		return fail_runtime(os.str());
	    }
	}
	if ( current_invoke_limits.memory_bytes > 0 )
	{
	    uint64_t used_resident = usage.ru_maxrss > 0
		? static_cast<uint64_t>(usage.ru_maxrss) * UINT64_C(1024)
		: 0;
	    if ( used_resident > current_invoke_limits.memory_bytes )
	    {
		std::ostringstream os;
		os << "program::call exceeded memory_bytes limit ("
		   << current_invoke_limits.memory_bytes
		   << " bytes, peak " << used_resident << " bytes)";
		return fail_runtime(os.str());
	    }
	}
	if ( current_invoke_limits.output_bytes > 0
	  && child_output_bytes > current_invoke_limits.output_bytes )
	{
	    std::ostringstream os;
	    os << "program::call exceeded output_bytes limit ("
	       << current_invoke_limits.output_bytes
	       << " bytes, produced " << child_output_bytes << " bytes)";
	    return fail_runtime(os.str());
	}

	if ( WIFSIGNALED(status) )
	{
	    std::ostringstream os;
	    os << "program::call child terminated by signal " << WTERMSIG(status);
	    return fail_runtime(os.str());
	}
	if ( !have_report )
	    return fail_runtime("program::call child did not return a report");
	return report.ok;
    }

    bool call(const std::string &name, const std::vector<value> &args, value *result)
    {
	verbose_scope vs(eng->verbose);
	if ( should_fork_invocation() )
	    return call_in_child(name, args, result);
	return invoke_with_limits("call", [this, &name, &args, result]() -> bool {
	    return perform_call(name, args, result);
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

    bool coerce_eval_bool(const value &resolved,
			  const std::string &label,
			  bool &out)
    {
	if ( resolved.is_boolean() )
	{
	    out = resolved.as_boolean();
	    return true;
	}
	if ( resolved.is_integer() )
	{
	    out = resolved.as_integer() != 0;
	    return true;
	}
	return fail_runtime(label + " requires a boolean-compatible result");
    }

    bool coerce_eval_int(const value &resolved,
			 const std::string &label,
			 int64_t &out)
    {
	if ( resolved.is_integer() )
	{
	    out = resolved.as_integer();
	    return true;
	}
	return fail_runtime(label + " requires an integer result");
    }

    bool coerce_eval_double(const value &resolved,
			    const std::string &label,
			    double &out)
    {
	if ( resolved.is_real() )
	{
	    out = resolved.as_real();
	    return true;
	}
	if ( resolved.is_integer() )
	{
	    out = (double)resolved.as_integer();
	    return true;
	}
	return fail_runtime(label + " requires a real-compatible result");
    }

    bool coerce_eval_string(const value &resolved,
			    const std::string &label,
			    std::string &out)
    {
	if ( resolved.is_string() )
	{
	    out = resolved.as_string();
	    return true;
	}
	return fail_runtime(label + " requires a string result");
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

void program::set_aot_mode(bool enabled)
{
    _impl->aot_mode = enabled;
    if ( _impl->pgm )
	_impl->pgm->aot_tracking = enabled;
}

bool program::compile_file(const std::string &path)
{
    return _impl->compile_file_with_display(path, path);
}

bool program::compile_string(const std::string &source, const std::string &virtual_filename)
{
    std::string display_name = virtual_filename.empty() ? std::string("<memory>") : virtual_filename;
    return _impl->compile_source_with_display(source, display_name);
}

bool program::is_compiled() const
{
    return _impl->pgm && _impl->pgm->root_fn;
}

bool program::save_object(const std::string &path)
{
    if ( !_impl->pgm || !_impl->pgm->root_fn )
    {
	_impl->clear_public_errors();
	_impl->public_last_error = error(error::severity::error,
					 error::phase::compiler,
					 "program::save_object requires a successfully compiled program");
	_impl->has_public_last_error = true;
	_impl->public_diagnostics.push_back(_impl->public_last_error);
	return false;
    }
    if ( !_impl->pgm->save_object(path) )
    {
	_impl->clear_public_errors();
	_impl->public_last_error = error(error::severity::error,
					 error::phase::compiler,
					 "program::save_object failed to write " + path);
	_impl->has_public_last_error = true;
	_impl->public_diagnostics.push_back(_impl->public_last_error);
	return false;
    }
    return true;
}

bool program::save_executable(const std::string &path)
{
    if ( !_impl->pgm || !_impl->pgm->root_fn )
    {
	_impl->clear_public_errors();
	_impl->public_last_error = error(error::severity::error,
					 error::phase::compiler,
					 "program::save_executable requires a successfully compiled program");
	_impl->has_public_last_error = true;
	_impl->public_diagnostics.push_back(_impl->public_last_error);
	return false;
    }
    if ( !_impl->pgm->save_executable(path) )
    {
	_impl->clear_public_errors();
	_impl->public_last_error = error(error::severity::error,
					 error::phase::compiler,
					 "program::save_executable failed to write " + path);
	_impl->has_public_last_error = true;
	_impl->public_diagnostics.push_back(_impl->public_last_error);
	return false;
    }
    return true;
}

bool program::load_object(const std::string &path)
{
    return _impl->load_object(path);
}

bool program::exec()
{
    return _impl->exec_compiled_with_display("<compiled>", "<compiled>");
}

bool program::exec_file(const std::string &path)
{
    return _impl->exec_file_with_display(path, path);
}

bool program::exec_string(const std::string &source, const std::string &virtual_filename)
{
    std::string display_name = virtual_filename.empty() ? std::string("<memory>") : virtual_filename;
    return _impl->exec_source_with_display(source, display_name);
}

bool program::eval_unit(const std::string &source,
			value *result,
			const std::string &virtual_filename)
{
    std::string display_name = virtual_filename.empty() ? std::string("<memory>") : virtual_filename;
    return _impl->eval_source_with_display(source, display_name, result);
}

bool program::eval_unit(const std::string &source,
			bool &result,
			const std::string &virtual_filename)
{
    value resolved;
    if ( !eval_unit(source, &resolved, virtual_filename) )
	return false;
    return _impl->coerce_eval_bool(resolved, "program::eval_unit", result);
}

bool program::eval_unit(const std::string &source,
			int64_t &result,
			const std::string &virtual_filename)
{
    value resolved;
    if ( !eval_unit(source, &resolved, virtual_filename) )
	return false;
    return _impl->coerce_eval_int(resolved, "program::eval_unit", result);
}

bool program::eval_unit(const std::string &source,
			double &result,
			const std::string &virtual_filename)
{
    value resolved;
    if ( !eval_unit(source, &resolved, virtual_filename) )
	return false;
    return _impl->coerce_eval_double(resolved, "program::eval_unit", result);
}

bool program::eval_unit(const std::string &source,
			std::string &result,
			const std::string &virtual_filename)
{
    value resolved;
    if ( !eval_unit(source, &resolved, virtual_filename) )
	return false;
    return _impl->coerce_eval_string(resolved, "program::eval_unit", result);
}

bool program::eval(const std::string &source,
		   value *result,
		   const std::string &virtual_filename)
{
    return eval_unit(source, result, virtual_filename);
}

bool program::eval(const std::string &source,
		   bool &result,
		   const std::string &virtual_filename)
{
    return eval_unit(source, result, virtual_filename);
}

bool program::eval(const std::string &source,
		   int64_t &result,
		   const std::string &virtual_filename)
{
    return eval_unit(source, result, virtual_filename);
}

bool program::eval(const std::string &source,
		   double &result,
		   const std::string &virtual_filename)
{
    return eval_unit(source, result, virtual_filename);
}

bool program::eval(const std::string &source,
		   std::string &result,
		   const std::string &virtual_filename)
{
    return eval_unit(source, result, virtual_filename);
}

bool program::eval_body(const std::string &source,
			value *result,
			native_type return_type,
			const std::string &virtual_filename)
{
    std::string display_name = virtual_filename.empty() ? std::string("<memory>") : virtual_filename;
    return _impl->eval_body_with_display(source, display_name, result, return_type);
}

bool program::eval_body(const std::string &source,
			bool &result,
			const std::string &virtual_filename)
{
    value resolved;
    if ( !eval_body(source, &resolved, native_type::boolean, virtual_filename) )
	return false;
    return _impl->coerce_eval_bool(resolved, "program::eval_body", result);
}

bool program::eval_body(const std::string &source,
			int64_t &result,
			const std::string &virtual_filename)
{
    value resolved;
    if ( !eval_body(source, &resolved, native_type::integer, virtual_filename) )
	return false;
    return _impl->coerce_eval_int(resolved, "program::eval_body", result);
}

bool program::eval_body(const std::string &source,
			double &result,
			const std::string &virtual_filename)
{
    value resolved;
    if ( !eval_body(source, &resolved, native_type::real, virtual_filename) )
	return false;
    return _impl->coerce_eval_double(resolved, "program::eval_body", result);
}

bool program::eval_body(const std::string &source,
			std::string &result,
			const std::string &virtual_filename)
{
    value resolved;
    if ( !eval_body(source, &resolved, native_type::c_string, virtual_filename) )
	return false;
    return _impl->coerce_eval_string(resolved, "program::eval_body", result);
}

bool program::eval_expression(const std::string &expression,
			      value *result,
			      const std::string &virtual_filename)
{
    return _impl->eval_expression(expression, result, virtual_filename);
}

bool program::eval_expression(const std::string &expression,
			      bool &result,
			      const std::string &virtual_filename)
{
    value resolved;
    if ( !eval_expression(expression, &resolved, virtual_filename) )
	return false;
    return _impl->coerce_eval_bool(resolved, "program::eval_expression", result);
}

bool program::eval_expression(const std::string &expression,
			      int64_t &result,
			      const std::string &virtual_filename)
{
    value resolved;
    if ( !eval_expression(expression, &resolved, virtual_filename) )
	return false;
    return _impl->coerce_eval_int(resolved, "program::eval_expression", result);
}

bool program::eval_expression(const std::string &expression,
			      double &result,
			      const std::string &virtual_filename)
{
    value resolved;
    if ( !eval_expression(expression, &resolved, virtual_filename) )
	return false;
    return _impl->coerce_eval_double(resolved, "program::eval_expression", result);
}

bool program::eval_expression(const std::string &expression,
			      std::string &result,
			      const std::string &virtual_filename)
{
    value resolved;
    if ( !eval_expression(expression, &resolved, virtual_filename) )
	return false;
    return _impl->coerce_eval_string(resolved, "program::eval_expression", result);
}

bool internal_program_runtime_eval_source(::Program &self,
					  const std::string &source_text,
					  madc::value &result,
					  const std::string &display_name,
					  const madc::value *context,
					  const char *wrapper_return_type)
{
    self.clear_diagnostics();
    self.clear_error();

    Program child(self.engine);
    child.registration_policy = runtime_eval_registration_policy_for_source_child(self.registration_policy);
    std::string effective_source = source_text;
    if ( wrapper_return_type
      && !source_contains_explicit_eval_entry(source_text) )
	effective_source = build_eval_body_wrapper_source(source_text, wrapper_return_type);
    std::string normalized_source = ensure_trailing_newline(effective_source);
    TokenProgram *tp = child.tokenize_buffer(normalized_source, display_name);
    if ( !tp )
    {
	copy_program_public_error(self, child);
	return false;
    }
    if ( !install_runtime_eval_scope_globals(child, context, display_name) )
    {
	copy_program_public_error(self, child);
	return false;
    }
    if ( !child.parse(tp) )
    {
	copy_program_public_error(self, child);
	return false;
    }
    if ( !child.compile() )
    {
	copy_program_public_error(self, child);
	return false;
    }
    if ( !invoke_program_zero_arg_function(child, eval_entry_name(), result) )
    {
	copy_program_public_error(self, child);
	return false;
    }
    return true;
}

bool internal_program_runtime_eval_expression(::Program &self,
					      const std::string &expression,
					      madc::value &result,
					      const std::string &display_name,
					      const madc::value *context)
{
    self.clear_diagnostics();
    self.clear_error();

    std::string validation_error;
    if ( expression.empty() )
	return fail_program_runtime(self,
				    "program::eval_expression requires a non-empty expression");
    if ( !validate_expression_source(expression, validation_error) )
	return fail_program_runtime(self,
				    std::string("program::eval_expression rejected input: ")
				    + validation_error,
				    display_name.c_str());
    if ( context )
    {
	if ( !context->is_object() )
	    return fail_program_runtime(self,
					"program::eval_expression rejected context: context must be an object value",
					display_name.c_str());
	std::string context_error;
	if ( !validate_expression_context_paths(*context, expression, context_error) )
	    return fail_program_runtime(self,
					std::string("program::eval_expression rejected context: ")
					+ context_error,
					display_name.c_str());
    }

    expression_policy policy;
    if ( self.registration_policy.enable_dlfcn_functions )
    {
	policy.allow_function_calls = true;
	policy.allowed_headers.push_back("math.h");
    }

    Program child(self.engine);
    child.registration_policy = self.registration_policy;
    child.registration_policy.allowed_headers = policy.allowed_headers;
    child.registration_policy.allowed_dlfcn_symbols = policy.allowed_functions;
    append_unique_strings(child.registration_policy.allowed_dlfcn_symbols,
			  expand_header_symbol_groups(policy.allowed_headers));
    child.set_expression_context_root(context && context->is_object() ? context : NULL);

    std::string source_text = build_expression_input_from_policy(policy, expression);
    TokenProgram *tp = child.tokenize_buffer(source_text, display_name);
    if ( !tp )
    {
	copy_program_public_error(self, child);
	return false;
    }
    if ( !validate_expression_function_policy(child, policy, child.tokens, display_name) )
    {
	copy_program_public_error(self, child);
	return false;
    }
    if ( !register_expression_header_functions(child, policy, display_name) )
    {
	copy_program_public_error(self, child);
	return false;
    }

    TokenBase *expr = child.parse_expression_unit(tp);
    if ( !expr )
    {
	copy_program_public_error(self, child);
	return false;
    }

    std::string ast_validation_error;
    if ( !validate_expression_ast(policy, expr, ast_validation_error) )
    {
	fail_program_runtime(child,
			     std::string("program::eval_expression rejected parsed expression: ")
			     + ast_validation_error,
			     display_name.c_str(),
			     expr->line,
			     expr->column);
	copy_program_public_error(self, child);
	return false;
    }

    DataDef *return_type = expression_result_datadef_internal(infer_expression_result_type(expr), true);
    if ( !return_type )
    {
	fail_program_runtime(child,
			     "program::eval_expression cannot marshal this result type",
			     display_name.c_str(),
			     expr->line,
			     expr->column);
	copy_program_public_error(self, child);
	return false;
    }

    if ( !child.build_expression_function(tp,
					  expr,
					  return_type,
					  expression_eval_name(),
					  true) )
    {
	fail_program_runtime(child,
			     "program::eval_expression failed to build synthetic expression function",
			     display_name.c_str(),
			     expr->line,
			     expr->column);
	copy_program_public_error(self, child);
	return false;
    }
    if ( !child.compile() )
    {
	copy_program_public_error(self, child);
	return false;
    }
    if ( !invoke_program_zero_arg_function(child, expression_eval_name(), result) )
    {
	copy_program_public_error(self, child);
	return false;
    }
    return true;
}

bool program::register_function(const std::string &name,
				native_function callback,
				const native_signature &signature)
{
    return _impl->register_function(name, callback, signature);
}

bool program::register_cpp_callback(const std::string &name,
				    void *callback_ptr,
				    const native_signature &signature,
				    native_function adapter_entry)
{
    return _impl->register_cpp_callback(name, callback_ptr, signature, adapter_entry);
}

bool program::has_function(const std::string &name) const
{
    return _impl->has_function(name);
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

void program::set_expression_policy(const expression_policy &policy)
{
    _impl->set_expression_policy(policy);
}

const expression_policy &program::get_expression_policy() const
{
    return _impl->current_expression_policy;
}

void program::set_runtime_eval_policy(const runtime_eval_policy &policy)
{
    _impl->set_runtime_eval_policy(policy);
}

const runtime_eval_policy &program::get_runtime_eval_policy() const
{
    return _impl->current_runtime_eval_policy;
}

void program::set_expression_bindings(const std::map<std::string, value> &bindings)
{
    _impl->set_expression_bindings(bindings);
}

const std::map<std::string, value> &program::get_expression_bindings() const
{
    return _impl->current_expression_bindings;
}

void program::clear_expression_bindings()
{
    _impl->current_expression_bindings.clear();
}

void program::set_expression_context(const value &context)
{
    _impl->set_expression_context(context);
}

const value &program::get_expression_context() const
{
    return _impl->current_expression_context;
}

void program::clear_expression_context()
{
    _impl->current_expression_context = value();
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

// ---------------------------------------------------------------------------
// madc::engine
// ---------------------------------------------------------------------------

struct engine::impl
{
    MadcEngine eng;
    compile_options current_compile_options;
    security_policy current_security_policy;
    expression_policy current_expression_policy;
    runtime_eval_policy current_runtime_eval_policy;
    invoke_limits current_invoke_limits;
    asmjit::JitRuntime callback_trampoline_runtime;
    std::vector<void *> callback_trampolines;

    impl()
    {
	eng.capture_output_to_buffer();
	eng.capture_error_to_buffer();
    }

    ~impl()
    {
	for ( std::size_t i = 0; i < callback_trampolines.size(); ++i )
	    callback_trampoline_runtime.release(callback_trampolines[i]);
    }

    void sync_registration_policy()
    {
	eng.registration_policy =
	    registration_policy_from_compile_options(current_compile_options);
	eng.registration_policy.runtime_eval_source_policy =
	    runtime_eval_child_policy_from_public(current_runtime_eval_policy);
    }
};

engine::engine()
    : _impl(new impl())
{
}

engine::~engine()
{
}

engine::engine(engine &&other) noexcept
    : _impl(std::move(other._impl))
{
}

engine &engine::operator=(engine &&other) noexcept
{
    if ( this != &other )
	_impl = std::move(other._impl);
    return *this;
}

program engine::create_program()
{
    return program(*this);
}

void engine::set_verbose(bool v)
{
    _impl->eng.verbose = v;
}

bool engine::get_verbose() const
{
    return _impl->eng.verbose;
}

void engine::set_compile_options(const compile_options &options)
{
    _impl->current_compile_options = clamp_compile_options_for_authority_mode(
	options, _impl->current_security_policy.mode);
    _impl->current_security_policy = clamp_security_policy_for_authority_mode(
	security_policy_from_compile_options(_impl->current_compile_options,
					     _impl->current_security_policy.mode));
    _impl->current_runtime_eval_policy = clamp_runtime_eval_policy_for_authority_mode(
	_impl->current_runtime_eval_policy,
	_impl->current_security_policy.mode);
    _impl->sync_registration_policy();
}

const compile_options &engine::get_compile_options() const
{
    return _impl->current_compile_options;
}

void engine::set_security_policy(const security_policy &policy)
{
    _impl->current_security_policy = clamp_security_policy_for_authority_mode(policy);
    _impl->current_compile_options = clamp_compile_options_for_authority_mode(
	compile_options_from_security_policy(_impl->current_security_policy),
	_impl->current_security_policy.mode);
    _impl->current_runtime_eval_policy = clamp_runtime_eval_policy_for_authority_mode(
	_impl->current_runtime_eval_policy,
	_impl->current_security_policy.mode);
    _impl->sync_registration_policy();
}

const security_policy &engine::get_security_policy() const
{
    return _impl->current_security_policy;
}

void engine::set_expression_policy(const expression_policy &policy)
{
    _impl->current_expression_policy = policy;
}

const expression_policy &engine::get_expression_policy() const
{
    return _impl->current_expression_policy;
}

void engine::set_runtime_eval_policy(const runtime_eval_policy &policy)
{
    _impl->current_runtime_eval_policy = clamp_runtime_eval_policy_for_authority_mode(
	policy, _impl->current_security_policy.mode);
    _impl->sync_registration_policy();
}

const runtime_eval_policy &engine::get_runtime_eval_policy() const
{
    return _impl->current_runtime_eval_policy;
}

void engine::set_invoke_limits(const invoke_limits &limits)
{
    _impl->current_invoke_limits = limits;
}

const invoke_limits &engine::get_invoke_limits() const
{
    return _impl->current_invoke_limits;
}

bool engine::register_function(const std::string &name,
			       program::native_function callback,
			       const program::native_signature &signature)
{
    if ( name.empty() || !callback )
	return false;

    _impl->eng.populate_default_registries();

    datatype_vec_t params;
    params.push_back(datatype_from_native_type(signature.returns));
    for ( std::size_t i = 0; i < signature.parameters.size(); ++i )
	params.push_back(datatype_from_native_type(signature.parameters[i]));

    _impl->eng.builtin_registry.add_core_function(name,
						   params,
						   reinterpret_cast<fVOIDFUNC>(callback));
    return true;
}

bool engine::register_cpp_callback(const std::string &name,
				   void *callback_ptr,
				   const program::native_signature &signature,
				   program::native_function adapter_entry)
{
    program::native_function trampoline = NULL;
    if ( !build_cpp_callback_trampoline(_impl->callback_trampoline_runtime,
					callback_ptr,
					adapter_entry,
					trampoline) )
	return false;
    _impl->callback_trampolines.push_back(reinterpret_cast<void *>(trampoline));
    return register_function(name, trampoline, signature);
}

// Deferred: needs engine::impl to be complete.
program::program(engine &eng)
    : _impl(new impl(&eng._impl->eng,
		     eng._impl->current_compile_options,
		     eng._impl->current_security_policy,
		     eng._impl->current_expression_policy,
		     eng._impl->current_runtime_eval_policy,
		     eng._impl->current_invoke_limits))
{
}

} // namespace madc
