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


#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_cir.h"
#include "cir_builder.h"	// call_emit_symbol — the one call-symbol resolver

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
bool text_list_contains(const std::vector<std::string> &items, const std::string &value);
std::vector<expression_function_spec> expression_header_function_specs(const std::string &header);
std::vector<std::string> expression_allowed_function_names(const expression_policy &policy);
std::vector<std::string> collect_expression_token_calls(const TokenStream &tokens,
							 const std::string &source_file);
void append_unique_strings(std::vector<std::string> &dst,
			   const std::vector<std::string> &src);
std::vector<std::string> expand_header_symbol_groups(const std::vector<std::string> &headers);
bool native_type_from_datadef(DataDef *type, program::native_type &out);
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

bool is_expression_compare_token(TokenID id)
{
    return id == TokenID::tkEquals || id == TokenID::tkNotEq
	|| id == TokenID::tkLT || id == TokenID::tkLE
	|| id == TokenID::tkGT || id == TokenID::tkGE
	|| id == TokenID::tk3Eq || id == TokenID::tk3NotEq;
}

bool expression_operand_is_string(TokenBase *tb)
{
    DataDef *dd = tb ? tb->datadef() : NULL;
    return dd && dd->type() == DataType::dtCHARptr;
}

Variable *ensure_expression_strcmp(Program &pgm)
{
    std::string id = "strcmp";
    if ( Variable *existing = pgm.findVariable(id) )
	return existing;
    void *sym = dlsym(RTLD_DEFAULT, "strcmp");
    if ( !sym )
	return NULL;
    // signed int return — embedded-headers.md comparison-family rule
    return pgm.addFunction(id, datatype_vec_t{DataType::dtINT, ptr_of(ddCHAR),
					      ptr_of(ddCHAR)}, (fVOIDFUNC)sym);
}

// The expression DSL compares string operands by VALUE (spec:
// docs/superpowers/specs/2026-06-10-eval-leftovers-design.md). Comparison
// nodes with two string operands become strcmp(a,b) OP 0 (`===` becomes a
// replacement strcmp(a,b) == 0 node and `!==` its strcmp(a,b) != 0 twin —
// the DSL's value semantics; the real language's ===/!== lower in CIR via
// strict_equality_lowering and never reach this pass); a string vs
// non-string mix is a loud error.
// The rewrite must cover EVERY compare token over string operands: an
// unrewritten compare keeps char* operands at the root, and
// infer_expression_result_type would marshal the int result as a pointer
// (the host then strlen()s it — a real crash, caught by unit test).
// The recursion set mirrors validate_expression_ast's node coverage — keep
// the two in sync. call->src_node is unreachable here: the validator rejects
// indirect calls before this pass runs.
bool rewrite_expression_string_compares(Program &pgm, TokenBase *&tb,
					std::string &error)
{
    if ( !tb )
	return true;
    if ( TokenMember *member = dynamic_cast<TokenMember *>(tb) )
    {
	// covers TokenCallMethod too (TokenCallMethod : TokenMember)
	if ( !rewrite_expression_string_compares(pgm, member->parent_expr, error) )
	    return false;
	for ( size_t i = 0; i < member->parameters.size(); ++i )
	    if ( !rewrite_expression_string_compares(pgm, member->parameters[i], error) )
		return false;
	return true;
    }
    if ( TokenCallFunc *call = dynamic_cast<TokenCallFunc *>(tb) )
    {
	for ( size_t i = 0; i < call->parameters.size(); ++i )
	    if ( !rewrite_expression_string_compares(pgm, call->parameters[i], error) )
		return false;
	return true;
    }
    if ( TokenSubscriptExpr *subexpr = dynamic_cast<TokenSubscriptExpr *>(tb) )
    {
	if ( !rewrite_expression_string_compares(pgm, subexpr->base_expr, error) )
	    return false;
	return rewrite_expression_string_compares(pgm, subexpr->index, error);
    }
    if ( TokenSubscript *sub = dynamic_cast<TokenSubscript *>(tb) )
    {
	if ( !rewrite_expression_string_compares(pgm, sub->index, error) )
	    return false;
	for ( size_t i = 0; i < sub->extra_indices.size(); ++i )
	    if ( !rewrite_expression_string_compares(pgm, sub->extra_indices[i], error) )
		return false;
	return true;
    }
    if ( TokenAddrExpr *addr = dynamic_cast<TokenAddrExpr *>(tb) )
	return rewrite_expression_string_compares(pgm, addr->expr, error);
    if ( TokenDerefExpr *deref = dynamic_cast<TokenDerefExpr *>(tb) )
	return rewrite_expression_string_compares(pgm, deref->expr, error);
    if ( TokenCast *cast = dynamic_cast<TokenCast *>(tb) )
	return rewrite_expression_string_compares(pgm, cast->expr, error);
    if ( TokenTerQ *tq = dynamic_cast<TokenTerQ *>(tb) )
    {
	if ( !rewrite_expression_string_compares(pgm, tq->condition, error) )
	    return false;
	if ( !rewrite_expression_string_compares(pgm, tq->true_expr, error) )
	    return false;
	return rewrite_expression_string_compares(pgm, tq->false_expr, error);
    }
    TokenOperator *op = dynamic_cast<TokenOperator *>(tb);
    if ( !op )
	return true;
    if ( !rewrite_expression_string_compares(pgm, op->left, error) )
	return false;
    if ( !rewrite_expression_string_compares(pgm, op->right, error) )
	return false;
    if ( !is_expression_compare_token(op->id()) )
	return true;
    bool ls = expression_operand_is_string(op->left);
    bool rs = expression_operand_is_string(op->right);
    if ( !ls && !rs )
	return true;
    if ( ls != rs )
    {
	error = "cannot compare string and non-string values";
	return false;
    }
    Variable *sv = ensure_expression_strcmp(pgm);
    if ( !sv )
    {
	error = "could not resolve strcmp for string comparison";
	return false;
    }
    TokenCallFunc *cmp = new TokenCallFunc(*sv);
    cmp->file = op->file;
    cmp->line = op->line;
    cmp->column = op->column;
    cmp->parameters.push_back(op->left);
    cmp->parameters.push_back(op->right);
    TokenInt *zero = new TokenInt(0);
    zero->file = op->file;
    zero->line = op->line;
    zero->column = op->column;
    if ( op->id() == TokenID::tk3Eq )
    {
	// `===` on two strings is same-type + equal-value, which for two
	// strings is exactly strcmp == 0. Replace the node outright.
	TokenEquals *eq = new TokenEquals();
	eq->file = op->file;
	eq->line = op->line;
	eq->column = op->column;
	eq->left = cmp;
	eq->right = zero;
	tb = eq;
	return true;
    }
    if ( op->id() == TokenID::tk3NotEq )
    {
	// `!==` on two strings is !(===): strcmp != 0. Replace the node.
	TokenNotEq *ne = new TokenNotEq();
	ne->file = op->file;
	ne->line = op->line;
	ne->column = op->column;
	ne->left = cmp;
	ne->right = zero;
	tb = ne;
	return true;
    }
    op->left = cmp;
    op->right = zero;
    return true;
}

