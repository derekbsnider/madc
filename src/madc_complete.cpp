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

void Program::completion_names(const std::string &word, CompletionContext ctx,
			       std::vector<std::string> &out)
{
    std::set<std::string> names;
    const bool underscored = !word.empty() && word[0] == '_';
    auto offer = [&](const std::string &n) {
	if ( n.size() < word.size() || n.compare(0, word.size(), word) != 0 )
	    return;
	// A qualified or instantiated key is no name written at the top level.
	if ( n.find_first_of(":<> ") != std::string::npos )
	    return;
	// The session's own names never; a reserved one (`__builtin_*`,
	// `_IO_*`) only for a word that starts with `_` (IPython's rule).
	if ( n.compare(0, 7, "__madc_") == 0 )
	    return;
	if ( !underscored && !n.empty() && n[0] == '_' )
	    return;
	names.insert(n);
    };
    // After struct / union / enum: a tag.
    if ( ctx == CompletionContext::Tag )
    {
	for ( datadef_map_citer it = struct_map.begin(); it != struct_map.end(); ++it )
	    offer(it->first);
	out.assign(names.begin(), names.end());
	return;
    }
    // Objects and functions: every entry's and every included header's.
    if ( tkProgram )
	for ( size_t i = 0; i < tkProgram->variables.size(); ++i )
	    if ( Variable *v = tkProgram->variables[i] )
		offer(v->name);
    for ( funcdef_map_iter it = funcdef_map.begin(); it != funcdef_map.end(); ++it )
	if ( it->second && it->second->method_display_name.empty() )
	    offer(it->first);	// a class method completes after its object
    // Types, and in C++ and madc a class's name, which is a type name there.
    datatype_map.for_each([&](const char *key, TokenDataType *&) -> bool {
	offer(key);
	return false;
    });
    if ( presents_as_cpp() )
    {
	for ( datadef_map_citer it = struct_map.begin(); it != struct_map.end(); ++it )
	    offer(it->first);
	for ( namespace_map_t::const_iterator it = namespace_map.begin();
	      it != namespace_map.end(); ++it )
	    offer(it->first);
	template_map.for_each([&](const char *, template_registry_entry_t &r) -> bool {
	    for ( size_t i = 0; i < r.namespace_variants.size(); ++i )
		offer(r.namespace_variants[i].class_name);
	    return false;
	});
	fn_template_map.for_each([&](const char *key, std::vector<FnTemplateDef> &) -> bool {
	    offer(key);
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
