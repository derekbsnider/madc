/* madc_complete.cpp — the completion service (plan §41.7a, slice 3).
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
 *     symbols and throws (plan §8's code check).
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
    // An empty word completes nothing (Julia), and a number is not a name.
    if ( word.empty() || (word[0] >= '0' && word[0] <= '9') )
	return out;
    CompletionContext ctx = completion_context(text.substr(0, s));
    // A member after `.`, `->` or `::` is slice 4's.
    if ( ctx != CompletionContext::Name && ctx != CompletionContext::Tag )
	return out;
    completion_names(word, ctx, out);
    return out;
}

Program::CompletionContext Program::completion_context(const std::string &before)
{
    // The probe stands where the word is. It can be no macro and no
    // declared name: the session reserves the __madc_ prefix.
    static const char probe[] = "__madc_completion_probe";
    static const char display[] = "<completion>";
    CompletionContext ctx = CompletionContext::None;
    EntryTransaction attempt(*this);
    ParseModeScope mode(*this, ParseMode::InteractiveEntry);
    DiagnosticRenderMute mute;
    // The attempt's diagnostics and tokens go with it, however it ends, as
    // an offered attempt's do at the next one.
    struct AttemptEnd
    {
	Program &pgm;
	~AttemptEnd() { pgm.begin_entry(); }
    } attempt_end = { *this };
    begin_entry();
    if ( !lex_entry(before + probe, display) )
	return ctx;		// the word is in an unclosed string or comment
    const char *fname = intern_file(display);
    TokenBase *last = NULL, *prev = NULL;
    for ( size_t i = 0; i < tokens.size(); ++i )
    {
	TokenBase *t = tokens[i];
	if ( !t || !t->file || strcmp(t->file, fname) != 0
	     || t->id() == TokenID::tkEndOfEntry )
	    continue;
	prev = last;
	last = t;
    }
    // A comment or a directive swallowed the probe: no name is written there.
    if ( !last || last->type() != TokenType::ttIdentifier
	 || !((TokenIdent *)last)->spelling_is(probe) )
	return ctx;
    ctx = CompletionContext::Name;
    if ( prev )
	switch ( prev->id() )
	{
	    case TokenID::tkDot:
	    case TokenID::tkDeRef:
		ctx = CompletionContext::Member;
		break;
	    case TokenID::tkNS:
		ctx = CompletionContext::Qualified;
		break;
	    case TokenID::tkSTRUCT:
	    case TokenID::tkUNION:
	    case TokenID::tkENUM:
		ctx = CompletionContext::Tag;
		break;
	    case TokenID::tkHash:
		// A directive's name (`#inc`), or a macro body's `#` operand:
		// no session name is written there.
		ctx = CompletionContext::None;
		break;
	    default:
		break;
	}
    return ctx;
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

void Program::completion_names(const std::string &word, CompletionContext ctx,
			       std::vector<std::string> &out)
{
    std::set<std::string> names;
    // A reserved name completes only a word shaped like one (IPython's rule
    // for `_`, and C++'s reservation of `__`), so the implementation's names
    // and madc's lowered ones stay out of the way.
    const bool reserved_word = reserved_name(word);
    auto offer = [&](const std::string &n) {
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
	if ( !reserved_word && reserved_name(n) )
	    return;
	names.insert(n);
    };
    // After struct / union / enum: a tag.
    if ( ctx == CompletionContext::Tag )
    {
	for ( datadef_map_citer it = struct_map.begin(); it != struct_map.end(); ++it )
	    if ( it->second && spelling_namespace(it->second->canonical_cpp_spelling()).empty() )
		offer(it->first);
	out.assign(names.begin(), names.end());
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
	const std::string &sp = dd->canonical_cpp_spelling();
	return sp.find('<') == std::string::npos	// an instantiation
	    && visible.count(spelling_namespace(sp));
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
    datatype_map.for_each([&](const char *key, TokenDataType *&tdt) -> bool {
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
	template_map.for_each([&](const char *, template_registry_entry_t &r) -> bool {
	    for ( size_t i = 0; i < r.namespace_variants.size(); ++i )
		if ( visible.count(r.namespace_variants[i].defining_namespace) )
		    offer(r.namespace_variants[i].class_name);
	    return false;
	});
	fn_template_map.for_each([&](const char *key, std::vector<FnTemplateDef> &defs) -> bool {
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
    keyword_map.for_each([&](const char *key, TokenKeyword *&) -> bool {
	offer(key);
	return false;
    });
    define_map.for_each([&](const char *key, std::string &) -> bool {
	offer(key);
	return false;
    });
    macro_map.for_each([&](const char *key, MacroDef &) -> bool {
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
    out.assign(names.begin(), names.end());
}
