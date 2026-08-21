/* cir_format.cpp — the GENERATED side of std::format / std::print /
 * std::println.
 *
 * These are declared in <bits/std_format> and defined NOWHERE: the compiler is
 * their implementation (the php::print_r / php::var_dump carrier — see
 * cir_dump.cpp, whose structure this file mirrors). A call site knows the
 * LITERAL format string and every argument's concrete type, so:
 *
 *   - the format string is parsed and VALIDATED here, at compile time, by the
 *     SAME C engine the runtime uses (src/rt/rt_format.c — one grammar owner,
 *     linked into madc itself). An invalid format string, a bad presentation
 *     type for an argument's type, an out-of-range index: all compile errors,
 *     which is exactly the C++23 format_string contract — achieved without
 *     any consteval machinery because the compiler IS the implementation;
 *
 *   - each literal run and each replacement field lowers to one typed call
 *     into the engine's primitives, against the dump runtime's byte sink
 *     (NULL -> stdout for print/println, a capture sink for format whose
 *     bytes __madc_fmt_take_cstr hands to the shared text ring — format
 *     returns ring-lifetime const char*, the c_str() contract, so dialect
 *     TUs never pull the <string> closure).
 *
 * libstdc++'s real std::format is the oracle for every emission behavior
 * (tests/unit/rt_format_oracle.inc); this file owns only the compile-time
 * validation and the call shapes.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <iostream>
#include <stdint.h>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "token_arena.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_dl.h"
#include "cir_builder.h"

extern "C" {
#include "c2mir/c2mir_api.h"
}

#include "rt/rt_format.h"

extern thread_local bool madc_verbose;

// ---------------------------------------------------------------------------
// Which compiler-implemented format intrinsic is this call?
// ---------------------------------------------------------------------------
// Reads the inline_builtin_kind tag the parser stamped at declaration
// registration (provenance-gated to <bits/std_format>), converted to an enum
// here at the boundary — enum-over-strings, the dump_flavor pattern.
static CirBuilder::FormatFlavor format_flavor(FuncDef *fd)
{
	if ( !fd )
		return CirBuilder::ffNone;
	if ( fd->inline_builtin_kind == "std_format" )
		return CirBuilder::ffFormat;
	if ( fd->inline_builtin_kind == "std_print" )
		return CirBuilder::ffPrint;
	if ( fd->inline_builtin_kind == "std_println" )
		return CirBuilder::ffPrintln;
	return CirBuilder::ffNone;
}

static const char *format_flavor_name(CirBuilder::FormatFlavor fl)
{
	if ( fl == CirBuilder::ffFormat )
		return "std::format";
	return fl == CirBuilder::ffPrintln ? "std::println" : "std::print";
}

// Is this presentation type char in the allowed set for an argument kind?
// The sets come straight from [format.string.std] as the oracle confirms
// them; "" (type 0) is always the kind's default presentation.
static bool format_type_allowed(char t, const char *allowed)
{
	return t == 0 || strchr(allowed, t) != NULL;
}

// ---------------------------------------------------------------------------
// Emission helpers
// ---------------------------------------------------------------------------

node_t CirBuilder::format_sink_arg(const std::string &sink_var,
				   TokenBase *origin)
{
	if ( sink_var.empty() )
		return node2(N_CAST, void_ptr_type(), integer(0, origin),
			     origin);
	return id(sink_var.c_str(), origin);
}

node_t CirBuilder::format_text_stmt(const std::string &bytes,
				    const std::string &sink_var,
				    TokenBase *origin)
{
	need_output_extern("__madc_fmt_text", false,
			   { { {N_VOID}, true }, { {N_CHAR}, true },
			     { {N_LONG, N_LONG}, false } });
	node_t a = list();
	append(a, format_sink_arg(sink_var, origin));
	append(a, str(bytes.c_str(), bytes.size() + 1, origin));
	append(a, integer((long)bytes.size(), origin));
	return node2(N_EXPR, list(),
		     node2(N_CALL, id("__madc_fmt_text", origin), a, origin),
		     origin);
}

bool CirBuilder::format_field_stmt(TokenBase *arg, const std::string &spec,
				   const std::string &sink_var,
				   std::vector<node_t> &out, TokenBase *origin,
				   std::string &why)
{
	DataDef *dd = arg ? arg->datadef() : NULL;
	if ( !dd )
	{
		why = "argument has no resolved type";
		return false;
	}
	DataDef *u = dd->unqualified();

	// A value& parameter (a reference over the carrier) IS the carrier
	// for formatting purposes — unwrap it here; the fkValue lowering's
	// object_arg_addr already reads a reference variable's stored
	// pointer. Without this the reference fell into the pointer arm
	// ("no formatter for pointer type 'array*'").
	if ( u->is_reference() )
	{
		DataDefPTR *rp = dynamic_cast<DataDefPTR *>(u);
		DataDef *base = rp ? rp->base_type : NULL;
		if ( base && base->unqualified()->is_madc_array() )
			u = base->unqualified();
	}

	// The spec's SHAPE — parsed by the one engine parser, so the message a
	// user sees at compile time names the same rule the runtime enforces.
	madc_fmt_spec sp;
	if ( const char *perr = __madc_fmt_parse_spec(spec.data(),
						      (long long)spec.size(),
						      &sp) )
	{
		why = perr;
		return false;
	}

	enum {
		fkI64, fkU64, fkF64, fkCstr, fkStdString, fkChar, fkBool,
		fkValue, fkPtr
	} kind;

	// Order matters exactly as dump_scalar documents: text pointers before
	// the integer arms (is_integer() is true for every pointer), the value
	// carrier and the string class before the scalar predicates.
	if ( u->is_madc_array() )
		kind = fkValue;
	else if ( u->is_cstr() )
		kind = fkCstr;
	else if ( is_class_object(u)
	       && strip_inline_namespaces(type_alias_spelling(u))
		  == "std::string" )
		kind = fkStdString;
	else if ( u->is_pointer() )
	{
		// std gives only void* (and nullptr) a formatter; any other
		// pointer type is ill-formed in real C++ too.
		DataDefPTR *up = dynamic_cast<DataDefPTR *>(u);
		DataDef *pt = up ? up->base_type : NULL;
		if ( pt && pt->rawtype() == DataType::dtVOID )
			kind = fkPtr;
		else
		{
			why = std::string("no formatter for pointer type '")
			    + u->name + "' (std::format formats void* — "
			      "cast the pointer)";
			return false;
		}
	}
	else if ( u->is_reference() || u->is_member_pointer()
	       || u->is_function() || u->is_simd() || u->is_complex()
	       || u->is_object() )
	{
		why = std::string("no formatter for type '") + u->name + "'";
		return false;
	}
	else if ( u->rawtype() == DataType::dtBOOL )
		kind = fkBool;
	else if ( u->rawtype() == DataType::dtINT8
	       || u->rawtype() == DataType::dtUINT8 )
		kind = fkChar;
	else if ( u->is_real() )
		kind = fkF64;
	else if ( u->is_integer() )
		kind = u->is_unsigned() ? fkU64 : fkI64;
	else
	{
		why = std::string("no formatter for type '") + u->name + "'";
		return false;
	}

	// Presentation-type validity, and the non-arithmetic restrictions
	// ([format.string.std]: sign/#/0 apply only to arithmetic
	// presentations; precision only to strings and floating point).
	switch ( kind )
	{
	case fkI64:
	case fkU64:
		if ( !format_type_allowed(sp.type, "bBcdoxX") )
		{
			why = std::string("format-spec type '") + sp.type
			    + "' is not valid for an integer argument";
			return false;
		}
		if ( sp.precision >= 0 )
		{
			why = "precision is not valid for an integer argument";
			return false;
		}
		break;
	case fkF64:
		if ( !format_type_allowed(sp.type, "fFeEgGaA") )
		{
			why = std::string("format-spec type '") + sp.type
			    + "' is not valid for a floating-point argument";
			return false;
		}
		break;
	case fkCstr:
	case fkStdString:
		if ( !format_type_allowed(sp.type, "s") )
		{
			why = std::string("format-spec type '") + sp.type
			    + "' is not valid for a string argument";
			return false;
		}
		if ( sp.sign || sp.alt || sp.zero )
		{
			why = "sign, '#' and '0' are not valid for a string "
			      "argument";
			return false;
		}
		break;
	case fkChar:
		if ( !format_type_allowed(sp.type, "cbBdoxX") )
		{
			why = std::string("format-spec type '") + sp.type
			    + "' is not valid for a char argument";
			return false;
		}
		if ( sp.precision >= 0 )
		{
			why = "precision is not valid for a char argument";
			return false;
		}
		if ( (sp.type == 0 || sp.type == 'c')
		  && (sp.sign || sp.alt || sp.zero) )
		{
			why = "sign, '#' and '0' are only valid for a char's "
			      "integer presentations";
			return false;
		}
		break;
	case fkBool:
		if ( !format_type_allowed(sp.type, "sbBdoxX") )
		{
			why = std::string("format-spec type '") + sp.type
			    + "' is not valid for a bool argument";
			return false;
		}
		if ( sp.precision >= 0 )
		{
			why = "precision is not valid for a bool argument";
			return false;
		}
		if ( (sp.type == 0 || sp.type == 's')
		  && (sp.sign || sp.alt || sp.zero) )
		{
			why = "sign, '#' and '0' are only valid for a bool's "
			      "integer presentations";
			return false;
		}
		break;
	case fkPtr:
		if ( !format_type_allowed(sp.type, "p") )
		{
			why = std::string("format-spec type '") + sp.type
			    + "' is not valid for a pointer argument";
			return false;
		}
		if ( sp.sign || sp.alt || sp.zero || sp.precision >= 0 )
		{
			why = "only fill, align and width apply to a pointer "
			      "argument";
			return false;
		}
		break;
	case fkValue:
		// A value's kind is only known at run time — the compile
		// check admits any presentation some kind accepts, and the
		// runtime dispatch enforces the rest (loud marker).
		if ( !format_type_allowed(sp.type, "sbBcdoxXfFeEgGaA") )
		{
			why = std::string("format-spec type '") + sp.type
			    + "' is not valid for any value kind";
			return false;
		}
		break;
	}

	const char *sym = NULL;
	node_t val = NULL;
	std::vector<ExternParam> params;
	params.push_back({ {N_VOID}, true });		// sink
	params.push_back({ {N_CHAR}, true });		// spec bytes
	params.push_back({ {N_LONG, N_LONG}, false });	// spec length
	switch ( kind )
	{
	case fkI64:
		sym = "__madc_fmt_i64";
		params.push_back({ {N_LONG, N_LONG}, false });
		val = translate_expr(arg);
		break;
	case fkU64:
		sym = "__madc_fmt_u64";
		params.push_back({ {N_UNSIGNED, N_LONG, N_LONG}, false });
		val = translate_expr(arg);
		break;
	case fkF64:
		sym = "__madc_fmt_f64";
		params.push_back({ {N_DOUBLE}, false });
		val = translate_expr(arg);
		break;
	case fkCstr:
		sym = "__madc_fmt_cstr";
		params.push_back({ {N_CHAR}, true });
		val = translate_expr(arg);
		break;
	case fkChar:
		sym = "__madc_fmt_char";
		params.push_back({ {N_INT}, false });
		val = translate_expr(arg);
		break;
	case fkBool:
		sym = "__madc_fmt_bool";
		params.push_back({ {N_INT}, false });
		val = translate_expr(arg);
		break;
	case fkPtr:
		sym = "__madc_fmt_ptr";
		params.push_back({ {N_VOID}, true });
		val = translate_expr(arg);
		break;
	case fkStdString:
	case fkValue:
	{
		sym = kind == fkValue ? "__madc_fmt_value"
				      : "__madc_fmt_stdstring";
		params.push_back({ {N_VOID}, true });
		DataDefCLASS *cls = dynamic_cast<DataDefCLASS *>(u);
		if ( !cls )
		{
			why = "argument's class type is unresolved";
			return false;
		}
		val = node2(N_CAST, void_ptr_type(),
			    object_arg_addr(arg, cls), origin);
		break;
	}
	}

	need_output_extern(sym, false, params);
	{
		node_t a = list();
		append(a, format_sink_arg(sink_var, origin));
		append(a, str(spec.c_str(), spec.size() + 1, origin));
		append(a, integer((long)spec.size(), origin));
		append(a, val);
		out.push_back(node2(N_EXPR, list(),
				    node2(N_CALL, id(sym, origin), a, origin),
				    origin));
	}
	return true;
}

bool CirBuilder::format_intrinsic_call(TokenCallFunc *tcf)
{
	if ( !tcf )
		return false;
	if ( format_flavor(call_target_funcdef(tcf)) != ffNone )
		return true;
	return format_flavor(dynamic_cast<FuncDef *>(tcf->var.type)) != ffNone;
}

// ---------------------------------------------------------------------------
// The call intercept
// ---------------------------------------------------------------------------
// Returns NULL when the callee is not a format intrinsic, so translate_expr's
// ordinary call path continues untouched.
node_t CirBuilder::lower_format_call(TokenCallFunc *tcf, FuncDef *fd,
				     TokenBase *origin)
{
	if ( !tcf )
		return NULL;
	if ( getenv("MADC_FMT_DEBUG") )
	{
		FuncDef *vfd = dynamic_cast<FuncDef *>(tcf->var.type);
		fprintf(stderr, "[fmt-hook] name=%s fd_kind=%s var_kind=%s\n",
			tcf->var.name.c_str(),
			fd ? fd->inline_builtin_kind.c_str() : "(no fd)",
			vfd ? vfd->inline_builtin_kind.c_str() : "(no var fd)");
	}
	FormatFlavor fl = format_flavor(fd);
	if ( fl == ffNone )
		// Same placeholder situation as lower_dump_call: the intrinsic
		// tag lives on the DECLARATION the call token is bound to.
		fl = format_flavor(dynamic_cast<FuncDef *>(tcf->var.type));
	if ( fl == ffNone )
		return NULL;
	const char *fname = format_flavor_name(fl);

	if ( tcf->parameters.empty() )
		return error_node((std::string(fname)
				   + " needs a format string").c_str(),
				  origin);
	// C++23 [print.fun]: print/println also take a LEADING stream
	// (`std::print(stderr, "...", ...)`). The stream argument is any
	// non-char pointer expression (FILE* — stderr/stdout/an opened
	// stream); the format literal then sits one slot later. Detected
	// from the actual argument's type, not from which declared overload
	// bound, so overload-ranking subtleties cannot misroute a call.
	// std::format has no stream form.
	size_t fmt_at = 0;
	TokenBase *stream_tok = NULL;
	if ( (fl == ffPrint || fl == ffPrintln)
	     && tcf->parameters.size() >= 2 )
	{
		TokenBase *a0 = tcf->parameters[0];
		DataDef *a0dd = a0 ? a0->datadef() : NULL;
		bool a0_char_ptr = false;
		if ( DataDefPTR *a0p = dynamic_cast<DataDefPTR *>(a0dd) )
			a0_char_ptr = a0p->base_type
				   && a0p->base_type->rawtype() == DataType::dtCHAR;
		bool a0_literal = a0 && a0->type() == TokenType::ttString;
		if ( !a0_literal )
			if ( TokenVar *a0v = dynamic_cast<TokenVar *>(a0) )
				a0_literal = a0v->var.name.compare(
					0, 11, "__literal__") == 0;
		if ( !a0_literal && a0dd && a0dd->is_pointer() && !a0_char_ptr )
		{
			stream_tok = a0;
			fmt_at = 1;
		}
	}
	// The literal's two arrival shapes: a raw TokenStr, or (the usual
	// expression path) the `__literal__<text>` const char* Variable
	// parseExpr materializes via addLiteral — the bytes ARE the name
	// suffix, the same read the subscript and global-init arms do.
	TokenBase *ftok = tcf->parameters[fmt_at];
	std::string f;
	bool have_literal = false;
	if ( ftok && ftok->type() == TokenType::ttString )
	{
		TokenStr *ts = dynamic_cast<TokenStr *>(ftok);
		if ( ts )
		{
			if ( ts->wide )
				return error_node((std::string(fname)
					+ ": wide format strings are not "
					  "supported").c_str(), origin);
			f = ts->str;
			have_literal = true;
		}
	}
	else if ( ftok && ftok->type() == TokenType::ttVariable )
	{
		TokenVar *tv = dynamic_cast<TokenVar *>(ftok);
		if ( tv && tv->var.name.compare(0, 11, "__literal__") == 0 )
		{
			f = tv->var.name.substr(11);
			have_literal = true;
		}
	}
	if ( !have_literal )
		return error_node((std::string(fname)
				   + ": the format string must be a string "
				     "literal (runtime format strings arrive "
				     "with std::vformat)").c_str(), origin);
	size_t nargs = tcf->parameters.size() - 1 - fmt_at;

	std::vector<node_t> stmts;
	std::string sink_var;
	if ( fl == ffFormat || stream_tok )
	{
		// void *<sink> = __madc_dump_sink_open();   (format capture)
		// void *<sink> = __madc_dump_sink_file(f);  (stream print)
		char sname[40];
		snprintf(sname, sizeof sname, "__madc_fmtsink_%d",
			 m_strtmp_counter++);
		sink_var = sname;
		node_t sinit;
		if ( stream_tok )
		{
			need_output_extern("__madc_dump_sink_file", true,
					   { { {N_VOID}, true } });
			node_t sa = list();
			append(sa, node2(N_CAST, void_ptr_type(),
					 translate_expr(stream_tok), origin));
			sinit = node2(N_CALL,
				      id("__madc_dump_sink_file", origin),
				      sa, origin);
		}
		else
		{
			need_output_extern("__madc_dump_sink_open", true, {});
			sinit = node2(N_CALL,
				      id("__madc_dump_sink_open", origin),
				      list(), origin);
		}
		node_t sspec = list();
		append(sspec, simple(N_VOID, origin));
		node_t sdeclr = list();
		append(sdeclr, pointer());
		node_t sdecl = simple(N_SPEC_DECL, origin);
		append(sdecl, node1(N_SHARE, sspec));
		append(sdecl, node2(N_DECL, id(sname, origin), sdeclr));
		append(sdecl, ignore());
		append(sdecl, ignore());
		append(sdecl, sinit);
		stmts.push_back(sdecl);
	}

	// Walk the literal with the engine's own iterator. Indexing mode
	// ([format.string]/automatic vs manual) is enforced here — the
	// iterator sees one field at a time and cannot.
	long long pos = 0;
	int automatic = -1;	// -1 unknown, 1 automatic ({}), 0 manual ({0})
	size_t next_arg = 0;
	for ( ;; )
	{
		madc_fmt_item it;
		const char *ierr = NULL;
		long long nx = __madc_fmt_next(f.data(), (long long)f.size(),
					       pos, &it, &ierr);
		if ( nx == -1 )
			break;
		if ( nx == -2 )
			return error_node((std::string(fname) + ": "
					   + ierr).c_str(), origin);
		pos = nx;
		if ( it.kind == MADC_FMT_TEXT )
		{
			if ( it.text_n > 0 )
				stmts.push_back(format_text_stmt(
					std::string(it.text,
						    (size_t)it.text_n),
					sink_var, origin));
			continue;
		}
		size_t ai;
		if ( it.arg_id >= 0 )
		{
			if ( automatic == 1 )
				return error_node((std::string(fname)
					+ ": cannot mix manual ({0}) and "
					  "automatic ({}) argument indexing")
					.c_str(), origin);
			automatic = 0;
			ai = (size_t)it.arg_id;
		}
		else
		{
			if ( automatic == 0 )
				return error_node((std::string(fname)
					+ ": cannot mix manual ({0}) and "
					  "automatic ({}) argument indexing")
					.c_str(), origin);
			automatic = 1;
			ai = next_arg++;
		}
		if ( ai >= nargs )
		{
			char msg[160];
			snprintf(msg, sizeof msg,
				 "%s: the format string wants argument %zu "
				 "but the call passes %zu",
				 fname, ai, nargs);
			return error_node(msg, origin);
		}
		std::string spec(it.spec ? it.spec : "", (size_t)it.spec_n);
		std::string why;
		if ( !format_field_stmt(tcf->parameters[fmt_at + 1 + ai], spec,
					sink_var, stmts, origin, why) )
			return error_node((std::string(fname) + ": "
					   + why).c_str(), origin);
	}
	if ( fl == ffPrintln )
		stmts.push_back(format_text_stmt(std::string("\n"), sink_var,
						 origin));
	// The stream sink is per-call: release it after the last byte.
	if ( stream_tok )
	{
		need_output_extern("__madc_dump_sink_close", false,
				   { { {N_VOID}, true } });
		node_t ca = list();
		append(ca, id(sink_var.c_str(), origin));
		stmts.push_back(node2(N_EXPR, list(),
				      node2(N_CALL,
					    id("__madc_dump_sink_close",
					       origin), ca, origin), origin));
	}

	if ( fl == ffFormat )
	{
		// The result: ring-lifetime const char* (the c_str() contract
		// — the fragment declares `const char *format(...)`, so
		// dialect TUs never pull the <string> closure). One statement
		// expression: the sink decl + field statements, closed by the
		// take call that moves the sink's bytes into the shared text
		// ring and closes the sink. Nothing here owns a destructor,
		// so the old hoisted-std::string shape (and its cleanup
		// choreography) is gone with the std::string return.
		need_output_extern("__madc_fmt_take_cstr", true,
				   { { {N_VOID}, true } });
		node_t a = list();
		append(a, id(sink_var.c_str(), origin));
		node_t take = node2(N_CAST, char_ptr_type(),
				    node2(N_CALL,
					  id("__madc_fmt_take_cstr", origin),
					  a, origin), origin);
		node_t items = list();
		for ( size_t i = 0; i < stmts.size(); i++ )
			append(items, stmts[i]);
		append(items, node2(N_EXPR, list(), take, origin));
		return node1(N_STMTEXPR,
			     node2(N_BLOCK, list(), items, origin), origin);
	}

	// print/println in expression position: one statement expression,
	// closed with a discarded 0 (c2mir wants the last statement to be an
	// expression — the dump lowering's rule).
	node_t items = list();
	for ( size_t i = 0; i < stmts.size(); i++ )
		append(items, stmts[i]);
	append(items, node2(N_EXPR, list(), integer(0, origin), origin));
	return node1(N_STMTEXPR, node2(N_BLOCK, list(), items, origin),
		     origin);
}
