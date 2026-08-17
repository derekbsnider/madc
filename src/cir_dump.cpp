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

// The type word for ANY type: a class goes through the class rule (alias, then
// canonical spelling), everything else through the scalar table. One owner, so
// a nested element word cannot disagree with a top-level one.
std::string CirBuilder::dump_type_word(DataDef *dd)
{
	if (dd && !dd->is_pointer() && !dd->is_reference())
		if (DataDefCLASS *c = is_class_object(dd)
				    ? dynamic_cast<DataDefCLASS *>(dd->unqualified())
				    : NULL)
			return dump_class_type_word(c);
	return dump_scalar_type_word(dd);
}

// A SEQUENCE's type word: the template's own name with its ELEMENT type, which
// is the one argument a reader wants. The canonical spelling carries every
// defaulted argument too —
// std::vector<std::__cxx11::basic_string<char,std::char_traits<char>,std::allocator<char>>,std::allocator<...same again...>>
// — and the allocator and the char traits are implementation detail, not
// information. The element type is not guessed: it is operator[]'s return type,
// the same one the walk dumps. (A std::array's extent drops out of the spelling
// and is printed as the COUNT instead.) A word with no '<' — a resolved alias
// like std::string, or a plain class — is already what the source wrote.
std::string CirBuilder::dump_sequence_type_word(DataDefCLASS *cls, DataDef *elem)
{
	std::string word = dump_class_type_word(cls);
	size_t lt = word.find('<');
	if (lt == std::string::npos || !elem)
		return word;
	return word.substr(0, lt) + "<" + dump_type_word(elem) + ">";
}

// An array's type word: the element's, with the extent.
std::string CirBuilder::dump_array_type_word(DataDef *elem, size_t count)
{
	char n[32];
	snprintf(n, sizeof(n), "[%llu]", (unsigned long long)count);
	return dump_type_word(elem) + n;
}

// The SOURCE's OWN NAME for this type, if anything names it. The datatype maps
// bind the spelling a `typedef` / `using` introduced to the DataDef it names
// (TokenDataType is { std::string str; DataDef &definition; }), so <string>'s
// `typedef basic_string<char> string;` puts std["string"] -> the basic_string
// instantiation. Inverting that table asks "what did the source call this type" —
// it is a type-IDENTITY lookup, NOT a name match on `basic_string`, which is the
// distinction this whole arc rests on.
//
// Shortest qualified spelling wins, then alphabetical, so the answer is
// deterministic when several aliases name one type. Empty when none does.
std::string CirBuilder::type_alias_spelling(DataDef *dd)
{
	if (!m_prog || !dd)
		return std::string();
	std::string best;
	m_prog->namespace_datatype_map.for_each(
		[&](const char *ns, datatype_map_t &m) -> bool {
			for (datatype_map_iter it = m.begin(); it != m.end(); ++it) {
				if (!it->second)
					continue;
				// Two keys are the type's OWN registration rather
				// than a name the source gave it: the template-id
				// spelling (holds '<') and the mangled tag madc
				// registers the instantiation under, which IS the
				// DataDef's own name. Without the second filter the
				// "alias" found for std::vector<int> is
				// std::vector_int32_t_std__allocator_int32_t_ —
				// worse than the spelling it replaced.
				if (it->first.find('<') != std::string::npos)
					continue;
				if (it->first == dd->name)
					continue;
				if (&it->second->definition != dd)
					continue;
				std::string cand = (ns && *ns)
						 ? std::string(ns) + "::" + it->first
						 : it->first;
				if (best.empty() || cand.size() < best.size()
				    || (cand.size() == best.size() && cand < best))
					best = cand;
			}
			return false;
		});
	return best;
}

