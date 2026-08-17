/* cir_dump.cpp — the GENERATED side of php::print_r / php::var_dump.
 *
 * These are declared in <ns_php> and defined NOWHERE: the compiler is their
 * implementation. A call site knows the concrete argument type, so the dumper
 * for that type is generated here (approach A) and the runtime carries only
 * OUTPUT PRIMITIVES (src/rt/rt_dump.c). No runtime type-descriptor table
 * exists, so a program that never dumps pays nothing.
 *
 * PHP is the oracle: php::print_r must render a madc value the way a PHP
 * developer expects PHP to render it. The contract, with php-cli 8.3.6 output
 * captured verbatim, is docs/plans/2026-08-17-php-print-r-var-dump-plan.md.
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
enum DumpFlavor { dfNone, dfPrintR };

static DumpFlavor dump_flavor(FuncDef *fd)
{
	if (!fd)
		return dfNone;
	if (fd->inline_builtin_kind == "php_print_r")
		return dfPrintR;
	return dfNone;
}

// ---------------------------------------------------------------------------
// Output-primitive call builders
// ---------------------------------------------------------------------------
// One place per primitive, so the extern proto and the call are never spelled
// apart. `col` and every key are compile-time constants: the walk is expanded
// per nesting level, so PHP's indentation needs no runtime state.

node_t CirBuilder::dump_pr_head(int col, const std::string &word,
				TokenBase *origin)
{
	need_output_extern("__madc_dump_pr_head", false,
			   { { {N_INT}, false }, { {N_CHAR}, true } });
	node_t a = list();
	append(a, integer(col, origin));
	append(a, str(word.c_str(), word.size() + 1, origin));
	return node2(N_EXPR, list(),
		     node2(N_CALL, id("__madc_dump_pr_head", origin), a, origin),
		     origin);
}

node_t CirBuilder::dump_pr_key(int col, const std::string &key, TokenBase *origin)
{
	need_output_extern("__madc_dump_pr_key", false,
			   { { {N_INT}, false }, { {N_CHAR}, true } });
	node_t a = list();
	append(a, integer(col, origin));
	append(a, str(key.c_str(), key.size() + 1, origin));
	return node2(N_EXPR, list(),
		     node2(N_CALL, id("__madc_dump_pr_key", origin), a, origin),
		     origin);
}

node_t CirBuilder::dump_pr_key_idx(int col, node_t idx, TokenBase *origin)
{
	need_output_extern("__madc_dump_pr_key_idx", false,
			   { { {N_INT}, false }, { {N_LONG, N_LONG}, false } });
	node_t a = list();
	append(a, integer(col, origin));
	append(a, idx);
	return node2(N_EXPR, list(),
		     node2(N_CALL, id("__madc_dump_pr_key_idx", origin), a, origin),
		     origin);
}

node_t CirBuilder::dump_pr_nl(TokenBase *origin)
{
	need_output_extern("__madc_dump_pr_nl", false, {});
	return node2(N_EXPR, list(),
		     node2(N_CALL, id("__madc_dump_pr_nl", origin), list(), origin),
		     origin);
}

node_t CirBuilder::dump_pr_tail(int col, bool blank, TokenBase *origin)
{
	need_output_extern("__madc_dump_pr_tail", false,
			   { { {N_INT}, false }, { {N_INT}, false } });
	node_t a = list();
	append(a, integer(col, origin));
	append(a, integer(blank ? 1 : 0, origin));
	return node2(N_EXPR, list(),
		     node2(N_CALL, id("__madc_dump_pr_tail", origin), a, origin),
		     origin);
}

// ---------------------------------------------------------------------------
// print_r of one value
// ---------------------------------------------------------------------------
// PHP's print_r on a SCALAR emits the value alone — no type word, no newline,
// no trailing space: print_r(42) is exactly "42", print_r(true) is "1", and
// print_r(false) and print_r(null) are both the empty string. The newline and
// the indentation of an entry belong to the AGGREGATE renderer, which is why
// nothing here emits either.
bool CirBuilder::dump_pr_scalar(const DumpAccess &acc, DataDef *dd,
				std::vector<node_t> &out, TokenBase *origin,
				std::string &why)
{
	if (!dd) {
		why = "unresolved type";
		return false;
	}

	const char *sym = NULL;
	node_t second = NULL;

	// A character POINTER is text (PHP's string), not a number. It must be
	// tested BEFORE the integer arms: is_integer() is true for every pointer.
	if (dd->is_cstr()) {
		sym = "__madc_dump_pr_cstr";
		need_output_extern(sym, false, { { {N_CHAR}, true } });
	}
	// Everything that is not a scalar, and everything whose scalar-looking
	// predicates would LIE, is refused by name of the type — never guessed at.
	// is_integer() is true for a pointer (DataDefPTR), for a pointer-to-data-
	// member and for a function pointer, and a SIMD vector's follows its
	// element: any of those reaching the integer arm would print an address or
	// a lane as a decimal. An enum's print_r must show the ENUMERATOR name, so
	// it waits for the slice that has the enumerator table rather than shipping
	// as a bare number a later slice would then change.
	else if (dd->is_pointer() || dd->is_reference() || dd->is_member_pointer()
		 || dd->is_object() || dd->is_function() || dd->is_simd()
		 || dd->is_complex() || dynamic_cast<DataDefENUM *>(dd) != NULL) {
		why = std::string("no dumper for type '") + dd->name + "' yet";
		return false;
	}
	else if (dd->rawtype() == DataType::dtBOOL) {
		sym = "__madc_dump_pr_bool";
		need_output_extern(sym, false, { { {N_INT}, false } });
	}
	// A char is one character of text. dtCHAR IS dtINT8, and uint8_t is
	// dtBYTE — the same aliasing C++ itself has, where `cout << (int8_t)65`
	// prints 'A'. Rendering the byte matches that, and matches what a PHP
	// developer sees from chr(65).
	else if (dd->rawtype() == DataType::dtINT8
		 || dd->rawtype() == DataType::dtUINT8) {
		sym = "__madc_dump_pr_char";
		need_output_extern(sym, false, { { {N_INT}, false } });
	}
	else if (dd->is_real()) {
		// long double narrows to double: PHP has no wider type, and the
		// 14-significant-digit format cannot show the difference.
		sym = "__madc_dump_pr_f64";
		need_output_extern(sym, false, { { {N_DOUBLE}, false } });
	}
	else if (dd->is_integer()) {
		sym = "__madc_dump_pr_i64";
		need_output_extern(sym, false, { { {N_LONG, N_LONG}, false },
						 { {N_INT}, false } });
		second = integer(dd->is_unsigned() ? 1 : 0, origin);
	}
	else {
		why = std::string("no dumper for type '") + dd->name + "' yet";
		return false;
	}

	node_t a = list();
	append(a, acc());
	if (second)
		append(a, second);
	out.push_back(node2(N_EXPR, list(),
			    node2(N_CALL, id(sym, origin), a, origin), origin));
	return true;
}

// A struct / class / union: PHP's object framing. Members are walked in
// DECLARATION order, which for a flattened base chain is base-first — the same
// order PHP prints an inherited property in.
bool CirBuilder::dump_pr_struct(const DumpAccess &acc, DataDefSTRUCT *sdd,
				int depth, bool nested, std::vector<node_t> &out,
				TokenBase *origin, std::string &why)
{
	int col = 8 * depth, kcol = col + 4;
	out.push_back(dump_pr_head(col, sdd->name + " Object", origin));

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

	for (size_t i = 0; i < sdd->members.size(); i++) {
		const std::string &mn = sdd->members[i].first;
		if (mn.empty())
			continue;               // unnamed bit-field padding
		if (namecount[mn] > 1)
			continue;               // shadowed — see above
		DataDef *mt = sdd->members[i].second;
		size_t count = i < sdd->member_counts.size()
			     ? sdd->member_counts[i] : 1;
		bool is_arr = count > 1
			   || (i < sdd->member_array_flags.size()
			       && sdd->member_array_flags[i]);

		// PHP's key spelling carries the ACCESS: "[prot:protected]" and
		// "[priv:Foo:private]", where Foo is the DECLARING class (captured
		// from php-cli 8.3.6, tmp/or_pr2.php).
		std::string key = mn;
		uint32_t acc_flags = i < sdd->member_access.size()
				   ? sdd->member_access[i] : 0;
		if (acc_flags & vfPRIVATE) {
			std::string owner = sdd->name;
			int origin_base = i < sdd->member_origin.size()
					? sdd->member_origin[i] : -1;
			if (cdd && origin_base >= 0
			    && (size_t)origin_base < cdd->bases.size()
			    && cdd->bases[origin_base].base)
				owner = cdd->bases[origin_base].base->name;
			key += ":" + owner + ":private";
		} else if (acc_flags & vfPROTECTED) {
			key += ":protected";
		}
		out.push_back(dump_pr_key(kcol, key, origin));

		// Rebuild the member access per use: a c2mir node has one parent.
		std::string mname = mn;
		DumpAccess macc = [this, acc, mname, origin]() -> node_t {
			return node2(N_FIELD, acc(), id(mname.c_str(), origin),
				     origin);
		};
		if (!dump_pr_any(macc, mt, count, is_arr, depth + 1, true, out,
				 origin, why)) {
			why = std::string("member '") + mn + "': " + why;
			return false;
		}
	}
	out.push_back(dump_pr_tail(col, nested, origin));
	return true;
}

// A fixed-extent array: PHP's array framing with positional keys. The elements
// are walked by a REAL loop (one expansion of the element dumper), never
// unrolled — `char buf[4096]` must not emit 4096 copies.
bool CirBuilder::dump_pr_array(const DumpAccess &acc, DataDef *elem,
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
		need_output_extern("__madc_dump_pr_cstr_n", false,
				   { { {N_CHAR}, true },
				     { {N_LONG, N_LONG}, false } });
		node_t a = list();
		append(a, acc());
		append(a, integer((int64_t)count, origin));
		out.push_back(node2(N_EXPR, list(),
				    node2(N_CALL, id("__madc_dump_pr_cstr_n", origin),
					  a, origin), origin));
		if (nested)
			out.push_back(dump_pr_nl(origin));
		return true;
	}

	int col = 8 * depth, kcol = col + 4;
	out.push_back(dump_pr_head(col, "Array", origin));

	char idx[40];
	snprintf(idx, sizeof(idx), "__dmp_i_%d", m_strtmp_counter++);
	std::string idxname = idx;

	std::vector<node_t> body;
	body.push_back(dump_pr_key_idx(kcol, id(idxname.c_str(), origin), origin));
	DumpAccess eacc = [this, acc, idxname, origin]() -> node_t {
		return node2(N_IND, acc(), id(idxname.c_str(), origin), origin);
	};
	if (!dump_pr_any(eacc, elem, 1, false, depth + 1, true, body, origin, why))
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

	out.push_back(dump_pr_tail(col, nested, origin));
	return true;
}

// The one dispatch: array framing, then aggregate framing, then scalar.
bool CirBuilder::dump_pr_any(const DumpAccess &acc, DataDef *dd, size_t count,
			     bool is_array, int depth, bool nested,
			     std::vector<node_t> &out, TokenBase *origin,
			     std::string &why)
{
	if (!dd) {
		why = "unresolved type";
		return false;
	}
	if (is_array)
		return dump_pr_array(acc, dd, count ? count : 1, depth, nested,
				     out, origin, why);
	// A DataDefSTRUCT is the ONE aggregate-layout owner, and a POD struct is
	// only PROMOTED to DataDefCLASS when it earns class-hood — so the walk
	// keys on DataDefSTRUCT, never on DataDefCLASS. A pointer or reference
	// whose pointee is a struct is NOT this case: following it is the pointer
	// slice, and dump_pr_scalar refuses it by name until then.
	if (!dd->is_pointer() && !dd->is_reference()) {
		if (DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(dd)) {
			// The intrinsic value carrier (madc::value) is a DDClass
			// with no madc members: its rendering is its own slice.
			if (dd->rawtype() == DataType::dtARRAY) {
				why = std::string("no dumper for type '")
				    + dd->name + "' yet";
				return false;
			}
			return dump_pr_struct(acc, sdd, depth, nested, out,
					      origin, why);
		}
	}
	if (!dump_pr_scalar(acc, dd, out, origin, why))
		return false;
	// The newline ENDS AN ENTRY, so only a value inside a parent gets one:
	// PHP's print_r(42) at top level is exactly "42".
	if (nested)
		out.push_back(dump_pr_nl(origin));
	return true;
}

// The php::print_r(v) entry point: wrap the walk in a statement expression so a
// call in expression position stays one node.
node_t CirBuilder::dump_print_r_value(TokenBase *arg, TokenBase *origin)
{
	if (!arg)
		return error_node("php::print_r: missing argument", origin);
	DataDef *dd = arg->datadef();
	if (!dd)
		return error_node("php::print_r: argument has no resolved type",
				  origin);

	// A fixed-extent array VARIABLE carries its extent on the Variable, not in
	// its DataDef (the member case reads member_counts instead) — the same
	// place translate_foreach_carray reads it.
	size_t count = 1;
	bool is_arr = false;
	if (TokenVar *tv = dynamic_cast<TokenVar *>(arg))
		if (tv->var.is_fixed_array() && !tv->var.is_vla()
		    && tv->var.total_elements() > 0) {
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
	if ((is_arr || dynamic_cast<DataDefSTRUCT *>(dd) != NULL) && !simple_lvalue)
		return error_node("php::print_r: an aggregate argument must be a"
				  " variable or member (a temporary would be"
				  " re-evaluated once per field)", origin);

	std::vector<node_t> stmts;
	std::string why;
	DumpAccess acc = [this, arg]() -> node_t { return translate_expr(arg); };
	if (!dump_pr_any(acc, dd, count, is_arr, 0, false, stmts, origin, why))
		return error_node((std::string("php::print_r: ") + why).c_str(),
				  origin);
	node_t items = list();
	for (size_t i = 0; i < stmts.size(); i++)
		append(items, stmts[i]);
	return node1(N_STMTEXPR, node2(N_BLOCK, list(), items, origin), origin);
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
	DumpFlavor flavor = dump_flavor(fd);
	if (flavor == dfNone)
		// The RESOLVED callee may be absent: a declared-only template's
		// placeholder FuncDef carries no parameters, so overload ranking
		// can filter it out of a 1-argument call. The intrinsic tag lives
		// on the DECLARATION the call token is bound to, which is that
		// placeholder either way.
		flavor = dump_flavor(dynamic_cast<FuncDef *>(tcf->var.type));
	switch (flavor) {
	case dfPrintR:
		// PHP's print_r($v) — the second ($return) argument is a separate
		// slice (it returns the text as a madc::value).
		if (tcf->parameters.size() != 1)
			return error_node("php::print_r takes one argument",
					  origin);
		return dump_print_r_value(tcf->parameters[0], origin);
	case dfNone:
		break;
	}
	return NULL;
}
