/* cir_dump.cpp — the GENERATED side of php::print_r / php::var_dump.
 *
 * These are declared in <ns_php> and defined NOWHERE: the compiler is their
 * implementation. A call site knows the concrete argument type, so the dumper
 * for that type is generated here (approach A) and the runtime carries only
 * OUTPUT PRIMITIVES (src/rt/rt_dump.c). No runtime type-descriptor table
 * exists, so a program that never dumps pays nothing.
 *
 * ONE WALK, TWO RENDERERS. print_r and var_dump iterate identically — the same
 * member census, the same access nodes, the same array loop — and differ only
 * in framing, which every dump_* helper below selects on the flavor.
 *
 * PHP is the oracle: php::print_r must render a madc value the way a PHP
 * developer expects PHP to render it, and php::var_dump the same except that it
 * reports REAL C/C++/madc types instead of simulated PHP ones. The contract,
 * with php-cli 8.3.6 output captured verbatim, is
 * docs/plans/2026-08-17-php-print-r-var-dump-plan.md.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <list>
#include <queue>
#include <algorithm>
#include <iostream>
#include <sstream>
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

extern thread_local bool madc_verbose;

// ---------------------------------------------------------------------------
// Which compiler-implemented dump is this call?
// ---------------------------------------------------------------------------
// The ONE place a dump intrinsic's spelling is compared. The parser stamped
// FuncDef::inline_builtin_kind at declaration registration (the same carrier
// std::addressof / std::forward / __destroy use, and one the forest already
// serializes), so this reads a tag rather than a user-facing name, and it is
// converted to an enum here — at the boundary — per enum-over-strings.
static CirBuilder::DumpFlavor dump_flavor(FuncDef *fd)
{
	if (!fd)
		return CirBuilder::dfNone;
	if (fd->inline_builtin_kind == "php_print_r")
		return CirBuilder::dfPrintR;
	if (fd->inline_builtin_kind == "php_var_dump")
		return CirBuilder::dfVarDump;
	return CirBuilder::dfNone;
}

// The intrinsic's own name, for a diagnostic.
static const char *dump_flavor_name(CirBuilder::DumpFlavor fl)
{
	return fl == CirBuilder::dfVarDump ? "php::var_dump" : "php::print_r";
}

// ---------------------------------------------------------------------------
// Columns
// ---------------------------------------------------------------------------
// print_r frames an aggregate with its "(" 8 per level and entries 4 further
// in; var_dump indents 2 per level, and an entry's key line and value line sit
// at the SAME column. Both captured with cat -A from php-cli 8.3.6
// (tmp/or_pr2.php, tmp/or_vd.php) — do not "tidy" either step.
static int dump_frame_col(CirBuilder::DumpFlavor fl, int depth)
{
	return fl == CirBuilder::dfVarDump ? 2 * depth : 8 * depth;
}

static int dump_entry_col(CirBuilder::DumpFlavor fl, int depth)
{
	return dump_frame_col(fl, depth) + (fl == CirBuilder::dfVarDump ? 2 : 4);
}

// ---------------------------------------------------------------------------
// Type words (var_dump only)
// ---------------------------------------------------------------------------
// var_dump's one deliberate divergence from PHP: it names the REAL type. The
// spelling comes from the type's own DataType, so it is the CANONICAL type and
// not the source's typedef — the same thing g++'s typeid reports. `long long`
// and `long` share dtINT64 in madc and therefore share the word "long".
static std::string dump_scalar_type_word(DataDef *dd)
{
	if (!dd)
		return "?";
	if (dd->is_cstr())
		return "char *";
	switch (dd->rawtype()) {
	case DataType::dtBOOL:     return "bool";
	case DataType::dtINT8:     return "char";
	case DataType::dtUINT8:    return "unsigned char";
	case DataType::dtINT16:    return "short";
	case DataType::dtUINT16:   return "unsigned short";
	case DataType::dtINT24:    return "int24_t";
	case DataType::dtUINT24:   return "uint24_t";
	case DataType::dtINT32:    return "int";
	case DataType::dtUINT32:   return "unsigned int";
	case DataType::dtINT64:    return "long";
	case DataType::dtUINT64:   return "unsigned long";
	case DataType::dtINT128:   return "__int128";
	case DataType::dtUINT128:  return "unsigned __int128";
	case DataType::dtFLOAT:    return "float";
	case DataType::dtDOUBLE:   return "double";
	case DataType::dtLDOUBLE:  return "long double";
	default:                   return dd->name;
	}
}

// An aggregate's type word. `struct` for every non-union aggregate, `union` for
// a union: madc PROMOTES a plain struct to DataDefCLASS when it earns
// class-hood (an object member, an NSDMI, a nested type), so "class" would be a
// claim about the SOURCE that the type graph cannot support.
static std::string dump_aggregate_type_word(DataDefSTRUCT *sdd)
{
	std::string kind = sdd->union_layout ? "union " : "struct ";
	return kind + sdd->name;
}

// An array's type word: the element's, with the extent.
static std::string dump_array_type_word(DataDef *elem, size_t count)
{
	char n[32];
	snprintf(n, sizeof(n), "[%llu]", (unsigned long long)count);
	return dump_scalar_type_word(elem) + n;
}

// PHP's key spelling for a member, per flavor. print_r writes
// "[prot:protected]" and "[priv:Foo:private]"; var_dump quotes the name and the
// class: ["prot":protected] and ["priv":"Foo":private].
static std::string dump_member_key(CirBuilder::DumpFlavor fl,
				   const std::string &name, uint32_t access,
				   const std::string &owner)
{
	bool q = fl == CirBuilder::dfVarDump;
	std::string key = q ? "\"" + name + "\"" : name;
	if (access & vfPRIVATE)
		key += q ? ":\"" + owner + "\":private" : ":" + owner + ":private";
	else if (access & vfPROTECTED)
		key += ":protected";
	return key;
}

// ---------------------------------------------------------------------------
// Output-primitive call builders
// ---------------------------------------------------------------------------
// One place per primitive, so the extern proto and the call are never spelled
// apart. Every column and every key is a compile-time constant: the walk is
// EXPANDED per nesting level, so the indentation needs no runtime state.

node_t CirBuilder::dump_call_stmt(const char *sym, node_t args, TokenBase *origin)
{
	return node2(N_EXPR, list(), node2(N_CALL, id(sym, origin), args, origin),
		     origin);
}

node_t CirBuilder::dump_head(DumpFlavor fl, int depth, const std::string &word,
			     size_t count, TokenBase *origin)
{
	node_t a = list();
	append(a, integer(dump_frame_col(fl, depth), origin));
	append(a, str(word.c_str(), word.size() + 1, origin));
	if (fl == dfVarDump) {
		// var_dump states the element/member COUNT in the head line.
		need_output_extern("__madc_dump_vd_head", false,
				   { { {N_INT}, false }, { {N_CHAR}, true },
				     { {N_LONG, N_LONG}, false } });
		append(a, integer((int64_t)count, origin));
		return dump_call_stmt("__madc_dump_vd_head", a, origin);
	}
	need_output_extern("__madc_dump_pr_head", false,
			   { { {N_INT}, false }, { {N_CHAR}, true } });
	return dump_call_stmt("__madc_dump_pr_head", a, origin);
}

node_t CirBuilder::dump_key(DumpFlavor fl, int depth, const std::string &key,
			    TokenBase *origin)
{
	const char *sym = fl == dfVarDump ? "__madc_dump_vd_key"
					  : "__madc_dump_pr_key";
	need_output_extern(sym, false, { { {N_INT}, false }, { {N_CHAR}, true } });
	node_t a = list();
	append(a, integer(dump_entry_col(fl, depth), origin));
	append(a, str(key.c_str(), key.size() + 1, origin));
	return dump_call_stmt(sym, a, origin);
}

node_t CirBuilder::dump_key_idx(DumpFlavor fl, int depth, node_t idx,
				TokenBase *origin)
{
	const char *sym = fl == dfVarDump ? "__madc_dump_vd_key_idx"
					  : "__madc_dump_pr_key_idx";
	need_output_extern(sym, false, { { {N_INT}, false },
					 { {N_LONG, N_LONG}, false } });
	node_t a = list();
	append(a, integer(dump_entry_col(fl, depth), origin));
	append(a, idx);
	return dump_call_stmt(sym, a, origin);
}

node_t CirBuilder::dump_tail(DumpFlavor fl, int depth, bool nested,
			     TokenBase *origin)
{
	node_t a = list();
	append(a, integer(dump_frame_col(fl, depth), origin));
	if (fl == dfVarDump) {
		need_output_extern("__madc_dump_vd_tail", false,
				   { { {N_INT}, false } });
		return dump_call_stmt("__madc_dump_vd_tail", a, origin);
	}
	// print_r follows a NESTED block's ")" with a blank line; the outermost
	// one gets none.
	need_output_extern("__madc_dump_pr_tail", false,
			   { { {N_INT}, false }, { {N_INT}, false } });
	append(a, integer(nested ? 1 : 0, origin));
	return dump_call_stmt("__madc_dump_pr_tail", a, origin);
}

node_t CirBuilder::dump_pr_nl(TokenBase *origin)
{
	need_output_extern("__madc_dump_pr_nl", false, {});
	return dump_call_stmt("__madc_dump_pr_nl", list(), origin);
}

// ---------------------------------------------------------------------------
// One scalar value
// ---------------------------------------------------------------------------
// print_r emits the value ALONE — no type word, no newline: print_r(42) is
// exactly "42", print_r(true) is "1", and print_r(false) and print_r(null) are
// both the empty string. var_dump emits an indented, type-tagged, newline-
// terminated line: int(42), bool(true), a string-like value with its length.
bool CirBuilder::dump_scalar(DumpFlavor fl, const DumpAccess &acc, DataDef *dd,
			     int depth, std::vector<node_t> &out,
			     TokenBase *origin, std::string &why)
{
	if (!dd) {
		why = "unresolved type";
		return false;
	}

	enum { skCstr = 0, skBool, skChar, skReal, skInt } kind;

	// A character POINTER is text (PHP's string), not a number. It must be
	// tested BEFORE the integer arms: is_integer() is true for every pointer.
	if (dd->is_cstr())
		kind = skCstr;
	// Everything that is not a scalar, and everything whose scalar-looking
	// predicates would LIE, is refused by name of the type — never guessed at.
	// is_integer() is true for a pointer (DataDefPTR), for a pointer-to-data-
	// member and for a function pointer, and a SIMD vector's follows its
	// element: any of those reaching the integer arm would print an address or
	// a lane as a decimal. An enum must show its ENUMERATOR name, so it waits
	// for the slice that has the enumerator table rather than shipping as a
	// bare number a later slice would then change.
	else if (dd->is_pointer() || dd->is_reference() || dd->is_member_pointer()
		 || dd->is_object() || dd->is_function() || dd->is_simd()
		 || dd->is_complex() || dynamic_cast<DataDefENUM *>(dd) != NULL) {
		why = std::string("no dumper for type '") + dd->name + "' yet";
		return false;
	}
	else if (dd->rawtype() == DataType::dtBOOL)
		kind = skBool;
	// A char is one character of text. dtCHAR IS dtINT8, and uint8_t is
	// dtBYTE — the same aliasing C++ itself has, where `cout << (int8_t)65`
	// prints 'A'. Rendering the byte matches that, and matches what a PHP
	// developer sees from chr(65).
	else if (dd->rawtype() == DataType::dtINT8
		 || dd->rawtype() == DataType::dtUINT8)
		kind = skChar;
	// long double narrows to double: PHP has no wider type, and the
	// 14-significant-digit format cannot show the difference.
	else if (dd->is_real())
		kind = skReal;
	else if (dd->is_integer())
		kind = skInt;
	else {
		why = std::string("no dumper for type '") + dd->name + "' yet";
		return false;
	}

	static const char *pr_syms[] = { "__madc_dump_pr_cstr",
					 "__madc_dump_pr_bool",
					 "__madc_dump_pr_char",
					 "__madc_dump_pr_f64",
					 "__madc_dump_pr_i64" };
	static const char *vd_syms[] = { "__madc_dump_vd_cstr",
					 "__madc_dump_vd_bool",
					 "__madc_dump_vd_char",
					 "__madc_dump_vd_f64",
					 "__madc_dump_vd_i64" };
	const char *sym = (fl == dfVarDump ? vd_syms : pr_syms)[kind];

	std::vector<ExternParam> params;
	node_t a = list();
	if (fl == dfVarDump) {
		// (col, type-word, value[, is_unsigned])
		params.push_back({ {N_INT}, false });
		params.push_back({ {N_CHAR}, true });
		append(a, integer(dump_frame_col(fl, depth), origin));
		std::string word = dump_scalar_type_word(dd);
		append(a, str(word.c_str(), word.size() + 1, origin));
	}
	switch (kind) {
	case skCstr: params.push_back({ {N_CHAR}, true });          break;
	case skBool:
	case skChar: params.push_back({ {N_INT}, false });          break;
	case skReal: params.push_back({ {N_DOUBLE}, false });       break;
	case skInt:  params.push_back({ {N_LONG, N_LONG}, false }); break;
	}
	append(a, acc());
	if (kind == skInt) {
		params.push_back({ {N_INT}, false });
		append(a, integer(dd->is_unsigned() ? 1 : 0, origin));
	}
	need_output_extern(sym, false, params);
	out.push_back(dump_call_stmt(sym, a, origin));
	// print_r's newline ENDS AN ENTRY, so only a value inside a parent gets
	// one: print_r(42) at top level is exactly "42". Every var_dump primitive
	// terminates its own line.
	if (fl == dfPrintR && depth > 0)
		out.push_back(dump_pr_nl(origin));
	return true;
}

// ---------------------------------------------------------------------------
// A struct / class / union
// ---------------------------------------------------------------------------
// Members are walked in DECLARATION order, which for a flattened base chain is
// base-first — the same order PHP prints an inherited property in.
bool CirBuilder::dump_struct(DumpFlavor fl, const DumpAccess &acc,
			     DataDefSTRUCT *sdd, int depth, bool nested,
			     std::vector<node_t> &out, TokenBase *origin,
			     std::string &why)
{
	// A member name that appears TWICE is a base member shadowed by a derived
	// one; the emitted C struct renames the hidden one to `<name>__flatN`
	// (class_struct_members owns that), and nothing outside that function can
	// reconstruct which. Skip it rather than print the wrong storage —
	// including it needs one owner for the emitted field name, which is a
	// refactor of its own.
	std::map<std::string, int> namecount;
	for (size_t i = 0; i < sdd->members.size(); i++)
		if (!sdd->members[i].first.empty())
			namecount[sdd->members[i].first]++;
	DataDefCLASS *cdd = dynamic_cast<DataDefCLASS *>(sdd);

	std::vector<size_t> shown;
	for (size_t i = 0; i < sdd->members.size(); i++) {
		const std::string &mn = sdd->members[i].first;
		if (mn.empty())
			continue;               // unnamed bit-field padding
		if (namecount[mn] > 1)
			continue;               // shadowed — see above
		shown.push_back(i);
	}

	out.push_back(dump_head(fl, depth,
				fl == dfVarDump
				  ? dump_aggregate_type_word(sdd)
				  : sdd->name + " Object",
				shown.size(), origin));

	for (size_t si = 0; si < shown.size(); si++) {
		size_t i = shown[si];
		const std::string &mn = sdd->members[i].first;
		DataDef *mt = sdd->members[i].second;
		size_t count = i < sdd->member_counts.size()
			     ? sdd->member_counts[i] : 1;
		bool is_arr = count > 1
			   || (i < sdd->member_array_flags.size()
			       && sdd->member_array_flags[i]);
		// member_counts holds the TOTAL element count, so a multi-dim
		// member would be walked as one flat run and `m[i]` would index
		// past a ROW: for `int m[2][3]`, m[4] is out of bounds. PHP shows
		// a nested array here; both need the dim chain, so refuse until
		// the walk carries it.
		if (is_arr && i < sdd->member_dims.size()
		    && sdd->member_dims[i].size() > 1) {
			why = std::string("member '") + mn + "': no dumper for a"
			      " multidimensional array yet";
			return false;
		}

		// The key carries the ACCESS, and a private member also names its
		// DECLARING class — both captured from php-cli 8.3.6.
		uint32_t acc_flags = i < sdd->member_access.size()
				   ? sdd->member_access[i] : 0;
		std::string owner = sdd->name;
		int origin_base = i < sdd->member_origin.size()
				? sdd->member_origin[i] : -1;
		if (cdd && origin_base >= 0
		    && (size_t)origin_base < cdd->bases.size()
		    && cdd->bases[origin_base].base)
			owner = cdd->bases[origin_base].base->name;
		out.push_back(dump_key(fl, depth,
				       dump_member_key(fl, mn, acc_flags, owner),
				       origin));

		// Rebuild the member access per use: a c2mir node has one parent.
		std::string mname = mn;
		DumpAccess macc = [this, acc, mname, origin]() -> node_t {
			return node2(N_FIELD, acc(), id(mname.c_str(), origin),
				     origin);
		};
		if (!dump_any(fl, macc, mt, count, is_arr, depth + 1, true, out,
			      origin, why)) {
			why = std::string("member '") + mn + "': " + why;
			return false;
		}
	}
	out.push_back(dump_tail(fl, depth, nested, origin));
	return true;
}

// ---------------------------------------------------------------------------
// A fixed-extent array
// ---------------------------------------------------------------------------
// Elements are walked by a REAL loop (one expansion of the element dumper),
// never unrolled — `char buf[4096]` must not emit 4096 copies.
bool CirBuilder::dump_array(DumpFlavor fl, const DumpAccess &acc, DataDef *elem,
			    size_t count, int depth, bool nested,
			    std::vector<node_t> &out, TokenBase *origin,
			    std::string &why)
{
	if (!elem) {
		why = "array of unresolved element type";
		return false;
	}
	// A char array IS text to a PHP developer, not an array of small ints —
	// the same rule that will make std::string print as its contents. Bounded
	// by the extent: a C array need not be NUL-terminated, and %s would read
	// past it.
	if (elem->rawtype() == DataType::dtINT8
	    || elem->rawtype() == DataType::dtUINT8) {
		const char *sym = fl == dfVarDump ? "__madc_dump_vd_cstr_n"
						  : "__madc_dump_pr_cstr_n";
		std::vector<ExternParam> params;
		node_t a = list();
		if (fl == dfVarDump) {
			params.push_back({ {N_INT}, false });
			params.push_back({ {N_CHAR}, true });
			append(a, integer(dump_frame_col(fl, depth), origin));
			std::string word = dump_array_type_word(elem, count);
			append(a, str(word.c_str(), word.size() + 1, origin));
		}
		params.push_back({ {N_CHAR}, true });
		params.push_back({ {N_LONG, N_LONG}, false });
		append(a, acc());
		append(a, integer((int64_t)count, origin));
		need_output_extern(sym, false, params);
		out.push_back(dump_call_stmt(sym, a, origin));
		if (fl == dfPrintR && depth > 0)
			out.push_back(dump_pr_nl(origin));
		return true;
	}

	out.push_back(dump_head(fl, depth,
				fl == dfVarDump
				  ? dump_array_type_word(elem, count)
				  : std::string("Array"),
				count, origin));

	char idx[40];
	snprintf(idx, sizeof(idx), "__dmp_i_%d", m_strtmp_counter++);
	std::string idxname = idx;

	std::vector<node_t> body;
	body.push_back(dump_key_idx(fl, depth, id(idxname.c_str(), origin),
				    origin));
	DumpAccess eacc = [this, acc, idxname, origin]() -> node_t {
		return node2(N_IND, acc(), id(idxname.c_str(), origin), origin);
	};
	if (!dump_any(fl, eacc, elem, 1, false, depth + 1, true, body, origin, why))
		return false;

	// for (long i = 0; i < count; i += 1) { ... }
	node_t ispec = list();
	append_i64(ispec, origin);
	node_t init = simple(N_SPEC_DECL, origin);
	append(init, node1(N_SHARE, ispec));
	append(init, node2(N_DECL, id(idxname.c_str(), origin), list()));
	append(init, ignore());
	append(init, ignore());
	append(init, integer(0, origin));
	node_t cond = node2(N_LT, id(idxname.c_str(), origin),
			    integer((int64_t)count, origin), origin);
	node_t incr = node2(N_ADD_ASSIGN, id(idxname.c_str(), origin),
			    integer(1, origin), origin);
	node_t items = list();
	for (size_t i = 0; i < body.size(); i++)
		append(items, body[i]);
	node_t blk = node2(N_BLOCK, list(), items, origin);
	out.push_back(node5(N_FOR, list(), init, cond, incr, blk, origin));

	out.push_back(dump_tail(fl, depth, nested, origin));
	return true;
}

// The one dispatch: array framing, then aggregate framing, then scalar.
bool CirBuilder::dump_any(DumpFlavor fl, const DumpAccess &acc, DataDef *dd,
			  size_t count, bool is_array, int depth, bool nested,
			  std::vector<node_t> &out, TokenBase *origin,
			  std::string &why)
{
	if (!dd) {
		why = "unresolved type";
		return false;
	}
	if (is_array)
		return dump_array(fl, acc, dd, count ? count : 1, depth, nested,
				  out, origin, why);
	// A DataDefSTRUCT is the ONE aggregate-layout owner, and a POD struct is
	// only PROMOTED to DataDefCLASS when it earns class-hood — so the walk
	// keys on DataDefSTRUCT, never on DataDefCLASS. A pointer or reference
	// whose pointee is a struct is NOT this case: following it is the pointer
	// slice, and dump_scalar refuses it by name until then.
	if (!dd->is_pointer() && !dd->is_reference()) {
		if (DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(dd)) {
			// The intrinsic value carrier (madc::value) is a DDClass
			// with no madc members: its rendering is its own slice.
			if (dd->rawtype() == DataType::dtARRAY) {
				why = std::string("no dumper for type '")
				    + dd->name + "' yet";
				return false;
			}
			return dump_struct(fl, acc, sdd, depth, nested, out,
					   origin, why);
		}
	}
	return dump_scalar(fl, acc, dd, depth, out, origin, why);
}

// ---------------------------------------------------------------------------
// One argument, at top level
// ---------------------------------------------------------------------------
bool CirBuilder::dump_argument(DumpFlavor fl, TokenBase *arg,
			       std::vector<node_t> &out, TokenBase *origin,
			       std::string &why)
{
	if (!arg) {
		why = "missing argument";
		return false;
	}
	DataDef *dd = arg->datadef();
	if (!dd) {
		why = "argument has no resolved type";
		return false;
	}

	// A fixed-extent array VARIABLE carries its extent on the Variable, not in
	// its DataDef (a struct MEMBER carries it in member_counts instead) — the
	// same place translate_foreach_carray reads it.
	size_t count = 1;
	bool is_arr = false;
	if (TokenVar *tv = dynamic_cast<TokenVar *>(arg))
		if (tv->var.is_fixed_array() && !tv->var.is_vla()
		    && tv->var.total_elements() > 0) {
			// total_elements() flattens every dimension, and `a[i]` on
			// a multi-dim array yields a ROW, not an element — walking
			// it flat would index past the first row. PHP renders a
			// nested array; both want the dim chain, so refuse until
			// the walk carries it.
			if (tv->var.dims.size() > 1) {
				why = "no dumper for a multidimensional array yet";
				return false;
			}
			count = tv->var.total_elements();
			is_arr = true;
		}

	// The walk rebuilds the access once PER MEMBER, so an aggregate argument
	// must be re-evaluable without side effects. A named variable or a member
	// selection is; a struct-returning CALL is not (it would run once per
	// member). ttVariable / ttMember are the discriminators — every call token
	// also derives from TokenVar, so a dynamic_cast alone would admit one.
	bool simple_lvalue = arg->type() == TokenType::ttVariable
			  || arg->type() == TokenType::ttMember;
	if ((is_arr || dynamic_cast<DataDefSTRUCT *>(dd) != NULL) && !simple_lvalue) {
		why = "an aggregate argument must be a variable or member (a"
		      " temporary would be re-evaluated once per field)";
		return false;
	}

	DumpAccess acc = [this, arg]() -> node_t { return translate_expr(arg); };
	return dump_any(fl, acc, dd, count, is_arr, 0, false, out, origin, why);
}

// ---------------------------------------------------------------------------
// The call intercept
// ---------------------------------------------------------------------------
// Returns NULL when the callee is not a dump intrinsic, so translate_expr's
// ordinary call path continues untouched.
node_t CirBuilder::lower_dump_call(TokenCallFunc *tcf, FuncDef *fd,
				   TokenBase *origin)
{
	if (!tcf)
		return NULL;
	DumpFlavor fl = dump_flavor(fd);
	if (fl == dfNone)
		// The RESOLVED callee may be absent: a declared-only template's
		// placeholder FuncDef carries no parameters, so overload ranking
		// can filter it out of a 1-argument call. The intrinsic tag lives
		// on the DECLARATION the call token is bound to, which is that
		// placeholder either way.
		fl = dump_flavor(dynamic_cast<FuncDef *>(tcf->var.type));
	if (fl == dfNone)
		return NULL;

	// print_r takes ONE value (PHP's second $return argument is a separate
	// slice — it returns the text as a madc::value). var_dump is variadic and
	// dumps each argument in order, exactly as PHP does.
	if (fl == dfPrintR && tcf->parameters.size() != 1)
		return error_node("php::print_r takes one argument", origin);
	if (tcf->parameters.empty())
		return error_node((std::string(dump_flavor_name(fl))
				   + " needs at least one argument").c_str(),
				  origin);

	std::vector<node_t> stmts;
	std::string why;
	for (size_t i = 0; i < tcf->parameters.size(); i++)
		if (!dump_argument(fl, tcf->parameters[i], stmts, origin, why))
			return error_node((std::string(dump_flavor_name(fl))
					   + ": " + why).c_str(), origin);

	// A statement expression keeps a call in expression position one node.
	node_t items = list();
	for (size_t i = 0; i < stmts.size(); i++)
		append(items, stmts[i]);
	return node1(N_STMTEXPR, node2(N_BLOCK, list(), items, origin), origin);
}
