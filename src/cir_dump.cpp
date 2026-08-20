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

// The dump output contract: the flavor discriminator, the column geometry and
// every primitive's prototype. Shared with src/rt/rt_dump.c (which emits the
// bytes) and src/rt_dump_value.cpp (which walks a madc::value at run time).
#include "rt/rt_dump.h"

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
// The geometry itself lives in src/rt/rt_dump.h, because the RUNTIME value walk
// computes the same columns as it descends and two copies of `8 * depth` would
// be free to diverge. These convert the compiler-side flavor enum to the shared
// wire discriminator and delegate — enum-over-strings: one conversion, at the
// edge.
static int dump_wire_flavor(CirBuilder::DumpFlavor fl)
{
	return fl == CirBuilder::dfVarDump ? MADC_DUMP_VAR_DUMP
					   : MADC_DUMP_PRINT_R;
}

static int dump_frame_col(CirBuilder::DumpFlavor fl, int depth)
{
	return madc_dump_frame_col(dump_wire_flavor(fl), depth);
}

static int dump_entry_col(CirBuilder::DumpFlavor fl, int depth)
{
	return madc_dump_entry_col(dump_wire_flavor(fl), depth);
}

// The column STEP per nesting level — the linearity the generated dumper rests
// on. frame_col(d) == step * d, so frame_col(a + b) == frame_col(a) +
// frame_col(b): a function that knows only its BASE column can still emit every
// column inside itself as base + a compile-time constant.
static int dump_col_step(CirBuilder::DumpFlavor fl)
{
	return madc_dump_frame_col(dump_wire_flavor(fl), 1);
}

// ---------------------------------------------------------------------------
// What is NOT a compile-time constant inside a generated dumper
// ---------------------------------------------------------------------------
// The in-line walk is EXPANDED per nesting level, so it knows its absolute
// depth and every column is a literal. A generated dumper FUNCTION is shared by
// call sites at DIFFERENT depths (the same `Node *` is dumped at top level and
// three levels into a struct), so three things become runtime values there: the
// column, the absolute depth, and whether this value is an ENTRY of an
// enclosing aggregate. Each gets ONE owner below, so no builder has to know
// which context it is in — they all just ask.

// A column: `depth` is RELATIVE to the enclosing generated dumper, absolute
// outside one.
node_t CirBuilder::dump_col(DumpFlavor fl, int depth, bool entry,
			    TokenBase *origin)
{
	int k = entry ? dump_entry_col(fl, depth) : dump_frame_col(fl, depth);
	if (m_dump_col_base.empty())
		return integer(k, origin);
	if (!k)
		return id(m_dump_col_base.c_str(), origin);
	return node2(N_ADD, id(m_dump_col_base.c_str(), origin),
		     integer(k, origin), origin);
}

// The ABSOLUTE depth, for the one primitive that needs a depth rather than a
// column: the runtime madc::value walk, which computes its own descent.
node_t CirBuilder::dump_depth_arg(int depth, TokenBase *origin)
{
	if (m_dump_fn_depth.empty())
		return integer(depth, origin);
	if (!depth)
		return id(m_dump_fn_depth.c_str(), origin);
	return node2(N_ADD, id(m_dump_fn_depth.c_str(), origin),
		     integer(depth, origin), origin);
}

// Is this value an ENTRY of an enclosing aggregate? print_r follows a nested
// block's ")" with a blank line and ends a scalar entry with a newline, and both
// ask this. At relative depth 0 inside a generated dumper the answer belongs to
// the CALLER, so it is that function's own parameter; anywhere deeper it is
// unconditionally true.
node_t CirBuilder::dump_nested_arg(int depth, bool nested, TokenBase *origin)
{
	if (!depth && !m_dump_fn_nested.empty())
		return id(m_dump_fn_nested.c_str(), origin);
	return integer(nested ? 1 : 0, origin);
}