DataDef *expression_result_datadef_internal(DataDef *expr_type, bool have_result)
{
    if ( !have_result )
	return &ddVOID;
    if ( !expr_type )
	return NULL;
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
					 const TokenStream &tokens,
					 const std::string &source_file,
					 const std::string &display_file)
{
    std::vector<std::string> calls = collect_expression_token_calls(tokens, source_file);
    if ( calls.empty() )
	return true;
    if ( !policy.allow_function_calls )
	return fail_program_runtime(pgm,
				    "program::eval_expression rejected input: function calls are not allowed",
				    display_file.c_str());

    std::vector<std::string> allowed = expression_allowed_function_names(policy);
    for ( std::size_t i = 0; i < calls.size(); ++i )
    {
	if ( text_list_contains(allowed, calls[i]) )
	    continue;
	return fail_program_runtime(pgm,
				    std::string("program::eval_expression rejected function call to '")
				    + calls[i] + "'",
				    display_file.c_str());
    }
    return true;
}

// ---------------------------------------------------------------------------
// Host-call shims — the ONE call surface to compiled script functions.
// ---------------------------------------------------------------------------
// The module carries a synthesized `long __madc_shim_<sym>(char*, char*)`
// adapter per host-callable function (CirBuilder::synth_call_shim): a packed
// madc_value argument array in, one madc_value out. ALL signature knowledge
// (validation, class construction/destruction, retbuf, conversions) lives in
// the compiled adapter, derived from the parsed headers — the host side
// below is signature-blind.
typedef long (*madc_shim_fn)(void *args, void *out);

std::string shim_symbol_for(FuncDef *func, const std::string &name)
{
    return "__madc_shim_" + CirBuilder::call_emit_symbol(func, name);
}

