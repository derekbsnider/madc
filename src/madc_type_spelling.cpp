/* madc_type_spelling.cpp — the source's spelling of a type (plan §41.8a).
 *
 * Moved whole from cir_dump.cpp, where D10's show and var_dump grew it, so
 * the session's `%type` reads the same words (include/madc_type_spelling.h).
 * CirBuilder's words forward here. The one change: the alias walk reads the
 * datatype maps read-only, as a query inside an entry transaction must.
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <list>
#include <queue>
#include <iostream>
#include <sstream>
#include <memory>
#include <stdint.h>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "cir_builder.h"	// dd_peel_pointers
#include "madc_type_spelling.h"

// var_dump's one deliberate divergence from PHP: it names the REAL type. The
// spelling comes from the type's own DataType, so it is the CANONICAL type and
// not the source's typedef — the same thing g++'s typeid reports. `long long`
// and `long` share dtINT64 in madc and therefore share the word "long".
std::string TypeSpeller::scalar_word(DataDef *dd)
{
	if (!dd)
		return "?";
	if (dd->is_cstr())
		return "char *";
	// The LLP64 platform `long` / `unsigned long` are DISTINCT 4-byte
	// singleton TYPES (task #46b) whose identity survives to here, and
	// their word is their own source name — `long l = 42` dumps long(42)
	// on every target, never the rank's "int". LP64 never instantiates
	// these classes, so this cannot rewrite an LP64 word.
	DataDef *u = dd->unqualified();
	if (dynamic_cast<DataDefPlatformLONG *>(u)
	    || dynamic_cast<DataDefPlatformULONG *>(u))
		return u->name;
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
std::string TypeSpeller::alias(DataDef *dd) const
{
	if (!pgm || !dd)
		return std::string();
	std::string best;
	pgm->namespace_datatype_map.for_each_readonly(
		[&](const char *ns, const datatype_map_t &m) -> bool {
			for (datatype_map_t::const_iterator it = m.begin(); it != m.end(); ++it) {
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
std::string TypeSpeller::class_word(DataDefCLASS *cls) const
{
	if (cls->union_layout)
		return "union " + cls->name;
	const std::string &canon = cls->canonical_cpp_spelling();
	if (canon.find('<') != std::string::npos) {
		std::string named = alias(cls);
		if (!named.empty())
			return strip_inline_namespaces(named);
	}
	if (!canon.empty())
		return strip_inline_namespaces(canon);
	return "struct " + cls->name;
}

// D10's show (plan §41.4a) spells a pointer or enum TYPE the way the entry's
// language writes it, so `(TYPE) value` re-enters: `enum E` and `struct P *` in
// C, `E` and `P *` in C++. A function pointer is its structural spelling
// (`int (*)(int)`, fptr_structural_spelling). cv is the one rule's
// (cv_qualified_spelling): the base's before it, each inner pointer's after
// its `*`. The outermost pointer's own cv is not part of a value's type.
// A function type's parameter types, as the entry's language writes the
// list: `int, ...`; an empty list is `void` in C, empty in C++. With the
// function's Method, each parameter carries its name (`int n`). The list is
// the one the source wrote: a member function's receiver slot (`__this`,
// which only its name tells from a real parameter) and the varargs slot
// (past fixed_param_count) are madc's, not the source's.
std::string TypeSpeller::parameter_list(FuncDef *fd, const Method *m) const
{
	const bool cxx = pgm && pgm->is_cpp_mode();
	size_t first = 0;
	if (m && !m->parameters.empty() && m->parameters[0]
	    && m->parameters[0]->name == "__this")
		first = 1;
	std::string params;
	for (size_t i = first; i < fd->fixed_param_count(); ++i) {
		const Variable *p = m && i < m->parameters.size()
				  ? m->parameters[i] : NULL;
		params += (i > first ? ", " : "")
			+ declared(fd->parameters[i], p ? p->name : std::string());
	}
	if (fd->is_varargs)
		params += params.empty() ? "..." : ", ...";
	else if (params.empty() && !cxx)
		params = "void";
	return params;
}

// A declaration's spelling: the type with its declarator-id where C writes
// it, inside the declarator (`int a[3]`, `int (*fp)(int)`, `char *s`); an
// empty name is the type's abstract spelling, as shown() writes it.
std::string TypeSpeller::declared(DataDef *dd, const std::string &name) const
{
	if (dd) {
		if (FuncDef *fd = dd->as_funcdef_dd())
			return declared(&fd->returns,
					name + "(" + parameter_list(fd, NULL) + ")");
		if (DataDefCArray *a = dd->unqualified()->as_carray_dd()) {
			std::string dims;
			DataDef *elem = a;
			for (DataDefCArray *c; elem
			     && (c = elem->unqualified()->as_carray_dd()) != NULL;
			     elem = c->element_type)
				dims += c->count_expr || !c->count
				      ? std::string("[]")
				      : "[" + std::to_string(c->count) + "]";
			return declared(elem, name + dims);
		}
		// A function pointer: the name goes with its stars, inside.
		int extra = 0;
		DataDef *b = dd;
		for (DataDefPTR *pp; b && !b->as_fptr_dd()
				     && (pp = b->as_pointer_dd()) != NULL;
		     b = pp->base_type)
			++extra;
		DataDefFPTR *fp = b ? b->as_fptr_dd() : NULL;
		if (fp && fp->target) {
			int stars = extra + (fp->ptr_syntax ? 1 : 0);
			return declared(&fp->target->returns,
					"(" + std::string(stars ? stars : 1, '*') + name
					+ ")(" + parameter_list(fp->target, NULL) + ")");
		}
	}
	std::string word = shown(dd);
	if (name.empty())
		return word;
	const char last = word[word.size() - 1];
	return word + (last == '*' || last == '&' ? "" : " ") + name;
}

std::string TypeSpeller::signature(const std::string &name, FuncDef *fd,
				   const Method *m) const
{
	// A member function's cv-qualifier-seq and ref-qualifier follow its
	// parameters ([dcl.fct]/1): the cv rule's after-the-operand form.
	std::string d = cv_qualified_spelling(name + "(" + parameter_list(fd, m) + ")",
					      fd->method_cv(), true);
	if (fd->ref_qualifier)
		d += fd->ref_qualifier == 1 ? " &" : " &&";
	return declared(&fd->returns, d);
}

std::string TypeSpeller::shown(DataDef *dd) const
{
	if (!dd)
		return "void *";
	const bool cxx = pgm && pgm->is_cpp_mode();
	// A reference (a function's return, a parameter, a member): its
	// referent, then `&`. madc lowers T& as T *, which the arms below
	// would spell; a value's type (the show's) is never one.
	if (DataDefREF *r = dd->as_reference_dd()) {
		const std::string w = shown(r->base_type);
		return w + (w[w.size() - 1] == '*' ? "&" : " &");
	}
	// A function's own type (a designator's, which %type asks for, never
	// the show): `int (int)`.
	if (FuncDef *fd = dd->as_funcdef_dd())
		return shown(&fd->returns) + " (" + parameter_list(fd, NULL) + ")";
	// An array (%type's too): its element's word, then each extent,
	// outermost first, `int [2][3]`; one no constant sizes is `[]`.
	if (DataDefCArray *a = dd->unqualified()->as_carray_dd()) {
		std::string dims;
		DataDef *elem = a;
		for (DataDefCArray *c; elem
		     && (c = elem->unqualified()->as_carray_dd()) != NULL;
		     elem = c->element_type)
			dims += c->count_expr || !c->count
			      ? std::string("[]")
			      : "[" + std::to_string(c->count) + "]";
		return shown(elem) + " " + dims;
	}
	// A function pointer, through any pointer layers above it, spelled from
	// its target's signature: `int (*)(void)`. (fptr_structural_spelling is
	// the MANGLER's spelling, `int32_t (*)()`, which C does not read back.)
	{
		int extra = 0;
		DataDef *b = dd;
		for (DataDefPTR *pp; b && !b->as_fptr_dd()
				     && (pp = b->as_pointer_dd()) != NULL;
		     b = pp->base_type)
			++extra;
		DataDefFPTR *fp = b ? b->as_fptr_dd() : NULL;
		if (fp && fp->target) {
			FuncDef *fd = fp->target;
			int stars = extra + (fp->ptr_syntax ? 1 : 0);
			return shown(&fd->returns) + " ("
			     + std::string(stars ? stars : 1, '*') + ")("
			     + parameter_list(fd, NULL) + ")";
		}
	}
	std::vector<unsigned> level_cv;
	DataDef *base = dd;
	int levels = dd_peel_pointers(base, &level_cv);
	DataDef *ub = base ? base->unqualified() : NULL;
	std::string word;
	if (!ub || ub->is_void())
		word = "void";
	else if (DataDefENUM *e = dynamic_cast<DataDefENUM *>(ub)) {
		const std::string &canon = e->canonical_cpp_spelling();
		word = cxx ? (canon.empty() ? e->name : canon) : "enum " + e->name;
	} else if (DataDefCLASS *c = dynamic_cast<DataDefCLASS *>(ub)) {
		// The carrier is `var` in the madc dialect (value-first), and
		// its canonical class elsewhere.
		if (c->is_madc_array() && pgm && pgm->language_std == Program::STD_MADC)
			word = "var";
		else if (cxx) {
			// dump_class_type_word falls back to C's `struct P` /
			// `union U` for a class with no canonical spelling; C++
			// names the class alone.
			word = class_word(c);
			static const char *const tags[] = { "struct ", "union " };
			for (size_t t = 0; t < 2; t++)
				if (word.compare(0, strlen(tags[t]), tags[t]) == 0) {
					word = word.substr(strlen(tags[t]));
					break;
				}
		} else
			word = (c->union_layout ? "union " : "struct ") + c->name;
	}
	else if (DataDefSTRUCT *s = dynamic_cast<DataDefSTRUCT *>(ub))
		word = cxx ? s->name
			   : (s->union_layout ? "union " : "struct ") + s->name;
	else
		word = scalar_word(ub);
	if (!level_cv.empty())
		word = cv_qualified_spelling(word, level_cv.back(), false);
	for (int i = levels - 1; i >= 0; --i) {
		word += " *";
		if (i > 0 && i < (int)level_cv.size())
			word = cv_qualified_spelling(word, level_cv[i], true);
	}
	return word;
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
std::string TypeSpeller::strip_inline_namespaces(const std::string &spelling) const
{
	if (!pgm)
		return spelling;
	std::string out = spelling;
	bool again = true;
	while (again) {
		again = false;
		std::map<std::string, std::vector<std::string> >::const_iterator it;
		for (it = pgm->inline_namespace_children.begin();
		     it != pgm->inline_namespace_children.end(); ++it) {
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
