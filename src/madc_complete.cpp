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
 * The top-level walk has a second consumer, `?name` (plan §41.8a, slice 2):
 * visit_top_level_names hands each candidate with its entity, and
 * describe_name keeps the ones spelled exactly as the name, under the same
 * name rule, so `?` describes a name exactly when Tab would offer it.
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
#include "madc_type_spelling.h"
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
    // Does the rule offer `n` for the word? `?name` asks it of the exact name
    // (the word is the name), so it describes only what Tab would offer.
    bool accepts(const std::string &n) const
    {
	if ( n.size() < word.size() || n.compare(0, word.size(), word) != 0 )
	    return false;
	// A name is written as an identifier: a qualified key, an instantiation
	// or an operator is not one.
	for ( size_t i = 0; i < n.size(); ++i )
	    if ( !madc::hub::text_buffer::word_byte(n[i]) )
		return false;
	// The session's own names never.
	if ( n.compare(0, 7, "__madc_") == 0 )
	    return false;
	// A reserved name completes only a word shaped like one (IPython's
	// rule for `_`, and C++'s reservation of `__`), so the implementation's
	// names and madc's lowered ones stay out of the way.
	return reserved_word || !reserved_name(n);
    }
    void operator()(const std::string &n)
    {
	if ( accepts(n) )
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

void Program::visit_top_level_names(CompletionContext ctx,
				    const std::function<void(const TopLevelName &)> &visit)
{
    typedef TopLevelName::Kind Kind;
    auto named = [&](Kind k, const std::string &n) {
	visit(TopLevelName(k, n));
    };
    // After struct / union / enum: a tag.
    if ( ctx == CompletionContext::Tag )
    {
	for ( datadef_map_citer it = struct_map.begin(); it != struct_map.end(); ++it )
	    if ( it->second && spelling_namespace(it->second->canonical_cpp_spelling()).empty() )
	    {
		TopLevelName n(Kind::tag, it->first);
		n.type = it->second;
		visit(n);
	    }
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
		{
		    FuncDef *fd = v->type ? v->type->as_funcdef_dd() : NULL;
		    TopLevelName n(fd ? Kind::function : Kind::object, v->name);
		    n.var = v;
		    n.fd = fd;
		    visit(n);
		}
    for ( funcdef_map_iter it = funcdef_map.begin(); it != funcdef_map.end(); ++it )
    {
	FuncDef *fd = it->second;
	// A class method completes after its object (slice 4).
	if ( !fd || !fd->method_display_name.empty()
	     || !visible.count(fd->namespace_name) )
	    continue;
	TopLevelName n(Kind::function, fd->function_display_name.empty()
				       ? it->first : fd->function_display_name);
	n.fd = fd;
	visit(n);
    }
    // Types, and in C++ and madc a class's name, which is a type name there.
    datatype_map.for_each_readonly([&](const char *key, TokenDataType *const &tdt) -> bool {
	if ( !tdt || type_reachable(&tdt->definition) )
	{
	    TopLevelName n(Kind::type, key);
	    n.type = tdt ? &tdt->definition : NULL;
	    visit(n);
	}
	return false;
    });
    if ( presents_as_cpp() )
    {
	for ( datadef_map_citer it = struct_map.begin(); it != struct_map.end(); ++it )
	    if ( type_reachable(it->second) )
	    {
		TopLevelName n(Kind::tag, it->first);
		n.type = it->second;
		visit(n);
	    }
	// Top-level namespaces (a scoped enum is registered as one, and its
	// name is a type name).
	for ( namespace_map_t::const_iterator it = namespace_map.begin();
	      it != namespace_map.end(); ++it )
	    named(Kind::name_space, it->first);
	template_map.for_each_readonly([&](const char *, const template_registry_entry_t &r) -> bool {	/* identity-read: names and namespaces only */
	    for ( size_t i = 0; i < r.namespace_variants.size(); ++i )
		if ( visible.count(r.namespace_variants[i].defining_namespace) )
		    named(Kind::class_template, r.namespace_variants[i].class_name);
	    return false;
	});
	fn_template_map.for_each_readonly([&](const char *key, const std::vector<FnTemplateDef> &defs) -> bool {	/* identity-read: names and namespaces only */
	    for ( size_t i = 0; i < defs.size(); ++i )
		if ( visible.count(defs[i].ns) )
		{
		    named(Kind::function_template, key);
		    break;
		}
	    return false;
	});
    }
    // The standard's keywords (keyword_map is gated by --std=), and macros.
    keyword_map.for_each_readonly([&](const char *key, TokenKeyword *const &) -> bool {
	named(Kind::keyword, key);
	return false;
    });
    define_map.for_each_readonly([&](const char *key, const std::string &body) -> bool {
	TopLevelName n(Kind::macro, key);
	n.definition = &body;
	visit(n);
	return false;
    });
    macro_map.for_each_readonly([&](const char *key, const MacroDef &m) -> bool {
	TopLevelName n(Kind::macro, key);
	n.macro = &m;
	visit(n);
	return false;
    });
    // Names an included header registers on first use, and in the madc
    // dialect the words the auto-include scan serves (`println`, `php`).
    for ( std::map<std::string, LazyEntry>::const_iterator it = lazy_map.begin();
	  it != lazy_map.end(); ++it )
	named(Kind::header_name, it->first);
    std::vector<std::string> words;
    auto_include_words(words);
    for ( size_t i = 0; i < words.size(); ++i )
	named(Kind::dialect_word, words[i]);
    // The result names (D12), once a value is kept.
    size_t kept = 0;
    for ( size_t i = 0; i < session_results.size(); ++i )
	if ( session_results[i].object )
	{
	    ++kept;
	    named(Kind::result, "_" + std::to_string(session_results[i].entry));
	}
    if ( kept )
    {
	named(Kind::result, "ans");
	named(Kind::result, "_");
    }
    if ( kept > 1 )
	named(Kind::result, "__");
    if ( kept > 2 )
	named(Kind::result, "___");
}

void Program::completion_names(const std::string &word, CompletionContext ctx,
			       std::vector<std::string> &out)
{
    CompletionOffer offer(word);
    visit_top_level_names(ctx, [&](const TopLevelName &n) { offer(n.name); });
    offer.take(out);
}

// `?name` (plan §41.8a, slice 2): what the session knows of a name, from the
// same walk completion reads, so `?` describes exactly the entities Tab
// would offer for the name typed whole. IPython's fields; a function's
// overloads each on their own line with their location, Julia's way. A
// location is the one the Program recorded, never invented: an object's or
// a type's TopDecl, a function's definition in the session's tree, a
// header prototype's file.
namespace {

// A field at IPython's alignment: its value at column 12.
void describe_field(std::string &out, const std::string &label,
		    const std::string &value)
{
    const std::string l = label.empty() ? std::string() : label + ":";
    out += (out.empty() || out[out.size() - 1] == '\n' ? "" : "\n")
	 + l + std::string(l.size() < 11 ? 11 - l.size() : 1, ' ') + value;
}

std::string where(const char *file, int line)
{
    if ( !file || !*file )
	return std::string();
    return "@ " + std::string(file)
	 + (line > 0 ? ":" + std::to_string(line) : std::string());
}

std::string top_decl_where(const Program::TopDecl &td)
{
    return td.origin ? where(td.origin->file, td.origin->line)
		     : where(td.file, td.line);
}

} // namespace

// Where an object was declared: its latest TopDecl (a redeclaration's).
std::string Program::object_location(const Variable *v) const
{
    for ( size_t i = top_decls.size(); i-- > 0; )
	if ( top_decls[i].kind == DeclKind::dkGlobalVar && top_decls[i].var == v )
	    return top_decl_where(top_decls[i]);
    return std::string();
}

// Where a type name was declared: the TopDecl of that name for that type,
// the tag's (its definition's) or the typedef's. A typedef names its target
// (`typedef struct P Pt;` records P), so the name tells the two apart.
std::string Program::type_location(const std::string &name, const DataDef *dd) const
{
    for ( size_t i = 0; i < top_decls.size(); ++i )
    {
	const TopDecl &td = top_decls[i];
	if ( td.kind != DeclKind::dkGlobalVar && td.name == name
	     && (td.kind == DeclKind::dkTypedef
		 ? td.tdt && &td.tdt->definition == dd : td.dd == dd) )
	    return top_decl_where(td);
    }
    return std::string();
}

// Where a function was defined: the latest definition in the session's tree
// (a redefinition replaces the earlier one, D5), else a prototype's file.
std::string Program::function_location(const Variable *v, const FuncDef *fd) const
{
    for ( size_t i = pending_funcs.size(); i-- > 0; )
    {
	TokenFunc *tf = pending_funcs[i] ? pending_funcs[i]->as_func_tok() : NULL;
	if ( tf && ((v && &tf->var == v) || tf->var.type == fd) )
	    return where(tf->file, tf->line);
    }
    return where(fd->decl_file, 0);
}

bool Program::describe_name(const std::string &name, std::string &out)
{
    typedef TopLevelName::Kind Kind;
    out.clear();
    // The candidates Tab would offer for the name typed whole, each entity
    // once (a function is both a global and a funcdef_map entry, a C++
    // class both a type and a tag).
    const CompletionOffer rule(name);
    std::vector<TopLevelName> found;
    std::set<std::pair<int, const void *> > seen;
    auto keep = [&](const TopLevelName &visited) {
	if ( visited.name != name || !rule.accepts(visited.name) )
	    return;
	// A function template's placeholder is no function: its parameters
	// are none the template declares.
	TopLevelName n = visited;
	if ( n.kind == Kind::function && n.fd && n.fd->stands_for_function_template() )
	    n.kind = Kind::function_template;
	const Kind group = n.kind == Kind::tag ? Kind::type : n.kind;
	const void *id = n.fd ? (const void *)n.fd
		       : n.var ? (const void *)n.var
		       : n.type ? (const void *)n.type
		       : n.definition ? (const void *)n.definition
		       : (const void *)n.macro;
	if ( seen.insert(std::make_pair((int)group, id)).second )
	    found.push_back(n);
    };
    visit_top_level_names(CompletionContext::Name, keep);
    visit_top_level_names(CompletionContext::Tag, keep);
    // A type keyword (`int`, `bool`) is registered as its type, not in the
    // keyword map: the type found under its own spelling is the keyword too.
    if ( keyword_origin(name) )
    {
	bool keyword = false, type = false;
	TypeSpeller own(this);
	for ( size_t i = 0; i < found.size(); ++i )
	{
	    keyword |= found[i].kind == Kind::keyword;
	    type |= found[i].kind == Kind::type && own.shown(found[i].type) == name;
	}
	if ( type && !keyword )
	    found.push_back(TopLevelName(Kind::keyword, name));
    }
    // A function's Variable carries its Method, whose parameters have names;
    // an overload registered under its own symbol is found by identity.
    std::map<const FuncDef *, Variable *> fn_var;
    if ( tkProgram )
	for ( size_t i = 0; i < tkProgram->variables.size(); ++i )
	    if ( Variable *v = tkProgram->variables[i] )
		if ( FuncDef *fd = v->type ? v->type->as_funcdef_dd() : NULL )
		    fn_var.insert(std::make_pair(fd, v));
    TypeSpeller speller(this);
    std::vector<std::string> sections;
    auto section_of = [&](Kind k) {
	std::string s;
	for ( size_t i = 0; i < found.size(); ++i )
	{
	    const TopLevelName &n = found[i];
	    if ( (n.kind == Kind::tag ? Kind::type : n.kind) != k )
		continue;
	    switch ( k )
	    {
		case Kind::macro:
		{
		    std::string def = "#define " + name;
		    if ( n.macro )
		    {
			def += "(";
			for ( size_t p = 0; p < n.macro->params.size(); ++p )
			    def += (p ? ", " : "") + n.macro->params[p];
			if ( n.macro->variadic )
			    def += (n.macro->params.empty() ? "" : ", ")
				 + n.macro->variadic_param + "...";
			def += ")";
			if ( !n.macro->body.empty() )
			    def += " " + n.macro->body;
		    }
		    else if ( n.definition && !n.definition->empty() )
			def += " " + *n.definition;
		    describe_field(s, "Macro", def);
		    break;
		}
		case Kind::keyword:
		{
		    // Where it comes from, not the session's standard: `while`
		    // is C's, `class` C++'s, `constexpr` C++11's.
		    const std::string from = keyword_provenance(name);
		    s += name + (from.empty() ? " is a keyword" : " is a keyword of " + from);
		    break;
		}
		case Kind::type:
		{
		    // A typedef names another type: `size_t` is unsigned long.
		    const std::string spelled = speller.shown(n.type);
		    describe_field(s, n.kind == Kind::type && spelled != name
				      ? "Typedef" : "Type", spelled);
		    std::string at = type_location(name, n.type);
		    if ( !at.empty() )
			describe_field(s, "Defined", at);
		    DataDefSTRUCT *st = n.type ? n.type->unqualified()->as_struct_dd() : NULL;
		    bool first = true;
		    if ( st )
			for ( size_t m = 0; m < st->members.size(); ++m )
			{
			    const std::string &mn = st->members[m].first;
			    // What an entry may write: a public member, never one
			    // of the session's own.
			    if ( mn.empty() || st->m_access(mn)
				 || mn.compare(0, 7, "__madc_") == 0 )
				continue;
			    DataDef *mt = member_array_type(*st, mn);
			    describe_field(s, first ? "Members" : "",
					   speller.declared(mt ? mt : st->m_type(mn), mn));
			    first = false;
			}
		    if ( DataDefCLASS *cls = n.type ? n.type->unqualified()->as_class_dd() : NULL )
		    {
			for ( std::map<std::string, DataDef *>::const_iterator t =
				  cls->static_member_types.begin();
			      t != cls->static_member_types.end(); ++t )
			{
			    describe_field(s, first ? "Members" : "",
					   "static " + speller.declared(t->second, t->first));
			    first = false;
			}
			for ( std::map<std::string, Variable *>::const_iterator m =
				  cls->method_map.begin(); m != cls->method_map.end(); ++m )
			{
			    Variable *mv = m->second;
			    FuncDef *mfd = mv && mv->type ? mv->type->as_funcdef_dd() : NULL;
			    if ( !mfd || (mv->flags & (vfPRIVATE | vfPROTECTED)) )
				continue;
			    describe_field(s, first ? "Members" : "",
					   speller.signature(m->first, mfd,
							     static_cast<Method *>(mv->data)));
			    first = false;
			}
		    }
		    break;
		}
		case Kind::object:
		{
		    DataDef *at = object_array_type(*n.var);
		    describe_field(s, "Type", speller.shown(at ? at : n.var->type));
		    std::string where_at = object_location(n.var);
		    if ( !where_at.empty() )
			describe_field(s, "Defined", where_at);
		    break;
		}
		case Kind::function:
		{
		    std::map<const FuncDef *, Variable *>::const_iterator v = fn_var.find(n.fd);
		    Variable *fv = n.var ? n.var : v != fn_var.end() ? v->second : NULL;
		    std::string at = function_location(fv, n.fd);
		    describe_field(s, "Signature",
				   speller.signature(name, n.fd,
						     fv ? static_cast<Method *>(fv->data) : NULL)
				   + (at.empty() ? "" : "  " + at));
		    break;
		}
		case Kind::name_space:
		    describe_field(s, "Type", "namespace");
		    break;
		case Kind::class_template:
		    describe_field(s, "Type", "class template");
		    break;
		case Kind::function_template:
		    if ( s.empty() )	// its placeholders and its registry entry
			describe_field(s, "Type", "function template");
		    break;
		case Kind::result:
		{
		    const SessionResult *r = session_result_named(name);
		    if ( !r || !r->object )
			break;
		    DataDef *t = r->object->type;
		    if ( r->alias )
			t = pointer_dd_of(t) ? pointer_dd_of(t)->base_type : NULL;
		    describe_field(s, "Type", speller.shown(t));
		    describe_field(s, "Result", "REPL[" + std::to_string(r->entry) + "]");
		    break;
		}
		case Kind::header_name:
		    describe_field(s, "Declared", "by an included header, registered at its first use");
		    break;
		case Kind::dialect_word:
		    describe_field(s, "Declared", "by the madc dialect, included at its first use");
		    break;
		case Kind::tag:
		    break;
	    }
	}
	if ( k == Kind::function && !s.empty() )
	    describe_field(s, "Type", "function");
	if ( !s.empty() )
	    sections.push_back(s);
    };
    // A macro first (it stands in for the name before any parse), then a
    // keyword, then the entities in the order a declaration names them: a
    // type (a C tag before the object of the same name, `struct stat` and
    // `stat`), a namespace or template, an object, a function; a result
    // name; last what is only declared so far, to be registered at its use.
    const Kind order[] = {
	Kind::macro, Kind::keyword, Kind::type, Kind::name_space,
	Kind::class_template, Kind::function_template, Kind::object,
	Kind::function, Kind::result, Kind::header_name, Kind::dialect_word
    };
    // What is only declared so far is described when nothing else is: the
    // parse above registered what a header or the dialect serves.
    for ( size_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i )
	if ( sections.empty()
	     || (order[i] != Kind::header_name && order[i] != Kind::dialect_word) )
	    section_of(order[i]);
    for ( size_t i = 0; i < sections.size(); ++i )
	out += (i ? "\n\n" : "") + sections[i];
    if ( out.empty() )
    {
	out = "'" + name + "' is not declared";
	return false;
    }
    return true;
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
    template_map.for_each_readonly([&](const char *, const template_registry_entry_t &r) -> bool {	/* identity-read: names and namespaces only */
	for ( size_t i = 0; i < r.namespace_variants.size(); ++i )
	    if ( r.namespace_variants[i].defining_namespace == scope )
		offer(r.namespace_variants[i].class_name);
	return false;
    });
    fn_template_map.for_each_readonly([&](const char *key, const std::vector<FnTemplateDef> &defs) -> bool {	/* identity-read: names and namespaces only */
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