// A class's type word for var_dump.
//
// A union keeps its keyword: with every member reading the same storage, "this
// is a union" is the most important thing the line can say, and an alias must
// not hide it.
//
// A TEMPLATE INSTANTIATION is the case that needs help. Its madc `name` is a
// mangled tag (vector_int32_t_std__allocator_int32_t_) and its canonical C++
// spelling is complete but unreadable —
// std::__cxx11::basic_string<char,std::char_traits<char>,std::allocator<char>> —
// so when the source itself has a NAME for that type, use it: `std::string`.
// Owner, 2026-08-17: "I really don't think anyone wants to see
// std::__cxx11::basic_string<...>".
//
// A plain aggregate keeps `struct X`. It is already the source's own spelling,
// and preferring an alias there would rename `struct Point` to whatever
// `typedef struct Point Point_t;` happened to add.
std::string CirBuilder::dump_class_type_word(DataDefCLASS *cls)
{
	if (cls->union_layout)
		return "union " + cls->name;
	const std::string &canon = cls->canonical_cpp_spelling();
	if (canon.find('<') != std::string::npos) {
		std::string alias = type_alias_spelling(cls);
		if (!alias.empty())
			return alias;
	}
	if (!canon.empty())
		return canon;
	return "struct " + cls->name;
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

// Declare a dump primitive. The leading `void *sink` is prepended HERE, once,
// so it cannot drift from the argument dump_call_stmt prepends below.
void CirBuilder::need_dump_extern(const char *sym,
				  std::vector<ExternParam> params)
{
	params.insert(params.begin(), ExternParam{ { N_VOID }, true });
	need_output_extern(sym, false, params);
}

node_t CirBuilder::dump_call_stmt(const char *sym, node_t args, TokenBase *origin)
{
	// Every primitive's first argument is the sink: the generated `void *`
	// local when this dump may capture, a null pointer when it prints. Built
	// by prepending to the caller's list, so no primitive builder has to
	// remember — the same reason need_dump_extern owns the matching shape.
	node_t all = list();
	node_t sink = m_dump_sink_var.empty()
		      ? node2(N_CAST, void_ptr_type(), integer(0, origin), origin)
		      : id(m_dump_sink_var.c_str(), origin);
	append(all, sink);
	// Move the caller's arguments in after the sink. c2mir op-lists are
	// intrusive, so a node cannot sit in two lists — splice (which REMOVES as
	// it goes) is the owner for this, not a copy loop.
	c2mir_op_splice_after(c2m, all, sink, args);
	return node2(N_EXPR, list(), node2(N_CALL, id(sym, origin), all, origin),
		     origin);
}

node_t CirBuilder::dump_head(DumpFlavor fl, int depth, const std::string &word,
			     size_t count, TokenBase *origin)
{
	return dump_head_node(fl, depth, word, integer((int64_t)count, origin),
			      origin);
}

// A container's element count is a RUNTIME value (`c.size()`), so the count
// arrives as a node. A compile-time count is the same call with a literal.
node_t CirBuilder::dump_head_node(DumpFlavor fl, int depth,
				  const std::string &word, node_t count,
				  TokenBase *origin)
{
	node_t a = list();
	append(a, integer(dump_frame_col(fl, depth), origin));
	append(a, str(word.c_str(), word.size() + 1, origin));
	if (fl == dfVarDump) {
		// var_dump states the element/member COUNT in the head line.
		need_dump_extern("__madc_dump_vd_head",
				   { { {N_INT}, false }, { {N_CHAR}, true },
				     { {N_LONG, N_LONG}, false } });
		append(a, count);
		return dump_call_stmt("__madc_dump_vd_head", a, origin);
	}
	need_dump_extern("__madc_dump_pr_head",
			   { { {N_INT}, false }, { {N_CHAR}, true } });
	return dump_call_stmt("__madc_dump_pr_head", a, origin);
}

node_t CirBuilder::dump_key(DumpFlavor fl, int depth, const std::string &key,
			    TokenBase *origin)
{
	const char *sym = fl == dfVarDump ? "__madc_dump_vd_key"
					  : "__madc_dump_pr_key";
	need_dump_extern(sym, { { {N_INT}, false }, { {N_CHAR}, true } });
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
	need_dump_extern(sym, { { {N_INT}, false },
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
		need_dump_extern("__madc_dump_vd_tail",
				   { { {N_INT}, false } });
		return dump_call_stmt("__madc_dump_vd_tail", a, origin);
	}
	// print_r follows a NESTED block's ")" with a blank line; the outermost
	// one gets none.
	need_dump_extern("__madc_dump_pr_tail",
			   { { {N_INT}, false }, { {N_INT}, false } });
	append(a, integer(nested ? 1 : 0, origin));
	return dump_call_stmt("__madc_dump_pr_tail", a, origin);
}

// var_dump's text form: `<type>(<len>) "` ... `"` with the characters between.
// The length is the container's own size(), a runtime value.
node_t CirBuilder::dump_vd_text_open(int depth, const std::string &word,
				     node_t len, TokenBase *origin)
{
	need_dump_extern("__madc_dump_vd_text_open",
			   { { {N_INT}, false }, { {N_CHAR}, true },
			     { {N_LONG, N_LONG}, false } });
	node_t a = list();
	append(a, integer(dump_frame_col(dfVarDump, depth), origin));
	append(a, str(word.c_str(), word.size() + 1, origin));
	append(a, len);
	return dump_call_stmt("__madc_dump_vd_text_open", a, origin);
}

node_t CirBuilder::dump_vd_text_close(TokenBase *origin)
{
	need_dump_extern("__madc_dump_vd_text_close", {});
	return dump_call_stmt("__madc_dump_vd_text_close", list(), origin);
}

node_t CirBuilder::dump_pr_nl(TokenBase *origin)
{
	need_dump_extern("__madc_dump_pr_nl", {});
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
		std::string word = dump_type_word(dd);
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
	need_dump_extern(sym, params);
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
		need_dump_extern(sym, params);
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

// ---------------------------------------------------------------------------
// A POSITIONAL container (std::string, std::vector, std::array, ...)
// ---------------------------------------------------------------------------
// The owner's rule decides the rendering: a std::vector<int> prints the way PHP
// prints an array of ints, and a std::string prints as its text. Both fall out
// of ONE structural test — class_index_iteration_protocol, the type-checked
// size()+operator[] predicate the range-for shares — plus the element type:
// a sequence whose element is a CHARACTER type is text, anything else is an
// array. No method is matched by name here (no c_str, no length): a name match
// is exactly the weakness that made `for (int v : map)` SIGSEGV.
bool CirBuilder::dump_sequence(DumpFlavor fl, const DumpAccess &acc,
			       DataDefCLASS *cls, int depth, bool nested,
			       std::vector<node_t> &out, TokenBase *origin,
			       std::string &why)
{
	Variable *szmv = NULL, *opmv = NULL;
	if (!class_index_iteration_protocol(cls, szmv, opmv))
		return false;
	FuncDef *opfd = opmv ? dynamic_cast<FuncDef *>(opmv->type) : NULL;
	if (!opfd)
		return false;
	DataDef *elem = &opfd->return_value_type();
	if (!elem)
		return false;

	// The count ONCE, into a local: the head line and the loop bound must
	// agree, and size() is a real call.
	char nm[40], ix[40];
	snprintf(nm, sizeof(nm), "__dmp_n_%d", m_strtmp_counter++);
	snprintf(ix, sizeof(ix), "__dmp_i_%d", m_strtmp_counter++);
	std::string nname = nm, idxname = ix;

	node_t szcall = class_nullary_call(cls, "size",
					   node1(N_ADDR, acc(), origin), origin);
	if (!szcall) {
		why = std::string("container '") + cls->name
		    + "' has no callable size()";
		return false;
	}
	node_t nspec = list();
	append_i64(nspec, origin);
	node_t ndecl = simple(N_SPEC_DECL, origin);
	append(ndecl, node1(N_SHARE, nspec));
	append(ndecl, node2(N_DECL, id(nname.c_str(), origin), list()));
	append(ndecl, ignore());
	append(ndecl, ignore());
	append(ndecl, szcall);
	out.push_back(ndecl);

	// One element access, rebuilt per use: the operator[] call through the ONE
	// call owner, deref'd when it returns T& (which is a T* at this level).
	DumpAccess eacc = [this, acc, idxname, cls, opfd, origin]() -> node_t {
		node_t call = class_subscript_addr_on(cls,
						      node1(N_ADDR, acc(), origin),
						      NULL, origin,
						      id(idxname.c_str(), origin));
		if (opfd->returns_reference())
			return node1(N_DEREF, call, origin);
		return call;
	};

	bool is_text = elem->rawtype() == DataType::dtINT8
		    || elem->rawtype() == DataType::dtUINT8;
	std::string word = fl == dfVarDump
			 ? dump_sequence_type_word(cls, elem)
			 : (is_text ? std::string() : std::string("Array"));

	std::vector<node_t> body;
	if (is_text) {
		// Text: the characters, in order, with no per-element framing.
		// var_dump still states the type and the length first.
		if (fl == dfVarDump)
			out.push_back(dump_vd_text_open(depth, word,
							id(nname.c_str(), origin),
							origin));
		need_dump_extern("__madc_dump_pr_char",
				   { { {N_INT}, false } });
		node_t ca = list();
		append(ca, eacc());
		body.push_back(dump_call_stmt("__madc_dump_pr_char", ca, origin));
	} else {
		out.push_back(dump_head_node(fl, depth, word,
					     id(nname.c_str(), origin), origin));
		body.push_back(dump_key_idx(fl, depth,
					    id(idxname.c_str(), origin), origin));
		if (!dump_any(fl, eacc, elem, 1, false, depth + 1, true, body,
			      origin, why))
			return false;
	}

	// for (long i = 0; i < n; i += 1) { ... }
	node_t ispec = list();
	append_i64(ispec, origin);
	node_t init = simple(N_SPEC_DECL, origin);
	append(init, node1(N_SHARE, ispec));
	append(init, node2(N_DECL, id(idxname.c_str(), origin), list()));
	append(init, ignore());
	append(init, ignore());
	append(init, integer(0, origin));
	node_t cond = node2(N_LT, id(idxname.c_str(), origin),
			    id(nname.c_str(), origin), origin);
	node_t incr = node2(N_ADD_ASSIGN, id(idxname.c_str(), origin),
			    integer(1, origin), origin);
	node_t items = list();
	for (size_t i = 0; i < body.size(); i++)
		append(items, body[i]);
	out.push_back(node5(N_FOR, list(), init, cond, incr,
			    node2(N_BLOCK, list(), items, origin), origin));

	if (is_text) {
		if (fl == dfVarDump)
			out.push_back(dump_vd_text_close(origin));
		else if (depth > 0)
			out.push_back(dump_pr_nl(origin));
	} else {
		out.push_back(dump_tail(fl, depth, nested, origin));
	}
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
		// A class that is a POSITIONAL SEQUENCE renders as one (an array,
		// or text when its element is a character) rather than as its
		// members: a std::vector<int>'s private pointers are not what a PHP
		// developer asked to see. Tried BEFORE the member walk, and only a
		// structural test decides it.
		// The intrinsic value carrier (madc::value == ddARRAY) is a class
		// WITH registered methods (its c_str is madarray_cstr), so it would
		// pass a structural sequence test and then be called through the
		// wrong runtime. Its rendering is its own slice; exclude it here.
		// is_class_object IS the shared predicate (as_class_instance behind
		// it, which already excludes ddARRAY by rawtype); the cast only
		// recovers the pointer it validated, so no condition is duplicated.
		DataDefCLASS *ccls = is_class_object(dd) && !is_array_object(dd)
				   ? dynamic_cast<DataDefCLASS *>(dd->unqualified())
				   : NULL;
		if (ccls) {
			// Built into a LOCAL first: a sequence that turns out not
			// to be dumpable must leave `out` untouched.
			std::vector<node_t> seq;
			std::string seq_why;
			if (dump_sequence(fl, acc, ccls, depth, nested, seq,
					  origin, seq_why)) {
				out.insert(out.end(), seq.begin(), seq.end());
				return true;
			}
			// A class that IS positional but whose element has no
			// dumper yet (a row pointer from a Matrix::operator[], say)
			// falls back to the member walk below rather than failing
			// the call: those members are real, and an enhancement must
			// not turn a working dump into an error.
		}
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
// print_r's $return flag: the capture sink and the returned madc::value
// ---------------------------------------------------------------------------
// Declare, as the first statements of the dump's block:
//
//     int   <ret>  = <the flag expression>;          // evaluated EXACTLY once
//     void *<sink> = <ret> ? __madc_dump_sink_open() : (void *)0;
//
// The flag rides a temp because it is a user expression: `print_r(x, f())` must
// call f() once, and both the sink decision and the result assignment below
// need to test it. A NULL sink is what makes the walk print instead of capture,
// so one variable expresses both PHP modes with no duplicated walk.
void CirBuilder::dump_sink_open(std::vector<node_t> &stmts, TokenBase *ret_arg,
				std::string &ret_var, std::string &sink_var,
				TokenBase *origin)
{
	char rname[40], sname[40];
	int n = m_strtmp_counter++;
	snprintf(rname, sizeof rname, "__madc_dumpret_%d", n);
	snprintf(sname, sizeof sname, "__madc_dumpsink_%d", n);
	ret_var = rname;
	sink_var = sname;

	// int <ret> = <flag>;
	node_t rspec = list();
	append(rspec, simple(N_INT, origin));
	node_t rdecl = simple(N_SPEC_DECL, origin);
	append(rdecl, node1(N_SHARE, rspec));
	append(rdecl, node2(N_DECL, id(rname, origin), list()));
	append(rdecl, ignore());
	append(rdecl, ignore());
	append(rdecl, translate_expr(ret_arg));
	stmts.push_back(rdecl);

	// void *<sink> = <ret> ? __madc_dump_sink_open() : (void *)0;
	need_output_extern("__madc_dump_sink_open", true, {});
	node_t opened = node2(N_CALL, id("__madc_dump_sink_open", origin),
			      list(), origin);
	node_t init = node3(N_COND, id(rname, origin), opened,
			    node2(N_CAST, void_ptr_type(), integer(0, origin),
				  origin), origin);
	node_t sspec = list();
	append(sspec, simple(N_VOID, origin));
	node_t sdeclr = list();
	append(sdeclr, pointer());
	node_t sdecl = simple(N_SPEC_DECL, origin);
	append(sdecl, node1(N_SHARE, sspec));
	append(sdecl, node2(N_DECL, id(sname, origin), sdeclr));
	append(sdecl, ignore());
	append(sdecl, ignore());
	append(sdecl, init);
	stmts.push_back(sdecl);
}

// The madc::value print_r returns. HOISTED to the enclosing statement through
// m_pending_stmts — the same route every object temp takes (object_arg_addr's
// __madc_objtmp) — because a value declared inside the statement expression
// would be destroyed by its `cleanup` attribute at block exit, i.e. before the
// caller copied from the reference we hand back. var_decl emits the storage and
// its cleanup attribute; the constructor is a separate statement (translate_block
// normally emits it, which does not run for a hoisted temp).
std::string CirBuilder::dump_result_value_temp(TokenBase *origin)
{
	char name[40];
	snprintf(name, sizeof name, "__madc_dumpval_%d", m_strtmp_counter++);
	Variable *tmp = new Variable(name, ddARRAY, 1, NULL, false);
	tmp->flags |= vfLOCAL;
	m_pending_stmts.push_back(var_decl(tmp, origin));
	m_pending_stmts.push_back(array_ctor_call(name, origin));
	return std::string(name);
}

// PHP returns the TEXT when $return is true and boolean TRUE when it is not,
// and one madc::value carries either — so this is `string|true`, not an
// approximation of it. With no sink there is no text to return, so the answer is
// unconditionally true.
node_t CirBuilder::dump_result_assign(const std::string &val_var,
				      const std::string &sink_var,
				      const std::string &ret_var,
				      TokenBase *origin)
{
	// WHICH runtime entry assigns a bool / a C string to a madc::value is
	// owned by parser.cpp's registered `operator=` overload set, so it is READ
	// off the registration (emit_symbol) rather than spelled here. Spelling
	// `madarray_assign_bool` at this site would be a second home for that
	// binding — the divergence-by-duplication this codebase gates against.
	FuncDef *bop = class_assign_scalar_operator_def(&ddARRAY, DataType::dtBOOL);
	FuncDef *cop = class_assign_cstr_operator_def(&ddARRAY);
	if (!bop || bop->emit_symbol.empty() || !cop || cop->emit_symbol.empty())
		return error_node("madc::value is missing its registered "
				  "operator= runtime binding", origin);

	need_output_extern(bop->emit_symbol.c_str(), true,
			   { { {N_VOID}, true }, { {N_LONG, N_LONG}, false } });
	node_t ba = list();
	append(ba, object_addr(val_var.c_str(), origin));
	append(ba, integer(1, origin));
	node_t as_true = node2(N_CALL, id(bop->emit_symbol.c_str(), origin), ba,
			       origin);
	if (sink_var.empty())
		return node2(N_EXPR, list(), as_true, origin);

	// <assign_cstr>(&val, __madc_dump_sink_text(sink))
	need_output_extern(cop->emit_symbol.c_str(), true,
			   { { {N_VOID}, true }, { {N_CHAR}, true } });
	need_output_extern("__madc_dump_sink_text", true,
			   { { {N_VOID}, true } }, { N_CHAR });
	node_t ta = list();
	append(ta, id(sink_var.c_str(), origin));
	node_t text = node2(N_CALL, id("__madc_dump_sink_text", origin), ta,
			    origin);
	node_t ca = list();
	append(ca, object_addr(val_var.c_str(), origin));
	append(ca, text);
	node_t as_text = node2(N_CALL, id(cop->emit_symbol.c_str(), origin), ca,
			       origin);
	return node2(N_EXPR, list(),
		     node3(N_COND, id(ret_var.c_str(), origin), as_text,
			   as_true, origin), origin);
}

node_t CirBuilder::dump_sink_close(const std::string &sink_var,
				   TokenBase *origin)
{
	need_output_extern("__madc_dump_sink_close", false,
			   { { {N_VOID}, true } });
	node_t a = list();
	append(a, id(sink_var.c_str(), origin));
	return node2(N_EXPR, list(),
		     node2(N_CALL, id("__madc_dump_sink_close", origin), a,
			   origin), origin);
}

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

	// PHP: print_r(mixed $value, bool $return = false): string|true — ONE
	// function with a DEFAULT second parameter, so one or two arguments.
	// var_dump is variadic and dumps each argument in order, as PHP does.
	if (fl == dfPrintR && tcf->parameters.size() > 2)
		return error_node("php::print_r takes one or two arguments "
				  "(the value, and PHP's $return flag)", origin);
	if (tcf->parameters.empty())
		return error_node((std::string(dump_flavor_name(fl))
				   + " needs at least one argument").c_str(),
				  origin);

	// The $return flag. ABSENT means false — the DEFAULT ARGUMENT is applied
	// here rather than by the parser, because the compiler IS this function's
	// implementation and the placeholder deliberately carries no parameters
	// (its zero-arity is load-bearing for overload ranking; plan §13.5).
	TokenBase *ret_arg = (fl == dfPrintR && tcf->parameters.size() == 2)
			     ? tcf->parameters[1] : NULL;
	// No literal folding here: an ABSENT flag is the only case worth a
	// separate shape (it is every call the suite makes, and it must stay off
	// the value/capture path). A PRESENT flag is always treated as a runtime
	// value — `print_r(x, true)` then emits a test on a constant, which c2mir
	// folds, and in exchange this handles a literal, a variable and an
	// expression through one path instead of three.
	bool may_capture = (ret_arg != NULL);
	// The result is read unless this very call IS the discarded expression
	// statement. When nobody reads it, no madc::value is materialized — which
	// is what keeps a plain `php::print_r(x);` on the pure-C11 runtime path,
	// with no value machinery linked (and so still ledger-clean for a
	// -static-libmadc AOT image).
	bool result_used = (fl == dfPrintR)
			   && m_discarded_stmt_expr != (TokenBase *)tcf;

	std::vector<node_t> stmts;
	std::string why;
	std::string sink_var, ret_var, val_var;
	if (may_capture)
		dump_sink_open(stmts, ret_arg, ret_var, sink_var, origin);
	if (result_used)
		val_var = dump_result_value_temp(origin);

	// Every primitive call inside the walk carries this sink (dump_call_stmt
	// prepends it). Restored after, so a dump nested in another expression
	// cannot inherit it.
	std::string saved_sink = m_dump_sink_var;
	m_dump_sink_var = sink_var;
	bool walked = true;
	// print_r dumps ONLY its first argument; the second is the flag.
	size_t nvals = (fl == dfPrintR) ? 1 : tcf->parameters.size();
	for (size_t i = 0; i < nvals && walked; i++)
		if (!dump_argument(fl, tcf->parameters[i], stmts, origin, why))
			walked = false;
	m_dump_sink_var = saved_sink;
	if (!walked)
		return error_node((std::string(dump_flavor_name(fl))
				   + ": " + why).c_str(), origin);

	// PHP's return value: the captured TEXT when $return is true, boolean
	// true when it is not. One madc::value carries either — that IS PHP's
	// `string|true`. Assign BEFORE closing the sink: the text points into the
	// sink's own buffer.
	if (result_used)
		stmts.push_back(dump_result_assign(val_var, sink_var, ret_var,
						   origin));
	if (!sink_var.empty())
		stmts.push_back(dump_sink_close(sink_var, origin));

	// When the RESULT IS READ, the whole dump is HOISTED to the enclosing
	// statement and the expression is just the value's name.
	//
	// It cannot be a statement expression: `({ ...; &tmp; })` is not an
	// LVALUE, and a consumer of a `value &` return legitimately takes its
	// address — `c = php::print_r(p, true);` failed with "lvalue required as
	// unary & operand" precisely there. A plain identifier IS an lvalue, so
	// every consumer (assignment, argument, initializer) sees the ordinary
	// shape it already handles. The walk is a list of STATEMENTS anyway;
	// m_pending_stmts is where statements produced while building an
	// expression belong, and it is the same route every object temp takes.
	if (result_used) {
		for (size_t i = 0; i < stmts.size(); i++)
			m_pending_stmts.push_back(stmts[i]);
		return id(val_var.c_str(), origin);
	}

	// A statement expression keeps a call in expression position one node.
	node_t items = list();
	for (size_t i = 0; i < stmts.size(); i++)
		append(items, stmts[i]);
	// c2mir requires a statement expression's LAST statement to be an
	// EXPRESSION, and a dump legitimately ends with a for-loop (print_r of a
	// text container at top level emits only the character loop). Close with
	// the RESULT when one is read, or a discarded 0 when none is, so no shape
	// has to remember.
	// The result is the value's ADDRESS: print_r is declared to return
	// `madc::value &`, and madc renders a reference return as a pointer. The
	// temp itself was hoisted to the enclosing statement (dump_result_value_temp
	// -> m_pending_stmts), so it outlives this block — a value declared INSIDE
	// would be destroyed by its cleanup attribute before the caller copied it.
	append(items, node2(N_EXPR, list(), integer(0, origin), origin));
	return node1(N_STMTEXPR, node2(N_BLOCK, list(), items, origin), origin);
}