bool invoke_call_shim(void *shim, const std::vector<value> &args,
		      value *result, std::string &error)
{
    std::vector<madc_value> cargs(args.size());
    for ( std::size_t i = 0; i < args.size(); ++i )
	madc_value_init(&cargs[i]);
    for ( std::size_t i = 0; i < args.size(); ++i )
    {
	if ( args[i].is_array() || args[i].is_object() )
	{
	    for ( std::size_t j = 0; j < i; ++j )
		madc_value_clear(&cargs[j]);
	    error = std::string("program::call cannot marshal argument kind '")
		    + value::kind_name(args[i].type()) + "'";
	    return false;
	}
	madc_value_copy(&cargs[i], &args[i].raw());
    }
    madc_value out;
    madc_value_init(&out);
    long rc = reinterpret_cast<madc_shim_fn>(shim)(
	cargs.empty() ? NULL : cargs.data(), &out);
    for ( std::size_t i = 0; i < cargs.size(); ++i )
	madc_value_clear(&cargs[i]);
    if ( rc != 0 )
    {
	madc_value_clear(&out);
	std::ostringstream os;
	if ( rc >= 1 && rc <= (long)args.size() )
	    os << "program::call argument " << (rc - 1)
	       << " has an incompatible kind";
	else
	    os << "program::call marshalling failed (shim code " << rc << ")";
	error = os.str();
	return false;
    }
    if ( result )
	*result = value::from_raw(out);
    madc_value_clear(&out);
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

    FuncDef *func = dynamic_cast<FuncDef *>(var->type);
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

    pgm.clear_diagnostics();
    pgm.clear_error();

    // CIR backend: this path serves ad-hoc child Programs (runtime eval /
    // the synthetic expression entry), so it builds a one-shot JIT session
    // for the bare Program — the named-entry analogue of madc_cir_execute.
    // The call itself goes through the function's synthesized marshalling
    // shim, like every host call.
    CirJitSession session;
    if ( !session.build(&pgm, "<eval>") )
	return fail_program_runtime(pgm, "eval compilation (CIR->MIR) failed");
    void *shim = session.function_code(shim_symbol_for(func, name).c_str());
    if ( !shim )
	return fail_program_runtime(pgm,
				    "program::call does not support this signature yet ('"
				    + name + "' has no marshalling shim)");
    if ( pgm.last_error.has_error )
	return false;

    try
    {
	pgm.push_runtime_scope();
	std::string err;
	bool ok = invoke_call_shim(shim, std::vector<value>(), &result, err);
	pgm.pop_runtime_scope();
	if ( !ok )
	    return fail_program_runtime(pgm, err);
	return true;
    }
    catch ( const std::exception &e )
    {
	pgm.pop_runtime_scope();
	return fail_program_runtime(pgm, e.what());
    }
}

std::string read_text_file(const std::string &path)
{
    std::ifstream is(path.c_str(), std::ios::binary);
    std::ostringstream os;
    os << is.rdbuf();
    return os.str();
}

bool text_list_contains(const std::vector<std::string> &items, const std::string &value)
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
	if ( !text_list_contains(dst, src[i]) )
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

    // A comparison yields int 0/1 regardless of operand types (C semantics);
    // never adopt a pointer or real OPERAND type as the comparison's result.
    // (Without this, `5 === 5.0` inferred double and `s !== "x"` inferred
    // char* — the host then mis-marshalled the int result.)
    if ( is_expression_compare_token(op->id()) )
	return &ddINT;

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
    // Split into lines, retaining each line's trailing '\n'.
    std::vector<std::string> lines;
    for ( size_t i = 0, n = source.size(); i < n; )
    {
	size_t eol = source.find('\n', i);
	size_t end = (eol == std::string::npos) ? n : eol + 1;
	lines.push_back(source.substr(i, end - i));
	i = end;
    }

    // Hoist the CONTIGUOUS leading run of preprocessor directives (blank lines
    // allowed) out of the wrapper FUNCTION BODY to file scope. A `#include`
    // left inside the body inlines the header's declarations as function-local
    // statements — e.g. <math.h> pulls <cmath>'s `namespace std`, whose
    // std-typed parameters then fail to resolve at function-body scope
    // ("Failed to find type 'std'"). Only the leading run is moved (the
    // conventional includes-at-top form), so code order is preserved and a
    // mid-body #if/#endif block is left untouched. Backslash-continued
    // directive lines are kept whole.
    auto first_nonws = [](const std::string &s) -> int {
	for ( size_t j = 0; j < s.size(); ++j )
	    if ( s[j] != ' ' && s[j] != '\t' && s[j] != '\r' && s[j] != '\n' )
		return (unsigned char)s[j];
	return -1;	// blank line
    };
    auto continues = [](const std::string &s) -> bool {
	size_t j = s.find_last_not_of("\r\n");
	return j != std::string::npos && s[j] == '\\';
    };
    std::string preamble, body;
    size_t k = 0;
    for ( ; k < lines.size(); ++k )
    {
	int c = first_nonws(lines[k]);
	if ( c == '#' )
	{
	    preamble += lines[k];
	    while ( continues(lines[k]) && k + 1 < lines.size() )
		preamble += lines[++k];
	    continue;
	}
	if ( c == -1 )		// blank line within the leading run
	{
	    preamble += lines[k];
	    continue;
	}
	break;			// first real code line — the rest is the body
    }
    for ( ; k < lines.size(); ++k )
	body += lines[k];

    std::ostringstream wrapped;
    wrapped << preamble
	    << return_type << " " << eval_entry_name() << "() {\n"
	    << body;
    if ( body.empty() || body[body.size() - 1] != '\n' )
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
    if ( !text_list_contains(allowed, call->var.name) )
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