// print_r's end of a SCALAR entry. NULL when nothing is owed — print_r(42) at
// top level is exactly "42". Inside a generated dumper at relative depth 0 the
// answer is only known at run time, so it becomes a test on `nested`.
node_t CirBuilder::dump_pr_end_entry(DumpFlavor fl, int depth,
				     TokenBase *origin)
{
	if (fl != dfPrintR)
		return NULL;		// every var_dump primitive ends its own line
	if (depth > 0)
		return dump_pr_nl(origin);
	if (m_dump_fn_nested.empty())
		return NULL;		// genuinely the top level
	node_t items = list();
	append(items, dump_pr_nl(origin));
	return node4(N_IF, list(), id(m_dump_fn_nested.c_str(), origin),
		     node2(N_BLOCK, list(), items, origin), ignore(), origin);
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

// print_r's word for an aggregate's opening line. PHP's own spelling, kept
// because a struct IS what a PHP developer reads as an object. ONE owner: the
// *RECURSION* marker prints the word of the frame it REPLACES, and a second
// " Object" spelling there would be free to drift from this one.
static std::string dump_pr_object_word(DataDefSTRUCT *sdd)
{
	return sdd->name + " Object";
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

// The type word for ANY type: a CONTAINER goes through the container rule, any
// other class through the class rule (alias, then canonical spelling), and
// everything else through the scalar table. One owner, so a nested element word
// cannot disagree with a top-level one — which it did: a std::map's VALUE word
// came straight from the canonical spelling and said
// std::vector<int32_t,std::allocator<int32_t>> on the head line while the very
// next line, rendered by the sequence walk, said std::vector<int>. The
// container arms feed their element types back through here, so the recursion
// terminates on the element nesting.
std::string CirBuilder::dump_type_word(DataDef *dd)
{
	if (dd && !dd->is_pointer() && !dd->is_reference())
		if (DataDefCLASS *c = is_class_object(dd)
				    ? dynamic_cast<DataDefCLASS *>(dd->unqualified())
				    : NULL)
			return dump_container_type_word(c);
	return dump_scalar_type_word(dd);
}

// A class's word, container-aware. The two container recognizers decide it, the
// same ones the two container WALKS use — so the word on an entry's head line
// and the word the walk below it prints are answers to one question.
std::string CirBuilder::dump_container_type_word(DataDefCLASS *cls)
{
	// A SELF-REFERENTIAL container's word contains itself. Fall back to the
	// canonical spelling rather than recursing: a `Box<int>` whose operator[]
	// returns `Box<int>&` would otherwise build its own name forever (a
	// non-template one exits below anyway, because dump_template_word needs a
	// '<'). m_dump_word_expanding is its own set — see the header for why it
	// cannot share the walk's.
	if (!cls || m_dump_word_expanding.count(cls))
		return cls ? dump_class_type_word(cls) : std::string("?");
	m_dump_word_expanding.insert(cls);
	std::string word = dump_container_type_word_inner(cls);
	m_dump_word_expanding.erase(cls);
	return word;
}

std::string CirBuilder::dump_container_type_word_inner(DataDefCLASS *cls)
{
	Variable *szmv = NULL, *opmv = NULL;
	if (class_index_iteration_protocol(cls, szmv, opmv)) {
		FuncDef *opfd = opmv ? dynamic_cast<FuncDef *>(opmv->type) : NULL;
		if (opfd)
			return dump_sequence_type_word(cls,
						       &opfd->return_value_type());
	}
	IterProtocol ip;
	if (class_iterator_iteration_protocol(cls, ip, NULL, this)) {
		DataDef *elem = ip.elem;
		std::vector<DataDef *> wargs;
		DataDef *kdd = NULL, *vdd = NULL;
		if (ip.keyed) {
			DataDefSTRUCT *pair = dynamic_cast<DataDefSTRUCT *>(
						elem ? elem->unqualified() : NULL);
			for (size_t i = 0; pair && i < pair->members.size(); i++) {
				if (pair->members[i].first == "first")
					kdd = pair->members[i].second;
				else if (pair->members[i].first == "second")
					vdd = pair->members[i].second;
			}
		}
		if (kdd && vdd) {
			wargs.push_back(kdd);
			wargs.push_back(vdd);
		} else {
			wargs.push_back(elem);
		}
		return dump_template_word(cls, wargs);
	}
	return dump_class_type_word(cls);
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
	std::vector<DataDef *> args;
	args.push_back(elem);
	return dump_template_word(cls, args);
}

// The general form, and the ONE owner of the defaulted-argument strip: a KEYED
// container carries TWO informative arguments (std::map<int,int>) where a
// sequence carries one, and the canonical spelling carries four
// (std::map<int,int,std::less<int>,std::allocator<std::pair<const int,int>>>).
// Both container walks render their word through here, so a map's word cannot
// use a different rule from a vector's.
std::string CirBuilder::dump_template_word(DataDefCLASS *cls,
					   const std::vector<DataDef *> &args)
{
	std::string word = dump_class_type_word(cls);
	size_t lt = word.find('<');
	if (lt == std::string::npos || args.empty())
		return word;
	std::string out = word.substr(0, lt) + "<";
	for (size_t i = 0; i < args.size(); i++) {
		if (!args[i])
			return word;   // an unresolved argument: keep the canonical
		if (i)
			out += ",";
		out += dump_type_word(args[i]);
	}
	return out + ">";
}

// An array's type word: the element's, with every extent from THIS dimension
// outward — `int[2][3]` at the outer level of an `int m[2][3]` and `int[3]` one
// level in, which is the shape C declares and the nesting PHP renders.
std::string CirBuilder::dump_array_type_word(DataDef *elem,
					     const std::vector<carray_dim_t> &dims,
					     size_t dim_ix)
{
	std::string word = dump_type_word(elem);
	for (size_t d = dim_ix; d < dims.size(); d++) {
		char n[32];
		snprintf(n, sizeof(n), "[%llu]", (unsigned long long)dims[d]);
		word += n;
	}
	return word;
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
			return strip_inline_namespaces(alias);
	}
	if (!canon.empty())
		return strip_inline_namespaces(canon);
	return "struct " + cls->name;
}

// An INLINE namespace is transparent to qualified lookup, so it is not part of
// the name anybody WRITES. std::list<int> is canonically
// std::__cxx11::list<int,std::allocator<int>> — the libstdc++ ABI inline
// namespace — and printing that is exactly the show-the-canonical-name-instead-
// of-the-source's mistake the alias lookup above exists to avoid for
// std::string. std::map has no such component and std::list does, so without
// this one var_dump line says std::map<int,int> and the next says
// std::__cxx11::list<int>.
//
// Driven by Program::inline_namespace_children — the parser's OWN record of
// which namespaces are inline, kept for [namespace.qual] lookup — so no
// spelling is hardcoded here and libc++'s __1 is handled by the same code.
// Each replacement strictly shortens the string (a child's qualified name
// contains its parent's as a prefix), so the outer pass terminates.
std::string CirBuilder::strip_inline_namespaces(const std::string &spelling)
{
	if (!m_prog)
		return spelling;
	std::string out = spelling;
	bool again = true;
	while (again) {
		again = false;
		std::map<std::string, std::vector<std::string> >::const_iterator it;
		for (it = m_prog->inline_namespace_children.begin();
		     it != m_prog->inline_namespace_children.end(); ++it) {
			for (size_t i = 0; i < it->second.size(); i++) {
				const std::string &child = it->second[i];
				if (child.empty() || child == it->first)
					continue;
				std::string from = child + "::";
				std::string to = it->first.empty()
					       ? std::string()
					       : it->first + "::";
				size_t at;
				while ((at = out.find(from)) != std::string::npos) {
					out.replace(at, from.size(), to);
					again = true;
				}
			}
		}
	}
	return out;
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
	append(a, dump_col(fl, depth, false, origin));
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
	append(a, dump_col(fl, depth, true, origin));
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
	append(a, dump_col(fl, depth, true, origin));
	append(a, idx);
	return dump_call_stmt(sym, a, origin);
}

// The two halves of an INLINE key. A struct member's key is a compile-time
// literal (dump_key above); a CONTAINER's key is a runtime value of a real type,
// so the walk renders it BETWEEN these. rt_dump.c writes the literal form in
// terms of the same pair, so the bracket spelling has one owner.
node_t CirBuilder::dump_key_open(DumpFlavor fl, int depth, bool quote,
				 TokenBase *origin)
{
	node_t a = list();
	append(a, dump_col(fl, depth, true, origin));
	if (fl == dfVarDump) {
		need_dump_extern("__madc_dump_vd_key_open",
				 { { {N_INT}, false }, { {N_INT}, false } });
		append(a, integer(quote ? 1 : 0, origin));
		return dump_call_stmt("__madc_dump_vd_key_open", a, origin);
	}
	need_dump_extern("__madc_dump_pr_key_open", { { {N_INT}, false } });
	return dump_call_stmt("__madc_dump_pr_key_open", a, origin);
}

node_t CirBuilder::dump_key_close(DumpFlavor fl, bool quote, TokenBase *origin)
{
	node_t a = list();
	if (fl == dfVarDump) {
		need_dump_extern("__madc_dump_vd_key_close",
				 { { {N_INT}, false } });
		append(a, integer(quote ? 1 : 0, origin));
		return dump_call_stmt("__madc_dump_vd_key_close", a, origin);
	}
	need_dump_extern("__madc_dump_pr_key_close", {});
	return dump_call_stmt("__madc_dump_pr_key_close", a, origin);
}

// A container ENTRY's key, rendered inline between those two.
//
// The key goes through the SAME walk as any other value — with the PRINT_R
// flavor regardless of `fl`, because print_r's scalar form IS the bare value
// with no type word and no newline, which is exactly what PHP puts inside the
// brackets under BOTH flavors: `[3] => 30`, `[3]=>`, `["b"]=>`. Depth 0 is what
// suppresses print_r's end-of-entry newline (dump_pr_end_entry decides that from
// the depth), so the key stays on its own line. Oracle: tmp/or/map.php,
// php-cli 8.3.6, cat -A.
//
// var_dump's one addition is the QUOTES around a text key, and whether the key
// is text is a compile-time property of its type.
bool CirBuilder::dump_key_value(DumpFlavor fl, const DumpAccess &kacc,
				DataDef *kdd, int depth,
				std::vector<node_t> &out, TokenBase *origin,
				std::string &why)
{
	bool quote = fl == dfVarDump && dump_type_is_text(kdd);
	out.push_back(dump_key_open(fl, depth, quote, origin));
	if (!dump_any(dfPrintR, kacc, kdd, NULL, 0, false, out, origin, why))
		return false;
	out.push_back(dump_key_close(fl, quote, origin));
	return true;
}

node_t CirBuilder::dump_tail(DumpFlavor fl, int depth, bool nested,
			     TokenBase *origin)
{
	node_t a = list();
	append(a, dump_col(fl, depth, false, origin));
	if (fl == dfVarDump) {
		need_dump_extern("__madc_dump_vd_tail",
				   { { {N_INT}, false } });
		return dump_call_stmt("__madc_dump_vd_tail", a, origin);
	}
	// print_r follows a NESTED block's ")" with a blank line; the outermost
	// one gets none.
	need_dump_extern("__madc_dump_pr_tail",
			   { { {N_INT}, false }, { {N_INT}, false } });
	append(a, dump_nested_arg(depth, nested, origin));
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
	append(a, dump_col(dfVarDump, depth, false, origin));
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

// var_dump's NULL line. A null pointer is PHP's null, and NULL is the one PHP
// word var_dump keeps ("no value" is not a C type). rt_dump.c owns the spelling,
// and a null `char *` already routes to the same primitive — so the two can
// never disagree.
node_t CirBuilder::dump_vd_null(int depth, TokenBase *origin)
{
	need_dump_extern("__madc_dump_vd_null", { { {N_INT}, false } });
	node_t a = list();
	append(a, dump_col(dfVarDump, depth, false, origin));
	return dump_call_stmt("__madc_dump_vd_null", a, origin);
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
	// a lane as a decimal.
	//
	// A pointer reaching HERE is one dump_any declined to follow — a function
	// pointer, a pointer-to-member, a `void *`. A followable one never arrives:
	// dump_any routes it to dump_pointer first, and an ENUM to dump_enum, so
	// neither needs an arm here.
	else if (dd->is_pointer() || dd->is_reference() || dd->is_member_pointer()
		 || dd->is_object() || dd->is_function() || dd->is_simd()
		 || dd->is_complex()) {
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
		append(a, dump_col(fl, depth, false, origin));
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
	if (node_t nl = dump_pr_end_entry(fl, depth, origin))
		out.push_back(nl);
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
				  : dump_pr_object_word(sdd),
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
		// The DIM CHAIN, not the flattened count. member_counts holds
		// the TOTAL element count (6 for `int m[2][3]`), and `m[i]`
		// yields a ROW rather than an element, so a flat walk would read
		// past the first row. member_dims carries the real shape; a
		// 1-dimensional member whose dims were not recorded falls back to
		// the count, which for one dimension IS the extent.
		std::vector<carray_dim_t> mdims;
		if (is_arr) {
			if (i < sdd->member_dims.size()
			    && !sdd->member_dims[i].empty())
				mdims = sdd->member_dims[i];
			else
				mdims.push_back((carray_dim_t)(count ? count : 1));
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
		if (!dump_any(fl, macc, mt, is_arr ? &mdims : NULL, depth + 1,
			      true, out, origin, why)) {
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
//
// MULTIPLE DIMENSIONS are ordinary recursion over the dim chain, one level per
// dimension — which is exactly how PHP renders one: nested arrays. `dim_ix` is
// the dimension this call frames, so `int m[2][3]` frames 2 rows here and each
// row re-enters at dim_ix 1 to frame its 3 elements. The access composes the
// same way: each level's `eacc` wraps the level above it, so the leaf emits
// `m[i][j]` and no level has to know how deep it is.
bool CirBuilder::dump_array(DumpFlavor fl, const DumpAccess &acc, DataDef *elem,
			    const std::vector<carray_dim_t> &dims, size_t dim_ix,
			    int depth, bool nested, std::vector<node_t> &out,
			    TokenBase *origin, std::string &why)
{
	if (!elem) {
		why = "array of unresolved element type";
		return false;
	}
	if (dim_ix >= dims.size()) {
		why = "array with no recorded extent";
		return false;
	}
	size_t count = (size_t)dims[dim_ix];
	if (!count)
		count = 1;
	bool last = dim_ix + 1 == dims.size();

	// A char array IS text to a PHP developer, not an array of small ints —
	// the same rule that makes std::string print as its contents. Bounded by
	// the extent: a C array need not be NUL-terminated, and %s would read
	// past it. The rule applies at the LAST dimension only: a row of
	// `char n[2][8]` is a string, and the level above it is an array of them.
	if (last && (elem->rawtype() == DataType::dtINT8
		     || elem->rawtype() == DataType::dtUINT8)) {
		const char *sym = fl == dfVarDump ? "__madc_dump_vd_cstr_n"
						  : "__madc_dump_pr_cstr_n";
		std::vector<ExternParam> params;
		node_t a = list();
		if (fl == dfVarDump) {
			params.push_back({ {N_INT}, false });
			params.push_back({ {N_CHAR}, true });
			append(a, dump_col(fl, depth, false, origin));
			std::string word = dump_array_type_word(elem, dims,
							       dim_ix);
			append(a, str(word.c_str(), word.size() + 1, origin));
		}
		params.push_back({ {N_CHAR}, true });
		params.push_back({ {N_LONG, N_LONG}, false });
		append(a, acc());
		append(a, integer((int64_t)count, origin));
		need_dump_extern(sym, params);
		out.push_back(dump_call_stmt(sym, a, origin));
		if (node_t nl = dump_pr_end_entry(fl, depth, origin))
			out.push_back(nl);
		return true;
	}

	out.push_back(dump_head(fl, depth,
				fl == dfVarDump
				  ? dump_array_type_word(elem, dims, dim_ix)
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
	// The innermost dimension holds ELEMENTS; every outer one holds arrays.
	bool inner_ok = last
		      ? dump_any(fl, eacc, elem, NULL, depth + 1, true, body,
				 origin, why)
		      : dump_array(fl, eacc, elem, dims, dim_ix + 1, depth + 1,
				   true, body, origin, why);
	if (!inner_ok)
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

// Is this a CHARACTER type — the element that makes a positional sequence TEXT
// rather than an array of numbers? ONE owner: dump_sequence keys its own framing
// on it, and dump_type_is_text asks it about a container's element one level up.
static bool dump_elem_is_char(DataDef *elem)
{
	return elem && (elem->rawtype() == DataType::dtINT8
		     || elem->rawtype() == DataType::dtUINT8);
}

// Does this type RENDER as PHP's string? var_dump quotes a STRING key and leaves
// every other key bare, and the compiler decides which — so this is the same
// question the walk's own text arms already answer, asked one level up: a char
// pointer (dump_scalar's is_cstr arm) or a positional sequence of characters
// (dump_sequence's is_text arm, which is what std::string is).
bool CirBuilder::dump_type_is_text(DataDef *dd)
{
	DataDef *u = dd ? dd->unqualified() : NULL;
	if (!u)
		return false;
	if (u->is_cstr())
		return true;
	DataDefCLASS *cls = is_class_object(u)
			  ? dynamic_cast<DataDefCLASS *>(u) : NULL;
	Variable *szmv = NULL, *opmv = NULL;
	if (!cls || !class_index_iteration_protocol(cls, szmv, opmv))
		return false;
	FuncDef *opfd = opmv ? dynamic_cast<FuncDef *>(opmv->type) : NULL;
	return opfd && dump_elem_is_char(&opfd->return_value_type());
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

	bool is_text = dump_elem_is_char(elem);
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
		if (!dump_any(fl, eacc, elem, NULL, depth + 1, true, body,
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
		else if (node_t nl = dump_pr_end_entry(fl, depth, origin))
			out.push_back(nl);
	} else {
		out.push_back(dump_tail(fl, depth, nested, origin));
	}
	return true;
}

// ---------------------------------------------------------------------------
// An ITERATOR container (std::map, std::set, std::list)
// ---------------------------------------------------------------------------
// A container with no POSITION: its elements are reached through C++'s own
// iterator protocol. The structural test is
// class_iterator_iteration_protocol — the SHARED type-checked predicate, the
// twin of the positional one dump_sequence uses, and the same one the range-for
// keys on. No method is matched by name here for the same reason it is not
// there: a by-name match is what made `for (int v : map)` SIGSEGV.
//
// The emitted shape is:
//
//     long n = c.size();
//     struct It it = c.begin();
//     <head n>
//     for (long k = 0; k < n; k += 1) { <key>; <value>; ++it; }
//     <tail>
//
// WHY COUNTED AND NOT `it != c.end()`: libstdc++ declares operator== and
// operator!= on every one of these iterators as FRIEND FREE functions, not
// members, so there is no member to dispatch and a generated comparison is not
// available at all. size() is the bound instead — which costs nothing, because
// var_dump's head line states the count anyway. The recognizer owns that
// requirement, which is why size() is part of the PREDICATE and not of this
// function.
//
// A KEYED container renders `[key] => value`; a set and a list render
// positionally, exactly like a vector. That is not a naming decision: PHP has no
// set, a list of values IS a list, and only a map has a key to print.
bool CirBuilder::dump_iterator(DumpFlavor fl, const DumpAccess &acc,
			       DataDefCLASS *cls, int depth, bool nested,
			       std::vector<node_t> &out, TokenBase *origin,
			       std::string &why)
{
	IterProtocol ip;
	if (!class_iterator_iteration_protocol(cls, ip, &why, this))
		return false;
	DataDefCLASS *itcls = ip.itcls;
	DataDef *elem = ip.elem;
	bool keyed = ip.keyed;

	// A KEYED container's element is a std::pair<const K, V>, and PHP renders
	// a map as `[key] => value`, never as a pair. The two halves come from the
	// ELEMENT type's own members — which is what `value_type` is — rather than
	// from the container, so nothing here knows the word "pair". If either is
	// missing the element renders positionally: honest, rather than wrong.
	DataDef *kdd = NULL, *vdd = NULL;
	std::string kname, vname;
	if (keyed) {
		DataDefSTRUCT *pair = dynamic_cast<DataDefSTRUCT *>(
					elem ? elem->unqualified() : NULL);
		for (size_t i = 0; pair && i < pair->members.size(); i++) {
			if (pair->members[i].first == "first") {
				kdd = pair->members[i].second;
				kname = "first";
			} else if (pair->members[i].first == "second") {
				vdd = pair->members[i].second;
				vname = "second";
			}
		}
		if (!kdd || !vdd)
			keyed = false;
	}

	char nm[40], kx[40], itn[40];
	snprintf(nm, sizeof(nm), "__dmp_n_%d", m_strtmp_counter++);
	snprintf(kx, sizeof(kx), "__dmp_k_%d", m_strtmp_counter++);
	snprintf(itn, sizeof(itn), "__dmp_it_%d", m_strtmp_counter++);
	std::string nname = nm, idxname = kx, itname = itn;

	// long n = c.size();  — the count ONCE, into a local: the head line and
	// the loop bound must agree, and size() is a real call.
	node_t szcall = class_nullary_call(cls, "size",
					   node1(N_ADDR, acc(), origin), origin);
	if (!szcall) {
		why = std::string("its size() is not callable");
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

	// struct It it = c.begin();  — an ordinary local, copy-initialized from
	// begin()'s native struct return (the recognizer rejected the __retbuf
	// shape, so this assignment is the whole of the copy).
	node_t bcall = class_nullary_call(cls, "begin",
					  node1(N_ADDR, acc(), origin), origin);
	if (!bcall) {
		why = std::string("its begin() is not callable");
		return false;
	}
	node_t ispecs = list();
	node_t istars = list();
	if (itcls) {
		append(ispecs, class_tag_ref(itcls));
	} else {
		// A raw-pointer iterator: the pointee's spec plus one star per
		// level, named ONCE by the same owner the pointer walk uses.
		DataDef *ibase = ip.itptr->unqualified();
		int stars = 1 + dd_peel_pointers(ibase);
		if (!dump_pointee_specs(ibase, ispecs)) {
			why = std::string("its iterator's pointee has no "
					  "renderable declaration");
			return false;
		}
		for (int i = 0; i < stars; i++)
			append(istars, pointer());
	}
	node_t idecl = simple(N_SPEC_DECL, origin);
	append(idecl, node1(N_SHARE, ispecs));
	append(idecl, node2(N_DECL, id(itname.c_str(), origin), istars));
	append(idecl, ignore());
	append(idecl, ignore());
	append(idecl, bcall);
	out.push_back(idecl);

	// The element LVALUE, rebuilt per use (a c2mir node has one parent): a
	// class iterator's `operator*` — class_nullary_call already dereferences a
	// reference return, so this IS the element and not its address — or a plain
	// dereference of a pointer iterator.
	DumpAccess eacc = [this, itname, itcls, origin]() -> node_t {
		if (!itcls)
			return node1(N_DEREF, id(itname.c_str(), origin), origin);
		return class_nullary_call(itcls, "operator*",
					  node1(N_ADDR,
						id(itname.c_str(), origin),
						origin),
					  origin);
	};

	std::vector<DataDef *> wargs;
	if (keyed) {
		wargs.push_back(kdd);
		wargs.push_back(vdd);
	} else {
		wargs.push_back(elem);
	}
	std::string word = fl == dfVarDump ? dump_template_word(cls, wargs)
					   : std::string("Array");
	out.push_back(dump_head_node(fl, depth, word, id(nname.c_str(), origin),
				     origin));

	std::vector<node_t> body;
	if (keyed) {
		DumpAccess kacc = [this, eacc, kname, origin]() -> node_t {
			return node2(N_FIELD, eacc(), id(kname.c_str(), origin),
				     origin);
		};
		DumpAccess vacc = [this, eacc, vname, origin]() -> node_t {
			return node2(N_FIELD, eacc(), id(vname.c_str(), origin),
				     origin);
		};
		if (!dump_key_value(fl, kacc, kdd, depth, body, origin, why))
			return false;
		if (!dump_any(fl, vacc, vdd, NULL, depth + 1, true, body, origin,
			      why))
			return false;
	} else {
		// No key to print, so the POSITION is the key — the same rendering
		// a vector gets, and the k the loop already counts.
		body.push_back(dump_key_idx(fl, depth,
					    id(idxname.c_str(), origin), origin));
		if (!dump_any(fl, eacc, elem, NULL, depth + 1, true, body, origin,
			      why))
			return false;
	}

	// The advance. For a class iterator, `++it` — the PREFIX operator++,
	// selected by ARITY (the postfix one is the same spelling with a dummy
	// int); class_nullary_call owns that selection, and discards the reference
	// result rather than dereferencing it. For a pointer iterator, `it += 1`,
	// which c2mir scales by the pointee exactly as C does.
	if (!itcls) {
		body.push_back(node2(N_EXPR, list(),
				     node2(N_ADD_ASSIGN,
					   id(itname.c_str(), origin),
					   integer(1, origin), origin),
				     origin));
	} else {
		node_t inc = class_nullary_call(itcls, "operator++",
						node1(N_ADDR,
						      id(itname.c_str(), origin),
						      origin),
						origin, true);
		if (!inc) {
			why = std::string("its iterator's operator++ is not "
					  "callable");
			return false;
		}
		body.push_back(node2(N_EXPR, list(), inc, origin));
	}

	// for (long k = 0; k < n; k += 1) { ... }
	node_t kspec = list();
	append_i64(kspec, origin);
	node_t init = simple(N_SPEC_DECL, origin);
	append(init, node1(N_SHARE, kspec));
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
	out.push_back(dump_tail(fl, depth, nested, origin));
	return true;
}

// Is this class a CONTAINER — something whose elements, not whose members, are
// its content?
//
// Asked only AFTER both container walks have declined, and it decides one thing:
// whether the decline is REFUSED BY NAME or falls through to the member walk. A
// plain aggregate that happens to expose a size() is not a container and must
// still be member-walked; a container's members are the library's internals, so
// walking them produced
//
//   php::print_r: member '_M_t': member '_M_impl': member '_M_header':
//   member '_M_color': no dumper for type '_Rb_tree_color' yet
//
// — four levels into libstdc++'s red-black tree, naming a type the user never
// wrote and never once mentioning std::map. So the test is the ITERATION concept
// itself: begin()/end() and nothing else. Whether the elements can actually be
// REACHED is class_iterator_iteration_protocol's question, and its `why` is what
// the refusal quotes — so a container this walk cannot render is refused by its
// own name AND says which piece is missing.
static bool container_needs_iterator_walk(DataDefCLASS *cls)
{
	if (!cls)
		return false;
	Variable *b = cls->findMethod(std::string("begin"));
	Variable *e = cls->findMethod(std::string("end"));
	return b && e && dynamic_cast<FuncDef *>(b->type) != NULL
	    && dynamic_cast<FuncDef *>(e->type) != NULL;
}

// ---------------------------------------------------------------------------
// An enum
// ---------------------------------------------------------------------------
// PHP 8.1 renders an enum as its own small frame — the case NAME and, for a
// BACKED enum, the backing value:
//
//     Suit Enum:string          enum(Suit::Hearts)          <- var_dump
//     (
//         [name] => Hearts
//         [value] => H
//     )
//
// A C enum is exactly a backed enum: a name and an integer. So print_r keeps
// PHP's shape verbatim and var_dump keeps PHP's one-liner, with the REAL backing
// type in the head word (`Color Enum:int`, `Color Enum:unsigned int`) rather
// than a simulated one. Oracle: tmp/or/enum.php, php-cli 8.3.6, cat -A.
//
// The value -> name resolution is a RUNTIME question (the value is a variable),
// so it becomes a generated lookup function per tag, memoized — the same
// machinery the pointer walk uses for its own per-type functions.

// static char *__madc_enumname_N(long long v)
// {
//         if (v == 0) return "RED";
//         if (v == 1) return "GREEN";
//         return "";
// }
//
// A flat if-chain, NOT a switch: duplicate values are legal C
// (`enum { A = 1, B = 1 }`) and would be duplicate switch labels, which c2mir
// rejects. The FIRST name wins — the source's own order, and what a debugger
// shows. `char *` and not `const char *` because a C string literal IS `char[]`,
// so this needs no const plumbing to be warning-free C.
std::string CirBuilder::dump_enum_name_fn(DataDefENUM *edd, TokenBase *origin)
{
	std::map<DataDef *, std::string>::iterator it
		= m_dump_enum_fn_syms.find(edd);
	if (it != m_dump_enum_fn_syms.end())
		return it->second;

	char nm[48];
	snprintf(nm, sizeof nm, "__madc_enumname_%d", m_dump_fn_counter++);
	std::string fname = nm;
	m_dump_enum_fn_syms[edd] = fname;

	static const char *P_V = "__dv";
	std::vector<node_t> stmts;
	std::set<int64_t> seen;
	for (size_t e = 0; e < edd->enumerators.size(); e++) {
		if (!seen.insert(edd->enumerators[e].second).second)
			continue;
		const std::string &en = edd->enumerators[e].first;
		node_t ritems = list();
		append(ritems, node2(N_RETURN, list(),
				     str(en.c_str(), en.size() + 1, origin),
				     origin));
		stmts.push_back(node4(N_IF, list(),
				      node2(N_EQ, id(P_V, origin),
					    integer(edd->enumerators[e].second,
						    origin),
					    origin),
				      node2(N_BLOCK, list(), ritems, origin),
				      ignore(), origin));
	}
	// A value naming NO enumerator is legal C. The EMPTY string is the honest
	// answer to "which enumerator is this", and it is also exactly what print_r
	// renders for a null — so no call site needs a branch, and var_dump's
	// primitive reads it as "report the type and the number instead".
	stmts.push_back(node2(N_RETURN, list(), str("", 1, origin), origin));

	node_t items = list();
	for (size_t i = 0; i < stmts.size(); i++)
		append(items, stmts[i]);

	node_t vspec = list();
	append(vspec, simple(N_LONG, origin));
	append(vspec, simple(N_LONG, origin));
	node_t params = list();
	append(params, dump_fn_param(vspec, 0, P_V, origin));

	node_t pspecs = list();
	append(pspecs, simple(N_STATIC, origin));
	append(pspecs, simple(N_CHAR, origin));
	node_t pdecl_list = list();
	append(pdecl_list, node1(N_FUNC, params));
	append(pdecl_list, pointer());
	node_t proto = simple(N_SPEC_DECL, origin);
	append(proto, node1(N_SHARE, pspecs));
	append(proto, node2(N_DECL, id(fname.c_str(), origin), pdecl_list));
	append(proto, ignore());
	append(proto, ignore());
	append(proto, ignore());

	node_t dparams = list();
	node_t dvspec = list();
	append(dvspec, simple(N_LONG, origin));
	append(dvspec, simple(N_LONG, origin));
	append(dparams, dump_fn_param(dvspec, 0, P_V, origin));
	node_t dspecs = list();
	append(dspecs, simple(N_STATIC, origin));
	append(dspecs, simple(N_CHAR, origin));
	node_t ddecl_list = list();
	append(ddecl_list, node1(N_FUNC, dparams));
	append(ddecl_list, pointer());
	node_t def = node4(N_FUNC_DEF, dspecs,
			   node2(N_DECL, id(fname.c_str(), origin), ddecl_list),
			   list(),
			   node2(N_BLOCK, list(), items, origin), origin);

	m_pending_top_protos.push_back(proto);
	m_pending_top_defs.push_back(def);
	return fname;
}

bool CirBuilder::dump_enum(DumpFlavor fl, const DumpAccess &acc,
			   DataDefENUM *edd, int depth, bool nested,
			   std::vector<node_t> &out, TokenBase *origin,
			   std::string &why)
{
	// An OPAQUE declaration (`enum E;`) has no enumerator table here, so no
	// name can be shown. Refused BY ITS OWN NAME rather than shipping a bare
	// number that would read as an int — the same rule every other uncovered
	// type in this file follows.
	if (edd->enumerators.empty()) {
		why = std::string("no dumper for enum '") + edd->name
		    + "' yet: no enumerators are visible here, so the value"
		      " cannot be named";
		return false;
	}
	std::string fn = dump_enum_name_fn(edd, origin);
	if (fn.empty()) {
		why = std::string("no dumper for enum '") + edd->name + "' yet";
		return false;
	}
	// The backing type: the declared fixed base, else the one computed from the
	// enumerator range at the definition's close. NULL only for an opaque
	// declaration, which was refused above; int is the documented fallback.
	DataDef *under = edd->underlying ? edd->underlying : &ddINT32;
	// The tag's own spelling. The CANONICAL one when there is one, so a
	// class-nested or namespaced tag reads as `Deck::Kind` /
	// `std::ios_base::event` rather than the bare `Kind` — the same spelling
	// (and the same owner) the forest keys its enumerator run on.
	const std::string &canon = edd->canonical_cpp_spelling();
	std::string tag = canon.empty() ? edd->name : canon;

	if (fl == dfVarDump) {
		need_dump_extern("__madc_dump_vd_enum",
				 { { {N_INT}, false }, { {N_CHAR}, true },
				   { {N_CHAR}, true },
				   { {N_LONG, N_LONG}, false } });
		node_t nargs = list();
		append(nargs, acc());
		node_t a = list();
		append(a, dump_col(fl, depth, false, origin));
		append(a, str(tag.c_str(), tag.size() + 1, origin));
		append(a, node2(N_CALL, id(fn.c_str(), origin), nargs, origin));
		append(a, acc());
		out.push_back(dump_call_stmt("__madc_dump_vd_enum", a, origin));
		return true;
	}

	// print_r: PHP's frame, with the real backing type after the colon.
	std::string word = tag + " Enum:" + dump_type_word(under);
	out.push_back(dump_head(fl, depth, word, 2, origin));

	out.push_back(dump_key(fl, depth, "name", origin));
	need_dump_extern("__madc_dump_pr_cstr", { { {N_CHAR}, true } });
	node_t nargs = list();
	append(nargs, acc());
	node_t na = list();
	append(na, node2(N_CALL, id(fn.c_str(), origin), nargs, origin));
	out.push_back(dump_call_stmt("__madc_dump_pr_cstr", na, origin));
	if (node_t nl = dump_pr_end_entry(fl, depth + 1, origin))
		out.push_back(nl);

	out.push_back(dump_key(fl, depth, "value", origin));
	need_dump_extern("__madc_dump_pr_i64",
			 { { {N_LONG, N_LONG}, false }, { {N_INT}, false } });
	node_t va = list();
	append(va, acc());
	append(va, integer(under->is_unsigned() ? 1 : 0, origin));
	out.push_back(dump_call_stmt("__madc_dump_pr_i64", va, origin));
	if (node_t nl = dump_pr_end_entry(fl, depth + 1, origin))
		out.push_back(nl);

	out.push_back(dump_tail(fl, depth, nested, origin));
	return true;
}

// ---------------------------------------------------------------------------
// A pointer
// ---------------------------------------------------------------------------
// PHP has no pointers, so what a PHP developer expects to see is the POINTEE:
// `Node *n` renders exactly as `*n` would, at the SAME depth — a pointer is an
// indirection, not a nesting level. A null pointer is PHP's null (nothing for
// print_r, NULL for var_dump), and the dereference is guarded so a null member
// cannot fault the dump.
//
// THE RECURSION IS A RUNTIME QUESTION, AND THAT DECIDES THE WHOLE SHAPE. Every
// other type in this file is EXPANDED in line, which works because the type
// bounds the walk. A pointer graph does not: `struct N { N *next; }` is a
// linked list, and how long it is — and whether it loops — is a property of the
// DATA. An earlier attempt expanded the pointee in line and guarded with a stack
// of pointee TYPES on the current path. It terminated, but only by REFUSING
// `struct N { N *next; }` — a linked list, the canonical thing anyone wants to
// print_r. That is loop AVOIDANCE BY REFUSAL, not loop detection, and the
// feature was absent behind a check that looked like one. (It also blew up on an
// ACYCLIC fan-out graph: the type stack is a PATH — it must be, see below — so a
// shared subtree re-expanded once per path, and a 14-level fan-out-2 chain took
// 57 seconds before dying.)
//
// So the pointee walk becomes a FUNCTION and the recursion becomes a CALL. That
// one move fixes the cycle, the long list and the fan-out together: a shared
// pointee is one call per site instead of one expansion, and the depth of the
// walk is the depth of the DATA, decided at run time by an ancestor stack.
//
// It must be an ANCESTOR STACK and not a visited set. Oracle, php-cli 8.3.6:
// an object reachable TWICE acyclically prints IN FULL BOTH TIMES with no marker
// (tmp/or/share.php), and only a genuine ring gets ` *RECURSION*`
// (tmp/or/ptr.php). A `set<void *> visited` would stamp the marker on the second
// sighting of a merely shared node — a wrong answer that looks like a feature.
// The owner's other standard method, a "visited" flag ON each element, is not
// available: these are the user's own structs (a SMAUG CHAR_DATA), there is
// nowhere to put a flag, and a dump must never write to the data it reads.

// The pointee's declared spec list. ONE owner, because the generated function's
// PARAMETER type and the cast at its call site have to be the SAME type — and a
// silent mismatch there is an ABI bug, not a diagnostic.
bool CirBuilder::dump_pointee_specs(DataDef *base, node_t specs)
{
	if (!base || base->rawtype() == DataType::dtVOID)
		return false;
	// A struct / class pointee is its TAG reference (`struct Node *`), which is
	// also what keeps an incomplete-at-this-point type legal in a prototype.
	// is_class_object IS the shared class predicate (the same one dump_any keys
	// on). The intrinsic value carrier is deliberately NOT here: it has no
	// struct tag in the emitted C — its storage is an opaque long long[] — so it
	// takes the ordinary spec path below, which append_type_specs owns.
	if ((base->is_struct() || is_class_object(base)) && !base->is_complex()) {
		append(specs, class_tag_ref(base));
		return true;
	}
	append_type_specs(specs, base);
	return true;
}

// One parameter of a generated dumper. The spec list is consumed (c2mir op-lists
// are intrusive: a node lives in exactly one list), so the caller builds a fresh
// one per use — which is why the prototype and the definition each build their
// own parameter list from scratch.
node_t CirBuilder::dump_fn_param(node_t specs, int stars, const char *name,
				 TokenBase *origin)
{
	node_t dl = list();
	for (int i = 0; i < stars; i++)
		append(dl, pointer());
	node_t pd = simple(N_SPEC_DECL, origin);
	append(pd, node1(N_SHARE, specs));
	append(pd, node2(N_DECL, id(name, origin), dl));
	append(pd, ignore());
	append(pd, ignore());
	append(pd, ignore());
	return pd;
}

// print_r's word for the frame the *RECURSION* marker replaces. The ARM ORDER
// mirrors dump_any's, and every spelling comes from that arm's own owner
// (dump_pr_object_word, and the literal "Array" both array arms use) — so this
// cannot say "Node Object" where the walk would have said "Array".
std::string CirBuilder::dump_pr_recursion_word(DataDef *dd)
{
	DataDef *u = dd ? dd->unqualified() : NULL;
	if (!u)
		return "Array";
	DataDefCLASS *cls = is_class_object(u)
			  ? dynamic_cast<DataDefCLASS *>(u) : NULL;
	Variable *szmv = NULL, *opmv = NULL;
	if (cls && class_index_iteration_protocol(cls, szmv, opmv))
		return "Array";
	IterProtocol ip;
	if (cls && class_iterator_iteration_protocol(cls, ip, NULL, this))
		return "Array";
	if (DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(u))
		return dump_pr_object_word(sdd);
	return "Array";
}

// The generated dumper's parameter names. Fixed, because the body refers to them
// by name; `__d`-prefixed so they cannot collide with the walk's own temporaries
// (__dmp_i_N / __dmp_n_N) or with anything a user wrote.
#define DUMPFN_SINK  "__dsink"
#define DUMPFN_PTR   "__dptr"
#define DUMPFN_DEPTH "__ddepth"
#define DUMPFN_NEST  "__dnest"
#define DUMPFN_COL   "__dcol"
#define DUMPFN_R     "__dr"

// (void *sink, T *p, int depth, int nested) — built fresh per use.
node_t CirBuilder::dump_fn_param_list(DataDef *base, int stars,
				      TokenBase *origin)
{
	node_t pspec = list();
	if (!dump_pointee_specs(base, pspec))
		return NULL;
	node_t params = list();
	node_t vspec = list();
	append(vspec, simple(N_VOID, origin));
	append(params, dump_fn_param(vspec, 1, DUMPFN_SINK, origin));
	append(params, dump_fn_param(pspec, stars, DUMPFN_PTR, origin));
	node_t dspec = list();
	append(dspec, simple(N_INT, origin));
	append(params, dump_fn_param(dspec, 0, DUMPFN_DEPTH, origin));
	node_t nspec = list();
	append(nspec, simple(N_INT, origin));
	append(params, dump_fn_param(nspec, 0, DUMPFN_NEST, origin));
	return params;
}

// `int <name> = <init>;`
node_t CirBuilder::dump_int_local(const char *name, node_t init,
				  TokenBase *origin)
{
	node_t spec = list();
	append(spec, simple(N_INT, origin));
	node_t decl = simple(N_SPEC_DECL, origin);
	append(decl, node1(N_SHARE, spec));
	append(decl, node2(N_DECL, id(name, origin), list()));
	append(decl, ignore());
	append(decl, ignore());
	append(decl, init);
	return decl;
}

// The name of the generated dumper for (pointee, flavor), minted on first use.
// Empty on failure, with `why` set.
std::string CirBuilder::dump_pointer_fn(DumpFlavor fl, DataDef *pointee,
					TokenBase *origin, std::string &why)
{
	DataDef *key = pointee ? pointee->unqualified() : NULL;
	if (!key) {
		why = "unresolved pointee type";
		return std::string();
	}
	std::pair<DataDef *, int> mk(key, (int)fl);
	std::map<std::pair<DataDef *, int>, std::string>::iterator it
		= m_dump_fn_syms.find(mk);
	if (it != m_dump_fn_syms.end())
		return it->second;

	// The parameter's type: the pointee's base plus one star for THIS pointer
	// and one for every level the pointee itself carries, so `int **` yields a
	// `int **` parameter and its body dumps an `int *` through a second dumper.
	DataDef *base = key;
	int stars = 1 + dd_peel_pointers(base);

	int n = m_dump_fn_counter++;
	char nm[48];
	snprintf(nm, sizeof nm, "__madc_dumpfn_%d", n);
	std::string fname = nm;
	// The TYPE tag for the ancestor stack. Derived from the function index, so
	// it is a bijection with (pointee, flavor) inside this TU — and a whole walk
	// is generated in ONE TU with ONE flavor, which makes it a bijection with
	// the pointee TYPE for every path that can actually be walked.
	unsigned tag = MADC_DUMP_TAG_FIRST + (unsigned)n;

	// Registered BEFORE the body is built. The body may reach this same
	// (pointee, flavor) again — that IS the cyclic case — and it must find a
	// CALL to make rather than start a second expansion that never ends.
	m_dump_fn_syms[mk] = fname;
	size_t proto_mark = m_pending_top_protos.size();
	size_t def_mark = m_pending_top_defs.size();

	// The walk runs against the function's OWN sink, column base, depth and
	// nested flag, at RELATIVE depth 0 — which is what makes the pointee render
	// at the same depth as the pointer.
	std::string saved_sink = m_dump_sink_var;
	std::string saved_base = m_dump_col_base;
	std::string saved_depth = m_dump_fn_depth;
	std::string saved_nest = m_dump_fn_nested;
	std::vector<node_t> saved_pending;
	saved_pending.swap(m_pending_stmts);
	// A GENERATED FUNCTION STARTS A NEW EXPANSION PATH. The type-path set exists
	// to bound an EXPANSION, and this body is not one: the recursion here is a
	// CALL, bounded by the memo registered just above. Carrying the caller's path
	// in refused the canonical case — `php::print_r(n)` on
	// `struct Node { int v; Node *next; }`, where the top-level expansion of Node
	// is still on the path when member `next` generates Node's dumper and its body
	// expands Node again. A green suite did NOT catch that: every pointer test
	// dumps a POINTER at top level (`print_r(&a)`), so Node was never on the path
	// when the function was built. tmp/probe/p15_selfref.mad did.
	std::set<DataDef *> saved_expanding;
	saved_expanding.swap(m_dump_expanding);
	m_dump_sink_var = DUMPFN_SINK;
	m_dump_col_base = DUMPFN_COL;
	m_dump_fn_depth = DUMPFN_DEPTH;
	m_dump_fn_nested = DUMPFN_NEST;

	std::vector<node_t> walk;
	DumpAccess pacc = [this, origin]() -> node_t {
		return node1(N_DEREF, id(DUMPFN_PTR, origin), origin);
	};
	bool ok = dump_any(fl, pacc, key, NULL, 0, true, walk, origin, why);
	// Anything the walk HOISTED belongs inside this function: it references
	// these parameters, so leaving it on the caller's list would emit it in a
	// scope where those names do not exist.
	std::vector<node_t> hoisted;
	hoisted.swap(m_pending_stmts);
	m_pending_stmts.swap(saved_pending);
	m_dump_expanding.swap(saved_expanding);
	// The three context names stay SET until every arm below is built. The
	// null, cycle and out-of-memory arms are part of this function's body too,
	// so they need its column base and its `nested` parameter exactly as the
	// walk did — restoring here instead emitted the var_dump NULL at column 0
	// and dropped print_r's end-of-entry newline entirely.

	if (!ok) {
		m_dump_sink_var = saved_sink;
		m_dump_col_base = saved_base;
		m_dump_fn_depth = saved_depth;
		m_dump_fn_nested = saved_nest;
		// Rolled back completely: the memo entry, and any dumper minted
		// for a NESTED pointee while this body was being built. A half
		// generated function left behind would be emitted as dead code in
		// a TU whose dump has already been refused.
		m_dump_fn_syms.erase(mk);
		m_pending_top_protos.resize(proto_mark);
		m_pending_top_defs.resize(def_mark);
		return std::string();
	}

	// int __dcol = <step> * __ddepth;   — the base column, computed ONCE.
	// The geometry is linear in depth, so every column inside the body is this
	// plus a compile-time constant (dump_col owns that).
	std::vector<node_t> body;
	body.insert(body.end(), hoisted.begin(), hoisted.end());
	body.push_back(dump_int_local(DUMPFN_COL,
				      node2(N_MUL,
					    integer(dump_col_step(fl), origin),
					    id(DUMPFN_DEPTH, origin), origin),
				      origin));

	// int __dr = __madc_dump_anc_push(__dptr, <tag>);
	need_output_extern("__madc_dump_anc_push", false,
			   { { {N_VOID}, true },
			     { {N_UNSIGNED, N_INT}, false } }, { N_INT });
	need_output_extern("__madc_dump_anc_pop", false, {});
	node_t pargs = list();
	append(pargs, id(DUMPFN_PTR, origin));
	append(pargs, integer((int64_t)tag, origin));
	std::vector<node_t> pushed;
	pushed.push_back(dump_int_local(DUMPFN_R,
					node2(N_CALL,
					      id("__madc_dump_anc_push", origin),
					      pargs, origin),
					origin));

	// if (__dr > 0) { <walk>; pop(); } else if (__dr == 0) { <cycle> }
	//                                    else { <could not grow> }
	node_t witems = list();
	for (size_t i = 0; i < walk.size(); i++)
		append(witems, walk[i]);
	append(witems, node2(N_EXPR, list(),
			     node2(N_CALL, id("__madc_dump_anc_pop", origin),
				   list(), origin), origin));

	node_t citems = list();
	if (fl == dfVarDump) {
		need_dump_extern("__madc_dump_vd_recursion",
				 { { {N_INT}, false } });
		node_t ca = list();
		append(ca, dump_col(fl, 0, false, origin));
		append(citems, dump_call_stmt("__madc_dump_vd_recursion", ca,
					      origin));
	} else {
		// print_r puts the word of the frame it is NOT going to open on
		// the entry line, then ` *RECURSION*`. rt_dump.c owns that shape.
		need_dump_extern("__madc_dump_pr_recursion",
				 { { {N_CHAR}, true } });
		std::string word = dump_pr_recursion_word(key);
		node_t ca = list();
		append(ca, str(word.c_str(), word.size() + 1, origin));
		append(citems, dump_call_stmt("__madc_dump_pr_recursion", ca,
					      origin));
	}

	// The stack could not GROW. Reported as itself and never as a cycle: a
	// plausible *RECURSION* where the truth is "out of memory" is the silent
	// wrong answer this arc refuses.
	need_dump_extern("__madc_dump_fail", { { {N_CHAR}, true } });
	static const char kOom[] = "dump too deep (ancestor stack exhausted)";
	node_t oa = list();
	append(oa, str(kOom, sizeof kOom, origin));
	node_t oitems = list();
	append(oitems, dump_call_stmt("__madc_dump_fail", oa, origin));

	node_t inner = node4(N_IF, list(),
			     node2(N_EQ, id(DUMPFN_R, origin),
				   integer(0, origin), origin),
			     node2(N_BLOCK, list(), citems, origin),
			     node2(N_BLOCK, list(), oitems, origin), origin);
	node_t iitems = list();
	append(iitems, inner);
	pushed.push_back(node4(N_IF, list(),
			       node2(N_GT, id(DUMPFN_R, origin),
				     integer(0, origin), origin),
			       node2(N_BLOCK, list(), witems, origin),
			       node2(N_BLOCK, list(), iitems, origin), origin));

	// The null arm. print_r renders null as the EMPTY string, so all it owes is
	// the end-of-entry newline a scalar entry would have emitted — and whether
	// it owes even that is the caller's question, which is why dump_pr_end_entry
	// turns it into a test on `nested` here. var_dump prints NULL.
	node_t nitems = list();
	if (fl == dfVarDump)
		append(nitems, dump_vd_null(0, origin));
	else if (node_t nl = dump_pr_end_entry(fl, 0, origin))
		append(nitems, nl);

	node_t pitems = list();
	for (size_t i = 0; i < pushed.size(); i++)
		append(pitems, pushed[i]);
	body.push_back(node4(N_IF, list(), id(DUMPFN_PTR, origin),
			     node2(N_BLOCK, list(), pitems, origin),
			     node2(N_BLOCK, list(), nitems, origin), origin));

	// The body is complete; the enclosing context resumes here.
	m_dump_sink_var = saved_sink;
	m_dump_col_base = saved_base;
	m_dump_fn_depth = saved_depth;
	m_dump_fn_nested = saved_nest;

	node_t bitems = list();
	for (size_t i = 0; i < body.size(); i++)
		append(bitems, body[i]);

	// static void <fname>(void *sink, T *p, int depth, int nested)
	//
	// STATIC, and therefore NOT declared through need_output_extern: an extern
	// prototype ahead of a static definition is a storage-class conflict. Static
	// is also what makes the per-TU name safe — two TUs that both dump a `Node *`
	// each get their own copy, and neither is a duplicate symbol at link.
	node_t proto_params = dump_fn_param_list(base, stars, origin);
	node_t def_params = dump_fn_param_list(base, stars, origin);
	if (!proto_params || !def_params) {
		why = std::string("no dumper for a pointer to '")
		    + (key->name.empty() ? std::string("?") : key->name)
		    + "' yet";
		m_dump_fn_syms.erase(mk);
		m_pending_top_protos.resize(proto_mark);
		m_pending_top_defs.resize(def_mark);
		return std::string();
	}

	node_t pspecs = list();
	append(pspecs, simple(N_STATIC, origin));
	append(pspecs, simple(N_VOID, origin));
	node_t proto = simple(N_SPEC_DECL, origin);
	append(proto, node1(N_SHARE, pspecs));
	append(proto, node2(N_DECL, id(fname.c_str(), origin),
			    node1(N_LIST, node1(N_FUNC, proto_params))));
	append(proto, ignore());
	append(proto, ignore());
	append(proto, ignore());

	node_t dspecs = list();
	append(dspecs, simple(N_STATIC, origin));
	append(dspecs, simple(N_VOID, origin));
	node_t def = node4(N_FUNC_DEF, dspecs,
			   node2(N_DECL, id(fname.c_str(), origin),
				 node1(N_LIST, node1(N_FUNC, def_params))),
			   list(),
			   node2(N_BLOCK, list(), bitems, origin), origin);

	m_pending_top_protos.push_back(proto);
	m_pending_top_defs.push_back(def);
	return fname;
}

// The call site: one call, and the pointee is rendered at THIS depth.
bool CirBuilder::dump_pointer(DumpFlavor fl, const DumpAccess &acc, DataDef *dd,
			      int depth, bool nested, std::vector<node_t> &out,
			      TokenBase *origin, std::string &why)
{
	DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(dd->unqualified());
	DataDef *pointee = pdd ? pdd->base_type : NULL;
	// Refused by name, never guessed at. A void pointer has no pointee to
	// render; a function pointer and a pointer-to-member are addresses rather
	// than handles on a value.
	if (!pointee || pointee->rawtype() == DataType::dtVOID
	    || pointee->is_function() || dd->is_member_pointer()) {
		why = std::string("no dumper for type '") + dd->name + "' yet";
		return false;
	}
	std::string fn = dump_pointer_fn(fl, pointee, origin, why);
	if (fn.empty()) {
		if (why.empty())
			why = std::string("no dumper for type '") + dd->name
			    + "' yet";
		return false;
	}

	// The pointer is CAST to the parameter's exact type. The pointee's own
	// qualifiers were peeled to name the type once (dump_pointee_specs), so a
	// `const Node *` argument reaches a `struct Node *` parameter — the cast
	// says so rather than leaving c2mir to accept a mismatch quietly.
	DataDef *base = pointee->unqualified();
	int stars = 1 + dd_peel_pointers(base);
	node_t cspecs = list();
	if (!dump_pointee_specs(base, cspecs)) {
		why = std::string("no dumper for type '") + dd->name + "' yet";
		return false;
	}
	node_t cdecl = list();
	for (int i = 0; i < stars; i++)
		append(cdecl, pointer());
	node_t ctype = node2(N_TYPE, cspecs, node2(N_DECL, ignore(), cdecl));

	node_t a = list();
	append(a, node2(N_CAST, ctype, acc(), origin));
	append(a, dump_depth_arg(depth, origin));
	append(a, dump_nested_arg(depth, nested, origin));
	// dump_call_stmt prepends the sink, exactly as it does for a primitive —
	// so the SAME generated function serves a printing dump and a capturing
	// one, and the memo does not have to be keyed on the sink.
	out.push_back(dump_call_stmt(fn.c_str(), a, origin));
	return true;
}

// ---------------------------------------------------------------------------
// A madc::value — the one type the compiler CANNOT expand
// ---------------------------------------------------------------------------
// Every other shape in this file is walked at compile time because the call site
// knows T. A value's KIND is a property of the VALUE, not of the type: the same
// `value v` holds an integer here and an array of maps there, so there is
// nothing to expand and no member census to take. One call to the runtime walk
// (src/rt_dump_value.cpp) covers all nine kinds, arbitrary nesting and PHP's
// *RECURSION* marker — which is why the owner is right that this is the EASIEST
// of the remaining shapes and not the hardest.
//
// The sink rides in front as it does for every primitive (dump_call_stmt
// prepends it), so print_r($v, true) captures a value dump with no extra work.
// `depth` and `nested` are compile-time here and become runtime parameters
// there: the value may sit inside a struct that the generated walk framed.
bool CirBuilder::dump_value(DumpFlavor fl, const DumpAccess &acc, int depth,
			    bool nested, std::vector<node_t> &out,
			    TokenBase *origin)
{
	need_dump_extern("__madc_dump_value",
			 { { {N_VOID}, true },  // const madc::value *
			   { {N_INT}, false },  // flavor
			   { {N_INT}, false },  // depth
			   { {N_INT}, false } });  // nested
	node_t a = list();
	append(a, node1(N_ADDR, acc(), origin));
	append(a, integer(dump_wire_flavor(fl), origin));
	append(a, dump_depth_arg(depth, origin));
	append(a, dump_nested_arg(depth, nested, origin));
	out.push_back(dump_call_stmt("__madc_dump_value", a, origin));
	return true;
}

// The one dispatch: array framing, then aggregate framing, then scalar.
//
// `dims` IS the array-ness: NULL (or empty) means this is not a fixed-extent
// array, and otherwise it is the whole dim chain. One parameter rather than the
// old count+flag pair, because the two could disagree — and did, for a
// multidimensional member, where the count was the FLATTENED total.
bool CirBuilder::dump_any(DumpFlavor fl, const DumpAccess &acc, DataDef *dd,
			  const std::vector<carray_dim_t> *dims, int depth,
			  bool nested, std::vector<node_t> &out,
			  TokenBase *origin, std::string &why)
{
	if (!dd) {
		why = "unresolved type";
		return false;
	}
	if (dims && !dims->empty())
		return dump_array(fl, acc, dd, *dims, 0, depth, nested, out,
				  origin, why);
	// An AGGREGATE already on the expansion path is refused by name. The walk is
	// EXPANDED per nesting level, so a type that contains itself — legally, via a
	// container whose element type is the container (`Node &operator[](long)`) —
	// has no bound: it consumed 4 GB and died in MADC_MEM_LIMIT with
	// `std::bad_alloc`, which says nothing about what happened. See
	// m_dump_expanding in the header for why the pointer path is exempt.
	DataDef *path_key = dd->unqualified();
	bool guarded = path_key && !dd->is_pointer() && !dd->is_reference()
		    && dynamic_cast<DataDefSTRUCT *>(path_key) != NULL;
	if (guarded && m_dump_expanding.count(path_key)) {
		why = std::string("no dumper for '") + dump_type_word(dd)
		    + "' yet: it contains ITSELF (a container whose element type is"
		      " the container), so expanding it has no bound. A"
		      " self-referential structure reached through a POINTER is"
		      " followed, with a runtime cycle check";
		return false;
	}
	// RAII, because every arm below has its own `return`.
	struct PathFrame {
		std::set<DataDef *> *set;
		DataDef *key;
		PathFrame(std::set<DataDef *> *s, DataDef *k) : set(s), key(k)
		{
			if (set) set->insert(key);
		}
		~PathFrame() { if (set) set->erase(key); }
	private:
		PathFrame(const PathFrame &);
		PathFrame &operator=(const PathFrame &);
	} frame(guarded ? &m_dump_expanding : NULL, path_key);
	// A DataDefSTRUCT is the ONE aggregate-layout owner, and a POD struct is
	// only PROMOTED to DataDefCLASS when it earns class-hood — so the walk
	// keys on DataDefSTRUCT, never on DataDefCLASS. A pointer or reference
	// whose pointee is a struct is NOT this case: following it is the pointer
	// slice, and dump_scalar refuses it by name until then.
	if (!dd->is_pointer() && !dd->is_reference()) {
		// The intrinsic value carrier (madc::value == ddARRAY) goes to the
		// RUNTIME walk, and must be taken before either branch below: it is
		// a class WITH registered methods (its c_str is madarray_cstr), so a
		// structural sequence test would pass and then call through the
		// wrong runtime, and it is a DataDefSTRUCT with no madc members, so
		// the member walk would print an empty aggregate.
		if (is_array_object(dd))
			return dump_value(fl, acc, depth, nested, out, origin);
		// A class that is a POSITIONAL SEQUENCE renders as one (an array,
		// or text when its element is a character) rather than as its
		// members: a std::vector<int>'s private pointers are not what a PHP
		// developer asked to see. Tried BEFORE the member walk, and only a
		// structural test decides it.
		// is_class_object IS the shared predicate (as_class_instance behind
		// it, which already excludes ddARRAY by rawtype); the cast only
		// recovers the pointer it validated, so no condition is duplicated.
		DataDefCLASS *ccls = is_class_object(dd)
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
			//
			// A CONTAINER's members are not real in that sense — they
			// are the library's own internals — so one is refused by
			// its own name here rather than walked.
			// A container with no POSITION but with C++'s ITERATOR
			// protocol (std::map, std::set, std::list): a generated
			// counted begin()/operator++ loop. Same local-first
			// discipline as the sequence arm — a partial expansion
			// must not reach `out`.
			std::vector<node_t> itr;
			std::string itr_why;
			if (dump_iterator(fl, acc, ccls, depth, nested, itr,
					  origin, itr_why)) {
				out.insert(out.end(), itr.begin(), itr.end());
				return true;
			}
			if (container_needs_iterator_walk(ccls)) {
				why = std::string("no dumper for container '")
				    + dump_class_type_word(ccls) + "' yet: "
				    + (itr_why.empty()
					 ? std::string("it has neither the "
						"positional size()/operator[] "
						"protocol nor a reachable "
						"iterator protocol")
					 : itr_why);
				return false;
			}
		}
		if (DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(dd))
			return dump_struct(fl, acc, sdd, depth, nested, out,
					   origin, why);
		// An enum is a NAME with a backing value, which is a frame under
		// print_r and one line under var_dump — not a scalar, so it is
		// dispatched here rather than in dump_scalar. Tried after the
		// aggregate arms and before the scalar tail: a DataDefENUM's rawtype
		// is dtINT, so the integer arm would otherwise print the number.
		if (DataDefENUM *edd
			    = dynamic_cast<DataDefENUM *>(dd->unqualified()))
			return dump_enum(fl, acc, edd, depth, nested, out,
					 origin, why);
	}
	// A pointer that is not TEXT is FOLLOWED, at this same depth. is_cstr() is
	// a char pointer — PHP's string — and belongs to the scalar arm. A REFERENCE
	// stays there too: madc renders one as a pointer, but it cannot be null, it
	// is not an indirection the source wrote, and the walk already reaches
	// through it wherever it dumps one.
	if (dd->is_pointer() && !dd->is_cstr() && !dd->is_reference())
		return dump_pointer(fl, acc, dd, depth, nested, out, origin, why);
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

	// A fixed-extent array VARIABLE carries its shape on the Variable, not in
	// its DataDef (a struct MEMBER carries it in member_dims instead) — the
	// same place translate_foreach_carray reads it. The DIM CHAIN, never
	// total_elements(): that flattens every dimension, and `a[i]` on a
	// multi-dim array yields a ROW, so a flat walk would read past the first
	// row.
	std::vector<carray_dim_t> adims;
	if (TokenVar *tv = dynamic_cast<TokenVar *>(arg))
		if (tv->var.is_fixed_array() && !tv->var.is_vla()
		    && tv->var.total_elements() > 0) {
			adims = tv->var.dims;
			if (adims.empty())
				adims.push_back((carray_dim_t)
						tv->var.total_elements());
		}
	bool is_arr = !adims.empty();

	// The walk rebuilds the access once PER MEMBER, so an aggregate argument
	// must be re-evaluable without side effects. A named variable or a member
	// selection is; a struct-returning CALL is not (it would run once per
	// member). ttVariable / ttMember are the discriminators — every call token
	// also derives from TokenVar, so a dynamic_cast alone would admit one.
	bool simple_lvalue = arg->type() == TokenType::ttVariable
			  || arg->type() == TokenType::ttMember;
	// A POINTER argument does NOT need this: it is passed to the generated
	// dumper ONCE, by value, and every rebuild of the access happens inside that
	// function against its own parameter. `php::print_r(list_head())` is
	// therefore fine where `php::print_r(make_point())` is not.
	if ((is_arr || dynamic_cast<DataDefSTRUCT *>(dd) != NULL) && !simple_lvalue) {
		why = "an aggregate argument must be a variable or member (a"
		      " temporary has no address to walk, and would be"
		      " re-evaluated once per field)";
		return false;
	}

	DumpAccess acc = [this, arg]() -> node_t { return translate_expr(arg); };
	return dump_any(fl, acc, dd, is_arr ? &adims : NULL, 0, false, out,
			origin, why);
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
