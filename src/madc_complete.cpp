/* madc_complete.cpp — the completion service (plan §41.7a, slices 3 and 4).
 *
 * Tab's question: which names complete the word before the caret. The core
 * answers it for the session's top level; the line editor only renders the
 * answer (D23). Two halves:
 *   - where the word stands (completion_context): the text before the word
 *     is lexed with a probe identifier in the word's place, as an attempt
 *     still being typed, inside an entry transaction that rolls back. The
 *     real lexer decides whether the caret is in code (a string or a comment
 *     swallows the probe), and the tokens before the probe give the context;
 *   - the names (completion_names): the registries are WALKED, never looked
 *     up, since a lookup materializes forest declarations, registers dlsym
 *     symbols and throws (plan §8's code check). After `.` or `->` the chain
 *     steps through its types from a session name (completion_members);
 *     after `::` the scope's registries are read (completion_scope_names),
 *     with the attempt parsed first, since a module's namespace fills only
 *     when its fragment is parsed.
 *
 * Thread contract: a query runs on the session's thread between entries,
 * as every other session verb does (D9); it changes nothing it does not
 * roll back.
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
#include <fstream>
#include <memory>
#include <stdint.h>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madcdis/text_buffer.h"	// the one word rule (word_byte)


// The probe stands where the word is. It can be no macro and no declared
// name: the session reserves the __madc_ prefix.
static const char completion_probe[] = "__madc_completion_probe";
static const char completion_display[] = "<completion>";

std::vector<std::string> Program::complete_entry(const std::string &text,
						 size_t caret, size_t &start)
{
    std::vector<std::string> out;
    if ( caret > text.size() )
	caret = text.size();
    size_t s = caret;
    // The word is the word rule's run before the caret, as the editor's
    // word motion reads one.
    while ( s > 0 && madc::hub::text_buffer::word_byte(text[s - 1]) )
	--s;
    start = s;
    const std::string word = text.substr(s, caret - s);
    // A number is not a name.
    if ( !word.empty() && word[0] >= '0' && word[0] <= '9' )
	return out;
    const std::string before = text.substr(0, s);
    // The attempt: lexed (and for a qualified name parsed) as an entry still
    // being typed, inside transactions that roll back, so a query leaves
    // nothing. Its diagnostics and tokens go with it, however it ends, as an
    // offered attempt's do at the next one.
    struct AttemptEnd
    {
	Program &pgm;
	~AttemptEnd() { pgm.begin_entry(); }
    } attempt_end = { *this };
    ParseModeScope mode(*this, ParseMode::InteractiveEntry);
    DiagnosticRenderMute mute;
    std::vector<std::string> chain;
    std::vector<bool> arrows;
    CompletionContext ctx;
    {
	// The context's lex rolls back before anything else runs: what it
	// included (an auto-included fragment) is marked included, and a
	// second lex in the same transaction would not include it again.
	EntryTransaction lexed(*this);
	ctx = completion_context(before, chain, arrows);
    }
    switch ( ctx )
    {
	case CompletionContext::Qualified:
	{
	    // A module's namespace fills when its fragment is PARSED (the
	    // auto-include scan only pulls it in at lex time): parse the
	    // attempt, which stops at the probe, and read the scope before its
	    // transaction rolls it back.
	    EntryTransaction parsed(*this);
	    parse_entry(before + completion_probe, completion_display);
	    std::string scope;
	    for ( size_t i = 0; i < chain.size(); ++i )
		scope += (i ? "::" : "") + chain[i];
	    completion_scope_names(scope, word, out);
	    break;
	}
	case CompletionContext::Member:
	    // The chain's root is a name the session already holds: nothing of
	    // the attempt is needed.
	    completion_members(chain, arrows, word, out);
	    break;
	case CompletionContext::Name:
	case CompletionContext::Tag:
	    // An empty word completes nothing here (Julia); after `.` or `::`
	    // it lists every member.
	    if ( !word.empty() )
		completion_names(word, ctx, out);
	    break;
	case CompletionContext::None:
	    break;
    }
    return out;
}

Program::CompletionContext Program::completion_context(const std::string &before,
						     std::vector<std::string> &chain,
						     std::vector<bool> &arrows)
{
    CompletionContext ctx = CompletionContext::None;
    chain.clear();
    arrows.clear();
    begin_entry();
    if ( !lex_entry(before + completion_probe, completion_display) )
	return ctx;		// the word is in an unclosed string or comment
    const char *fname = intern_file(completion_display);
    std::vector<TokenBase *> toks;		// the attempt's own, in order
    for ( size_t i = 0; i < tokens.size(); ++i )
    {
	TokenBase *t = tokens[i];
	if ( t && t->file && strcmp(t->file, fname) == 0
	     && t->id() != TokenID::tkEndOfEntry )
	    toks.push_back(t);
    }
    // A comment or a directive swallowed the probe: no name is written there.
    if ( toks.empty() || toks.back()->type() != TokenType::ttIdentifier
	 || !((TokenIdent *)toks.back())->spelling_is(completion_probe) )
	return ctx;
    ctx = CompletionContext::Name;
    if ( toks.size() < 2 )
	return ctx;
    TokenBase *prev = toks[toks.size() - 2];
    switch ( prev->id() )
    {
	case TokenID::tkDot:
	case TokenID::tkDeRef:
	case TokenID::tkNS:
	{
	    // The chain back to its root: names joined by the same kind of
	    // join (`.` / `->` for an object, `::` for a scope). Anything else
	    // in it (a call, a subscript) is an expression, which completes
	    // nothing yet.
	    const bool scope = prev->id() == TokenID::tkNS;
	    size_t i = toks.size() - 1;		// the probe
	    for (;;)
	    {
		TokenBase *join = i >= 1 ? toks[i - 1] : NULL;
		bool is_join = join && (scope ? join->id() == TokenID::tkNS
					      : (join->id() == TokenID::tkDot
						 || join->id() == TokenID::tkDeRef));
		if ( !is_join )
		    break;
		TokenBase *name = i >= 2 ? toks[i - 2] : NULL;
		if ( !name || name->type() != TokenType::ttIdentifier )
		{
		    // `::x` names the global scope: a top-level name.
		    if ( scope && chain.empty() && !name )
			return ctx;
		    return CompletionContext::None;
		}
		chain.insert(chain.begin(), ((TokenIdent *)name)->spelling());
		arrows.insert(arrows.begin(), join->id() == TokenID::tkDeRef);
		i -= 2;
	    }
	    return scope ? CompletionContext::Qualified : CompletionContext::Member;
	}
	case TokenID::tkSTRUCT:
	case TokenID::tkUNION:
	case TokenID::tkENUM:
	    return CompletionContext::Tag;
	case TokenID::tkHash:
	    // A directive's name (`#inc`), or a macro body's `#` operand: no
	    // session name is written there.
	    return CompletionContext::None;
	default:
	    return ctx;
    }
}

// A name the implementation reserves (C11 7.1.3, C++ [lex.name]/3): one
// that begins with `_` or holds `__`. madc's own lowering spells every class
// member and instantiation it registers as a global that way
// (`allocator_char__operator=`, `Box__take__o2`).
static bool reserved_name(const std::string &n)
{
    return (!n.empty() && n[0] == '_') || n.find("__") != std::string::npos;
}

// The namespace a canonical C++ spelling names its entity in ("" for the
// global one).
static std::string spelling_namespace(const std::string &spelling)
{
    size_t at = spelling.rfind("::");
    return at == std::string::npos ? std::string() : spelling.substr(0, at);
}

// Is a registered type an instantiation (`allocator<char>`, which madc
// registers as `allocator_char`)? No one wrote its registered name.
static bool instantiation_type(const DataDef &dd)
{
    return dd.canonical_cpp_spelling().find('<') != std::string::npos;
}

// The name a qualified spelling ends in (`vector` for `std::vector`).
static std::string spelling_leaf(const std::string &spelling)
{
    size_t at = spelling.rfind("::");
    return at == std::string::npos ? spelling : spelling.substr(at + 2);
}

// The name rule every completion shares: the candidates that start with the
// word and are written as identifiers, sorted, each once.
class CompletionOffer
{
public:
    explicit CompletionOffer(const std::string &word)
	: word(word), reserved_word(reserved_name(word)) {}
    void operator()(const std::string &n)
    {
	if ( n.size() < word.size() || n.compare(0, word.size(), word) != 0 )
	    return;
	// A name is written as an identifier: a qualified key, an instantiation
	// or an operator is not one.
	for ( size_t i = 0; i < n.size(); ++i )
	    if ( !madc::hub::text_buffer::word_byte(n[i]) )
		return;
	// The session's own names never.
	if ( n.compare(0, 7, "__madc_") == 0 )
	    return;
	// A reserved name completes only a word shaped like one (IPython's
	// rule for `_`, and C++'s reservation of `__`), so the implementation's
	// names and madc's lowered ones stay out of the way.
	if ( !reserved_word && reserved_name(n) )
	    return;
	names.insert(n);
    }
    void take(std::vector<std::string> &out) const
    {
	out.assign(names.begin(), names.end());
    }
private:
    const std::string &word;
    const bool reserved_word;
    std::set<std::string> names;
};

void Program::completion_names(const std::string &word, CompletionContext ctx,
			       std::vector<std::string> &out)
{
    CompletionOffer offer(word);
    // After struct / union / enum: a tag.
    if ( ctx == CompletionContext::Tag )
    {
	for ( datadef_map_citer it = struct_map.begin(); it != struct_map.end(); ++it )
	    if ( it->second && spelling_namespace(it->second->canonical_cpp_spelling()).empty() )
		offer(it->first);
	offer.take(out);
	return;
    }
    // The namespaces whose members an unqualified name reaches at the top
    // level: those a using-directive names, less the implementation's own
    // (libstdc++'s `std::__debug`, recorded without its scope), and in the
    // madc dialect std, whose names dialect code writes bare (value-first).
    // In C++ nothing else: g++ refuses a bare `vector` (BUGS.md B52).
    std::set<std::string> visible;
    visible.insert(std::string());
    for ( size_t i = 0; i < active_using_namespaces.size(); ++i )
	if ( !reserved_name(active_using_namespaces[i]) )
	    visible.insert(active_using_namespaces[i]);
    if ( language_std == STD_MADC )
	visible.insert("std");
    // Every namespace's members, by identity: madc registers a namespace's
    // objects and functions as globals too, so the global scope alone cannot
    // say whose a Variable is.
    std::map<const Variable *, const std::string *> member_of;
    for ( namespace_map_t::iterator ns = namespace_map.begin();
	  ns != namespace_map.end(); ++ns )
	for ( variable_map_iter m = ns->second.begin(); m != ns->second.end(); ++m )
	    if ( m->second )
		member_of.insert(std::make_pair(m->second, &ns->first));
    auto reachable = [&](const Variable *v) {
	std::map<const Variable *, const std::string *>::const_iterator it =
	    member_of.find(v);
	return it == member_of.end() || visible.count(*it->second);
    };
    auto type_reachable = [&](const DataDef *dd) {
	if ( !dd )
	    return false;
	return !instantiation_type(*dd)
	    && visible.count(spelling_namespace(dd->canonical_cpp_spelling()));
    };
    // Objects and functions: every entry's and every included header's; an
    // instantiation's products are no name anyone wrote.
    if ( tkProgram )
	for ( size_t i = 0; i < tkProgram->variables.size(); ++i )
	    if ( Variable *v = tkProgram->variables[i] )
		if ( !(v->flags & vfINSTPRODUCT) && reachable(v) )
		    offer(v->name);
    for ( funcdef_map_iter it = funcdef_map.begin(); it != funcdef_map.end(); ++it )
    {
	FuncDef *fd = it->second;
	// A class method completes after its object (slice 4).
	if ( !fd || !fd->method_display_name.empty()
	     || !visible.count(fd->namespace_name) )
	    continue;
	offer(fd->function_display_name.empty() ? it->first
						: fd->function_display_name);
    }
    // Types, and in C++ and madc a class's name, which is a type name there.
    datatype_map.for_each_readonly([&](const char *key, TokenDataType *const &tdt) -> bool {
	if ( !tdt || type_reachable(&tdt->definition) )
	    offer(key);
	return false;
    });
    if ( presents_as_cpp() )
    {
	for ( datadef_map_citer it = struct_map.begin(); it != struct_map.end(); ++it )
	    if ( type_reachable(it->second) )
		offer(it->first);
	// Top-level namespaces (a scoped enum is registered as one, and its
	// name is a type name).
	for ( namespace_map_t::const_iterator it = namespace_map.begin();
	      it != namespace_map.end(); ++it )
	    offer(it->first);
	template_map.for_each_readonly([&](const char *, const template_registry_entry_t &r) -> bool {
	    for ( size_t i = 0; i < r.namespace_variants.size(); ++i )
		if ( visible.count(r.namespace_variants[i].defining_namespace) )
		    offer(r.namespace_variants[i].class_name);
	    return false;
	});
	fn_template_map.for_each_readonly([&](const char *key, const std::vector<FnTemplateDef> &defs) -> bool {
	    for ( size_t i = 0; i < defs.size(); ++i )
		if ( visible.count(defs[i].ns) )
		{
		    offer(key);
		    break;
		}
	    return false;
	});
    }
    // The standard's keywords (keyword_map is gated by --std=), and macros.
    keyword_map.for_each_readonly([&](const char *key, TokenKeyword *const &) -> bool {
	offer(key);
	return false;
    });
    define_map.for_each_readonly([&](const char *key, const std::string &) -> bool {
	offer(key);
	return false;
    });
    macro_map.for_each_readonly([&](const char *key, const MacroDef &) -> bool {
	offer(key);
	return false;
    });
    // Names an included header registers on first use, and in the madc
    // dialect the words the auto-include scan serves (`println`, `php`).
    for ( std::map<std::string, LazyEntry>::const_iterator it = lazy_map.begin();
	  it != lazy_map.end(); ++it )
	offer(it->first);
    std::vector<std::string> words;
    auto_include_words(words);
    for ( size_t i = 0; i < words.size(); ++i )
	offer(words[i]);
    // The result names (D12), once a value is kept.
    size_t kept = 0;
    for ( size_t i = 0; i < session_results.size(); ++i )
	if ( session_results[i].object )
	{
	    ++kept;
	    offer("_" + std::to_string(session_results[i].entry));
	}
    if ( kept )
    {
	offer("ans");
	offer("_");
    }
    if ( kept > 1 )
	offer("__");
    if ( kept > 2 )
	offer("___");
    offer.take(out);
}

// The object a value of type `dd` is: a reference its referent, cv peeled.
static DataDef *object_type(DataDef *dd)
{
    dd = TokenSubscript::referent_type(dd ? dd->unqualified() : NULL);
    return dd ? dd->unqualified() : NULL;
}

// A member's type in a struct or class: a data member (a base's are already
// in `members`, flattened), else a static one, own or a base's. NULL when
// there is none, or it is a method (whose call completes nothing yet).
static DataDef *member_type(DataDef *dd, const std::string &name)
{
    DataDefSTRUCT *st = dd ? dd->as_struct_dd() : NULL;
    if ( !st )
	return NULL;
    if ( DataDef *m = st->m_type(name) )
	return m;
    DataDefCLASS *cls = dd->as_class_dd();
    if ( !cls )
	return NULL;
    std::map<std::string, DataDef *>::const_iterator s =
	cls->static_member_types.find(name);
    if ( s != cls->static_member_types.end() )
	return s->second;
    for ( size_t b = 0; b < cls->bases.size(); ++b )
	if ( DataDef *m = member_type(cls->bases[b].base, name) )
	    return m;
    return NULL;
}

// What a top-level entry may write after an object of type `dd` and `.`:
// its public data members, methods and static members, and its bases'. A
// private or protected member is refused there (access_flag_violation's
// rule: an entry is no member and no friend); madc's carrier is a class
// whose methods are the script methods.
static void offer_object_members(const DataDef *dd, CompletionOffer &offer,
				 std::set<const DataDef *> &seen)
{
    const DataDefSTRUCT *st = dd ? dd->as_struct_dd() : NULL;
    if ( !st || !seen.insert(st).second )
	return;
    for ( size_t i = 0; i < st->members.size(); ++i )
	if ( !st->m_access(st->members[i].first) )
	    offer(st->members[i].first);
    const DataDefCLASS *cls = dd->as_class_dd();
    if ( !cls )
	return;
    for ( std::map<std::string, Variable *>::const_iterator m = cls->method_map.begin();
	  m != cls->method_map.end(); ++m )
	if ( m->second && !(m->second->flags & (vfPRIVATE | vfPROTECTED)) )
	    offer(m->first);
    for ( std::map<std::string, DataDef *>::const_iterator s = cls->static_member_types.begin();
	  s != cls->static_member_types.end(); ++s )
	offer(s->first);
    for ( size_t b = 0; b < cls->bases.size(); ++b )
	offer_object_members(cls->bases[b].base, offer, seen);
}

void Program::completion_members(const std::vector<std::string> &chain,
				 const std::vector<bool> &arrows,
				 const std::string &word, std::vector<std::string> &out)
{
    if ( chain.empty() || chain.size() != arrows.size() || !tkProgram )
	return;
    // The root: a session name, found through the program scope's index
    // (which materializes nothing), or a result name (D12), whose aliased
    // form points at the value. A reference variable is held as the pointer
    // madc lowers it to.
    DataDef *dd = NULL;
    if ( Variable *v = tkProgram->findVariable(strpool, chain[0]) )
    {
	dd = v->type;
	if ( v->is_reference() )
	    dd = pointer_dd_of(dd) ? pointer_dd_of(dd)->base_type : NULL;
    }
    else if ( const SessionResult *r = session_result_named(chain[0]) )
    {
	dd = r->object ? r->object->type : NULL;
	if ( r->alias )
	    dd = pointer_dd_of(dd) ? pointer_dd_of(dd)->base_type : NULL;
    }
    // Each link: `arrows[i]` is the join after chain[i].
    for ( size_t i = 0; dd; ++i )
    {
	dd = object_type(dd);
	if ( arrows[i] )
	{
	    // `->` takes one pointer level; a class's operator-> is later.
	    DataDefPTR *p = pointer_dd_of(dd);
	    dd = p ? object_type(p->base_type) : NULL;
	}
	else if ( dd && dd->is_pointer() )
	    dd = NULL;		// `.` on a pointer names no member
	if ( !dd || i + 1 == chain.size() )
	    break;
	dd = member_type(dd, chain[i + 1]);
    }
    CompletionOffer offer(word);
    std::set<const DataDef *> seen;
    offer_object_members(dd, offer, seen);
    offer.take(out);
}

// The class a qualifier names, found in the type registries: a global type
// or class, or a namespace's type (`std::string`). NULL for anything else.
DataDefCLASS *Program::completion_scope_class(const std::string &scope)
{
    const std::string ns = spelling_namespace(scope);
    const std::string name = spelling_leaf(scope);
    DataDef *dd = NULL;
    if ( ns.empty() )
    {
	if ( TokenDataType *const *tdt = datatype_map.find_readonly(name) )
	    dd = *tdt ? &(*tdt)->definition : NULL;
	if ( !dd )
	{
	    datadef_map_citer it = struct_map.find(name);
	    dd = it != struct_map.end() ? it->second : NULL;
	}
    }
    else if ( const datatype_map_t *types = namespace_datatype_map.find_readonly(ns) )
    {
	datatype_map_t::const_iterator it = types->find(name);
	dd = it != types->end() && it->second ? &it->second->definition : NULL;
    }
    dd = object_type(dd);
    return dd ? dd->as_class_dd() : NULL;
}

void Program::completion_scope_names(const std::string &scope, const std::string &word,
				     std::vector<std::string> &out)
{
    CompletionOffer offer(word);
    // A namespace: its members (a scoped enum's enumerators too, as it is
    // registered as one), types, nested namespaces and templates.
    namespace_map_t::iterator ns = namespace_map.find(scope);
    if ( ns != namespace_map.end() )
	for ( variable_map_iter m = ns->second.begin(); m != ns->second.end(); ++m )
	    if ( m->second && !(m->second->flags & vfINSTPRODUCT) )
		offer(m->first);
    if ( const datatype_map_t *types = namespace_datatype_map.find_readonly(scope) )
	for ( datatype_map_t::const_iterator it = types->begin(); it != types->end(); ++it )
	    if ( it->second && !instantiation_type(it->second->definition) )
		offer(it->first);
    const std::string inner = scope + "::";
    for ( namespace_map_t::iterator it = namespace_map.lower_bound(inner);
	  it != namespace_map.end() && it->first.compare(0, inner.size(), inner) == 0;
	  ++it )
	offer(it->first.substr(inner.size()));	// a deeper one is no identifier
    template_map.for_each_readonly([&](const char *, const template_registry_entry_t &r) -> bool {
	for ( size_t i = 0; i < r.namespace_variants.size(); ++i )
	    if ( r.namespace_variants[i].defining_namespace == scope )
		offer(r.namespace_variants[i].class_name);
	return false;
    });
    fn_template_map.for_each_readonly([&](const char *key, const std::vector<FnTemplateDef> &defs) -> bool {
	for ( size_t i = 0; i < defs.size(); ++i )
	    if ( defs[i].ns == scope )
	    {
		offer(spelling_leaf(key));
		break;
	    }
	return false;
    });
    // A class: what its object gives, which an entry writes qualified too
    // (`&C::get`, `sizeof(C::w)`, `C::count`), and its nested types. The
    // class's alias to itself is its injected-class-name ([class.pre]/2),
    // which after `C::` names the constructor.
    if ( const DataDefCLASS *cls = completion_scope_class(scope) )
    {
	std::set<const DataDef *> seen;
	offer_object_members(cls, offer, seen);
	for ( std::map<std::string, DataDef *>::const_iterator t = cls->type_aliases.begin();
	      t != cls->type_aliases.end(); ++t )
	    if ( t->second != cls )
		offer(t->first);
    }
    offer.take(out);
}