std::vector<std::string> collect_expression_token_calls(const TokenStream &tokens,
							 const std::string &source_file)
{
    std::vector<std::string> calls;
    for ( std::size_t i = 0; i < tokens.size(); ++i )
    {
	TokenBase *tb = tokens[i];
	if ( !tb || tb->type() != TokenType::ttIdentifier )
	    continue;
	// The policy validates the USER EXPRESSION's tokens only. The eval TU
	// is synthesized (policy #includes + the expression) and tokenized
	// from ONE source file; the expression's tokens carry exactly that
	// file, while every #include'd token — the real header and its whole
	// transitive closure (/usr/include/math.h, bits/*.h, …) — carries its
	// own header path. Keep only tokens from the tokenized source file.
	// (The old policy-header-name EXCLUDE list matched the embedded-era
	// literal "math.h" token files but not real header paths, and never
	// covered transitive includes — real <math.h>'s `decltype (…)` and
	// `__iseqsig (…)` then poisoned the allowlist check.)
	if ( !tb->file || source_file != tb->file )
	    continue;
	if ( i + 1 >= tokens.size() || !tokens[i + 1] || tokens[i + 1]->id() != TokenID::tkOpBrk )
	    continue;
	std::string identifier = ((TokenIdent *)tb)->str;
	if ( is_expression_keyword_identifier(identifier) )
	    continue;
	if ( !text_list_contains(calls, identifier) )
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

// The recursion set here is mirrored by rewrite_expression_string_compares
// (the DSL strcmp lowering pass) — keep the two in sync.
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
		    if ( !text_list_contains(paths, path) )
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


// Storage-form read: marshal one scalar at `data` of type `type` into a
// host value. `data` may be the parser's var->data OR a resolved live MIR
// data-item address (get_global) — callers own that choice; this function
// must therefore access EXACTLY type->size bytes (an 8-byte read of a
// 4-byte int is fine on calloc'd parse-time storage but reads a neighbor
// on real MIR data layout).
bool value_from_storage(DataDef *type, void *data, size_t count,
			bool is_array_like, value &out)
{
    if ( !type || !data )
	return false;

    if ( count != 1 || is_array_like )
	return false;

    if ( type == &ddBOOL )
    {
	out = value(static_cast<bool>(*static_cast<bool *>(data)));
	return true;
    }
    if ( type->is_integer() || type->is_pointer() )
    {
	bool u = type->is_unsigned();
	int64_t v = 0;
	switch ( type->size )
	{
	    case 1:
		v = u ? (int64_t)*static_cast<uint8_t *>(data)
		      : (int64_t)*static_cast<int8_t *>(data);
		break;
	    case 2:
	    case 3:	// int24 storage mirrors the 16-bit write dispatch
		v = u ? (int64_t)*static_cast<uint16_t *>(data)
		      : (int64_t)*static_cast<int16_t *>(data);
		break;
	    case 4:
		v = u ? (int64_t)*static_cast<uint32_t *>(data)
		      : (int64_t)*static_cast<int32_t *>(data);
		break;
	    case 8:
		v = *static_cast<int64_t *>(data);
		break;
	    default:
		return false;
	}
	out = value(v);
	return true;
    }
    if ( type->is_real() )
    {
	out = value(type->size == 4
		    ? static_cast<double>(*static_cast<float *>(data))
		    : *static_cast<double *>(data));
	return true;
    }
    return false;
}

bool value_from_variable(Variable *var, value &out)
{
    if ( !var )
	return false;
    return value_from_storage(var->type, var->data, var->count,
			      var->is_fixed_array() || var->is_vla(), out);
}

// Storage-form write: see value_from_storage — accesses exactly
// type->size bytes so a resolved MIR data address is safe (the old
// dd-identity dispatch wrote ddINT as int64: an 8-byte store on a 4-byte
// int, neighbor-clobbering on real data layout).
bool set_storage_from_value(DataDef *type, void *data, size_t count,
			    bool is_array_like, const value &in)
{
    if ( !type || !data )
	return false;

    if ( count != 1 || is_array_like )
	return false;

    if ( type == &ddBOOL )
    {
	*static_cast<bool *>(data) = in.as_boolean();
	return true;
    }
    if ( type == &ddCHARptr && in.is_string() )
    {
	// Text binding: the storage OWNS a copy (addLiteral's `data holds
	// a stable char*` convention). Borrowing the value's payload broke
	// here: the JIT build that reads this pointer runs LAZILY at call
	// time, after eval_expression clears the bindings map (a latent
	// use-after-free in the std::string era, exposed by SSO).
	size_t len = 0;
	const char *text = madc_value_text(&in.raw(), &len);
	char *copy = static_cast<char *>(std::malloc(len + 1));
	if ( copy == NULL )
	    return false;
	std::memcpy(copy, text ? text : "", len);
	copy[len] = '\0';
	*static_cast<const char **>(data) = copy;
	return true;
    }
    if ( type->is_pointer() )
	return false;
    if ( type->is_integer() )
    {
	if ( !in.is_integer() )
	    return false;
	int64_t v = in.as_integer();
	switch ( type->size )
	{
	    case 1:
		*static_cast<uint8_t *>(data) = static_cast<uint8_t>(v);
		break;
	    case 2:
	    case 3:	// int24 keeps its historical 16-bit storage dispatch
		*static_cast<uint16_t *>(data) = static_cast<uint16_t>(v);
		break;
	    case 4:
		*static_cast<uint32_t *>(data) = static_cast<uint32_t>(v);
		break;
	    case 8:
		*static_cast<int64_t *>(data) = v;
		break;
	    default:
		return false;
	}
	return true;
    }
    if ( type->is_real() )
    {
	double d = in.is_real() ? in.as_real() : static_cast<double>(in.as_integer());
	if ( type->size == 4 )
	    *static_cast<float *>(data) = static_cast<float>(d);
	else
	    *static_cast<double *>(data) = d;
	return true;
    }
    return false;
}

bool set_variable_from_value(Variable *var, const value &in)
{
    if ( !var )
	return false;
    if ( !set_storage_from_value(var->type, var->data, var->count,
				 var->is_fixed_array() || var->is_vla(), in) )
	return false;
    var->modified();
    return true;
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
	    return &ddCHARptr;
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


} // namespace

// One host-callback registration as stored by program/engine impls. `entry`
// is the address the trampoline's import resolves to (the deduced form's
// typed adapter, or the explicit form's callback itself); `bound` carries
// the deduced form's user callback, passed as the adapter's hidden first
// argument (0 for the explicit form).
struct host_callback_entry
{
    std::string name;
    uintptr_t entry = 0;
    uintptr_t bound = 0;
    program::native_signature signature;
};

static bool valid_host_callback_name(const std::string &name)
{
    if ( name.empty() )
	return false;
    if ( !(isalpha((unsigned char)name[0]) || name[0] == '_') )
	return false;
    for ( char c : name )
	if ( !(isalnum((unsigned char)c) || c == '_') )
	    return false;
    return true;
}

static bool valid_host_callback_signature(const program::native_signature &sig)
{
    typedef program::native_type nt;
    switch ( sig.returns )
    {
	case nt::void_type: case nt::boolean: case nt::integer:
	case nt::real: case nt::c_string:
	    break;
	default:
	    return false;
    }
    for ( nt p : sig.parameters )
	if ( p != nt::boolean && p != nt::integer
	  && p != nt::real && p != nt::c_string )
	    return false;
    return true;
}

static int host_kind_from_native(program::native_type t)
{
    typedef program::native_type nt;
    switch ( t )
    {
	case nt::boolean:  return Program::HostCallbackReg::K_BOOL;
	case nt::integer:  return Program::HostCallbackReg::K_INT;
	case nt::real:     return Program::HostCallbackReg::K_REAL;
	case nt::c_string: return Program::HostCallbackReg::K_CSTR;
	default:           return Program::HostCallbackReg::K_VOID;
    }
}

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
    std::vector<void *> callback_trampolines;
    // Host-callback registrations (register_function): installed into the
    // fresh Program's host_callback_regs on every reset_program, so they
    // apply to whatever is compiled next.
    std::vector<host_callback_entry> host_callbacks;
    // The CIR->c2mir->MIR in-process JIT session behind exec/call/eval.
    // Built lazily on first run from the parsed Program; reset (with the
    // Program) on every recompile. See docs/plans/2026-06-10-libmadc-eval-
    // on-cir-plan.md.
    std::unique_ptr<CirJitSession> jit;
    bool compiled_ok = false;
    std::string compiled_display;

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
	// Callback trampolines were JIT-built by asmjit, which is gone.
	// The vector is never populated now; nothing to release.
    }

    MadcEngine &engine() { return *eng; }

    void reset_program()
    {
	eng->registration_policy = registration_policy_from_compile_options(current_compile_options);
	eng->registration_policy.runtime_eval_source_policy =
	    runtime_eval_child_policy_from_public(current_runtime_eval_policy);
	pgm = eng->create_program();
	pgm->aot_tracking = aot_mode;
	install_host_callbacks();
	runtime_initialized = false;
	jit.reset();           // the old session references the old Program's tree
	compiled_ok = false;
	compiled_display.clear();
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
	compiled_ok = true;
	compiled_display = path;
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
	compiled_ok = true;
	compiled_display = display_file;
	sync_public_errors();
	return true;
    }

    // Build (or reuse) the CIR->MIR JIT session for the compiled Program.
    bool ensure_jit_built()
    {
	if ( !compiled_ok || !pgm )
	    return fail_runtime("program requires a successfully compiled program");
	if ( jit && jit->built() )
	    return true;
	if ( !jit )
	    jit.reset(new CirJitSession());
	const char *name = compiled_display.empty() ? "<memory>"
						    : compiled_display.c_str();
	if ( !jit->build(pgm.get(), name) )
	{
	    jit.reset();
	    sync_public_errors();
	    return fail_runtime("program compilation (CIR->MIR) failed");
	}
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
	    return run_main_now();
	});
	if ( !ok && has_public_last_error && !pgm->last_error.has_error )
	    return false;
	if ( display_file != path )
	    sync_public_errors(display_file, path);
	else
	    sync_public_errors();
	return ok;
    }

    // Run the compiled program's main() through the CIR JIT session (the
    // body both exec lambdas share). A missing main is a runtime error,
    // matching the CLI's behavior.
    bool run_main_now()
    {
	if ( !ensure_jit_built() )
	    return false;
	static char progname[] = "madc-program";
	char *argv0[2] = { progname, NULL };
	bool ran = false;
	jit->run_main(1, argv0, &ran);
	if ( !ran )
	    return fail_runtime("program::exec: main() not found");
	runtime_initialized = !pgm->last_error.has_error;
	sync_public_errors();
	return !pgm->last_error.has_error;
    }

    bool exec_compiled_with_display(const std::string &actual_file,
				    const std::string &display_file)
    {
	if ( current_security_policy.execution == execution_mode::fork_per_invocation )
	    return exec_compiled_in_child(actual_file, display_file);

	bool ok = invoke_with_limits("exec", [this]() -> bool {
	    return run_main_now();
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
	    // The child runs main through the CIR JIT session (built here, in
	    // the child — that IS the fork-isolation model: the parent process
	    // never executes script code). run_main_now syncs pgm diagnostics;
	    // skip the re-sync when it failed via fail_runtime with no pgm
	    // error (e.g. missing main), as the non-fork exec paths do.
	    bool ran_ok = run_main_now();
	    if ( !(!ran_ok && has_public_last_error && !pgm->last_error.has_error) )
	    {
		if ( display_file != path )
		    sync_public_errors(display_file, path);
		else
		    sync_public_errors();
	    }

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

    bool validate_expression_function_policy(const TokenStream &tokens,
					    const std::string &source_file,
					    const std::string &display_file)
    {
	std::vector<std::string> calls = collect_expression_token_calls(tokens, source_file);
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
	    if ( text_list_contains(allowed, calls[i]) )
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

	if ( !validate_expression_function_policy(pgm->tokens, path,
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

	std::string compare_error;
	if ( !rewrite_expression_string_compares(*pgm, expr, compare_error) )
	{
	    clear_public_errors();
	    public_last_error = error(error::severity::error,
				      error::phase::runtime,
				      std::string("program::eval_expression rejected parsed expression: ")
				      + compare_error,
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
	if ( ok )
	{
	    compiled_ok = true;
	    compiled_display = display_file.empty() ? path : display_file;
	}
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
	// Explicit-signature form: the trampoline calls the callback itself
	// through the signature's low types (no adapter, no hidden arg).
	return add_host_callback(name, (uintptr_t)callback, 0, signature);
    }

    bool register_cpp_callback(const std::string &name,
			       void *callback_ptr,
			       const native_signature &signature,
			       native_function adapter_entry)
    {
	// Deduced form: the trampoline calls the typed adapter, passing the
	// user callback as the adapter's hidden first argument.
	return add_host_callback(name, (uintptr_t)adapter_entry,
				 (uintptr_t)callback_ptr, signature);
    }

    bool add_host_callback(const std::string &name, uintptr_t entry,
			   uintptr_t bound, const native_signature &signature)
    {
	if ( !valid_host_callback_name(name) )
	    return fail_runtime("register_function: invalid function name '"
				+ name + "'");
	if ( !entry )
	    return fail_runtime("register_function: null callback for '"
				+ name + "'");
	if ( !valid_host_callback_signature(signature) )
	    return fail_runtime("register_function: unsupported signature for '"
				+ name + "'");
	for ( const host_callback_entry &e : host_callbacks )
	    if ( e.name == name )
		return fail_runtime("register_function: '" + name
				    + "' is already registered");
	host_callback_entry e;
	e.name = name;
	e.entry = entry;
	e.bound = bound;
	e.signature = signature;
	host_callbacks.push_back(e);
	// Applies at the next compile: every compile path begins with
	// reset_program, which installs the registrations into the fresh
	// Program (install_host_callbacks).
	return true;
    }

    void install_host_callbacks()
    {
	pgm->host_callback_regs.clear();
	int k = 0;
	for ( const host_callback_entry &e : host_callbacks )
	{
	    Program::HostCallbackReg reg;
	    reg.name = e.name;
	    reg.import_sym = "__madc_host_cb_" + std::to_string(k++);
	    reg.entry = e.entry;
	    reg.bound = e.bound;
	    reg.returns = host_kind_from_native(e.signature.returns);
	    for ( program::native_type p : e.signature.parameters )
		reg.params.push_back(host_kind_from_native(p));
	    pgm->host_callback_regs.push_back(reg);
	}
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
	if ( !compiled_ok || !pgm )
	    return fail_runtime("program::call requires a successfully compiled program");

	// CIR backend: "runtime initialized" = the JIT session is built and
	// linked, and dynamic global initializers have run. (The asmjit-era
	// root_fn() top-level run is gone.)
	pgm->clear_diagnostics();
	pgm->clear_error();
	if ( !ensure_jit_built() )
	    return false;
	sync_public_errors();
	if ( pgm->last_error.has_error )
	    return false;
	// File-scope class globals (e.g. `std::string g = "hi";`) construct in
	// the synthesized module function __madc_global_init — main calls it
	// too, but a call-only session never runs main. The function carries a
	// static once-guard, so init-here + a later run_main stays single-shot.
	if ( jit )
	{
	    void *ginit = jit->function_code("__madc_global_init");
	    if ( ginit )
		((void (*)())ginit)();
	}
	runtime_initialized = true;
	return true;
    }

    bool should_fork_invocation() const
    {
	return current_security_policy.execution == execution_mode::fork_per_invocation;
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
	// CIR backend: a parsed function is callable — the JIT session
	// generates its code on first call; no per-Method code pointer.
	return true;
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

	FuncDef *func = dynamic_cast<FuncDef *>(var->type);
	if ( !func )
	    return fail_runtime("program::call target '" + name + "' has no function metadata");
	if ( func->is_multi_return() )
	    return fail_runtime("program::call does not support multi-return functions yet");
	if ( func->is_varargs )
	    return fail_runtime("program::call does not support variadic functions yet");
	if ( args.size() != func->parameters.size() )
	    return fail_runtime("program::call argument count mismatch for '" + name + "'");

	// The ONE call path: the function's synthesized marshalling shim
	// (CirBuilder::synth_call_shim). A function without one has a
	// signature the boundary cannot marshal yet.
	void *shim = jit ? jit->function_code(shim_symbol_for(func, name).c_str()) : NULL;
	if ( !shim )
	    return fail_runtime("program::call does not support this signature yet ('"
				+ name + "' has no marshalling shim)");

	try
	{
	    pgm->push_runtime_scope();
	    std::string err;
	    bool ok = invoke_call_shim(shim, args, result, err);
	    pgm->pop_runtime_scope();
	    if ( !ok )
		return fail_runtime(err);
	    return true;
	}
	catch ( const std::exception &e )
	{
	    pgm->pop_runtime_scope();
	    return fail_runtime(e.what());
	}
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

	    // Compiled code reads the MIR module's data item, not the
	    // parser's var->data backing store — resolve the live address
	    // (globals emit under their source identifier) and fall back to
	    // parse-time storage only when the item is absent.
	    void *storage = jit ? jit->data_address(id.c_str()) : NULL;
	    // Text-carrier globals (real libstdc++ std::string objects under
	    // the header model) marshal by reading the LIVE object — same
	    // mechanism as __madc_scope_set_string_runtime (parser.cpp).
	    // Requires the resolved MIR object; the parse-time fallback
	    // would read unconstructed memory.
	    if ( storage && var->count == 1 && var->type
	    &&   var->type->marshals_value_text() )
	    {
		*result = value(*static_cast<const std::string *>(storage));
		return true;
	    }
	    value out;
	    bool marshaled = storage
		? value_from_storage(var->type, storage, var->count,
				     var->is_fixed_array() || var->is_vla(), out)
		: value_from_variable(var, out);
	    if ( !marshaled )
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
		// Write the LIVE MIR data item when present (see get_global);
		// parse-time var->data is the fallback for unemitted vars.
		void *storage = jit ? jit->data_address(id.c_str()) : NULL;
		if ( storage && var->count == 1 && var->type
		&&   var->type->marshals_value_text() && new_value.is_string() )
		{
		    // Assign through the live object: the host's libstdc++
		    // operator= manages the real string's storage.
		    *static_cast<std::string *>(storage) = new_value.as_string();
		    var->modified();
		    return true;
		}
		bool stored = storage
		    ? set_storage_from_value(var->type, storage, var->count,
					     var->is_fixed_array() || var->is_vla(),
					     new_value)
		    : set_variable_from_value(var, new_value);
		if ( !stored )
		    return fail_runtime("program::set_global does not support this variable type yet");
		if ( storage )
		    var->modified();
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
    return _impl->pgm && _impl->compiled_ok;
}

bool program::save_object(const std::string &path)
{
    if ( !_impl->pgm || !_impl->compiled_ok )
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
    if ( !_impl->pgm || !_impl->compiled_ok )
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
    if ( !validate_expression_function_policy(child, policy, child.tokens,
					      display_name.empty() ? "<memory>" : display_name,
					      display_name) )
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

    std::string compare_error;
    if ( !rewrite_expression_string_compares(child, expr, compare_error) )
    {
	fail_program_runtime(child,
			     std::string("program::eval_expression rejected parsed expression: ")
			     + compare_error,
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
    std::vector<void *> callback_trampolines;
    // Engine-level host-callback registrations: copied into each program
    // created from this engine (create_program), ahead of the program's own.
    std::vector<host_callback_entry> host_callbacks;

    impl()
    {
	eng.capture_output_to_buffer();
	eng.capture_error_to_buffer();
    }

    ~impl()
    {
	// Callback trampolines were JIT-built by asmjit, which is gone.
	// The vector is never populated now; nothing to release.
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

static bool engine_add_host_callback(std::vector<host_callback_entry> &list,
				     const std::string &name, uintptr_t entry,
				     uintptr_t bound,
				     const program::native_signature &signature)
{
    if ( !valid_host_callback_name(name) || !entry
      || !valid_host_callback_signature(signature) )
	return false;
    for ( const host_callback_entry &e : list )
	if ( e.name == name )
	    return false;
    host_callback_entry e;
    e.name = name;
    e.entry = entry;
    e.bound = bound;
    e.signature = signature;
    list.push_back(e);
    return true;
}

bool engine::register_function(const std::string &name,
			       program::native_function callback,
			       const program::native_signature &signature)
{
    // Explicit-signature form; applies to programs created AFTER this call
    // (create_program copies the engine's registrations into the program).
    return engine_add_host_callback(_impl->host_callbacks, name,
				    (uintptr_t)callback, 0, signature);
}

bool engine::register_cpp_callback(const std::string &name,
				   void *callback_ptr,
				   const program::native_signature &signature,
				   program::native_function adapter_entry)
{
    // Deduced form: the adapter receives the user callback as its hidden
    // first argument.
    return engine_add_host_callback(_impl->host_callbacks, name,
				    (uintptr_t)adapter_entry,
				    (uintptr_t)callback_ptr, signature);
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
    // Engine-level host-callback registrations seed the program's list (the
    // program may add its own on top). Installed into the parse Program at
    // the next compile's reset_program.
    _impl->host_callbacks = eng._impl->host_callbacks;
}

} // namespace madc

//////////////////////////////////////////////////////////////////////////
// Legacy asmjit-JIT entry points — now stubs.
//
// The asmjit JIT codegen backend (compiler.cpp, typesafe.cpp,
// compiler_*.cpp) and the AOT ELF writer (madc_elf.cpp) were removed.
// CIR (madc parse → cir_node → c2mir → MIR) is the sole backend and is
// driven through madc_cir_execute(). The public Program methods below
// keep their signatures (for libmadc API/ABI stability) but no longer
// generate or run native code: they report a clear error and fail.
//////////////////////////////////////////////////////////////////////////

bool Program::compile()
{
    // On the CIR backend, Program-level compilation IS the front end: the
    // tokenize+parse that already ran. Code generation happens at the
    // CIR->c2mir->MIR stage (CirJitSession / madc_cir_execute), driven
    // lazily at execute/call/eval time. A parsed Program is compile-ready.
    //
    // ONE policy gate lives here: with dynamic symbol fallback disabled, a
    // USER-SOURCE prototype with no body and no sanctioned binding (no
    // emit_symbol, not an addFunction registration — those create FuncDefs
    // with declaration_only false) could only ever resolve through the JIT
    // link's dlsym fallback. Reject it at compile, the late-bind analogue of
    // the parse-time undeclared-identifier gate. Curated header declarations
    // (decl_file != the main translation unit) stay permitted.
    if ( !last_error.has_error && !is_dynamic_symbol_fallback_enabled()
      && tkProgram && !tkProgram->source.empty() )
    {
	for ( funcdef_map_iter fmi = funcdef_map.begin();
	      fmi != funcdef_map.end(); ++fmi )
	{
	    FuncDef *fd = fmi->second;
	    if ( !fd || !fd->declaration_only || !fd->emit_symbol.empty() )
		continue;
	    if ( !fd->decl_file || tkProgram->source != fd->decl_file )
		continue;
	    set_error(DiagnosticPhase::compiler,
		      "dynamic symbol fallback is disabled by registration policy");
	    return false;
	}
    }
    return !last_error.has_error;
}

void Program::execute()
{
    set_error(Program::DiagnosticPhase::runtime,
	      "Program::execute() is unavailable: the asmjit JIT codegen "
	      "backend was removed. Use the CIR backend via madc_cir_execute().");
}

bool Program::save_object(const std::string &path) const
{
    (void)path;
    std::cerr << "AOT object output is not available on the CIR backend; "
		 "emit C with --emit-c and compile with gcc/clang" << std::endl;
    return false;
}

bool Program::save_executable(const std::string &path)
{
    (void)path;
    std::cerr << "AOT executable output is not available on the CIR backend; "
		 "emit C with --emit-c and compile with gcc/clang" << std::endl;
    return false;
}

bool Program::load_object(const std::string &path)
{
    (void)path;
    std::cerr << "loading saved AOT objects is not available on the CIR "
		 "backend (the asmjit/ELF code path was removed)" << std::endl;
    return false;
}

bool Program::has_loaded_function(const std::string &name) const
{
    (void)name;
    return false;
}

void *Program::loaded_function_ptr(const std::string &name) const
{
    (void)name;
    return NULL;
}

void Program::unload_object()
{
}

void Program::add_include_dir(const std::string &dir)
{
	if (dir.empty())
		return;
	std::string p = dir;
	if (p.back() != '/')
		p += '/';
	include_paths.push_back(p);
}

void Program::add_cli_define(const std::string &def)
{
	if (def.empty())
		return;
	std::string::size_type eq = def.find('=');
	std::string name = (eq == std::string::npos) ? def : def.substr(0, eq);
	std::string value = (eq == std::string::npos) ? std::string("1") : def.substr(eq + 1);
	if (!name.empty())
		cli_defines.push_back(std::make_pair(name, value));
}
